/*
 * Xtensa ISA:
 * http://www.tensilica.com/products/literature-docs/documentation/xtensa-isa-databook.htm
 *
 * Copyright (c) 2011, Max Filippov, Open Source and Linux Lab.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Open Source and Linux Lab nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/helper-gen.h"
#include "exec/helper-proto.h"
#include "exec/log.h"
#include "exec/translator.h"
#include "qemu/log.h"
#include "qemu/qemu-print.h"
#include "semihosting/semihost.h"
#include "tcg/tcg-op.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

#include "translate.h"

/* Dynamic simulator addresses — resolved from firmware ELF at load time. */
#include "hw/xtensa/mofei-sim-addrs.h"

extern uint32_t mofei_sim_wrap_malloc_addr;
extern uint32_t mofei_sim_reset_display_update_control_addr;
extern uint32_t mofei_display_writeLutFull_addr;
extern uint32_t mofei_display_writeLutFast_addr;
extern uint32_t mofei_display_writeLutDu_addr;

#define MOFEI_SIM_APB_FREQUENCY_HZ 80000000u
#define MOFEI_ESP32S3_ROM_DELAY_US_ADDR 0x40000600u

static TCGv_i32 cpu_pc;
static TCGv_i32 cpu_R[16];
static TCGv_i32 cpu_FR[16];
static TCGv_i64 cpu_FRD[16];
static TCGv_i32 cpu_MR[4];
static TCGv_i32 cpu_BR[16];
static TCGv_i32 cpu_BR4[4];
static TCGv_i32 cpu_BR8[2];
TCGv_i32 cpu_SR[256];
static TCGv_i32 cpu_UR[256];
static TCGv_i32 cpu_windowbase_next;
static TCGv_i32 cpu_exclusive_addr;
static TCGv_i32 cpu_exclusive_val;

static GHashTable* xtensa_regfile_table;

static char* sr_name[256];
static char* ur_name[256];

void xtensa_collect_sr_names(const XtensaConfig* config) {
  xtensa_isa isa = config->isa;
  int n = xtensa_isa_num_sysregs(isa);
  int i;

  for (i = 0; i < n; ++i) {
    int sr = xtensa_sysreg_number(isa, i);

    if (sr >= 0 && sr < 256) {
      const char* name = xtensa_sysreg_name(isa, i);
      char** pname = (xtensa_sysreg_is_user(isa, i) ? ur_name : sr_name) + sr;

      if (*pname) {
        if (strstr(*pname, name) == NULL) {
          char* new_name = malloc(strlen(*pname) + strlen(name) + 2);

          strcpy(new_name, *pname);
          strcat(new_name, "/");
          strcat(new_name, name);
          free(*pname);
          *pname = new_name;
        }
      } else {
        *pname = strdup(name);
      }
    }
  }
}

void xtensa_translate_init(void) {
  static const char* const regnames[] = {
      "ar0", "ar1", "ar2",  "ar3",  "ar4",  "ar5",  "ar6",  "ar7",
      "ar8", "ar9", "ar10", "ar11", "ar12", "ar13", "ar14", "ar15",
  };
  static const char* const fregnames[] = {
      "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15",
  };
  static const char* const mregnames[] = {
      "m0",
      "m1",
      "m2",
      "m3",
  };
  static const char* const bregnames[] = {
      "b0", "b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8", "b9", "b10", "b11", "b12", "b13", "b14", "b15",
  };
  int i;

  cpu_pc = tcg_global_mem_new_i32(tcg_env, offsetof(CPUXtensaState, pc), "pc");

  for (i = 0; i < 16; i++) {
    cpu_R[i] = tcg_global_mem_new_i32(tcg_env, offsetof(CPUXtensaState, regs[i]), regnames[i]);
  }

  for (i = 0; i < 16; i++) {
    cpu_FR[i] = tcg_global_mem_new_i32(tcg_env, offsetof(CPUXtensaState, fregs[i].f32[FP_F32_LOW]), fregnames[i]);
  }

  for (i = 0; i < 16; i++) {
    cpu_FRD[i] = tcg_global_mem_new_i64(tcg_env, offsetof(CPUXtensaState, fregs[i].f64), fregnames[i]);
  }

  for (i = 0; i < 4; i++) {
    cpu_MR[i] = tcg_global_mem_new_i32(tcg_env, offsetof(CPUXtensaState, sregs[MR + i]), mregnames[i]);
  }

  for (i = 0; i < 16; i++) {
    cpu_BR[i] = tcg_global_mem_new_i32(tcg_env, offsetof(CPUXtensaState, sregs[BR]), bregnames[i]);
    if (i % 4 == 0) {
      cpu_BR4[i / 4] = tcg_global_mem_new_i32(tcg_env, offsetof(CPUXtensaState, sregs[BR]), bregnames[i]);
    }
    if (i % 8 == 0) {
      cpu_BR8[i / 8] = tcg_global_mem_new_i32(tcg_env, offsetof(CPUXtensaState, sregs[BR]), bregnames[i]);
    }
  }

  for (i = 0; i < 256; ++i) {
    if (sr_name[i]) {
      cpu_SR[i] = tcg_global_mem_new_i32(tcg_env, offsetof(CPUXtensaState, sregs[i]), sr_name[i]);
    }
  }

  for (i = 0; i < 256; ++i) {
    if (ur_name[i]) {
      cpu_UR[i] = tcg_global_mem_new_i32(tcg_env, offsetof(CPUXtensaState, uregs[i]), ur_name[i]);
    }
  }

  cpu_windowbase_next = tcg_global_mem_new_i32(tcg_env, offsetof(CPUXtensaState, windowbase_next), "windowbase_next");
  cpu_exclusive_addr = tcg_global_mem_new_i32(tcg_env, offsetof(CPUXtensaState, exclusive_addr), "exclusive_addr");
  cpu_exclusive_val = tcg_global_mem_new_i32(tcg_env, offsetof(CPUXtensaState, exclusive_val), "exclusive_val");
}

void** xtensa_get_regfile_by_name(const char* name, int entries, int bits) {
  char* geometry_name;
  void** res;

  if (xtensa_regfile_table == NULL) {
    xtensa_regfile_table = g_hash_table_new(g_str_hash, g_str_equal);
    /*
     * AR is special. Xtensa translator uses it as a current register
     * window, but configuration overlays represent it as a complete
     * physical register file.
     */
    g_hash_table_insert(xtensa_regfile_table, (void*)"AR 16x32", (void*)cpu_R);
    g_hash_table_insert(xtensa_regfile_table, (void*)"AR 32x32", (void*)cpu_R);
    g_hash_table_insert(xtensa_regfile_table, (void*)"AR 64x32", (void*)cpu_R);

    g_hash_table_insert(xtensa_regfile_table, (void*)"MR 4x32", (void*)cpu_MR);

    g_hash_table_insert(xtensa_regfile_table, (void*)"FR 16x32", (void*)cpu_FR);
    g_hash_table_insert(xtensa_regfile_table, (void*)"FR 16x64", (void*)cpu_FRD);

    g_hash_table_insert(xtensa_regfile_table, (void*)"BR 16x1", (void*)cpu_BR);
    g_hash_table_insert(xtensa_regfile_table, (void*)"BR4 4x4", (void*)cpu_BR4);
    g_hash_table_insert(xtensa_regfile_table, (void*)"BR8 2x8", (void*)cpu_BR8);
  }

  geometry_name = g_strdup_printf("%s %dx%d", name, entries, bits);
  res = (void**)g_hash_table_lookup(xtensa_regfile_table, geometry_name);
  g_free(geometry_name);
  return res;
}

static inline bool option_enabled(DisasContext* dc, int opt) { return xtensa_option_enabled(dc->config, opt); }

static void init_sar_tracker(DisasContext* dc) {
  dc->sar_5bit = false;
  dc->sar_m32_5bit = false;
  dc->sar_m32 = NULL;
}

static void gen_right_shift_sar(DisasContext* dc, TCGv_i32 sa) {
  tcg_gen_andi_i32(cpu_SR[SAR], sa, 0x1f);
  if (dc->sar_m32_5bit) {
    tcg_gen_discard_i32(dc->sar_m32);
  }
  dc->sar_5bit = true;
  dc->sar_m32_5bit = false;
}

static void gen_left_shift_sar(DisasContext* dc, TCGv_i32 sa) {
  if (!dc->sar_m32) {
    dc->sar_m32 = tcg_temp_new_i32();
  }
  tcg_gen_andi_i32(dc->sar_m32, sa, 0x1f);
  tcg_gen_sub_i32(cpu_SR[SAR], tcg_constant_i32(32), dc->sar_m32);
  dc->sar_5bit = false;
  dc->sar_m32_5bit = true;
}

static void mofei_gen_reload_logical_window_from_env(void) {
  for (unsigned i = 0; i < 16; ++i) {
    tcg_gen_ld_i32(cpu_R[i], tcg_env, offsetof(CPUXtensaState, regs[i]));
  }
}

static void mofei_gen_reload_window_state_from_env(void) {
  tcg_gen_ld_i32(cpu_SR[WINDOW_BASE], tcg_env, offsetof(CPUXtensaState, sregs[WINDOW_BASE]));
  tcg_gen_ld_i32(cpu_SR[WINDOW_START], tcg_env, offsetof(CPUXtensaState, sregs[WINDOW_START]));
  tcg_gen_ld_i32(cpu_SR[PS], tcg_env, offsetof(CPUXtensaState, sregs[PS]));
  tcg_gen_ld_i32(cpu_windowbase_next, tcg_env, offsetof(CPUXtensaState, windowbase_next));
}

static void mofei_gen_sync_logical_window_to_env(void) {
  for (unsigned i = 0; i < 16; ++i) {
    tcg_gen_st_i32(cpu_R[i], tcg_env, offsetof(CPUXtensaState, regs[i]));
  }
  tcg_gen_st_i32(cpu_SR[WINDOW_BASE], tcg_env, offsetof(CPUXtensaState, sregs[WINDOW_BASE]));
  tcg_gen_st_i32(cpu_SR[WINDOW_START], tcg_env, offsetof(CPUXtensaState, sregs[WINDOW_START]));
  tcg_gen_st_i32(cpu_SR[PS], tcg_env, offsetof(CPUXtensaState, sregs[PS]));
  tcg_gen_st_i32(cpu_windowbase_next, tcg_env, offsetof(CPUXtensaState, windowbase_next));
}

static void gen_exception(DisasContext* dc, int excp) { gen_helper_exception(tcg_env, tcg_constant_i32(excp)); }

void gen_exception_cause(DisasContext* dc, uint32_t cause) {
  TCGv_i32 pc = tcg_constant_i32(dc->pc);
  gen_helper_exception_cause(tcg_env, pc, tcg_constant_i32(cause));
  if (cause == ILLEGAL_INSTRUCTION_CAUSE || cause == SYSCALL_CAUSE) {
    dc->base.is_jmp = DISAS_NORETURN;
  }
}

static void gen_debug_exception(DisasContext* dc, uint32_t cause) {
  TCGv_i32 pc = tcg_constant_i32(dc->pc);
  gen_helper_debug_exception(tcg_env, pc, tcg_constant_i32(cause));
  if (cause & (DEBUGCAUSE_IB | DEBUGCAUSE_BI | DEBUGCAUSE_BN)) {
    dc->base.is_jmp = DISAS_NORETURN;
  }
}

static bool gen_check_privilege(DisasContext* dc) {
#ifndef CONFIG_USER_ONLY
  if (!dc->cring) {
    return true;
  }
#endif
  gen_exception_cause(dc, PRIVILEGED_CAUSE);
  dc->base.is_jmp = DISAS_NORETURN;
  return false;
}

static bool gen_check_cpenable(DisasContext* dc, uint32_t cp_mask) {
  cp_mask &= ~dc->cpenable;

  if (option_enabled(dc, XTENSA_OPTION_COPROCESSOR) && cp_mask) {
    /* The simulator runs firmware synchronously without the ESP-IDF task
     * scheduler path that normally restores CPENABLE for floating-point work.
     * Activity renders can still contain FPU load/store instructions (for
     * example WeatherClockActivity); enable the requested coprocessor bits in
     * the TB and continue instead of falling into a blank guest exception
     * vector. */
    tcg_gen_ori_i32(cpu_SR[CPENABLE], cpu_SR[CPENABLE], cp_mask);
    dc->cpenable |= cp_mask;
  }
  return true;
}

static int gen_postprocess(DisasContext* dc, int slot);

static void gen_jump_slot(DisasContext* dc, TCGv dest, int slot) {
  tcg_gen_mov_i32(cpu_pc, dest);
  if (dc->icount) {
    tcg_gen_mov_i32(cpu_SR[ICOUNT], dc->next_icount);
  }
  if (dc->op_flags & XTENSA_OP_POSTPROCESS) {
    slot = gen_postprocess(dc, slot);
  }
  if (slot >= 0) {
    tcg_gen_goto_tb(slot);
    tcg_gen_exit_tb(dc->base.tb, slot);
  } else {
    tcg_gen_exit_tb(NULL, 0);
  }
  dc->base.is_jmp = DISAS_NORETURN;
}

static void gen_jump(DisasContext* dc, TCGv dest) { gen_jump_slot(dc, dest, -1); }

static int adjust_jump_slot(DisasContext* dc, uint32_t dest, int slot) {
  return translator_use_goto_tb(&dc->base, dest) ? slot : -1;
}

static void gen_jumpi(DisasContext* dc, uint32_t dest, int slot) {
  gen_jump_slot(dc, tcg_constant_i32(dest), adjust_jump_slot(dc, dest, slot));
}

static void gen_callw_slot(DisasContext* dc, int callinc, TCGv_i32 dest, int slot) {
  tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(callinc), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
  tcg_gen_movi_i32(cpu_R[callinc << 2], (callinc << 30) | (dc->base.pc_next & 0x3fffffff));
  gen_jump_slot(dc, dest, slot);
}

static bool gen_check_loop_end(DisasContext* dc, int slot) {
  if (dc->base.pc_next == dc->lend) {
    TCGLabel* label = gen_new_label();

    tcg_gen_brcondi_i32(TCG_COND_EQ, cpu_SR[LCOUNT], 0, label);
    tcg_gen_subi_i32(cpu_SR[LCOUNT], cpu_SR[LCOUNT], 1);
    if (dc->lbeg_off) {
      gen_jumpi(dc, dc->base.pc_next - dc->lbeg_off, slot);
    } else {
      gen_jump(dc, cpu_SR[LBEG]);
    }
    gen_set_label(label);
    gen_jumpi(dc, dc->base.pc_next, -1);
    return true;
  }
  return false;
}

static void gen_jumpi_check_loop_end(DisasContext* dc, int slot) {
  if (!gen_check_loop_end(dc, slot)) {
    gen_jumpi(dc, dc->base.pc_next, slot);
  }
}

static void gen_brcond(DisasContext* dc, TCGCond cond, TCGv_i32 t0, TCGv_i32 t1, uint32_t addr) {
  TCGLabel* label = gen_new_label();

  tcg_gen_brcond_i32(cond, t0, t1, label);
  gen_jumpi_check_loop_end(dc, 0);
  gen_set_label(label);
  gen_jumpi(dc, addr, 1);
}

static void gen_brcondi(DisasContext* dc, TCGCond cond, TCGv_i32 t0, uint32_t t1, uint32_t addr) {
  gen_brcond(dc, cond, t0, tcg_constant_i32(t1), addr);
}

static uint32_t test_exceptions_sr(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  return xtensa_option_enabled(dc->config, par[1]) ? 0 : XTENSA_OP_ILL;
}

static uint32_t test_exceptions_ccompare(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  unsigned n = par[0] - CCOMPARE;

  if (n >= dc->config->nccompare) {
    return XTENSA_OP_ILL;
  }
  return test_exceptions_sr(dc, arg, par);
}

static uint32_t test_exceptions_dbreak(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  unsigned n = MAX_NDBREAK;

  if (par[0] >= DBREAKA && par[0] < DBREAKA + MAX_NDBREAK) {
    n = par[0] - DBREAKA;
  }
  if (par[0] >= DBREAKC && par[0] < DBREAKC + MAX_NDBREAK) {
    n = par[0] - DBREAKC;
  }
  if (n >= dc->config->ndbreak) {
    return XTENSA_OP_ILL;
  }
  return test_exceptions_sr(dc, arg, par);
}

static uint32_t test_exceptions_ibreak(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  unsigned n = par[0] - IBREAKA;

  if (n >= dc->config->nibreak) {
    return XTENSA_OP_ILL;
  }
  return test_exceptions_sr(dc, arg, par);
}

static uint32_t test_exceptions_hpi(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  unsigned n = MAX_NLEVEL + 1;

  if (par[0] >= EXCSAVE1 && par[0] < EXCSAVE1 + MAX_NLEVEL) {
    n = par[0] - EXCSAVE1 + 1;
  }
  if (par[0] >= EPC1 && par[0] < EPC1 + MAX_NLEVEL) {
    n = par[0] - EPC1 + 1;
  }
  if (par[0] >= EPS2 && par[0] < EPS2 + MAX_NLEVEL - 1) {
    n = par[0] - EPS2 + 2;
  }
  if (n > dc->config->nlevel) {
    return XTENSA_OP_ILL;
  }
  return test_exceptions_sr(dc, arg, par);
}

MemOp gen_load_store_alignment(DisasContext* dc, MemOp mop, TCGv_i32 addr) {
  if ((mop & MO_SIZE) == MO_8) {
    return mop;
  }
  if ((mop & MO_AMASK) == MO_UNALN && !option_enabled(dc, XTENSA_OPTION_HW_ALIGNMENT)) {
    mop |= MO_ALIGN;
  }
  if (!option_enabled(dc, XTENSA_OPTION_UNALIGNED_EXCEPTION)) {
    tcg_gen_andi_i32(addr, addr, ~0 << memop_alignment_bits(mop));
  }
  return mop;
}

static bool gen_window_check(DisasContext* dc, uint32_t mask) {
  unsigned r = 31 - clz32(mask);

  if (r / 4 > dc->window) {
    TCGv_i32 pc = tcg_constant_i32(dc->pc);
    TCGv_i32 w = tcg_constant_i32(r / 4);

    gen_helper_window_check(tcg_env, pc, w);
    dc->base.is_jmp = DISAS_NORETURN;
    return false;
  }
  return true;
}

static TCGv_i32 gen_mac16_m(TCGv_i32 v, bool hi, bool is_unsigned) {
  TCGv_i32 m = tcg_temp_new_i32();

  if (hi) {
    (is_unsigned ? tcg_gen_shri_i32 : tcg_gen_sari_i32)(m, v, 16);
  } else {
    (is_unsigned ? tcg_gen_ext16u_i32 : tcg_gen_ext16s_i32)(m, v);
  }
  return m;
}

static void gen_zero_check(DisasContext* dc, const OpcodeArg arg[]) {
  TCGLabel* label = gen_new_label();

  tcg_gen_brcondi_i32(TCG_COND_NE, arg[2].in, 0, label);
  gen_exception_cause(dc, INTEGER_DIVIDE_BY_ZERO_CAUSE);
  gen_set_label(label);
}

static inline unsigned xtensa_op0_insn_len(DisasContext* dc, uint8_t op0) {
  return xtensa_isa_length_from_chars(dc->config->isa, &op0);
}

static bool mofei_decode_diag_pc(uint32_t pc) { return pc >= 0x420acadfu && pc <= 0x420acaf2u; }

static int gen_postprocess(DisasContext* dc, int slot) {
  uint32_t op_flags = dc->op_flags;

#ifndef CONFIG_USER_ONLY
  if (op_flags & XTENSA_OP_CHECK_INTERRUPTS) {
    translator_io_start(&dc->base);
    gen_helper_check_interrupts(tcg_env);
  }
#endif
  if (op_flags & XTENSA_OP_SYNC_REGISTER_WINDOW) {
    gen_helper_sync_windowbase(tcg_env);
    mofei_gen_reload_window_state_from_env();
    mofei_gen_reload_logical_window_from_env();
  }
  if (op_flags & XTENSA_OP_EXIT_TB_M1) {
    slot = -1;
  }
  return slot;
}

struct opcode_arg_copy {
  uint32_t resource;
  void* temp;
  OpcodeArg* arg;
};

struct opcode_arg_info {
  uint32_t resource;
  int index;
};

struct slot_prop {
  XtensaOpcodeOps* ops;
  OpcodeArg arg[MAX_OPCODE_ARGS];
  struct opcode_arg_info in[MAX_OPCODE_ARGS];
  struct opcode_arg_info out[MAX_OPCODE_ARGS];
  unsigned n_in;
  unsigned n_out;
  uint32_t op_flags;
};

enum resource_type {
  RES_REGFILE,
  RES_STATE,
  RES_MAX,
};

static uint32_t encode_resource(enum resource_type r, unsigned g, unsigned n) {
  assert(r < RES_MAX && g < 256 && n < 65536);
  return (r << 24) | (g << 16) | n;
}

static enum resource_type get_resource_type(uint32_t resource) { return resource >> 24; }

/*
 * a depends on b if b must be executed before a,
 * because a's side effects will destroy b's inputs.
 */
static bool op_depends_on(const struct slot_prop* a, const struct slot_prop* b) {
  unsigned i = 0;
  unsigned j = 0;

  if (a->op_flags & XTENSA_OP_CONTROL_FLOW) {
    return true;
  }
  if ((a->op_flags & XTENSA_OP_LOAD_STORE) < (b->op_flags & XTENSA_OP_LOAD_STORE)) {
    return true;
  }
  while (i < a->n_out && j < b->n_in) {
    if (a->out[i].resource < b->in[j].resource) {
      ++i;
    } else if (a->out[i].resource > b->in[j].resource) {
      ++j;
    } else {
      return true;
    }
  }
  return false;
}

/*
 * Try to break a dependency on b, append temporary register copy records
 * to the end of copy and update n_copy in case of success.
 * This is not always possible: e.g. control flow must always be the last,
 * load/store must be first and state dependencies are not supported yet.
 */
static bool break_dependency(struct slot_prop* a, struct slot_prop* b, struct opcode_arg_copy* copy, unsigned* n_copy) {
  unsigned i = 0;
  unsigned j = 0;
  unsigned n = *n_copy;
  bool rv = false;

  if (a->op_flags & XTENSA_OP_CONTROL_FLOW) {
    return false;
  }
  if ((a->op_flags & XTENSA_OP_LOAD_STORE) < (b->op_flags & XTENSA_OP_LOAD_STORE)) {
    return false;
  }
  while (i < a->n_out && j < b->n_in) {
    if (a->out[i].resource < b->in[j].resource) {
      ++i;
    } else if (a->out[i].resource > b->in[j].resource) {
      ++j;
    } else {
      int index = b->in[j].index;

      if (get_resource_type(a->out[i].resource) != RES_REGFILE || index < 0) {
        return false;
      }
      copy[n].resource = b->in[j].resource;
      copy[n].arg = b->arg + index;
      ++n;
      ++j;
      rv = true;
    }
  }
  *n_copy = n;
  return rv;
}

/*
 * Calculate evaluation order for slot opcodes.
 * Build opcode order graph and output its nodes in topological sort order.
 * An edge a -> b in the graph means that opcode a must be followed by
 * opcode b.
 */
static bool tsort(struct slot_prop* slot, struct slot_prop* sorted[], unsigned n, struct opcode_arg_copy* copy,
                  unsigned* n_copy) {
  struct tsnode {
    unsigned n_in_edge;
    unsigned n_out_edge;
    unsigned out_edge[MAX_INSN_SLOTS];
  } node[MAX_INSN_SLOTS];

  unsigned in[MAX_INSN_SLOTS];
  unsigned i, j;
  unsigned n_in = 0;
  unsigned n_out = 0;
  unsigned n_edge = 0;
  unsigned in_idx = 0;
  unsigned node_idx = 0;

  for (i = 0; i < n; ++i) {
    node[i].n_in_edge = 0;
    node[i].n_out_edge = 0;
  }

  for (i = 0; i < n; ++i) {
    unsigned n_out_edge = 0;

    for (j = 0; j < n; ++j) {
      if (i != j && op_depends_on(slot + j, slot + i)) {
        node[i].out_edge[n_out_edge] = j;
        ++node[j].n_in_edge;
        ++n_out_edge;
        ++n_edge;
      }
    }
    node[i].n_out_edge = n_out_edge;
  }

  for (i = 0; i < n; ++i) {
    if (!node[i].n_in_edge) {
      in[n_in] = i;
      ++n_in;
    }
  }

again:
  for (; in_idx < n_in; ++in_idx) {
    i = in[in_idx];
    sorted[n_out] = slot + i;
    ++n_out;
    for (j = 0; j < node[i].n_out_edge; ++j) {
      --n_edge;
      if (--node[node[i].out_edge[j]].n_in_edge == 0) {
        in[n_in] = node[i].out_edge[j];
        ++n_in;
      }
    }
  }
  if (n_edge) {
    for (; node_idx < n; ++node_idx) {
      struct tsnode* cnode = node + node_idx;

      if (cnode->n_in_edge) {
        for (j = 0; j < cnode->n_out_edge; ++j) {
          unsigned k = cnode->out_edge[j];

          if (break_dependency(slot + k, slot + node_idx, copy, n_copy) && --node[k].n_in_edge == 0) {
            in[n_in] = k;
            ++n_in;
            --n_edge;
            cnode->out_edge[j] = cnode->out_edge[cnode->n_out_edge - 1];
            --cnode->n_out_edge;
            goto again;
          }
        }
      }
    }
  }
  return n_edge == 0;
}

static void opcode_add_resource(struct slot_prop* op, uint32_t resource, char direction, int index) {
  switch (direction) {
    case 'm':
    case 'i':
      assert(op->n_in < ARRAY_SIZE(op->in));
      op->in[op->n_in].resource = resource;
      op->in[op->n_in].index = index;
      ++op->n_in;
      /* fall through */
    case 'o':
      if (direction == 'm' || direction == 'o') {
        assert(op->n_out < ARRAY_SIZE(op->out));
        op->out[op->n_out].resource = resource;
        op->out[op->n_out].index = index;
        ++op->n_out;
      }
      break;
    default:
      g_assert_not_reached();
  }
}

static int resource_compare(const void* a, const void* b) {
  const struct opcode_arg_info* pa = a;
  const struct opcode_arg_info* pb = b;

  return pa->resource < pb->resource ? -1 : (pa->resource > pb->resource ? 1 : 0);
}

static int arg_copy_compare(const void* a, const void* b) {
  const struct opcode_arg_copy* pa = a;
  const struct opcode_arg_copy* pb = b;

  return pa->resource < pb->resource ? -1 : (pa->resource > pb->resource ? 1 : 0);
}

/* Quick inline helper: check if a dynamically-resolved address matches.
 * Returns false if the address wasn't resolved (0).
 * Masks off bits 30-31 (0xC0000000) because ESP32-S3 flash instructions
 * may be fetched via the ICACHE at different virtual mappings (e.g.
 * 0x420xxxxx or 0x820xxxxx) depending on cache state. Both refer to the
 * same physical flash content. */
static inline bool mofei_pc_match(uint32_t pc, uint32_t resolved_addr) {
  return resolved_addr && (pc & 0x3FFFFFFFu) == (resolved_addr & 0x3FFFFFFFu);
}

static bool mofei_trace_enabled(const char* env_name) {
  const char* value = getenv(env_name);
  return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

static bool mofei_try_translate_retw_intercept(DisasContext* dc, bool before_underflow);
static void mofei_gen_normal_retw_jump(DisasContext* dc);
static bool mofei_pc_is_retw_patch(uint32_t pc);
static bool mofei_pc_is_retw_normal_precheck(uint32_t pc);

static void disas_xtensa_insn(CPUXtensaState* env, DisasContext* dc) {
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.renderActivitySync_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(3));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.murphySimulatorTraceActivity_addr)) {
    gen_helper_mofei_trace_murphy_activity(tcg_env, tcg_constant_i32(dc->pc), cpu_R[10], cpu_R[11], cpu_R[12],
                                           cpu_R[13], cpu_R[14]);
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.dashboardRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(1));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.settingsRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(2));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.buttonRemapRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(24));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.deviceDiagnosticsRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(25));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.statusBarSettingsRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(26));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.calendarRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(4));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.weatherClockRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(5));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.arcadeHubRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(6));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.game2048Render_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(29));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.recentBooksRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(7));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.fileBrowserRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(8));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.appletsRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(38));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.luaAppRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(39));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.readingHubRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(14));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.studyHubRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(15));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.studyCardsTodayRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(27));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.opdsServerListRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(16));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.opdsBookBrowserRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(17));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.opdsSettingsRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(18));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.dictionaryRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(37));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.keyboardEntryRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(19));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epubChapterSelectRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(20));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epubPercentSelectionRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(28));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epubSearchResultsRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(21));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.txtSearchResultsRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(22));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.txtBookmarksRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(34));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epubBookmarksRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(35));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epubReaderFootnotesRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(36));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.ttfFontSelectRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(9));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.timeZoneSelectRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(30));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.traditionalChineseFontsRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(31));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.languageSelectRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(32));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.sleepWallpaperRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(33));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.readerFrontlightSelectionRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(23));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.readerRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(10));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epubReaderRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(11));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.txtReaderRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(12));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.xtcReaderRender_addr)) {
    gen_helper_mofei_trace_activity(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(13));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.fillRect_addr)) {
    fprintf(stderr, "[MOFEI-TRACE] %-24s pc=0x%08x\n", "GfxRenderer::fillRect", dc->pc);
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.displayBuffer_addr)) {
    fprintf(stderr, "[MOFEI-TRACE] %-24s pc=0x%08x\n", "GfxRenderer::displayBuffer", dc->pc);
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.drawLine_addr)) {
    fprintf(stderr, "[MOFEI-TRACE] %-24s pc=0x%08x\n", "GfxRenderer::drawLine", dc->pc);
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.fillPhysicalRect_addr)) {
    fprintf(stderr, "[MOFEI-TRACE] %-24s pc=0x%08x\n", "fillPhysicalRect", dc->pc);
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epubLoad_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(1));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epubSetupCacheDir_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(2));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.zipFileOpen_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(3));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.bmcBeginWrite_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(4));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.bmcBeginContentOpfPass_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(5));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epubParseContentOpf_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(6));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epubFindContentOpfFile_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(7));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epubReadItemStream_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(8));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epubReadItemUtf8Stream_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(9));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.zipReadFileToStream_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(10));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.contentOpfWrite_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(11));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.contentOpfFlush_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(12));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.bmcCreateSpineEntry_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(13));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.bmcEndContentOpfPass_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(14));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.expatPsramRealloc_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(15));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.bmcBeginTocPass_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(16));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.bmcEndWrite_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(17));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.bmcBuildBookBin_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(18));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.bmcCleanupBuildArtifacts_addr)) {
    gen_helper_mofei_trace_epub_pipeline(tcg_env, tcg_constant_i32(dc->pc), tcg_constant_i32(19));
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.memset_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 dest = tcg_temp_new_i32();
    TCGv_i32 value = tcg_temp_new_i32();
    TCGv_i32 len = tcg_temp_new_i32();
    tcg_gen_mov_i32(dest, cpu_R[10]);
    tcg_gen_mov_i32(value, cpu_R[11]);
    tcg_gen_mov_i32(len, cpu_R[12]);
    gen_helper_mofei_memset_direct(ret_addr, tcg_env, dest, value, len);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.memcpy_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 dest = tcg_temp_new_i32();
    TCGv_i32 src = tcg_temp_new_i32();
    TCGv_i32 len = tcg_temp_new_i32();
    tcg_gen_mov_i32(dest, cpu_R[10]);
    tcg_gen_mov_i32(src, cpu_R[11]);
    tcg_gen_mov_i32(len, cpu_R[12]);
    gen_helper_mofei_memcpy_direct(ret_addr, tcg_env, dest, src, len);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.strcmp_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 left = tcg_temp_new_i32();
    TCGv_i32 right = tcg_temp_new_i32();
    tcg_gen_mov_i32(left, cpu_R[10]);
    tcg_gen_mov_i32(right, cpu_R[11]);
    gen_helper_mofei_strcmp_direct(ret_addr, tcg_env, left, right);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.strlen_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 str = tcg_temp_new_i32();
    tcg_gen_mov_i32(str, cpu_R[10]);
    gen_helper_mofei_strlen_direct(ret_addr, tcg_env, str);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.strdup_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 str = tcg_temp_new_i32();
    tcg_gen_mov_i32(str, cpu_R[10]);
    gen_helper_mofei_strdup_direct(ret_addr, tcg_env, str);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, MOFEI_ESP32S3_ROM_DIVDI3_STUB_ADDR) ||
      mofei_pc_match(dc->pc, MOFEI_ESP32S3_ROM_DIVDI3_ADDR)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 dividend_lo = tcg_temp_new_i32();
    TCGv_i32 dividend_hi = tcg_temp_new_i32();
    TCGv_i32 divisor_lo = tcg_temp_new_i32();
    TCGv_i32 divisor_hi = tcg_temp_new_i32();
    tcg_gen_mov_i32(dividend_lo, cpu_R[10]);
    tcg_gen_mov_i32(dividend_hi, cpu_R[11]);
    tcg_gen_mov_i32(divisor_lo, cpu_R[12]);
    tcg_gen_mov_i32(divisor_hi, cpu_R[13]);
    gen_helper_mofei_divdi3_direct(ret_addr, tcg_env, dividend_lo, dividend_hi, divisor_lo, divisor_hi);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, MOFEI_ESP32S3_ROM_MODDI3_STUB_ADDR) ||
      mofei_pc_match(dc->pc, MOFEI_ESP32S3_ROM_MODDI3_ADDR)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 dividend_lo = tcg_temp_new_i32();
    TCGv_i32 dividend_hi = tcg_temp_new_i32();
    TCGv_i32 divisor_lo = tcg_temp_new_i32();
    TCGv_i32 divisor_hi = tcg_temp_new_i32();
    tcg_gen_mov_i32(dividend_lo, cpu_R[10]);
    tcg_gen_mov_i32(dividend_hi, cpu_R[11]);
    tcg_gen_mov_i32(divisor_lo, cpu_R[12]);
    tcg_gen_mov_i32(divisor_hi, cpu_R[13]);
    gen_helper_mofei_moddi3_direct(ret_addr, tcg_env, dividend_lo, dividend_hi, divisor_lo, divisor_hi);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, MOFEI_ESP32S3_ROM_UDIVDI3_STUB_ADDR) ||
      mofei_pc_match(dc->pc, MOFEI_ESP32S3_ROM_UDIVDI3_ADDR)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 dividend_lo = tcg_temp_new_i32();
    TCGv_i32 dividend_hi = tcg_temp_new_i32();
    TCGv_i32 divisor_lo = tcg_temp_new_i32();
    TCGv_i32 divisor_hi = tcg_temp_new_i32();
    tcg_gen_mov_i32(dividend_lo, cpu_R[10]);
    tcg_gen_mov_i32(dividend_hi, cpu_R[11]);
    tcg_gen_mov_i32(divisor_lo, cpu_R[12]);
    tcg_gen_mov_i32(divisor_hi, cpu_R[13]);
    gen_helper_mofei_udivdi3_direct(ret_addr, tcg_env, dividend_lo, dividend_hi, divisor_lo, divisor_hi);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, MOFEI_ESP32S3_ROM_UMODDI3_STUB_ADDR) ||
      mofei_pc_match(dc->pc, MOFEI_ESP32S3_ROM_UMODDI3_ADDR)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 dividend_lo = tcg_temp_new_i32();
    TCGv_i32 dividend_hi = tcg_temp_new_i32();
    TCGv_i32 divisor_lo = tcg_temp_new_i32();
    TCGv_i32 divisor_hi = tcg_temp_new_i32();
    tcg_gen_mov_i32(dividend_lo, cpu_R[10]);
    tcg_gen_mov_i32(dividend_hi, cpu_R[11]);
    tcg_gen_mov_i32(divisor_lo, cpu_R[12]);
    tcg_gen_mov_i32(divisor_hi, cpu_R[13]);
    gen_helper_mofei_umoddi3_direct(ret_addr, tcg_env, dividend_lo, dividend_hi, divisor_lo, divisor_hi);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.pthread_key_create_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_pthread_key_create(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.pthread_getspecific_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_pthread_getspecific(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.pthread_setspecific_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_pthread_setspecific(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.esp_system_init_mbedtls_psa_crypto_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_return_zero(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* ESP-IDF SDMMC queues events from the hardware ISR.  The simulator stubs
   * FreeRTOS queues, so xQueueReceive can report success without filling the
   * sdmmc_event_t payload.  Intercept the host wait function and synthesize
   * the same event fields from the QEMU DWC SDMMC MMIO registers. */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.sdmmc_host_wait_for_event_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_sdmmc_wait_for_event(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Bump-allocated simulator heap pointers are not registered in ESP-IDF's
   * heap list.  Heap capability queries also need simulator-owned answers so
   * firmware memory preflights do not see a zero-byte heap while allocations
   * are routed through the bump allocator. */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.heap_caps_get_free_size_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_heap_caps_get_free_size(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.heap_caps_get_largest_free_block_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_heap_caps_get_largest_free_block(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.heap_caps_get_total_size_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_heap_caps_get_total_size(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* SDMMC queries the allocated size to set DMA buffer lengths, so answer from
   * the simulator allocation table instead of walking the real heap registry. */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.heap_caps_get_allocated_size_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_heap_caps_get_allocated_size(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Arduino ESP.get*Psram wrappers call psramFound() before heap_caps_*.
   * mofei_sim intentionally boots with a QEMU-compatible board profile, so the
   * wrapper body can report zero before reaching the heap capability helpers.
   * Return values directly from the simulator PSRAM ledger instead. */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.esp_get_psram_size_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_esp_get_psram_size(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.esp_get_free_psram_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_esp_get_free_psram(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.esp_get_max_alloc_psram_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_esp_get_max_alloc_psram(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* esp_intr_alloc returns simulator-owned fake handles.  Do not let
   * ESP-IDF's interrupt lifecycle functions dereference those objects. */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.esp_intr_enable_addr) ||
      mofei_pc_match(dc->pc, mofei_sim_addrs.esp_intr_disable_addr) ||
      mofei_pc_match(dc->pc, mofei_sim_addrs.esp_intr_free_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_return_zero(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* LilyGo 的 RadioLib 使用 stack TX/RX 緩衝區；模擬 DMA 不會可靠搬運這類
   * 緩衝區。僅在 radio CS 已選中時由 helper 直通既有 GPSPI2/SX1262 模型；
   * 其他裝置（尤其 SD）由 fallback 繼續執行原 ESP-IDF polling path。 */
  if (mofei_sim_board_is_lilygo_t5s3_pro() &&
      mofei_pc_match(dc->pc, mofei_sim_addrs.spi_device_polling_transmit_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGLabel* fallback = gen_new_label();
    gen_helper_mofei_try_spi_radio_transfer(ret_addr, tcg_env);
    tcg_gen_brcondi_i32(TCG_COND_EQ, ret_addr, 0, fallback);
    tcg_gen_mov_i32(cpu_pc, ret_addr);
    if (dc->icount) tcg_gen_mov_i32(cpu_SR[ICOUNT], dc->next_icount);
    tcg_gen_exit_tb(NULL, 0);
    gen_set_label(fallback);
  }

  /* ESP-IDF 的 blocking transmit wrapper 依賴 FreeRTOS ISR；模擬器改走
   * ABI 相容的 polling path，並保留真實 GPSPI2/GDMA transaction。 */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.spi_device_transmit_addr) &&
      mofei_sim_addrs.spi_device_polling_transmit_addr != 0) {
    gen_jumpi(dc, mofei_sim_addrs.spi_device_polling_transmit_addr, -1);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Mofei: Skip sleep functions — GPIO state is not emulated, so the firmware
   * may incorrectly detect button presses and trigger deep sleep / light sleep.
   * These functions call powerManager.startDeepSleep() or startLightSleep()
   * which would put the MCU to sleep permanently in QEMU.  Skip them entirely
   * so the firmware continues looping normally. */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.enterDeepSleep_addr) ||
      mofei_pc_match(dc->pc, mofei_sim_addrs.enterAutoLightSleep_addr)) {
    fprintf(stderr, "[MOFEI] Skipping sleep function @ 0x%08x\n", dc->pc);
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_void(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Mofei: ADC/eFuse calibration functions that read from eFuse or SPI flash
   * which are not emulated in QEMU, causing infinite loops. Skip them entirely
   * by reading the return address from the calling register and jumping back
   * with A2=0. Since entry hasn't run yet, no window rotation is needed.
   * These are checked at translate time because the runtime ADC patches may
   * not be visible through the ICACHE overlay. */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.read_cal_channel_addr) ||
      mofei_pc_match(dc->pc, mofei_sim_addrs.adc_hal_set_controller_addr) ||
      mofei_pc_match(dc->pc, mofei_sim_addrs.adc_hal_self_calibration_addr) ||
      mofei_pc_match(dc->pc, mofei_sim_addrs.adc_hal_calibration_init_addr) ||
      mofei_pc_match(dc->pc, mofei_sim_addrs.adc_hal_set_calibration_param_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_return_zero(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3; /* placeholder, won't be reached */
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.esp_efuse_read_field_blob_addr) ||
      mofei_pc_match(dc->pc, mofei_sim_addrs.esp_efuse_read_field_blob_part_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_esp_efuse_read_field_blob(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* FreeRTOS 的 context switch 需要真實 TCB 與 scheduler state；host
   * simulator 只提供同步執行模型。RadioLib 的等待迴圈仍可依靠模擬時間
   * 前進，因此 yield 在函式入口直接返回，避免進入 port 層解參考假 TCB。 */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.vPortYield_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_return_zero(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Mofei: APB change callback functions dereference NULL linked list
   * pointers (FreeRTOS not running), causing cause=15 crashes during
   * SPI initialization (spiStopBus → removeApbChangeCallback).
   * Skip and return true (pdTRUE) so callers proceed normally. */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.addApbChangeCallback_addr) ||
      mofei_pc_match(dc->pc, mofei_sim_addrs.removeApbChangeCallback_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_return_one(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* LilyGo 的同步 I2C 驅動依賴 ESP-IDF ISR 將 RX FIFO 搬回呼叫端緩衝區。
   * 模擬器沒有執行該 ISR，因此在函式入口直接交給 QEMU I2C 裝置模型，
   * 並由 helper 將讀取資料寫回 guest 緩衝區。 */
  if (mofei_sim_board_uses_gt911()) {
    uint32_t transfer_kind = 0;
    if (mofei_pc_match(dc->pc, mofei_sim_addrs.i2c_master_transmit_addr)) {
      transfer_kind = MOFEI_I2C_TRANSFER_TRANSMIT;
    } else if (mofei_pc_match(dc->pc, mofei_sim_addrs.i2c_master_receive_addr)) {
      transfer_kind = MOFEI_I2C_TRANSFER_RECEIVE;
    } else if (mofei_pc_match(dc->pc, mofei_sim_addrs.i2c_master_transmit_receive_addr)) {
      transfer_kind = MOFEI_I2C_TRANSFER_TRANSMIT_RECEIVE;
    } else if (mofei_pc_match(dc->pc, mofei_sim_addrs.i2c_master_multi_buffer_transmit_addr)) {
      transfer_kind = MOFEI_I2C_TRANSFER_MULTI_TRANSMIT;
    }
    if (transfer_kind != 0) {
      TCGv_i32 ret_addr = tcg_temp_new_i32();
      gen_helper_mofei_skip_fn_i2c_transfer(ret_addr, tcg_env, tcg_constant_i32(transfer_kind));
      gen_jump(dc, ret_addr);
      dc->base.pc_next = dc->pc + 3;
      return;
    }
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.gpio_config_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_return_zero(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.gpio_set_level_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_gpio_set_level(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.gpio_get_level_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_gpio_get_level(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Mofei: Arduino GPIO setup/write/read enters ESP-IDF GPIO register paths
   * and periman logging that are not useful in the simulator.  Skip pinMode
   * and digitalWrite entirely; the SSD1677 uses FSM-based command/data
   * detection instead of DC pin state. */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.pinMode_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_return_zero(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.digitalWrite_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_return_zero(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Mofei: __digitalRead reads GPIO pin levels which are not emulated
   * in QEMU. Physical buttons are active-low with pull-ups, so returning
   * HIGH (1) means "not pressed". Without this intercept the firmware
   * sees all buttons as pressed and enters an infinite enterDeepSleep()
   * loop because the GPIO registers return 0/LOW. */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.digitalRead_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_return_one(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Mofei simulator touch hook: the host IPC layer queues panel coordinates
   * inside QEMU, but the firmware symbol itself is intentionally only a stub
   * so the firmware can still link.  Let QEMU own the actual implementation by
   * skipping the stub and returning false when no helper patch is installed. */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.mofeiSimulatorTouchRead_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 x_ptr = tcg_temp_new_i32();
    TCGv_i32 y_ptr = tcg_temp_new_i32();
    TCGv_i32 action_ptr = tcg_temp_new_i32();
    TCGv_i32 touch_id_ptr = tcg_temp_new_i32();
    /* The firmware calls this hook through a callx8 function pointer from
     * MofeiTouchDriver::readPoint().  The intercepted PC is the hook's ENTRY
     * instruction, before ENTRY rotates into the callee window, so the four
     * arguments are still in the caller's call8 registers A10-A13. */
    tcg_gen_mov_i32(x_ptr, cpu_R[10]);
    tcg_gen_mov_i32(y_ptr, cpu_R[11]);
    tcg_gen_mov_i32(action_ptr, cpu_R[12]);
    tcg_gen_mov_i32(touch_id_ptr, cpu_R[13]);
    mofei_gen_sync_logical_window_to_env();
    gen_helper_mofei_touch_read(ret_addr, tcg_env, x_ptr, y_ptr, action_ptr, touch_id_ptr);
    mofei_gen_reload_window_state_from_env();
    mofei_gen_reload_logical_window_from_env();
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.mofeiSimulatorButtonRead_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 button_id_ptr = tcg_temp_new_i32();
    TCGv_i32 released_ptr = tcg_temp_new_i32();
    tcg_gen_mov_i32(button_id_ptr, cpu_R[10]);
    tcg_gen_mov_i32(released_ptr, cpu_R[11]);
    mofei_gen_sync_logical_window_to_env();
    gen_helper_mofei_button_read(ret_addr, tcg_env, button_id_ptr, released_ptr);
    mofei_gen_reload_window_state_from_env();
    mofei_gen_reload_logical_window_from_env();
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.mofeiSimulatorTraceFileBrowserDirectory_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 path_ptr = tcg_temp_new_i32();
    TCGv_i32 entry_count = tcg_temp_new_i32();
    tcg_gen_mov_i32(path_ptr, cpu_R[10]);
    tcg_gen_mov_i32(entry_count, cpu_R[11]);
    gen_helper_mofei_trace_file_browser_directory(ret_addr, tcg_env, path_ptr, entry_count);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.mofeiSimulatorTraceRpipe_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 line_ptr = tcg_temp_new_i32();
    tcg_gen_mov_i32(line_ptr, cpu_R[10]);
    gen_helper_mofei_trace_rpipe(ret_addr, tcg_env, line_ptr);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.mofeiSimulatorTraceOpdsFetch_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 url_ptr = tcg_temp_new_i32();
    TCGv_i32 entry_count = tcg_temp_new_i32();
    TCGv_i32 has_search = tcg_temp_new_i32();
    tcg_gen_mov_i32(url_ptr, cpu_R[10]);
    tcg_gen_mov_i32(entry_count, cpu_R[11]);
    tcg_gen_mov_i32(has_search, cpu_R[12]);
    gen_helper_mofei_trace_opds_fetch(ret_addr, tcg_env, url_ptr, entry_count, has_search);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  if (mofei_pc_match(dc->pc, mofei_sim_addrs.mofeiSimulatorTraceOpdsSearch_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 url_ptr = tcg_temp_new_i32();
    TCGv_i32 query_ptr = tcg_temp_new_i32();
    tcg_gen_mov_i32(url_ptr, cpu_R[10]);
    tcg_gen_mov_i32(query_ptr, cpu_R[11]);
    gen_helper_mofei_trace_opds_search(ret_addr, tcg_env, url_ptr, query_ptr);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Mofei: Skip SPIClass SPI transaction functions.
   * beginTransaction/endTransaction call spiGetClockDiv, spiTransaction, etc.
   * which crash because the SPI driver state is uninitialized in QEMU.
   * SPIClass::transfer calls spiTransferByteNL which crashes when reading
   * GPSPI2 registers. Since we intercept MofeiDisplay::sendCommand/sendData
   * to forward bytes directly to the SSD1677, these are all no-ops. */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.spi_beginTransaction_addr) ||
      mofei_pc_match(dc->pc, mofei_sim_addrs.spi_endTransaction_addr) ||
      mofei_pc_match(dc->pc, mofei_sim_addrs.spi_transfer_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_return_zero(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Mofei: SPI.writeBytes is used by writeRamPartial to send pixel data.
   * Forward bytes to SSD1677 instead of skipping — writeRamPartial manages
   * DC/CS pins itself and always calls this in data mode. call8 convention:
   * a10=this, a11=data_ptr, a12=len (pre-entry, caller's window). */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.spi_writeBytes_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 data_ptr = tcg_temp_new_i32();
    TCGv_i32 data_len = tcg_temp_new_i32();
    tcg_gen_mov_i32(data_ptr, cpu_R[11]);
    tcg_gen_mov_i32(data_len, cpu_R[12]);
    gen_helper_mofei_spi_data_buf(ret_addr, tcg_env, data_ptr, data_len);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Mofei: resetDisplayUpdateControl only sends panel-control bytes through
   * sendData().  The sendData TCG skip path is enough for direct calls, but
   * entering this tiny wrapper has repeatedly exposed corrupted window return
   * state and re-entered updateFull at the previous call boundary.  Skip the
   * wrapper itself with the same CALLN contract used by other hardware no-ops. */
  if (mofei_pc_match(dc->pc, mofei_sim_reset_display_update_control_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_void(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Mofei: writeLutFull writes the full LUT waveform table to the SSD1677
   * via sendData().  The underlying SPI sendData path is already intercepted,
   * but entering writeLutFull itself triggers the same corrupted window
   * return-state issues as resetDisplayUpdateControl.  Skip it with the same
   * void-function skip pattern. */
  if (mofei_pc_match(dc->pc, mofei_display_writeLutFull_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_void(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Mofei: writeLutFast and writeLutDu are the same class of display
   * hardware LUT writers as writeLutFull.  They forward bytes through
   * writeLut -> sendCommand/sendData which is already intercepted at the
   * SPI level.  Skip the wrapper itself with the same void-function skip
   * pattern to avoid corrupted window return-state issues. */
  if (mofei_pc_match(dc->pc, mofei_display_writeLutFast_addr) ||
      mofei_pc_match(dc->pc, mofei_display_writeLutDu_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_void(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  /* Simulator e-ink: Intercept selected board display SPI functions and forward bytes
   * directly to the active e-ink model. This bypasses the entire SPI
   * hardware path (GPSPI2 registers, spiWriteNL, FreeRTOS primitives)
   * which crashes due to uninitialized driver state in the simulator.
   *
   * These functions are called via call8 (CALLINC=2).  The intercept
   * fires at the entry instruction address BEFORE entry rotates the
   * window, so arguments are still in the caller's a10/a11/a12:
   *   a10 = this (MofeiDisplay*), not needed
   *   a11 = 1st real argument (command/data byte, or data pointer)
   *   a12 = 2nd real argument (length for buf variant)
   *
   * The display FSM-based parser correctly distinguishes commands
   * from data using its state machine, so DC pin state is irrelevant. */
  /* Panda AI OS uses the portable EpdBusEsp transport instead of the legacy
   * MofeiDisplay wrapper. Keep the same direct SSD1677 bridge for both paths;
   * this avoids entering ESP-IDF SPI internals that are intentionally absent
   * from the simulator hardware model. */
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epdBusWriteCommand_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 byte_val = tcg_temp_new_i32();
    tcg_gen_mov_i32(byte_val, cpu_R[11]);
    gen_helper_mofei_spi_cmd(ret_addr, tcg_env, byte_val);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epdBusWriteData_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 data_ptr = tcg_temp_new_i32();
    TCGv_i32 data_len = tcg_temp_new_i32();
    tcg_gen_mov_i32(data_ptr, cpu_R[11]);
    tcg_gen_mov_i32(data_len, cpu_R[12]);
    gen_helper_mofei_spi_data_buf(ret_addr, tcg_env, data_ptr, data_len);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.epdBusWaitBusy_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_void(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.sendCommand_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 byte_val = tcg_temp_new_i32();
    tcg_gen_mov_i32(byte_val, cpu_R[11]); /* a11 = cmd byte (call8, pre-entry) */
    gen_helper_mofei_spi_cmd(ret_addr, tcg_env, byte_val);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.sendData_byte_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 byte_val = tcg_temp_new_i32();
    tcg_gen_mov_i32(byte_val, cpu_R[11]); /* a11 = data byte (call8, pre-entry) */
    gen_helper_mofei_spi_data(ret_addr, tcg_env, byte_val);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.sendData_buf_addr)) {
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    TCGv_i32 data_ptr = tcg_temp_new_i32();
    TCGv_i32 data_len = tcg_temp_new_i32();
    tcg_gen_mov_i32(data_ptr, cpu_R[11]); /* a11 = data pointer (call8, pre-entry) */
    tcg_gen_mov_i32(data_len, cpu_R[12]); /* a12 = length (call8, pre-entry) */
    gen_helper_mofei_spi_data_buf(ret_addr, tcg_env, data_ptr, data_len);
    gen_jump(dc, ret_addr);
    dc->base.pc_next = dc->pc + 3;
    return;
  }

  xtensa_isa isa = dc->config->isa;
  unsigned char b[MAX_INSN_LENGTH] = {translator_ldub(env, &dc->base, dc->pc)};
  unsigned len = xtensa_op0_insn_len(dc, b[0]);
  bool decode_diag = false;
  unsigned char decode_diag_peek[6] = {b[0]};
  xtensa_format fmt;
  int slot, slots;
  unsigned i;
  uint32_t op_flags = 0;
  struct slot_prop slot_prop[MAX_INSN_SLOTS];
  struct slot_prop* ordered[MAX_INSN_SLOTS];
  struct opcode_arg_copy arg_copy[MAX_INSN_SLOTS * MAX_OPCODE_ARGS];
  unsigned n_arg_copy = 0;
  uint32_t debug_cause = 0;
  uint32_t windowed_register = 0;
  uint32_t coprocessor = 0;

  if (mofei_decode_diag_pc(dc->pc)) {
    static unsigned decode_diag_count;
    if (decode_diag_count < 64) {
      decode_diag = true;
      decode_diag_count++;
      for (i = 1; i < sizeof(decode_diag_peek); ++i) {
        decode_diag_peek[i] = translator_ldub(env, &dc->base, dc->pc + i);
      }
      fprintf(stderr, "[DECODE-DIAG] pc=%08x peek=", dc->pc);
      for (i = 0; i < sizeof(decode_diag_peek); ++i) {
        fprintf(stderr, "%s%02x", i == 0 ? "" : " ", decode_diag_peek[i]);
      }
      fprintf(stderr, " len=%u%s\n", len, len == XTENSA_UNDEFINED ? " (undefined)" : "");
    }
  }

  if (len == XTENSA_UNDEFINED) {
    if (decode_diag) {
      fprintf(stderr, "[DECODE-DIAG] pc=%08x length undefined -> illegal, pc_next=%08x\n", dc->pc, dc->pc + 1);
    }
    qemu_log_mask(LOG_GUEST_ERROR, "unknown instruction length (pc = %08x)\n", dc->pc);
    gen_exception_cause(dc, ILLEGAL_INSTRUCTION_CAUSE);
    dc->base.pc_next = dc->pc + 1;
    return;
  }

  dc->base.pc_next = dc->pc + len;
  for (i = 1; i < len; ++i) {
    b[i] = translator_ldub(env, &dc->base, dc->pc + i);
  }
  xtensa_insnbuf_from_chars(isa, dc->insnbuf, b, len);
  fmt = xtensa_format_decode(isa, dc->insnbuf);
  if (decode_diag) {
    fprintf(stderr, "[DECODE-DIAG] pc=%08x bytes=", dc->pc);
    for (i = 0; i < len; ++i) {
      fprintf(stderr, "%s%02x", i == 0 ? "" : " ", b[i]);
    }
    fprintf(stderr, " fmt=%d pc_next=%08x\n", (int)fmt, dc->base.pc_next);
  }
  if (fmt == XTENSA_UNDEFINED) {
    if (decode_diag) {
      fprintf(stderr, "[DECODE-DIAG] pc=%08x format undefined -> illegal\n", dc->pc);
    }
    qemu_log_mask(LOG_GUEST_ERROR, "unrecognized instruction format (pc = %08x)\n", dc->pc);
    gen_exception_cause(dc, ILLEGAL_INSTRUCTION_CAUSE);
    return;
  }
  slots = xtensa_format_num_slots(isa, fmt);
  for (slot = 0; slot < slots; ++slot) {
    xtensa_opcode opc;
    int opnd, vopnd, opnds;
    OpcodeArg* arg = slot_prop[slot].arg;
    XtensaOpcodeOps* ops;

    xtensa_format_get_slot(isa, fmt, slot, dc->insnbuf, dc->slotbuf);
    opc = xtensa_opcode_decode(isa, fmt, slot, dc->slotbuf);
    if (decode_diag) {
      const char* opc_name = opc == XTENSA_UNDEFINED ? "<undefined>" : xtensa_opcode_name(isa, opc);
      fprintf(stderr, "[DECODE-DIAG] pc=%08x slot=%d opcode=%d name=%s pc_next=%08x\n", dc->pc, slot, (int)opc,
              opc_name ? opc_name : "<null>", dc->base.pc_next);
    }
    if (opc == XTENSA_UNDEFINED) {
      if (decode_diag) {
        fprintf(stderr, "[DECODE-DIAG] pc=%08x opcode undefined in slot=%d -> illegal\n", dc->pc, slot);
      }
      qemu_log_mask(LOG_GUEST_ERROR, "unrecognized opcode in slot %d (pc = %08x)\n", slot, dc->pc);
      gen_exception_cause(dc, ILLEGAL_INSTRUCTION_CAUSE);
      return;
    }
    opnds = xtensa_opcode_num_operands(isa, opc);

    for (opnd = vopnd = 0; opnd < opnds; ++opnd) {
      void** register_file = NULL;
      xtensa_regfile rf;

      if (xtensa_operand_is_register(isa, opc, opnd)) {
        rf = xtensa_operand_regfile(isa, opc, opnd);
        register_file = dc->config->regfile[rf];

        if (rf == dc->config->a_regfile) {
          uint32_t v;

          xtensa_operand_get_field(isa, opc, opnd, fmt, slot, dc->slotbuf, &v);
          xtensa_operand_decode(isa, opc, opnd, &v);
          windowed_register |= 1u << v;
        }
      }
      if (xtensa_operand_is_visible(isa, opc, opnd)) {
        uint32_t v;

        xtensa_operand_get_field(isa, opc, opnd, fmt, slot, dc->slotbuf, &v);
        xtensa_operand_decode(isa, opc, opnd, &v);
        arg[vopnd].raw_imm = v;
        if (xtensa_operand_is_PCrelative(isa, opc, opnd)) {
          xtensa_operand_undo_reloc(isa, opc, opnd, &v, dc->pc);
        }
        arg[vopnd].imm = v;
        if (register_file) {
          arg[vopnd].in = register_file[v];
          arg[vopnd].out = register_file[v];
          arg[vopnd].num_bits = xtensa_regfile_num_bits(isa, rf);
        } else {
          arg[vopnd].num_bits = 32;
        }
        ++vopnd;
      }
    }
    ops = dc->config->opcode_ops[opc];
    slot_prop[slot].ops = ops;

    if (ops) {
      op_flags |= ops->op_flags;
      if (ops->test_exceptions) {
        op_flags |= ops->test_exceptions(dc, arg, ops->par);
      }
    } else {
      qemu_log_mask(LOG_UNIMP, "unimplemented opcode '%s' in slot %d (pc = %08x)\n", xtensa_opcode_name(isa, opc), slot,
                    dc->pc);
      op_flags |= XTENSA_OP_ILL;
    }
    if (op_flags & XTENSA_OP_ILL) {
      gen_exception_cause(dc, ILLEGAL_INSTRUCTION_CAUSE);
      return;
    }
    if (op_flags & XTENSA_OP_DEBUG_BREAK) {
      debug_cause |= ops->par[0];
    }
    if (ops->test_overflow) {
      windowed_register |= ops->test_overflow(dc, arg, ops->par);
    }
    coprocessor |= ops->coprocessor;

    if (slots > 1) {
      slot_prop[slot].n_in = 0;
      slot_prop[slot].n_out = 0;
      slot_prop[slot].op_flags = ops->op_flags & XTENSA_OP_LOAD_STORE;

      opnds = xtensa_opcode_num_operands(isa, opc);

      for (opnd = vopnd = 0; opnd < opnds; ++opnd) {
        bool visible = xtensa_operand_is_visible(isa, opc, opnd);

        if (xtensa_operand_is_register(isa, opc, opnd)) {
          xtensa_regfile rf = xtensa_operand_regfile(isa, opc, opnd);
          uint32_t v = 0;

          xtensa_operand_get_field(isa, opc, opnd, fmt, slot, dc->slotbuf, &v);
          xtensa_operand_decode(isa, opc, opnd, &v);
          opcode_add_resource(slot_prop + slot, encode_resource(RES_REGFILE, rf, v),
                              xtensa_operand_inout(isa, opc, opnd), visible ? vopnd : -1);
        }
        if (visible) {
          ++vopnd;
        }
      }

      opnds = xtensa_opcode_num_stateOperands(isa, opc);

      for (opnd = 0; opnd < opnds; ++opnd) {
        xtensa_state state = xtensa_stateOperand_state(isa, opc, opnd);

        opcode_add_resource(slot_prop + slot, encode_resource(RES_STATE, 0, state),
                            xtensa_stateOperand_inout(isa, opc, opnd), -1);
      }
      if (xtensa_opcode_is_branch(isa, opc) || xtensa_opcode_is_jump(isa, opc) || xtensa_opcode_is_loop(isa, opc) ||
          xtensa_opcode_is_call(isa, opc)) {
        slot_prop[slot].op_flags |= XTENSA_OP_CONTROL_FLOW;
      }

      qsort(slot_prop[slot].in, slot_prop[slot].n_in, sizeof(slot_prop[slot].in[0]), resource_compare);
      qsort(slot_prop[slot].out, slot_prop[slot].n_out, sizeof(slot_prop[slot].out[0]), resource_compare);
    }
  }

  if (slots > 1) {
    if (!tsort(slot_prop, ordered, slots, arg_copy, &n_arg_copy)) {
      qemu_log_mask(LOG_UNIMP, "Circular resource dependencies (pc = %08x)\n", dc->pc);
      gen_exception_cause(dc, ILLEGAL_INSTRUCTION_CAUSE);
      return;
    }
  } else {
    ordered[0] = slot_prop + 0;
  }

  if ((op_flags & XTENSA_OP_PRIVILEGED) && !gen_check_privilege(dc)) {
    return;
  }

  if (op_flags & XTENSA_OP_SYSCALL) {
    gen_exception_cause(dc, SYSCALL_CAUSE);
    return;
  }

  if (op_flags & XTENSA_OP_DEBUG_BREAK) {
    return;
  }

  if (dc->pc == 0x40378194) {
    fprintf(stderr, "[QEMU-DBG] Reached _xt_panic — continuing (not exiting).\n");
    /* Don't exit — let the panic handler run so we can see UART output
     * and continue booting past the initial crash. */
  }

  dc->op_flags = op_flags;
  if ((op_flags & XTENSA_OP_UNDERFLOW) && mofei_try_translate_retw_intercept(dc, true)) {
    return;
  }

  if (windowed_register && !gen_window_check(dc, windowed_register)) {
    return;
  }

  if ((op_flags & XTENSA_OP_UNDERFLOW) && !mofei_pc_is_retw_patch(dc->pc)) {
    TCGv_i32 pc = tcg_constant_i32(dc->pc);

    mofei_gen_sync_logical_window_to_env();
    gen_helper_test_underflow_retw(tcg_env, pc);
    mofei_gen_reload_window_state_from_env();
    mofei_gen_reload_logical_window_from_env();
  }

  if (op_flags & XTENSA_OP_ALLOCA) {
    TCGv_i32 pc = tcg_constant_i32(dc->pc);

    gen_helper_movsp(tcg_env, pc);
  }

  if (coprocessor && !gen_check_cpenable(dc, coprocessor)) {
    return;
  }

  if (n_arg_copy) {
    uint32_t resource;
    void* temp;
    unsigned j;

    qsort(arg_copy, n_arg_copy, sizeof(*arg_copy), arg_copy_compare);
    for (i = j = 0; i < n_arg_copy; ++i) {
      if (i == 0 || arg_copy[i].resource != resource) {
        resource = arg_copy[i].resource;
        if (arg_copy[i].arg->num_bits <= 32) {
          temp = tcg_temp_new_i32();
          tcg_gen_mov_i32(temp, arg_copy[i].arg->in);
        } else if (arg_copy[i].arg->num_bits <= 64) {
          temp = tcg_temp_new_i64();
          tcg_gen_mov_i64(temp, arg_copy[i].arg->in);
        } else {
          g_assert_not_reached();
        }
        arg_copy[i].temp = temp;

        if (i != j) {
          arg_copy[j] = arg_copy[i];
        }
        ++j;
      }
      arg_copy[i].arg->in = temp;
    }
    n_arg_copy = j;
  }

  /* ESP32-S3 QEMU: Skip divide-by-zero exception generation.
   * Our custom translate_quou/translate_quos/translate_remu functions
   * already guard against division by zero (returning 0 instead).
   * The gen_zero_check generates an exception path that causes infinite
   * recursive exceptions in the firmware's panic handler. */
#if 0
    if (op_flags & XTENSA_OP_DIVIDE_BY_ZERO) {
        for (slot = 0; slot < slots; ++slot) {
            if (slot_prop[slot].ops->op_flags & XTENSA_OP_DIVIDE_BY_ZERO) {
                gen_zero_check(dc, slot_prop[slot].arg);
            }
        }
    }
#endif

  for (slot = 0; slot < slots; ++slot) {
    struct slot_prop* pslot = ordered[slot];
    XtensaOpcodeOps* ops = pslot->ops;

    ops->translate(dc, pslot->arg, ops->par);
  }

  if (dc->base.is_jmp == DISAS_NEXT) {
    gen_postprocess(dc, 0);
    dc->op_flags = 0;
    if (op_flags & XTENSA_OP_EXIT_TB_M1) {
      /* Change in mmu index, memory mapping or tb->flags; exit tb */
      gen_jumpi_check_loop_end(dc, -1);
    } else if (op_flags & XTENSA_OP_EXIT_TB_0) {
      gen_jumpi_check_loop_end(dc, 0);
    } else {
      gen_check_loop_end(dc, 0);
    }
  }
  dc->pc = dc->base.pc_next;
}

static inline unsigned xtensa_insn_len(CPUXtensaState* env, DisasContext* dc) {
  uint8_t b0 = translator_ldub(env, &dc->base, dc->pc);
  return xtensa_op0_insn_len(dc, b0);
}

static void xtensa_tr_init_disas_context(DisasContextBase* dcbase, CPUState* cpu) {
  DisasContext* dc = container_of(dcbase, DisasContext, base);
  uint32_t tb_flags = dc->base.tb->flags;

  dc->config = cpu_env(cpu)->config;
  dc->pc = dc->base.pc_first;
  dc->ring = tb_flags & XTENSA_TBFLAG_RING_MASK;
  dc->cring = (tb_flags & XTENSA_TBFLAG_EXCM) ? 0 : dc->ring;
  dc->lbeg_off = (dc->base.tb->cs_base & XTENSA_CSBASE_LBEG_OFF_MASK) >> XTENSA_CSBASE_LBEG_OFF_SHIFT;
  dc->lend = (dc->base.tb->cs_base & XTENSA_CSBASE_LEND_MASK) + (dc->base.pc_first & TARGET_PAGE_MASK);
  dc->debug = tb_flags & XTENSA_TBFLAG_DEBUG;
  dc->icount = tb_flags & XTENSA_TBFLAG_ICOUNT;
  dc->cpenable = (tb_flags & XTENSA_TBFLAG_CPENABLE_MASK) >> XTENSA_TBFLAG_CPENABLE_SHIFT;
  dc->window = ((tb_flags & XTENSA_TBFLAG_WINDOW_MASK) >> XTENSA_TBFLAG_WINDOW_SHIFT);
  dc->cwoe = tb_flags & XTENSA_TBFLAG_CWOE;
  dc->callinc = ((tb_flags & XTENSA_TBFLAG_CALLINC_MASK) >> XTENSA_TBFLAG_CALLINC_SHIFT);
  init_sar_tracker(dc);
}

static void xtensa_tr_tb_start(DisasContextBase* dcbase, CPUState* cpu) {
  DisasContext* dc = container_of(dcbase, DisasContext, base);
  static unsigned tb_count = 0;
  tb_count++;
  /* Translation-block tracing is useful when debugging QEMU control-flow
   * stalls, but it drowns the simulator's high-signal firmware/peripheral logs.
   * Keep it opt-in for focused runs. */
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_TB") && (tb_count <= 5000 || (tb_count % 500) == 0)) {
    fprintf(stderr, "[TB #%u] pc=0x%08x\n", tb_count, (uint32_t)dcbase->pc_next);
    fflush(stderr);
  }

  if (dc->icount) {
    dc->next_icount = tcg_temp_new_i32();
  }
}

static void xtensa_tr_insn_start(DisasContextBase* dcbase, CPUState* cpu) { tcg_gen_insn_start(dcbase->pc_next); }

static void xtensa_tr_translate_insn(DisasContextBase* dcbase, CPUState* cpu) {
  DisasContext* dc = container_of(dcbase, DisasContext, base);
  CPUXtensaState* env = cpu_env(cpu);
  target_ulong page_start;

  /* These two conditions only apply to the first insn in the TB,
     but this is the first TranslateOps hook that allows exiting.  */
  if ((tb_cflags(dc->base.tb) & CF_USE_ICOUNT) && (dc->base.tb->flags & XTENSA_TBFLAG_YIELD)) {
    gen_exception(dc, EXCP_YIELD);
    dc->base.pc_next = dc->pc + 1;
    dc->base.is_jmp = DISAS_NORETURN;
    return;
  }

  if (dc->icount) {
    TCGLabel* label = gen_new_label();

    tcg_gen_addi_i32(dc->next_icount, cpu_SR[ICOUNT], 1);
    tcg_gen_brcondi_i32(TCG_COND_NE, dc->next_icount, 0, label);
    tcg_gen_mov_i32(dc->next_icount, cpu_SR[ICOUNT]);
    if (dc->debug) {
      gen_debug_exception(dc, DEBUGCAUSE_IC);
    }
    gen_set_label(label);
  }

  disas_xtensa_insn(env, dc);

  if (dc->icount) {
    tcg_gen_mov_i32(cpu_SR[ICOUNT], dc->next_icount);
  }

  /* End the TB if the next insn will cross into the next page.  */
  page_start = dc->base.pc_first & TARGET_PAGE_MASK;
  if (dc->base.is_jmp == DISAS_NEXT &&
      (dc->pc - page_start >= TARGET_PAGE_SIZE || dc->pc - page_start + xtensa_insn_len(env, dc) > TARGET_PAGE_SIZE)) {
    dc->base.is_jmp = DISAS_TOO_MANY;
  }
}

static void xtensa_tr_tb_stop(DisasContextBase* dcbase, CPUState* cpu) {
  DisasContext* dc = container_of(dcbase, DisasContext, base);

  switch (dc->base.is_jmp) {
    case DISAS_NORETURN:
      break;
    case DISAS_TOO_MANY:
      gen_jumpi(dc, dc->pc, 0);
      break;
    default:
      g_assert_not_reached();
  }
}

static const TranslatorOps xtensa_translator_ops = {
    .init_disas_context = xtensa_tr_init_disas_context,
    .tb_start = xtensa_tr_tb_start,
    .insn_start = xtensa_tr_insn_start,
    .translate_insn = xtensa_tr_translate_insn,
    .tb_stop = xtensa_tr_tb_stop,
};

void gen_intermediate_code(CPUState* cpu, TranslationBlock* tb, int* max_insns, vaddr pc, void* host_pc) {
  DisasContext dc = {};
  translator_loop(cpu, tb, max_insns, pc, host_pc, &xtensa_translator_ops, &dc.base);
}

void xtensa_cpu_dump_state(CPUState* cs, FILE* f, int flags) {
  CPUXtensaState* env = cpu_env(cs);
  xtensa_isa isa = env->config->isa;
  int i, j;

  qemu_fprintf(f, "PC=%08x\n\n", env->pc);

  for (i = j = 0; i < xtensa_isa_num_sysregs(isa); ++i) {
    const uint32_t* reg = xtensa_sysreg_is_user(isa, i) ? env->uregs : env->sregs;
    int regno = xtensa_sysreg_number(isa, i);

    if (regno >= 0) {
      qemu_fprintf(f, "%12s=%08x%c", xtensa_sysreg_name(isa, i), reg[regno], (j++ % 4) == 3 ? '\n' : ' ');
    }
  }

  qemu_fprintf(f, (j % 4) == 0 ? "\n" : "\n\n");

  for (i = 0; i < 16; ++i) {
    qemu_fprintf(f, " A%02d=%08x%c", i, env->regs[i], (i % 4) == 3 ? '\n' : ' ');
  }

  xtensa_sync_phys_from_window(env);
  qemu_fprintf(f, "\n");

  for (i = 0; i < env->config->nareg; ++i) {
    qemu_fprintf(f, "AR%02d=%08x ", i, env->phys_regs[i]);
    if (i % 4 == 3) {
      bool ws = (env->sregs[WINDOW_START] & (1 << (i / 4))) != 0;
      bool cw = env->sregs[WINDOW_BASE] == i / 4;

      qemu_fprintf(f, "%c%c\n", ws ? '<' : ' ', cw ? '=' : ' ');
    }
  }

  if ((flags & CPU_DUMP_FPU) && xtensa_option_enabled(env->config, XTENSA_OPTION_FP_COPROCESSOR)) {
    qemu_fprintf(f, "\n");

    for (i = 0; i < 16; ++i) {
      qemu_fprintf(f, "F%02d=%08x (%-+15.8e)%c", i, float32_val(env->fregs[i].f32[FP_F32_LOW]),
                   *(float*)(env->fregs[i].f32 + FP_F32_LOW), (i % 2) == 1 ? '\n' : ' ');
    }
  }

  if ((flags & CPU_DUMP_FPU) && xtensa_option_enabled(env->config, XTENSA_OPTION_DFP_COPROCESSOR) &&
      !xtensa_option_enabled(env->config, XTENSA_OPTION_DFPU_SINGLE_ONLY)) {
    qemu_fprintf(f, "\n");

    for (i = 0; i < 16; ++i) {
      qemu_fprintf(f, "F%02d=%016" PRIx64 " (%-+24.16le)%c", i, float64_val(env->fregs[i].f64),
                   *(double*)(&env->fregs[i].f64), (i % 2) == 1 ? '\n' : ' ');
    }
  }
}

static void translate_abs(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_abs_i32(arg[0].out, arg[1].in);
}

static void translate_add(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_add_i32(arg[0].out, arg[1].in, arg[2].in);
}

static void translate_addi(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_addi_i32(arg[0].out, arg[1].in, arg[2].imm);
}

static void translate_addx(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp = tcg_temp_new_i32();
  tcg_gen_shli_i32(tmp, arg[1].in, par[0]);
  tcg_gen_add_i32(arg[0].out, tmp, arg[2].in);
}

static void translate_all(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  uint32_t shift = par[1];
  TCGv_i32 mask = tcg_constant_i32(((1 << shift) - 1) << arg[1].imm);
  TCGv_i32 tmp = tcg_temp_new_i32();

  tcg_gen_and_i32(tmp, arg[1].in, mask);
  if (par[0]) {
    tcg_gen_addi_i32(tmp, tmp, 1 << arg[1].imm);
  } else {
    tcg_gen_add_i32(tmp, tmp, mask);
  }
  tcg_gen_shri_i32(tmp, tmp, arg[1].imm + shift);
  tcg_gen_deposit_i32(arg[0].out, arg[0].out, tmp, arg[0].imm, 1);
}

static void translate_and(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_and_i32(arg[0].out, arg[1].in, arg[2].in);
}

static void translate_ball(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp = tcg_temp_new_i32();
  tcg_gen_and_i32(tmp, arg[0].in, arg[1].in);
  gen_brcond(dc, par[0], tmp, arg[1].in, arg[2].imm);
}

static void translate_bany(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp = tcg_temp_new_i32();
  tcg_gen_and_i32(tmp, arg[0].in, arg[1].in);
  gen_brcondi(dc, par[0], tmp, 0, arg[2].imm);
}

static void translate_b(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_brcond(dc, par[0], arg[0].in, arg[1].in, arg[2].imm);
}

static void translate_bb(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp = tcg_temp_new_i32();

  tcg_gen_andi_i32(tmp, arg[1].in, 0x1f);
  if (TARGET_BIG_ENDIAN) {
    tcg_gen_shr_i32(tmp, tcg_constant_i32(0x80000000u), tmp);
  } else {
    tcg_gen_shl_i32(tmp, tcg_constant_i32(0x00000001u), tmp);
  }
  tcg_gen_and_i32(tmp, arg[0].in, tmp);
  gen_brcondi(dc, par[0], tmp, 0, arg[2].imm);
}

static void translate_bbi(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp = tcg_temp_new_i32();
#if TARGET_BIG_ENDIAN
  tcg_gen_andi_i32(tmp, arg[0].in, 0x80000000u >> arg[1].imm);
#else
  tcg_gen_andi_i32(tmp, arg[0].in, 0x00000001u << arg[1].imm);
#endif
  gen_brcondi(dc, par[0], tmp, 0, arg[2].imm);
}

static void translate_bi(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_brcondi(dc, par[0], arg[0].in, arg[1].imm, arg[2].imm);
}

static void translate_bz(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_brcondi(dc, par[0], arg[0].in, 0, arg[1].imm);
}

enum {
  BOOLEAN_AND,
  BOOLEAN_ANDC,
  BOOLEAN_OR,
  BOOLEAN_ORC,
  BOOLEAN_XOR,
};

static void translate_boolean(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  static void (*const op[])(TCGv_i32, TCGv_i32, TCGv_i32) = {
      [BOOLEAN_AND] = tcg_gen_and_i32, [BOOLEAN_ANDC] = tcg_gen_andc_i32, [BOOLEAN_OR] = tcg_gen_or_i32,
      [BOOLEAN_ORC] = tcg_gen_orc_i32, [BOOLEAN_XOR] = tcg_gen_xor_i32,
  };

  TCGv_i32 tmp1 = tcg_temp_new_i32();
  TCGv_i32 tmp2 = tcg_temp_new_i32();

  tcg_gen_shri_i32(tmp1, arg[1].in, arg[1].imm);
  tcg_gen_shri_i32(tmp2, arg[2].in, arg[2].imm);
  op[par[0]](tmp1, tmp1, tmp2);
  tcg_gen_deposit_i32(arg[0].out, arg[0].out, tmp1, arg[0].imm, 1);
}

static void translate_bp(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp = tcg_temp_new_i32();

  tcg_gen_andi_i32(tmp, arg[0].in, 1 << arg[0].imm);
  gen_brcondi(dc, par[0], tmp, 0, arg[1].imm);
}

static void translate_call0(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_movi_i32(cpu_R[0], dc->base.pc_next);
  gen_jumpi(dc, arg[0].imm, 0);
}

static void mofei_skip_unentered_call_return_i32(DisasContext* dc, int callinc, uint32_t value) {
  /* This path skips before gen_callw_slot() and before the callee's entry,
   * so the caller's register window is still active.  A skipped call8 would
   * normally return through the caller's A10 (callee A2 after ENTRY rotates
   * the window), call4 through A6, and call12 through A14.  Writing current A2
   * corrupts the caller's live `this`/locals and breaks later firmware state. */
  tcg_gen_movi_i32(cpu_R[(callinc << 2) + 2], value);
  gen_jumpi(dc, dc->base.pc_next, 0);
}

static void mofei_skip_unentered_call_return_time_us(DisasContext* dc, int callinc) {
  TCGv_i64 now_us = tcg_temp_new_i64();
  gen_helper_mofei_time_us(now_us);
  tcg_gen_extrl_i64_i32(cpu_R[(callinc << 2) + 2], now_us);
  tcg_gen_extrh_i64_i32(cpu_R[(callinc << 2) + 3], now_us);
  gen_jumpi(dc, dc->base.pc_next, 0);
}

static void mofei_skip_unentered_call_return_queue_handle(DisasContext* dc, int callinc) {
  /* Queue/mutex creation returns a fake handle through the caller-visible
   * return register without entering FreeRTOS or running a patched RETW. */
  TCGv_i32 handle = tcg_temp_new_i32();
  gen_helper_mofei_next_queue_handle(handle);
  tcg_gen_mov_i32(cpu_R[(callinc << 2) + 2], handle);
  gen_jumpi(dc, dc->base.pc_next, 0);
}

static bool mofei_skip_unentered_call_alloc_kind(DisasContext* dc, int callinc, uint32_t target) {
  for (int i = 0; i < mofei_sim_addrs.alloc_patch_count; i++) {
    const uint32_t retw_pc = mofei_sim_addrs.alloc_patch_addrs[i].addr;
    if (retw_pc < 3 || !mofei_pc_match(target, retw_pc - 3)) {
      continue;
    }

    tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(callinc), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
    tcg_gen_movi_i32(cpu_R[callinc << 2], (callinc << 30) | (dc->base.pc_next & 0x3fffffff));
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    mofei_gen_sync_logical_window_to_env();
    gen_helper_mofei_skip_fn_bump_alloc_kind(ret_addr, tcg_env,
                                             tcg_constant_i32(mofei_sim_addrs.alloc_patch_addrs[i].size_kind),
                                             tcg_constant_i32(mofei_sim_addrs.alloc_patch_addrs[i].old_ptr_arg),
                                             tcg_constant_i32(mofei_sim_addrs.alloc_patch_addrs[i].caps_arg));
    mofei_gen_reload_window_state_from_env();
    mofei_gen_reload_logical_window_from_env();
    gen_jump(dc, ret_addr);
    return true;
  }
  return false;
}

static bool mofei_skip_unentered_call_atomic_exchange_1(DisasContext* dc, int callinc, uint32_t target) {
  if (!mofei_pc_match(target, mofei_sim_addrs.atomic_s32c1i_exchange_1_addr)) {
    return false;
  }

  tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(callinc), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
  tcg_gen_movi_i32(cpu_R[callinc << 2], (callinc << 30) | (dc->base.pc_next & 0x3fffffff));
  TCGv_i32 ret_addr = tcg_temp_new_i32();
  gen_helper_mofei_skip_fn_atomic_exchange_1(ret_addr, tcg_env);
  gen_jump(dc, ret_addr);
  return true;
}

static bool mofei_skip_unentered_call_atomic_fetch_add_2(DisasContext* dc, int callinc, uint32_t target) {
  if (!mofei_pc_match(target, mofei_sim_addrs.atomic_fetch_add_2_addr)) {
    return false;
  }

  tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(callinc), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
  tcg_gen_movi_i32(cpu_R[callinc << 2], (callinc << 30) | (dc->base.pc_next & 0x3fffffff));
  TCGv_i32 ret_addr = tcg_temp_new_i32();
  gen_helper_mofei_skip_fn_atomic_fetch_add_2(ret_addr, tcg_env);
  gen_jump(dc, ret_addr);
  return true;
}

static bool mofei_skip_unentered_call_atomic_fetch_add_4(DisasContext* dc, int callinc, uint32_t target) {
  if (!mofei_pc_match(target, mofei_sim_addrs.atomic_fetch_add_4_addr)) {
    return false;
  }

  tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(callinc), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
  tcg_gen_movi_i32(cpu_R[callinc << 2], (callinc << 30) | (dc->base.pc_next & 0x3fffffff));
  TCGv_i32 ret_addr = tcg_temp_new_i32();
  gen_helper_mofei_skip_fn_atomic_fetch_add_4(ret_addr, tcg_env);
  gen_jump(dc, ret_addr);
  return true;
}

static bool mofei_skip_unentered_call_atomic_compare_exchange_1(DisasContext* dc, int callinc, uint32_t target) {
  if (!mofei_pc_match(target, mofei_sim_addrs.atomic_s32c1i_compare_exchange_1_addr)) {
    return false;
  }

  tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(callinc), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
  tcg_gen_movi_i32(cpu_R[callinc << 2], (callinc << 30) | (dc->base.pc_next & 0x3fffffff));
  TCGv_i32 ret_addr = tcg_temp_new_i32();
  gen_helper_mofei_skip_fn_atomic_compare_exchange_1(ret_addr, tcg_env);
  gen_jump(dc, ret_addr);
  return true;
}

static bool mofei_skip_unentered_call_atomic_compare_exchange_4(DisasContext* dc, int callinc, uint32_t target) {
  if (!mofei_pc_match(target, mofei_sim_addrs.atomic_s32c1i_compare_exchange_4_addr) &&
      !mofei_pc_match(target, mofei_sim_addrs.atomic_compare_exchange_4_addr)) {
    return false;
  }

  tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(callinc), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
  tcg_gen_movi_i32(cpu_R[callinc << 2], (callinc << 30) | (dc->base.pc_next & 0x3fffffff));
  TCGv_i32 ret_addr = tcg_temp_new_i32();
  gen_helper_mofei_skip_fn_atomic_compare_exchange_4(ret_addr, tcg_env);
  gen_jump(dc, ret_addr);
  return true;
}

static void mofei_branch_to_unentered_callx_stub(DisasContext* dc, TCGv_i32 masked_target, uint32_t target,
                                                 TCGLabel* stub) {
  tcg_gen_brcondi_i32(TCG_COND_EQ, masked_target, target & 0x3FFFFFFFu, stub);
}

static void mofei_emit_unentered_callx_stub_return_i32(DisasContext* dc, int callinc, uint32_t value) {
  tcg_gen_movi_i32(cpu_R[(callinc << 2) + 2], value);
  tcg_gen_movi_i32(cpu_pc, dc->base.pc_next);
  if (dc->icount) {
    tcg_gen_mov_i32(cpu_SR[ICOUNT], dc->next_icount);
  }
  tcg_gen_exit_tb(NULL, 0);
}

static void mofei_emit_unentered_callx_stub_task_create(DisasContext* dc, int callinc, TCGv_i32 target) {
  mofei_gen_sync_logical_window_to_env();
  gen_helper_mofei_callx_intercept(tcg_env, target);
  tcg_gen_movi_i32(cpu_R[(callinc << 2) + 2], 1);
  tcg_gen_movi_i32(cpu_pc, dc->base.pc_next);
  if (dc->icount) {
    tcg_gen_mov_i32(cpu_SR[ICOUNT], dc->next_icount);
  }
  tcg_gen_exit_tb(NULL, 0);
}

static void mofei_emit_unentered_callx_stub_return_queue_handle(DisasContext* dc, int callinc) {
  TCGv_i32 handle = tcg_temp_new_i32();
  gen_helper_mofei_next_queue_handle(handle);
  tcg_gen_mov_i32(cpu_R[(callinc << 2) + 2], handle);
  tcg_gen_movi_i32(cpu_pc, dc->base.pc_next);
  if (dc->icount) {
    tcg_gen_mov_i32(cpu_SR[ICOUNT], dc->next_icount);
  }
  tcg_gen_exit_tb(NULL, 0);
}

static void mofei_emit_unentered_callx_stub_button_read(DisasContext* dc, int callinc, TCGv_i32 button_id_ptr,
                                                        TCGv_i32 released_ptr) {
  tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(callinc), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
  tcg_gen_movi_i32(cpu_R[callinc << 2], (callinc << 30) | (dc->base.pc_next & 0x3fffffff));
  TCGv_i32 ret_addr = tcg_temp_new_i32();
  mofei_gen_sync_logical_window_to_env();
  gen_helper_mofei_button_read(ret_addr, tcg_env, button_id_ptr, released_ptr);
  mofei_gen_reload_window_state_from_env();
  mofei_gen_reload_logical_window_from_env();
  tcg_gen_mov_i32(cpu_pc, ret_addr);
  if (dc->icount) {
    tcg_gen_mov_i32(cpu_SR[ICOUNT], dc->next_icount);
  }
  tcg_gen_exit_tb(NULL, 0);
}

static void mofei_emit_unentered_callx_stub_alloc_kind(DisasContext* dc, int callinc, uint32_t size_kind,
                                                       uint32_t old_ptr_arg, uint32_t caps_arg) {
  tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(callinc), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
  tcg_gen_movi_i32(cpu_R[callinc << 2], (callinc << 30) | (dc->base.pc_next & 0x3fffffff));
  TCGv_i32 ret_addr = tcg_temp_new_i32();
  mofei_gen_sync_logical_window_to_env();
  gen_helper_mofei_skip_fn_bump_alloc_kind(ret_addr, tcg_env, tcg_constant_i32(size_kind),
                                           tcg_constant_i32(old_ptr_arg), tcg_constant_i32(caps_arg));
  mofei_gen_reload_window_state_from_env();
  mofei_gen_reload_logical_window_from_env();
  tcg_gen_mov_i32(cpu_pc, ret_addr);
  if (dc->icount) {
    tcg_gen_mov_i32(cpu_SR[ICOUNT], dc->next_icount);
  }
  tcg_gen_exit_tb(NULL, 0);
}

static void mofei_emit_unentered_callx_stub_atomic_compare_exchange_4(DisasContext* dc, int callinc) {
  tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(callinc), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
  tcg_gen_movi_i32(cpu_R[callinc << 2], (callinc << 30) | (dc->base.pc_next & 0x3fffffff));
  TCGv_i32 ret_addr = tcg_temp_new_i32();
  mofei_gen_sync_logical_window_to_env();
  gen_helper_mofei_skip_fn_atomic_compare_exchange_4(ret_addr, tcg_env);
  mofei_gen_reload_window_state_from_env();
  mofei_gen_reload_logical_window_from_env();
  tcg_gen_mov_i32(cpu_pc, ret_addr);
  if (dc->icount) {
    tcg_gen_mov_i32(cpu_SR[ICOUNT], dc->next_icount);
  }
  tcg_gen_exit_tb(NULL, 0);
}

static void mofei_emit_unentered_callx_stub_atomic_fetch_add_4(DisasContext* dc, int callinc) {
  tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(callinc), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
  tcg_gen_movi_i32(cpu_R[callinc << 2], (callinc << 30) | (dc->base.pc_next & 0x3fffffff));
  TCGv_i32 ret_addr = tcg_temp_new_i32();
  mofei_gen_sync_logical_window_to_env();
  gen_helper_mofei_skip_fn_atomic_fetch_add_4(ret_addr, tcg_env);
  mofei_gen_reload_window_state_from_env();
  mofei_gen_reload_logical_window_from_env();
  tcg_gen_mov_i32(cpu_pc, ret_addr);
  if (dc->icount) {
    tcg_gen_mov_i32(cpu_SR[ICOUNT], dc->next_icount);
  }
  tcg_gen_exit_tb(NULL, 0);
}

static void translate_callw(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  uint32_t target = arg[0].imm;

  if (mofei_pc_match(target, mofei_sim_wrap_malloc_addr)) {
    tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(par[0]), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
    tcg_gen_movi_i32(cpu_R[par[0] << 2], (par[0] << 30) | (dc->base.pc_next & 0x3fffffff));
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_wrapped_malloc(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    return;
  }

  if (mofei_skip_unentered_call_alloc_kind(dc, par[0], target)) {
    return;
  }

  if (mofei_skip_unentered_call_atomic_exchange_1(dc, par[0], target)) {
    return;
  }

  if (mofei_skip_unentered_call_atomic_fetch_add_2(dc, par[0], target)) {
    return;
  }

  if (mofei_skip_unentered_call_atomic_fetch_add_4(dc, par[0], target)) {
    return;
  }

  if (mofei_skip_unentered_call_atomic_compare_exchange_1(dc, par[0], target)) {
    return;
  }

  if (mofei_skip_unentered_call_atomic_compare_exchange_4(dc, par[0], target)) {
    return;
  }

  /* Lua uses setjmp for protected calls.  The ESP32-S3 ROM implementation
   * enters Xtensa window-overflow paths that the simulator does not model
   * reliably, so emulate the initial setjmp return at the call boundary. */
  if (mofei_pc_match(target, mofei_sim_addrs.setjmp_addr)) {
    mofei_skip_unentered_call_return_i32(dc, par[0], 0);
    return;
  }

  /* Mofei: intercept calls to stubbed-out functions before any window
   * rotation happens.  This keeps unsafe logging code from entering nested
   * call/retw paths that QEMU cannot model reliably. */
  if (mofei_pc_match(target, mofei_sim_addrs.logPrintf_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.wrap_log_printf_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.log_printf_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.log_printfv_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.esp_log_addr) || mofei_pc_match(target, mofei_sim_addrs.esp_log_va_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.esp_log_write_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.esp_log_writev_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.esp_log_impl_lock_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.esp_log_impl_unlock_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.__env_lock_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.__env_unlock_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.esp_system_init_mbedtls_psa_crypto_addr)) {
    mofei_skip_unentered_call_return_i32(dc, par[0], 0);
    return;
  }
  if (mofei_pc_match(target, mofei_sim_addrs.getApbFrequency_addr)) {
    mofei_skip_unentered_call_return_i32(dc, par[0], MOFEI_SIM_APB_FREQUENCY_HZ);
    return;
  }
  if (mofei_pc_match(target, mofei_sim_addrs.pinMode_addr)) {
    mofei_skip_unentered_call_return_i32(dc, par[0], 0);
    return;
  }
  if (mofei_pc_match(target, mofei_sim_addrs.digitalWrite_addr)) {
    mofei_skip_unentered_call_return_i32(dc, par[0], 0);
    return;
  }
  if (mofei_pc_match(target, mofei_sim_addrs.digitalRead_addr)) {
    mofei_skip_unentered_call_return_i32(dc, par[0], 1);
    return;
  }
  if (mofei_pc_match(target, mofei_sim_addrs.xQueueGenericCreate_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.xQueueCreateMutex_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.xQueueCreateWithCaps_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.xSemaphoreCreateGenericWithCaps_addr)) {
    mofei_skip_unentered_call_return_queue_handle(dc, par[0]);
    return;
  }
  if (mofei_pc_match(target, mofei_sim_addrs.cxa_guard_acquire_addr)) {
    tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(par[0]), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
    tcg_gen_movi_i32(cpu_R[par[0] << 2], (par[0] << 30) | (dc->base.pc_next & 0x3fffffff));
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    mofei_gen_sync_logical_window_to_env();
    gen_helper_mofei_cxa_guard_acquire(ret_addr, tcg_env);
    mofei_gen_reload_window_state_from_env();
    mofei_gen_reload_logical_window_from_env();
    gen_jump(dc, ret_addr);
    return;
  }
  if (mofei_pc_match(target, mofei_sim_addrs.cxa_guard_release_addr)) {
    tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(par[0]), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
    tcg_gen_movi_i32(cpu_R[par[0] << 2], (par[0] << 30) | (dc->base.pc_next & 0x3fffffff));
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    mofei_gen_sync_logical_window_to_env();
    gen_helper_mofei_cxa_guard_release(ret_addr, tcg_env);
    mofei_gen_reload_window_state_from_env();
    mofei_gen_reload_logical_window_from_env();
    gen_jump(dc, ret_addr);
    return;
  }
  /* 计时调用通过调用者可见的 A2:A3 返回 64 位微秒值。通用 32 位 NULL
   * 路径会遗留旧 A3，使超时期限被误判为立即到期。 */
  if (mofei_pc_match(target, mofei_sim_addrs.esp_timer_get_time_addr) ||
      mofei_pc_match(target, mofei_sim_addrs.systimer_hal_get_counter_value_addr)) {
    mofei_skip_unentered_call_return_time_us(dc, par[0]);
    return;
  }

  /* 返回 NULL 的分区与 OTA 分区查询。 */
  {
    bool is_null = false;
    for (int i = 0; i < mofei_sim_addrs.call_null_count; i++) {
      if (target == mofei_sim_addrs.call_null_addrs[i]) {
        is_null = true;
        break;
      }
    }
    if (is_null) {
      mofei_skip_unentered_call_return_i32(dc, par[0], 0);
      return;
    }
  }
  /* ESP_FAIL-return calls (NVS, OTA operations) */
  {
    bool is_fail = false;
    for (int i = 0; i < mofei_sim_addrs.call_fail_count; i++) {
      if (target == mofei_sim_addrs.call_fail_addrs[i]) {
        is_fail = true;
        break;
      }
    }
    if (is_fail) {
      mofei_skip_unentered_call_return_i32(dc, par[0], 0xffffffff);
      return;
    }
  }

  /* Mofei: redirect call8 xTaskCreateUniversal → loopTask.
   * xTaskCreateUniversal would call FreeRTOS task creation (all stubbed),
   * return to app_main, which then returns — leaving the firmware with
   * nothing to do.  Instead, directly call loopTask (which runs setup()
   * then loops calling loop()).  Seed loopTask's callee A1 first so it gets
   * a dedicated simulated task stack after the ENTRY window rotation. */
  if (mofei_pc_match(target, mofei_sim_addrs.xTaskCreateUniversal_addr)) {
    const uint32_t loop_task_stack_top = MOFEI_SIM_FAKE_LOOP_TASK_STACK_TOP;
    TCGv_i32 loop_target = tcg_constant_i32(mofei_sim_addrs.loopTask_addr);
    tcg_gen_movi_i32(cpu_R[(par[0] << 2) + 1], loop_task_stack_top);
    gen_callw_slot(dc, par[0], loop_target, -1);
    return;
  }

  /* Mofei: intercept xTaskCreatePinnedToCore from ActivityManager::begin().
   * This ROM function crashes in QEMU.  Return pdPASS (1) and write a
   * non-NULL sentinel to *a15 (pxCreatedTask output param) so the caller's
   * assert(renderTaskHandle != nullptr) passes. */
  if (mofei_pc_match(target, mofei_sim_addrs.xTaskCreatePinnedToCore_addr)) {
    mofei_skip_unentered_call_return_i32(dc, par[0], 1); /* pdPASS */
    return;
  }

  TCGv_i32 tmp = tcg_constant_i32(arg[0].imm);
  gen_callw_slot(dc, par[0], tmp, adjust_jump_slot(dc, arg[0].imm, 0));
}

static void translate_callx0(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp = tcg_temp_new_i32();
  tcg_gen_mov_i32(tmp, arg[0].in);
  tcg_gen_movi_i32(cpu_R[0], dc->base.pc_next);
  gen_jump(dc, tmp);
}

static void translate_callxw(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (mofei_sim_addrs.alloc_patch_count > 0) {
    TCGv_i32 masked_target = tcg_temp_new_i32();
    tcg_gen_andi_i32(masked_target, arg[0].in, 0x3FFFFFFFu);
    for (int i = 0; i < mofei_sim_addrs.alloc_patch_count; i++) {
      const uint32_t retw_pc = mofei_sim_addrs.alloc_patch_addrs[i].addr;
      if (retw_pc < 3) {
        continue;
      }
      TCGLabel* not_alloc_stub = gen_new_label();
      tcg_gen_brcondi_i32(TCG_COND_NE, masked_target, (retw_pc - 3) & 0x3FFFFFFFu, not_alloc_stub);
      mofei_emit_unentered_callx_stub_alloc_kind(dc, par[0], mofei_sim_addrs.alloc_patch_addrs[i].size_kind,
                                                 mofei_sim_addrs.alloc_patch_addrs[i].old_ptr_arg,
                                                 mofei_sim_addrs.alloc_patch_addrs[i].caps_arg);
      gen_set_label(not_alloc_stub);
    }
  }

  if (mofei_sim_addrs.setjmp_addr) {
    TCGLabel* not_setjmp_stub = gen_new_label();
    TCGv_i32 masked_target = tcg_temp_new_i32();

    tcg_gen_andi_i32(masked_target, arg[0].in, 0x3FFFFFFFu);
    tcg_gen_brcondi_i32(TCG_COND_NE, masked_target, mofei_sim_addrs.setjmp_addr & 0x3FFFFFFFu, not_setjmp_stub);
    mofei_emit_unentered_callx_stub_return_i32(dc, par[0], 0);

    gen_set_label(not_setjmp_stub);
  }

  if (mofei_sim_addrs.xQueueGenericCreate_addr || mofei_sim_addrs.xQueueCreateMutex_addr ||
      mofei_sim_addrs.xQueueCreateWithCaps_addr || mofei_sim_addrs.xSemaphoreCreateGenericWithCaps_addr) {
    TCGLabel* not_queue_create_stub = gen_new_label();
    TCGLabel* queue_create_stub = gen_new_label();
    TCGv_i32 masked_target = tcg_temp_new_i32();

    tcg_gen_andi_i32(masked_target, arg[0].in, 0x3FFFFFFFu);
    if (mofei_sim_addrs.xQueueGenericCreate_addr) {
      mofei_branch_to_unentered_callx_stub(dc, masked_target, mofei_sim_addrs.xQueueGenericCreate_addr,
                                           queue_create_stub);
    }
    if (mofei_sim_addrs.xQueueCreateMutex_addr) {
      mofei_branch_to_unentered_callx_stub(dc, masked_target, mofei_sim_addrs.xQueueCreateMutex_addr,
                                           queue_create_stub);
    }
    if (mofei_sim_addrs.xQueueCreateWithCaps_addr) {
      mofei_branch_to_unentered_callx_stub(dc, masked_target, mofei_sim_addrs.xQueueCreateWithCaps_addr,
                                           queue_create_stub);
    }
    if (mofei_sim_addrs.xSemaphoreCreateGenericWithCaps_addr) {
      mofei_branch_to_unentered_callx_stub(dc, masked_target, mofei_sim_addrs.xSemaphoreCreateGenericWithCaps_addr,
                                           queue_create_stub);
    }
    tcg_gen_br(not_queue_create_stub);

    gen_set_label(queue_create_stub);
    mofei_emit_unentered_callx_stub_return_queue_handle(dc, par[0]);

    gen_set_label(not_queue_create_stub);
  }

  if (mofei_sim_addrs.xTaskCreatePinnedToCore_addr) {
    TCGLabel* not_task_create_stub = gen_new_label();
    TCGLabel* task_create_stub = gen_new_label();
    TCGv_i32 masked_target = tcg_temp_new_i32();

    tcg_gen_andi_i32(masked_target, arg[0].in, 0x3FFFFFFFu);
    mofei_branch_to_unentered_callx_stub(dc, masked_target, mofei_sim_addrs.xTaskCreatePinnedToCore_addr,
                                         task_create_stub);
    tcg_gen_br(not_task_create_stub);

    gen_set_label(task_create_stub);
    mofei_emit_unentered_callx_stub_task_create(dc, par[0], arg[0].in);

    gen_set_label(not_task_create_stub);
  }

  {
    TCGLabel* not_delay_stub = gen_new_label();
    TCGv_i32 masked_target = tcg_temp_new_i32();

    tcg_gen_andi_i32(masked_target, arg[0].in, 0x3FFFFFFFu);
    tcg_gen_brcondi_i32(TCG_COND_NE, masked_target, MOFEI_ESP32S3_ROM_DELAY_US_ADDR & 0x3FFFFFFFu, not_delay_stub);
    gen_helper_mofei_startup_sync_flags(tcg_env, tcg_constant_i32(dc->pc));
    mofei_emit_unentered_callx_stub_return_i32(dc, par[0], 0);
    gen_set_label(not_delay_stub);
  }

  if (mofei_sim_addrs.pinMode_addr || mofei_sim_addrs.digitalWrite_addr || mofei_sim_addrs.digitalRead_addr) {
    TCGLabel* not_gpio_stub = gen_new_label();
    TCGLabel* pin_mode_stub = gen_new_label();
    TCGLabel* digital_write_stub = gen_new_label();
    TCGLabel* digital_read_stub = gen_new_label();
    TCGv_i32 masked_target = tcg_temp_new_i32();

    tcg_gen_andi_i32(masked_target, arg[0].in, 0x3FFFFFFFu);
    if (mofei_sim_addrs.pinMode_addr) {
      mofei_branch_to_unentered_callx_stub(dc, masked_target, mofei_sim_addrs.pinMode_addr, pin_mode_stub);
    }
    if (mofei_sim_addrs.digitalWrite_addr) {
      mofei_branch_to_unentered_callx_stub(dc, masked_target, mofei_sim_addrs.digitalWrite_addr, digital_write_stub);
    }
    if (mofei_sim_addrs.digitalRead_addr) {
      mofei_branch_to_unentered_callx_stub(dc, masked_target, mofei_sim_addrs.digitalRead_addr, digital_read_stub);
    }
    tcg_gen_br(not_gpio_stub);

    gen_set_label(pin_mode_stub);
    mofei_emit_unentered_callx_stub_return_i32(dc, par[0], 0);

    gen_set_label(digital_write_stub);
    mofei_emit_unentered_callx_stub_return_i32(dc, par[0], 0);

    gen_set_label(digital_read_stub);
    mofei_emit_unentered_callx_stub_return_i32(dc, par[0], 1);

    gen_set_label(not_gpio_stub);
  }

  if (mofei_sim_addrs.mofeiSimulatorButtonRead_addr) {
    TCGLabel* not_button_read_stub = gen_new_label();
    TCGv_i32 masked_target = tcg_temp_new_i32();

    tcg_gen_andi_i32(masked_target, arg[0].in, 0x3FFFFFFFu);
    tcg_gen_brcondi_i32(TCG_COND_NE, masked_target, mofei_sim_addrs.mofeiSimulatorButtonRead_addr & 0x3FFFFFFFu,
                        not_button_read_stub);
    mofei_emit_unentered_callx_stub_button_read(dc, par[0], cpu_R[10], cpu_R[11]);
    gen_set_label(not_button_read_stub);
  }

  if (mofei_sim_addrs.logPrintf_addr || mofei_sim_addrs.wrap_log_printf_addr || mofei_sim_addrs.log_printf_addr ||
      mofei_sim_addrs.log_printfv_addr || mofei_sim_addrs.esp_log_addr || mofei_sim_addrs.esp_log_va_addr ||
      mofei_sim_addrs.esp_log_write_addr || mofei_sim_addrs.esp_log_writev_addr ||
      mofei_sim_addrs.esp_log_impl_lock_addr || mofei_sim_addrs.esp_log_impl_unlock_addr) {
    TCGLabel* not_log_stub = gen_new_label();
    TCGLabel* log_stub = gen_new_label();
    TCGv_i32 masked_target = tcg_temp_new_i32();
    tcg_gen_andi_i32(masked_target, arg[0].in, 0x3FFFFFFFu);
    if (mofei_sim_addrs.logPrintf_addr) {
      tcg_gen_brcondi_i32(TCG_COND_EQ, masked_target, mofei_sim_addrs.logPrintf_addr & 0x3FFFFFFFu, log_stub);
    }
    if (mofei_sim_addrs.wrap_log_printf_addr) {
      tcg_gen_brcondi_i32(TCG_COND_EQ, masked_target, mofei_sim_addrs.wrap_log_printf_addr & 0x3FFFFFFFu, log_stub);
    }
    if (mofei_sim_addrs.log_printf_addr) {
      tcg_gen_brcondi_i32(TCG_COND_EQ, masked_target, mofei_sim_addrs.log_printf_addr & 0x3FFFFFFFu, log_stub);
    }
    if (mofei_sim_addrs.log_printfv_addr) {
      tcg_gen_brcondi_i32(TCG_COND_EQ, masked_target, mofei_sim_addrs.log_printfv_addr & 0x3FFFFFFFu, log_stub);
    }
    if (mofei_sim_addrs.esp_log_addr) {
      tcg_gen_brcondi_i32(TCG_COND_EQ, masked_target, mofei_sim_addrs.esp_log_addr & 0x3FFFFFFFu, log_stub);
    }
    if (mofei_sim_addrs.esp_log_va_addr) {
      tcg_gen_brcondi_i32(TCG_COND_EQ, masked_target, mofei_sim_addrs.esp_log_va_addr & 0x3FFFFFFFu, log_stub);
    }
    if (mofei_sim_addrs.esp_log_write_addr) {
      tcg_gen_brcondi_i32(TCG_COND_EQ, masked_target, mofei_sim_addrs.esp_log_write_addr & 0x3FFFFFFFu, log_stub);
    }
    if (mofei_sim_addrs.esp_log_writev_addr) {
      tcg_gen_brcondi_i32(TCG_COND_EQ, masked_target, mofei_sim_addrs.esp_log_writev_addr & 0x3FFFFFFFu, log_stub);
    }
    if (mofei_sim_addrs.esp_log_impl_lock_addr) {
      tcg_gen_brcondi_i32(TCG_COND_EQ, masked_target, mofei_sim_addrs.esp_log_impl_lock_addr & 0x3FFFFFFFu, log_stub);
    }
    if (mofei_sim_addrs.esp_log_impl_unlock_addr) {
      tcg_gen_brcondi_i32(TCG_COND_EQ, masked_target, mofei_sim_addrs.esp_log_impl_unlock_addr & 0x3FFFFFFFu, log_stub);
    }
    tcg_gen_br(not_log_stub);
    gen_set_label(log_stub);
    mofei_emit_unentered_callx_stub_return_i32(dc, par[0], 0);
    gen_set_label(not_log_stub);
  }

  if (mofei_sim_wrap_malloc_addr) {
    TCGLabel* not_wrap_malloc = gen_new_label();
    /* Mask off ICACHE alias bits (0xC0000000) so that register values like
     * 0x020b855c match the resolved symbol 0x420b855c.  Same logic as
     * mofei_pc_match() used in translate_callw. */
    TCGv_i32 masked_target = tcg_temp_new_i32();
    tcg_gen_andi_i32(masked_target, arg[0].in, 0x3FFFFFFFu);
    tcg_gen_brcondi_i32(TCG_COND_NE, masked_target, mofei_sim_wrap_malloc_addr & 0x3FFFFFFFu, not_wrap_malloc);
    tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(par[0]), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
    tcg_gen_movi_i32(cpu_R[par[0] << 2], (par[0] << 30) | (dc->base.pc_next & 0x3fffffff));
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    gen_helper_mofei_skip_fn_wrapped_malloc(ret_addr, tcg_env);
    gen_jump(dc, ret_addr);
    gen_set_label(not_wrap_malloc);
  }

  if (mofei_sim_addrs.atomic_fetch_add_2_addr) {
    TCGLabel* not_atomic_fetch_add_2 = gen_new_label();
    TCGv_i32 masked_target = tcg_temp_new_i32();
    tcg_gen_andi_i32(masked_target, arg[0].in, 0x3FFFFFFFu);
    tcg_gen_brcondi_i32(TCG_COND_NE, masked_target, mofei_sim_addrs.atomic_fetch_add_2_addr & 0x3FFFFFFFu,
                        not_atomic_fetch_add_2);
    tcg_gen_deposit_i32(cpu_SR[PS], cpu_SR[PS], tcg_constant_i32(par[0]), PS_CALLINC_SHIFT, PS_CALLINC_LEN);
    tcg_gen_movi_i32(cpu_R[par[0] << 2], (par[0] << 30) | (dc->base.pc_next & 0x3fffffff));
    TCGv_i32 ret_addr = tcg_temp_new_i32();
    mofei_gen_sync_logical_window_to_env();
    gen_helper_mofei_skip_fn_atomic_fetch_add_2(ret_addr, tcg_env);
    mofei_gen_reload_window_state_from_env();
    mofei_gen_reload_logical_window_from_env();
    gen_jump(dc, ret_addr);
    gen_set_label(not_atomic_fetch_add_2);
  }

  if (mofei_sim_addrs.atomic_fetch_add_4_addr) {
    TCGLabel* not_atomic_fetch_add_4 = gen_new_label();
    TCGv_i32 masked_target = tcg_temp_new_i32();
    tcg_gen_andi_i32(masked_target, arg[0].in, 0x3FFFFFFFu);
    tcg_gen_brcondi_i32(TCG_COND_NE, masked_target, mofei_sim_addrs.atomic_fetch_add_4_addr & 0x3FFFFFFFu,
                        not_atomic_fetch_add_4);
    mofei_emit_unentered_callx_stub_atomic_fetch_add_4(dc, par[0]);
    gen_set_label(not_atomic_fetch_add_4);
  }

  if (mofei_sim_addrs.atomic_s32c1i_compare_exchange_4_addr || mofei_sim_addrs.atomic_compare_exchange_4_addr) {
    TCGLabel* not_atomic_compare_exchange_4 = gen_new_label();
    TCGLabel* atomic_compare_exchange_4 = gen_new_label();
    TCGv_i32 masked_target = tcg_temp_new_i32();
    tcg_gen_andi_i32(masked_target, arg[0].in, 0x3FFFFFFFu);
    if (mofei_sim_addrs.atomic_s32c1i_compare_exchange_4_addr) {
      mofei_branch_to_unentered_callx_stub(dc, masked_target, mofei_sim_addrs.atomic_s32c1i_compare_exchange_4_addr,
                                           atomic_compare_exchange_4);
    }
    if (mofei_sim_addrs.atomic_compare_exchange_4_addr) {
      mofei_branch_to_unentered_callx_stub(dc, masked_target, mofei_sim_addrs.atomic_compare_exchange_4_addr,
                                           atomic_compare_exchange_4);
    }
    tcg_gen_br(not_atomic_compare_exchange_4);

    gen_set_label(atomic_compare_exchange_4);
    mofei_emit_unentered_callx_stub_atomic_compare_exchange_4(dc, par[0]);

    gen_set_label(not_atomic_compare_exchange_4);
  }

  /* ActivityManager::begin is handled in firmware under MOFEI_SIMULATOR.
   * Do not apply any page/range-based callx interception here: with tiny
   * simulator-only function bodies, nearby ActivityManager helper methods can
   * share the same page and contain callx8 virtual/interface calls that must
   * execute normally for input handling and rendering. */
  if (par[0] == 2 && mofei_sim_addrs.activityManager_begin_addr &&
      (dc->pc & 0xFFFFF000) == (mofei_sim_addrs.activityManager_begin_addr & 0xFFFFF000)) {
    if (mofei_trace_enabled("MOFEI_SIM_DEBUG_CALLX")) {
      fprintf(stderr, "[CALLX-DBG] callx8 at pc=0x%x (not in ActivityManager::begin @ 0x%x)\n", dc->pc,
              mofei_sim_addrs.activityManager_begin_addr);
    }
  }

  TCGv_i32 tmp = tcg_temp_new_i32();
  tcg_gen_mov_i32(tmp, arg[0].in);
  gen_callw_slot(dc, par[0], tmp, -1);
}

static void translate_clamps(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp1 = tcg_constant_i32(-1u << arg[2].imm);
  TCGv_i32 tmp2 = tcg_constant_i32((1 << arg[2].imm) - 1);

  tcg_gen_smax_i32(arg[0].out, tmp1, arg[1].in);
  tcg_gen_smin_i32(arg[0].out, arg[0].out, tmp2);
}

static void translate_clrb_expstate(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  /* TODO: GPIO32 may be a part of coprocessor */
  tcg_gen_andi_i32(cpu_UR[EXPSTATE], cpu_UR[EXPSTATE], ~(1u << arg[0].imm));
}

static void translate_clrex(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_movi_i32(cpu_exclusive_addr, -1);
}

static void translate_const16(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 c = tcg_constant_i32(arg[1].imm);

  tcg_gen_deposit_i32(arg[0].out, c, arg[0].in, 16, 16);
}

static void translate_dcache(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 addr = tcg_temp_new_i32();
  TCGv_i32 res = tcg_temp_new_i32();

  tcg_gen_addi_i32(addr, arg[0].in, arg[1].imm);
  tcg_gen_qemu_ld_i32(res, addr, dc->cring, MO_UB);
}

static void translate_depbits(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_deposit_i32(arg[1].out, arg[1].in, arg[0].in, arg[2].imm, arg[3].imm);
}

static void translate_diwbuip(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_addi_i32(arg[0].out, arg[0].in, dc->config->dcache_line_bytes);
}

static uint32_t test_exceptions_entry(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (arg[0].imm > 3 || !dc->cwoe) {
    qemu_log_mask(LOG_GUEST_ERROR, "Illegal entry instruction(pc = %08x)\n", dc->pc);
    return XTENSA_OP_ILL;
  } else {
    return 0;
  }
}

static uint32_t test_overflow_entry(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  return 1 << (dc->callinc * 4);
}

static void translate_entry(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (mofei_pc_match(dc->pc, mofei_sim_addrs.loop_addr)) {
    gen_helper_mofei_throttle_loop(tcg_env);
  }

  TCGv_i32 pc = tcg_constant_i32(dc->pc);
  TCGv_i32 s = tcg_constant_i32(arg[0].imm);
  TCGv_i32 imm = tcg_constant_i32(arg[1].imm);
  gen_helper_entry(tcg_env, pc, s, imm);
}

static void translate_extui(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  int maskimm = (1 << arg[3].imm) - 1;

  TCGv_i32 tmp = tcg_temp_new_i32();
  tcg_gen_shri_i32(tmp, arg[1].in, arg[2].imm);
  tcg_gen_andi_i32(arg[0].out, tmp, maskimm);
}

static void translate_getex(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp = tcg_temp_new_i32();

  tcg_gen_extract_i32(tmp, cpu_SR[ATOMCTL], 8, 1);
  tcg_gen_deposit_i32(cpu_SR[ATOMCTL], cpu_SR[ATOMCTL], arg[0].in, 8, 1);
  tcg_gen_mov_i32(arg[0].out, tmp);
}

static void translate_icache(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  TCGv_i32 addr = tcg_temp_new_i32();

  tcg_gen_movi_i32(cpu_pc, dc->pc);
  tcg_gen_addi_i32(addr, arg[0].in, arg[1].imm);
  gen_helper_itlb_hit_test(tcg_env, addr);
#endif
}

static void translate_itlb(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  TCGv_i32 dtlb = tcg_constant_i32(par[0]);

  gen_helper_itlb(tcg_env, arg[0].in, dtlb);
#endif
}

static void translate_j(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) { gen_jumpi(dc, arg[0].imm, 0); }

static void translate_jx(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) { gen_jump(dc, arg[0].in); }

static void translate_l32e(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 addr = tcg_temp_new_i32();
  MemOp mop;

  tcg_gen_addi_i32(addr, arg[1].in, arg[2].imm);
  mop = gen_load_store_alignment(dc, MO_TEUL, addr);
  tcg_gen_qemu_ld_tl(arg[0].out, addr, dc->ring, mop);
}

#ifdef CONFIG_USER_ONLY
static void gen_check_exclusive(DisasContext* dc, TCGv_i32 addr, bool is_write) {}
#else
static void gen_check_exclusive(DisasContext* dc, TCGv_i32 addr, bool is_write) {
  if (!option_enabled(dc, XTENSA_OPTION_MPU)) {
    TCGv_i32 pc = tcg_constant_i32(dc->pc);

    gen_helper_check_exclusive(tcg_env, pc, addr, tcg_constant_i32(is_write));
  }
}
#endif

static void translate_l32ex(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 addr = tcg_temp_new_i32();
  MemOp mop;

  tcg_gen_mov_i32(addr, arg[1].in);
  mop = gen_load_store_alignment(dc, MO_TEUL | MO_ALIGN, addr);
  gen_check_exclusive(dc, addr, false);
  tcg_gen_qemu_ld_i32(arg[0].out, addr, dc->cring, mop);
  tcg_gen_mov_i32(cpu_exclusive_addr, addr);
  tcg_gen_mov_i32(cpu_exclusive_val, arg[0].out);
}

static void translate_ldst(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 addr = tcg_temp_new_i32();
  MemOp mop;

  tcg_gen_addi_i32(addr, arg[1].in, arg[2].imm);
  mop = gen_load_store_alignment(dc, par[0], addr);

  if (par[2]) {
    if (par[1]) {
      tcg_gen_mb(TCG_BAR_STRL | TCG_MO_ALL);
    }
    tcg_gen_qemu_st_tl(arg[0].in, addr, dc->cring, mop);
  } else {
    tcg_gen_qemu_ld_tl(arg[0].out, addr, dc->cring, mop);
    if (par[1]) {
      tcg_gen_mb(TCG_BAR_LDAQ | TCG_MO_ALL);
    }
  }
}

static void translate_lct(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_movi_i32(arg[0].out, 0);
}

static void translate_l32r(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp;

  if (dc->base.tb->flags & XTENSA_TBFLAG_LITBASE) {
    tmp = tcg_temp_new();
    tcg_gen_addi_i32(tmp, cpu_SR[LITBASE], arg[1].raw_imm - 1);
  } else {
    tmp = tcg_constant_i32(arg[1].imm);
  }
  tcg_gen_qemu_ld_i32(arg[0].out, tmp, dc->cring, MO_TEUL);
}

static void translate_loop(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  uint32_t lend = arg[1].imm;

  tcg_gen_subi_i32(cpu_SR[LCOUNT], arg[0].in, 1);
  tcg_gen_movi_i32(cpu_SR[LBEG], dc->base.pc_next);
  tcg_gen_movi_i32(cpu_SR[LEND], lend);

  if (par[0] != TCG_COND_NEVER) {
    TCGLabel* label = gen_new_label();
    tcg_gen_brcondi_i32(par[0], arg[0].in, 0, label);
    gen_jumpi(dc, lend, 1);
    gen_set_label(label);
  }

  gen_jumpi(dc, dc->base.pc_next, 0);
}

enum {
  MAC16_UMUL,
  MAC16_MUL,
  MAC16_MULA,
  MAC16_MULS,
  MAC16_NONE,
};

enum {
  MAC16_LL,
  MAC16_HL,
  MAC16_LH,
  MAC16_HH,

  MAC16_HX = 0x1,
  MAC16_XH = 0x2,
};

static void translate_mac16(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  int op = par[0];
  unsigned half = par[1];
  uint32_t ld_offset = par[2];
  unsigned off = ld_offset ? 2 : 0;
  TCGv_i32 vaddr = tcg_temp_new_i32();
  TCGv_i32 mem32 = tcg_temp_new_i32();

  if (ld_offset) {
    MemOp mop;

    tcg_gen_addi_i32(vaddr, arg[1].in, ld_offset);
    mop = gen_load_store_alignment(dc, MO_TEUL, vaddr);
    tcg_gen_qemu_ld_tl(mem32, vaddr, dc->cring, mop);
  }
  if (op != MAC16_NONE) {
    TCGv_i32 m1 = gen_mac16_m(arg[off].in, half & MAC16_HX, op == MAC16_UMUL);
    TCGv_i32 m2 = gen_mac16_m(arg[off + 1].in, half & MAC16_XH, op == MAC16_UMUL);

    if (op == MAC16_MUL || op == MAC16_UMUL) {
      tcg_gen_mul_i32(cpu_SR[ACCLO], m1, m2);
      if (op == MAC16_UMUL) {
        tcg_gen_movi_i32(cpu_SR[ACCHI], 0);
      } else {
        tcg_gen_sari_i32(cpu_SR[ACCHI], cpu_SR[ACCLO], 31);
      }
    } else {
      TCGv_i32 lo = tcg_temp_new_i32();
      TCGv_i32 hi = tcg_temp_new_i32();

      tcg_gen_mul_i32(lo, m1, m2);
      tcg_gen_sari_i32(hi, lo, 31);
      if (op == MAC16_MULA) {
        tcg_gen_add2_i32(cpu_SR[ACCLO], cpu_SR[ACCHI], cpu_SR[ACCLO], cpu_SR[ACCHI], lo, hi);
      } else {
        tcg_gen_sub2_i32(cpu_SR[ACCLO], cpu_SR[ACCHI], cpu_SR[ACCLO], cpu_SR[ACCHI], lo, hi);
      }
      tcg_gen_ext8s_i32(cpu_SR[ACCHI], cpu_SR[ACCHI]);
    }
  }
  if (ld_offset) {
    tcg_gen_mov_i32(arg[1].out, vaddr);
    tcg_gen_mov_i32(cpu_SR[MR + arg[0].imm], mem32);
  }
}

static void translate_memw(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_mb(TCG_BAR_SC | TCG_MO_ALL);
}

static void translate_smin(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_smin_i32(arg[0].out, arg[1].in, arg[2].in);
}

static void translate_umin(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_umin_i32(arg[0].out, arg[1].in, arg[2].in);
}

static void translate_smax(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_smax_i32(arg[0].out, arg[1].in, arg[2].in);
}

static void translate_umax(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_umax_i32(arg[0].out, arg[1].in, arg[2].in);
}

static void translate_mov(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_mov_i32(arg[0].out, arg[1].in);
}

static void translate_movcond(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 zero = tcg_constant_i32(0);

  tcg_gen_movcond_i32(par[0], arg[0].out, arg[2].in, zero, arg[1].in, arg[0].in);
}

static void translate_movi(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_movi_i32(arg[0].out, arg[1].imm);
}

static void translate_movp(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 zero = tcg_constant_i32(0);
  TCGv_i32 tmp = tcg_temp_new_i32();

  tcg_gen_andi_i32(tmp, arg[2].in, 1 << arg[2].imm);
  tcg_gen_movcond_i32(par[0], arg[0].out, tmp, zero, arg[1].in, arg[0].in);
}

static void translate_movsp(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_mov_i32(arg[0].out, arg[1].in);
}

static void translate_mul16(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 v1 = tcg_temp_new_i32();
  TCGv_i32 v2 = tcg_temp_new_i32();

  if (par[0]) {
    tcg_gen_ext16s_i32(v1, arg[1].in);
    tcg_gen_ext16s_i32(v2, arg[2].in);
  } else {
    tcg_gen_ext16u_i32(v1, arg[1].in);
    tcg_gen_ext16u_i32(v2, arg[2].in);
  }
  tcg_gen_mul_i32(arg[0].out, v1, v2);
}

static void translate_mull(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_mul_i32(arg[0].out, arg[1].in, arg[2].in);
}

static void translate_mulh(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 lo = tcg_temp_new();

  if (par[0]) {
    tcg_gen_muls2_i32(lo, arg[0].out, arg[1].in, arg[2].in);
  } else {
    tcg_gen_mulu2_i32(lo, arg[0].out, arg[1].in, arg[2].in);
  }
}

static void translate_neg(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_neg_i32(arg[0].out, arg[1].in);
}

static void translate_nop(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {}

static void translate_nsa(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_clrsb_i32(arg[0].out, arg[1].in);
}

static void translate_nsau(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_clzi_i32(arg[0].out, arg[1].in, 32);
}

static void translate_or(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_or_i32(arg[0].out, arg[1].in, arg[2].in);
}

static void translate_ptlb(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  TCGv_i32 dtlb = tcg_constant_i32(par[0]);

  tcg_gen_movi_i32(cpu_pc, dc->pc);
  gen_helper_ptlb(arg[0].out, tcg_env, arg[1].in, dtlb);
#endif
}

static void translate_pptlb(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  tcg_gen_movi_i32(cpu_pc, dc->pc);
  gen_helper_pptlb(arg[0].out, tcg_env, arg[1].in);
#endif
}

static void translate_quos(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGLabel* label1 = gen_new_label();
  TCGLabel* label2 = gen_new_label();
  TCGLabel* label_skip = gen_new_label();

  /* Guard: skip if divisor is 0 (divide-by-zero) */
  tcg_gen_brcondi_i32(TCG_COND_NE, arg[2].in, 0, label_skip);

  tcg_gen_movi_i32(arg[0].out, 0);
  tcg_gen_br(label2);

  gen_set_label(label_skip);
  tcg_gen_brcondi_i32(TCG_COND_NE, arg[1].in, 0x80000000, label1);
  tcg_gen_brcondi_i32(TCG_COND_NE, arg[2].in, 0xffffffff, label1);
  tcg_gen_movi_i32(arg[0].out, par[0] ? 0x80000000 : 0);
  tcg_gen_br(label2);
  gen_set_label(label1);
  if (par[0]) {
    tcg_gen_div_i32(arg[0].out, arg[1].in, arg[2].in);
  } else {
    tcg_gen_rem_i32(arg[0].out, arg[1].in, arg[2].in);
  }
  gen_set_label(label2);
}

static void translate_quou(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  /* Guard against divide-by-zero: if divisor is 0, result is 0.
   * This can happen when peripheral calibration returns 0 in QEMU. */
  TCGv_i32 zero = tcg_constant_i32(0);
  TCGv_i32 one = tcg_constant_i32(1);
  TCGv_i32 safe_div = tcg_temp_new_i32();
  tcg_gen_movcond_i32(TCG_COND_EQ, safe_div, arg[2].in, zero, one, arg[2].in);
  tcg_gen_divu_i32(arg[0].out, arg[1].in, safe_div);
}

static void translate_read_impwire(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  /* TODO: GPIO32 may be a part of coprocessor */
  tcg_gen_movi_i32(arg[0].out, 0);
}

static void translate_remu(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 zero = tcg_constant_i32(0);
  TCGv_i32 one = tcg_constant_i32(1);
  TCGv_i32 safe_div = tcg_temp_new_i32();
  tcg_gen_movcond_i32(TCG_COND_EQ, safe_div, arg[2].in, zero, one, arg[2].in);
  tcg_gen_remu_i32(arg[0].out, arg[1].in, safe_div);
}

static void translate_rer(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_rer(arg[0].out, tcg_env, arg[1].in);
}

static void translate_ret(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) { gen_jump(dc, cpu_R[0]); }

static uint32_t test_exceptions_retw(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (mofei_pc_is_retw_patch(dc->pc) || mofei_pc_is_retw_normal_precheck(dc->pc) ||
      mofei_pc_match(dc->pc, mofei_sim_addrs.setupDisplayAndFonts_retw)) {
    return 0;
  }

  if (!dc->cwoe) {
    qemu_log_mask(LOG_GUEST_ERROR, "Illegal retw instruction(pc = %08x)\n", dc->pc);
    return XTENSA_OP_ILL;
  } else {
    TCGv_i32 pc = tcg_constant_i32(dc->pc);

    gen_helper_test_ill_retw(tcg_env, pc);
    return 0;
  }
}

static void mofei_gen_safe_retw_jump(DisasContext* dc) {
  TCGv_i32 ret_pc = tcg_temp_new_i32();
  /* 补丁 helper 可能已更新 env 中的返回寄存器；从 env 重载，不能用旧 cpu_R 覆盖结果。 */
  mofei_gen_reload_logical_window_from_env();
  gen_helper_mofei_retw_safe(ret_pc, tcg_env, tcg_constant_i32(dc->pc));
  mofei_gen_reload_window_state_from_env();
  mofei_gen_reload_logical_window_from_env();
  gen_jump(dc, ret_pc);
}

static void mofei_gen_normal_retw_jump(DisasContext* dc) {
  TCGv_i32 ret_pc = tcg_temp_new();
  TCGv_i32 current_window_bit = tcg_temp_new();

  tcg_gen_shl_i32(current_window_bit, tcg_constant_i32(1), cpu_SR[WINDOW_BASE]);
  tcg_gen_andc_i32(cpu_SR[WINDOW_START], cpu_SR[WINDOW_START], current_window_bit);
  tcg_gen_movi_i32(ret_pc, dc->pc);
  tcg_gen_deposit_i32(ret_pc, ret_pc, cpu_R[0], 0, 30);
  mofei_gen_sync_logical_window_to_env();
  gen_helper_retw(tcg_env, cpu_R[0]);
  mofei_gen_reload_window_state_from_env();
  mofei_gen_reload_logical_window_from_env();
  gen_jump(dc, ret_pc);
}

static bool mofei_pc_is_retw_patch(uint32_t pc) {
  for (int i = 0; i < mofei_sim_addrs.retw_patch_count; i++) {
    if (mofei_pc_match(pc, mofei_sim_addrs.retw_patch_addrs[i])) {
      return true;
    }
  }
  return false;
}

static bool mofei_pc_is_retw_normal_precheck(uint32_t pc) {
  for (int i = 0; i < mofei_sim_addrs.retw_normal_precheck_count; i++) {
    if (mofei_pc_match(pc, mofei_sim_addrs.retw_normal_precheck_addrs[i])) {
      return true;
    }
  }
  return false;
}

static bool mofei_try_translate_retw_intercept(DisasContext* dc, bool before_underflow) {
  /* Mofei intercepts for RETW-patched functions.
   * For malloc: set A2 to a bump-allocated address.
   * For queue creation: set A2 to a valid queue object.
   * For vTaskStartScheduler: redirect to app_main.
   * For queue/task operations that return success/fail: set A2=1 (pdTRUE).
   *
   * For ALL patched RETWs: use mofei_retw_safe to avoid triggering
   * the window underflow exception handler (which loops in QEMU).
   *
   * Addresses are resolved from the firmware ELF at load time and stored
   * in mofei_sim_addrs.  Patched functions have their body patched at
   * offset +3 with RETW (entry instruction preserved, RETW at byte 3),
   * so the RETW PC = function_addr + 3. */
  uint32_t pc = dc->pc;
  bool is_patched = false;

  /* 所有 RETW 补丁 helper 都从 env 读取当前窗口参数；先同步 TCG 逻辑寄存器。 */
  mofei_gen_sync_logical_window_to_env();

  /* setupDisplayAndFonts ends before the first Dashboard render, but the
   * simulator still needs a framebuffer push even if ESP-IDF SPI/FreeRTOS
   * paths are stubbed. The pre-underflow pass samples the framebuffer, then
   * lets native RETW underflow restore the caller window. The post-underflow
   * pass completes ordinary translated-register RETW so the live A0 return
   * target is captured from cpu_R[0], not stale env->regs. */
  if (mofei_pc_match(pc, mofei_sim_addrs.setupDisplayAndFonts_retw)) {
    if (before_underflow) {
      fprintf(stderr, "[MOFEI] setupDisplayAndFonts retw @ 0x%08x -> inject+native-underflow\n", pc);
      gen_helper_mofei_inject_framebuffer(tcg_env);
      return false;
    }
    fprintf(stderr, "[MOFEI] setupDisplayAndFonts retw @ 0x%08x -> inject+normal-retw\n", pc);
    gen_helper_mofei_inject_framebuffer(tcg_env);
    mofei_gen_normal_retw_jump(dc);
    return true;
  }

  if (mofei_pc_is_retw_normal_precheck(pc)) {
    if (before_underflow) {
      return false;
    }
    fprintf(stderr, "[MOFEI] real wrapper retw @ 0x%08x -> normal-retw\n", pc);
    mofei_gen_normal_retw_jump(dc);
    return true;
  }

  /* The firmware publish wrappers are real one-frame functions
   * (`entry; memw; retw`) in simulator builds. Inject at their RETW so QEMU
   * samples the framebuffer after the activity render, then let QEMU perform
   * the ordinary RETW window rotation. The simulator-safe RETW helper is for
   * QEMU-stubbed functions; using it on this real call can leave the caller's
   * register window stale and corrupt the next ActivityManager virtual call. */
  if (mofei_pc_match(pc, mofei_sim_addrs.gfxSimulatorPublishFrameBuffer_return)) {
    if (before_underflow) {
      return false;
    }
    fprintf(stderr, "[MOFEI] simulator publish wrapper retw @ 0x%08x -> inject+normal-retw\n", pc);
    gen_helper_mofei_inject_framebuffer(tcg_env);
    mofei_gen_normal_retw_jump(dc);
    return true;
  }

  /* Current firmware calls this exported marker through a volatile function
   * pointer because gfxSimulatorPublishFrameBuffer() is inlined away. The
   * marker is still a real firmware `entry; retw` frame, so keep native RETW
   * window rotation after sampling the framebuffer. */
  if (mofei_pc_match(pc, mofei_sim_addrs.mofeiSimulatorPublishFramebuffer_retw)) {
    if (before_underflow) {
      return false;
    }
    fprintf(stderr, "[MOFEI] simulator publish retw @ 0x%08x -> inject+normal-retw\n", pc);
    gen_helper_mofei_inject_framebuffer(tcg_env);
    mofei_gen_normal_retw_jump(dc);
    return true;
  }

  if (mofei_pc_match(pc, mofei_sim_addrs.murphySimulatorPublishFramebuffer_retw)) {
    if (before_underflow) {
      return false;
    }
    fprintf(stderr, "[MURPHY] simulator publish retw @ 0x%08x -> inject+normal-retw\n", pc);
    gen_helper_mofei_inject_framebuffer(tcg_env);
    mofei_gen_normal_retw_jump(dc);
    return true;
  }

  /* Check allocator-family RETW patches.  Each entry records size, old-pointer,
   * and capability argument roles so heap_caps_* calls preserve device memory
   * class semantics instead of collapsing into one simulator heap. */
  for (int i = 0; i < mofei_sim_addrs.alloc_patch_count && !is_patched; i++) {
    if (pc == mofei_sim_addrs.alloc_patch_addrs[i].addr) {
      TCGv_i32 a2 = tcg_temp_new_i32();
      TCGv_i32 a3 = tcg_temp_new_i32();
      TCGv_i32 a4 = tcg_temp_new_i32();
      TCGv_i32 a5 = tcg_temp_new_i32();
      TCGv_i32 result = tcg_temp_new_i32();
      tcg_gen_mov_i32(a2, cpu_R[2]);
      tcg_gen_mov_i32(a3, cpu_R[3]);
      tcg_gen_mov_i32(a4, cpu_R[4]);
      tcg_gen_mov_i32(a5, cpu_R[5]);
      gen_helper_mofei_bump_alloc_kind(result, tcg_env,
                                       tcg_constant_i32(mofei_sim_addrs.alloc_patch_addrs[i].size_kind),
                                       tcg_constant_i32(mofei_sim_addrs.alloc_patch_addrs[i].old_ptr_arg),
                                       tcg_constant_i32(mofei_sim_addrs.alloc_patch_addrs[i].caps_arg), a2, a3, a4, a5);
      tcg_gen_mov_i32(cpu_R[2], result);
      tcg_gen_st_i32(result, tcg_env, offsetof(CPUXtensaState, regs[2]));
      is_patched = true;
    }
  }

  /* Queue creation RETW patches */
  if (!is_patched) {
    if (mofei_pc_match(pc, mofei_sim_addrs.xQueueGenericCreate_addr + 3) ||
        mofei_pc_match(pc, mofei_sim_addrs.xQueueCreateMutex_addr + 3) ||
        mofei_pc_match(pc, mofei_sim_addrs.xQueueCreateWithCaps_addr + 3) ||
        mofei_pc_match(pc, mofei_sim_addrs.xSemaphoreCreateGenericWithCaps_addr + 3)) {
      gen_helper_mofei_bump_queue_create(tcg_env);
      is_patched = true;
    }
  }

  /* vTaskStartScheduler → always redirect to app_main */
  if (!is_patched && mofei_pc_match(pc, mofei_sim_addrs.vTaskStartScheduler_addr + 3)) {
    gen_helper_mofei_jump_to_app_main(tcg_env);
    const uint32_t entry_addr = mofei_sim_addrs.murphySimulatorMain_addr ? mofei_sim_addrs.murphySimulatorMain_addr
                                                                         : mofei_sim_addrs.app_main_addr;
    gen_jump(dc, tcg_constant_i32(entry_addr));
    return true;
  }

  /* __assert_func → redirect once, then normal retw */
  if (!is_patched && mofei_pc_match(pc, mofei_sim_addrs.__assert_func_addr + 3)) {
    gen_helper_mofei_assert_redirect(tcg_env);
    is_patched = true;
  }

  /* Interrupt allocation returns esp_err_t and writes an out handle.  It must
   * not use the allocator bump path just because the symbol name contains
   * "alloc"; SDMMC host init checks the returned handle before it can issue
   * card commands. */
  if (!is_patched && mofei_pc_match(pc, mofei_sim_addrs.esp_intr_alloc_addr + 3)) {
    gen_helper_mofei_retw_intr_alloc(tcg_env);
    is_patched = true;
  }
  if (!is_patched && mofei_pc_match(pc, mofei_sim_addrs.esp_intr_alloc_intrstatus_addr + 3)) {
    gen_helper_mofei_retw_intr_alloc_intrstatus(tcg_env);
    is_patched = true;
  }

  /* Queue success-return patches */
  if (!is_patched && mofei_pc_match(pc, mofei_sim_addrs.xQueueReceive_addr + 5)) {
    gen_helper_mofei_retw_queue_receive(tcg_env);
    is_patched = true;
  }
  if (!is_patched) {
    uint32_t queue_fns[] = {
        mofei_sim_addrs.xQueueGenericSend_addr,
        mofei_sim_addrs.xQueueSemaphoreTake_addr,
        mofei_sim_addrs.xQueueGiveMutexRecursive_addr,
        mofei_sim_addrs.xQueueTakeMutexRecursive_addr,
    };
    for (int i = 0; i < (int)(sizeof(queue_fns) / sizeof(queue_fns[0])); i++) {
      if (queue_fns[i] && pc == queue_fns[i] + 5) {
        gen_helper_mofei_retw_success(tcg_env);
        is_patched = true;
        break;
      }
    }
  }

  if (!is_patched && (mofei_pc_match(pc, mofei_sim_addrs.esp_timer_get_time_addr + 3) ||
                      mofei_pc_match(pc, mofei_sim_addrs.systimer_hal_get_counter_value_addr + 3))) {
    gen_helper_mofei_retw_time_us(tcg_env);
    is_patched = true;
  }

  /* 返回 NULL 的分区与 OTA 分区查询补丁。 */
  if (!is_patched) {
    for (int i = 0; i < mofei_sim_addrs.call_null_count; i++) {
      if (pc == mofei_sim_addrs.call_null_addrs[i] + 3) {
        gen_helper_mofei_retw_null(tcg_env);
        is_patched = true;
        break;
      }
    }
  }

  /* ESP_FAIL-return patches (NVS, OTA operations) */
  if (!is_patched) {
    for (int i = 0; i < mofei_sim_addrs.call_fail_count; i++) {
      if (pc == mofei_sim_addrs.call_fail_addrs[i] + 3) {
        gen_helper_mofei_retw_fail(tcg_env);
        is_patched = true;
        break;
      }
    }
  }

  /* Fallback: check against the OLD hardcoded addresses for any symbols
   * we might have missed.  This ensures backward compatibility during
   * the transition. */
  if (!is_patched) {
    /* Queue create — these may not have been resolved by name.
     * Keep hardcoded ROM addresses as fallback since ROM functions
     * that are patched in place have stable patch locations. */
    /* TODO: resolve queue create functions by name if possible */
  }

  if (!is_patched && mofei_pc_is_retw_patch(pc)) {
    is_patched = true;
  }

  if (is_patched) {
    /* Use the simulator-safe RETW helper for patched functions.  Plain
     * gen_helper_retw may raise a real window overflow/underflow while
     * returning from QEMU-stubbed malloc/RTOS functions; the firmware's
     * vector then looks like an unusable fetch artifact and can resume at
     * the caller body after ENTRY, leaving the caller's final RETW illegal. */
    mofei_gen_safe_retw_jump(dc);
    return true;
  }

  return false;
}

static void translate_retw(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (mofei_try_translate_retw_intercept(dc, false)) {
    return;
  }

  /* Ordinary firmware RETW must keep QEMU's native register-window transfer.
   * Only explicit QEMU-patched stubs use the env-based safe RETW helper. */
  mofei_gen_normal_retw_jump(dc);
}

static void translate_rfde(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_jump(dc, cpu_SR[dc->config->ndepc ? DEPC : EPC1]);
}

static void translate_rfe(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_andi_i32(cpu_SR[PS], cpu_SR[PS], ~PS_EXCM);
  gen_jump(dc, cpu_SR[EPC1]);
}

static void translate_rfi(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_mov_i32(cpu_SR[PS], cpu_SR[EPS2 + arg[0].imm - 2]);
  gen_jump(dc, cpu_SR[EPC1 + arg[0].imm - 1]);
}

static void translate_rfw(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp = tcg_temp_new();

  tcg_gen_andi_i32(cpu_SR[PS], cpu_SR[PS], ~PS_EXCM);
  tcg_gen_shl_i32(tmp, tcg_constant_i32(1), cpu_SR[WINDOW_BASE]);

  if (par[0]) {
    tcg_gen_andc_i32(cpu_SR[WINDOW_START], cpu_SR[WINDOW_START], tmp);
  } else {
    tcg_gen_or_i32(cpu_SR[WINDOW_START], cpu_SR[WINDOW_START], tmp);
  }

  mofei_gen_sync_logical_window_to_env();
  gen_helper_restore_owb(tcg_env);
  mofei_gen_reload_window_state_from_env();
  mofei_gen_reload_logical_window_from_env();
  gen_jump(dc, cpu_SR[EPC1]);
}

static void translate_rotw(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_addi_i32(cpu_windowbase_next, cpu_SR[WINDOW_BASE], arg[0].imm);
}

static void translate_rsil(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_mov_i32(arg[0].out, cpu_SR[PS]);
  tcg_gen_andi_i32(cpu_SR[PS], cpu_SR[PS], ~PS_INTLEVEL);
  tcg_gen_ori_i32(cpu_SR[PS], cpu_SR[PS], arg[1].imm);
}

static void translate_rsr(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (sr_name[par[0]]) {
    tcg_gen_mov_i32(arg[0].out, cpu_SR[par[0]]);
  } else {
    tcg_gen_movi_i32(arg[0].out, 0);
  }
}

static void translate_rsr_ccount(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  translator_io_start(&dc->base);
  gen_helper_update_ccount(tcg_env);
  tcg_gen_mov_i32(arg[0].out, cpu_SR[par[0]]);
#endif
}

static void translate_rsr_ptevaddr(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  TCGv_i32 tmp = tcg_temp_new_i32();

  tcg_gen_shri_i32(tmp, cpu_SR[EXCVADDR], 10);
  tcg_gen_or_i32(tmp, tmp, cpu_SR[PTEVADDR]);
  tcg_gen_andi_i32(arg[0].out, tmp, 0xfffffffc);
#endif
}

static void translate_rtlb(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  static void (*const helper[])(TCGv_i32 r, TCGv_env env, TCGv_i32 a1, TCGv_i32 a2) = {
      gen_helper_rtlb0,
      gen_helper_rtlb1,
  };
  TCGv_i32 dtlb = tcg_constant_i32(par[0]);

  helper[par[1]](arg[0].out, tcg_env, arg[1].in, dtlb);
#endif
}

static void translate_rptlb0(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  gen_helper_rptlb0(arg[0].out, tcg_env, arg[1].in);
#endif
}

static void translate_rptlb1(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  gen_helper_rptlb1(arg[0].out, tcg_env, arg[1].in);
#endif
}

static void translate_rur(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_mov_i32(arg[0].out, cpu_UR[par[0]]);
}

static void translate_setb_expstate(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  /* TODO: GPIO32 may be a part of coprocessor */
  tcg_gen_ori_i32(cpu_UR[EXPSTATE], cpu_UR[EXPSTATE], 1u << arg[0].imm);
}

#ifdef CONFIG_USER_ONLY
static void gen_check_atomctl(DisasContext* dc, TCGv_i32 addr) {}
#else
static void gen_check_atomctl(DisasContext* dc, TCGv_i32 addr) {
  TCGv_i32 pc = tcg_constant_i32(dc->pc);

  gen_helper_check_atomctl(tcg_env, pc, addr);
}
#endif

static void translate_s32c1i(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp = tcg_temp_new_i32();
  TCGv_i32 addr = tcg_temp_new_i32();

  tcg_gen_mov_i32(tmp, arg[0].in);
  tcg_gen_addi_i32(addr, arg[1].in, arg[2].imm);
  gen_check_atomctl(dc, addr);
  gen_helper_mofei_s32c1i(arg[0].out, tcg_env, addr, cpu_SR[SCOMPARE1], tmp);
}

static void translate_s32e(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 addr = tcg_temp_new_i32();
  MemOp mop;

  tcg_gen_addi_i32(addr, arg[1].in, arg[2].imm);
  mop = gen_load_store_alignment(dc, MO_TEUL, addr);
  tcg_gen_qemu_st_tl(arg[0].in, addr, dc->ring, mop);
}

static void translate_s32ex(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 prev = tcg_temp_new_i32();
  TCGv_i32 addr = tcg_temp_new_i32();
  TCGv_i32 res = tcg_temp_new_i32();
  TCGLabel* label = gen_new_label();

  tcg_gen_movi_i32(res, 0);
  tcg_gen_mov_i32(addr, arg[1].in);
  tcg_gen_brcond_i32(TCG_COND_NE, addr, cpu_exclusive_addr, label);
  gen_check_exclusive(dc, addr, true);
  gen_helper_mofei_s32ex(prev, tcg_env, addr, cpu_exclusive_val, arg[0].in);
  tcg_gen_setcond_i32(TCG_COND_EQ, res, prev, cpu_exclusive_val);
  tcg_gen_movcond_i32(TCG_COND_EQ, cpu_exclusive_val, prev, cpu_exclusive_val, prev, cpu_exclusive_val);
  tcg_gen_movi_i32(cpu_exclusive_addr, -1);
  gen_set_label(label);
  tcg_gen_extract_i32(arg[0].out, cpu_SR[ATOMCTL], 8, 1);
  tcg_gen_deposit_i32(cpu_SR[ATOMCTL], cpu_SR[ATOMCTL], res, 8, 1);
}

static void translate_salt(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_setcond_i32(par[0], arg[0].out, arg[1].in, arg[2].in);
}

static void translate_sext(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_sextract_i32(arg[0].out, arg[1].in, 0, arg[2].imm + 1);
}

static uint32_t test_exceptions_simcall(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  bool is_semi = semihosting_enabled(dc->cring != 0);
#ifdef CONFIG_USER_ONLY
  bool ill = true;
#else
  /* Between RE.2 and RE.3 simcall opcode's become nop for the hardware. */
  bool ill = dc->config->hw_version <= 250002 && !is_semi;
#endif
  if (ill || !is_semi) {
    qemu_log_mask(LOG_GUEST_ERROR, "SIMCALL but semihosting is disabled\n");
  }
  return ill ? XTENSA_OP_ILL : 0;
}

static void translate_simcall(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  if (semihosting_enabled(dc->cring != 0)) {
    gen_helper_simcall(tcg_env);
  }
#endif
}

/*
 * Note: 64 bit ops are used here solely because SAR values
 * have range 0..63
 */
#define gen_shift_reg(cmd, reg)           \
  do {                                    \
    TCGv_i64 tmp = tcg_temp_new_i64();    \
    tcg_gen_extu_i32_i64(tmp, reg);       \
    tcg_gen_##cmd##_i64(v, v, tmp);       \
    tcg_gen_extrl_i64_i32(arg[0].out, v); \
  } while (0)

#define gen_shift(cmd) gen_shift_reg(cmd, cpu_SR[SAR])

static void translate_sll(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (dc->sar_m32_5bit) {
    tcg_gen_shl_i32(arg[0].out, arg[1].in, dc->sar_m32);
  } else {
    TCGv_i64 v = tcg_temp_new_i64();
    TCGv_i32 s = tcg_temp_new();
    tcg_gen_subfi_i32(s, 32, cpu_SR[SAR]);
    tcg_gen_andi_i32(s, s, 0x3f);
    tcg_gen_extu_i32_i64(v, arg[1].in);
    gen_shift_reg(shl, s);
  }
}

static void translate_slli(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (arg[2].imm == 32) {
    qemu_log_mask(LOG_GUEST_ERROR, "slli a%d, a%d, 32 is undefined\n", arg[0].imm, arg[1].imm);
  }
  tcg_gen_shli_i32(arg[0].out, arg[1].in, arg[2].imm & 0x1f);
}

static void translate_sra(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (dc->sar_m32_5bit) {
    tcg_gen_sar_i32(arg[0].out, arg[1].in, cpu_SR[SAR]);
  } else {
    TCGv_i64 v = tcg_temp_new_i64();
    tcg_gen_ext_i32_i64(v, arg[1].in);
    gen_shift(sar);
  }
}

static void translate_srai(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_sari_i32(arg[0].out, arg[1].in, arg[2].imm);
}

static void translate_src(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i64 v = tcg_temp_new_i64();
  tcg_gen_concat_i32_i64(v, arg[2].in, arg[1].in);
  gen_shift(shr);
}

static void translate_srl(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (dc->sar_m32_5bit) {
    tcg_gen_shr_i32(arg[0].out, arg[1].in, cpu_SR[SAR]);
  } else {
    TCGv_i64 v = tcg_temp_new_i64();
    tcg_gen_extu_i32_i64(v, arg[1].in);
    gen_shift(shr);
  }
}

#undef gen_shift
#undef gen_shift_reg

static void translate_srli(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_shri_i32(arg[0].out, arg[1].in, arg[2].imm);
}

static void translate_ssa8b(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp = tcg_temp_new_i32();
  tcg_gen_shli_i32(tmp, arg[0].in, 3);
  gen_left_shift_sar(dc, tmp);
}

static void translate_ssa8l(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp = tcg_temp_new_i32();
  tcg_gen_shli_i32(tmp, arg[0].in, 3);
  gen_right_shift_sar(dc, tmp);
}

static void translate_ssai(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_right_shift_sar(dc, tcg_constant_i32(arg[0].imm));
}

static void translate_ssl(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_left_shift_sar(dc, arg[0].in);
}

static void translate_ssr(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_right_shift_sar(dc, arg[0].in);
}

static void translate_sub(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_sub_i32(arg[0].out, arg[1].in, arg[2].in);
}

static void translate_subx(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 tmp = tcg_temp_new_i32();
  tcg_gen_shli_i32(tmp, arg[1].in, par[0]);
  tcg_gen_sub_i32(arg[0].out, tmp, arg[2].in);
}

static void translate_waiti(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  TCGv_i32 pc = tcg_constant_i32(dc->base.pc_next);

  translator_io_start(&dc->base);
  gen_helper_waiti(tcg_env, pc, tcg_constant_i32(arg[0].imm));
#endif
}

static void translate_wtlb(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  TCGv_i32 dtlb = tcg_constant_i32(par[0]);

  gen_helper_wtlb(tcg_env, arg[0].in, arg[1].in, dtlb);
#endif
}

static void translate_wptlb(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  gen_helper_wptlb(tcg_env, arg[0].in, arg[1].in);
#endif
}

static void translate_wer(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_wer(tcg_env, arg[0].in, arg[1].in);
}

static void translate_wrmsk_expstate(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  /* TODO: GPIO32 may be a part of coprocessor */
  tcg_gen_and_i32(cpu_UR[EXPSTATE], arg[0].in, arg[1].in);
}

static void translate_wsr(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (sr_name[par[0]]) {
    tcg_gen_mov_i32(cpu_SR[par[0]], arg[0].in);
  }
}

static void translate_wsr_mask(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (sr_name[par[0]]) {
    tcg_gen_andi_i32(cpu_SR[par[0]], arg[0].in, par[2]);
  }
}

static void translate_wsr_acchi(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_ext8s_i32(cpu_SR[par[0]], arg[0].in);
}

static void translate_wsr_ccompare(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  uint32_t id = par[0] - CCOMPARE;

  assert(id < dc->config->nccompare);
  translator_io_start(&dc->base);
  tcg_gen_mov_i32(cpu_SR[par[0]], arg[0].in);
  gen_helper_update_ccompare(tcg_env, tcg_constant_i32(id));
#endif
}

static void translate_wsr_ccount(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  translator_io_start(&dc->base);
  gen_helper_wsr_ccount(tcg_env, arg[0].in);
#endif
}

static void translate_wsr_dbreaka(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  unsigned id = par[0] - DBREAKA;

  assert(id < dc->config->ndbreak);
  gen_helper_wsr_dbreaka(tcg_env, tcg_constant_i32(id), arg[0].in);
#endif
}

static void translate_wsr_dbreakc(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  unsigned id = par[0] - DBREAKC;

  assert(id < dc->config->ndbreak);
  gen_helper_wsr_dbreakc(tcg_env, tcg_constant_i32(id), arg[0].in);
#endif
}

static void translate_wsr_ibreaka(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  unsigned id = par[0] - IBREAKA;

  assert(id < dc->config->nibreak);
  gen_helper_wsr_ibreaka(tcg_env, tcg_constant_i32(id), arg[0].in);
#endif
}

static void translate_wsr_ibreakenable(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  gen_helper_wsr_ibreakenable(tcg_env, arg[0].in);
#endif
}

static void translate_wsr_icount(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  if (dc->icount) {
    tcg_gen_mov_i32(dc->next_icount, arg[0].in);
  } else {
    tcg_gen_mov_i32(cpu_SR[par[0]], arg[0].in);
  }
#endif
}

static void translate_wsr_intclear(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  gen_helper_intclear(tcg_env, arg[0].in);
#endif
}

static void translate_wsr_intset(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  gen_helper_intset(tcg_env, arg[0].in);
#endif
}

static void translate_wsr_memctl(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  gen_helper_wsr_memctl(tcg_env, arg[0].in);
#endif
}

static void translate_wsr_mpuenb(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  gen_helper_wsr_mpuenb(tcg_env, arg[0].in);
#endif
}

static void translate_wsr_ps(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  uint32_t mask = PS_WOE | PS_CALLINC | PS_OWB | PS_UM | PS_EXCM | PS_INTLEVEL;

  if (option_enabled(dc, XTENSA_OPTION_MMU) || option_enabled(dc, XTENSA_OPTION_MPU)) {
    mask |= PS_RING;
  }
  tcg_gen_andi_i32(cpu_SR[par[0]], arg[0].in, mask);
#endif
}

static void translate_wsr_rasid(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  gen_helper_wsr_rasid(tcg_env, arg[0].in);
#endif
}

static void translate_wsr_sar(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_andi_i32(cpu_SR[par[0]], arg[0].in, 0x3f);
  if (dc->sar_m32_5bit) {
    tcg_gen_discard_i32(dc->sar_m32);
  }
  dc->sar_5bit = false;
  dc->sar_m32_5bit = false;
}

static void translate_wsr_windowbase(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  tcg_gen_mov_i32(cpu_windowbase_next, arg[0].in);
#endif
}

static void translate_wsr_windowstart(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  tcg_gen_andi_i32(cpu_SR[par[0]], arg[0].in, (1 << dc->config->nareg / 4) - 1);
#endif
}

static void translate_wur(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_mov_i32(cpu_UR[par[0]], arg[0].in);
}

static void translate_xur_f64(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) { /* no-op */ }

static void translate_xor(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_xor_i32(arg[0].out, arg[1].in, arg[2].in);
}

static void translate_xsr(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (sr_name[par[0]]) {
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_mov_i32(tmp, arg[0].in);
    tcg_gen_mov_i32(arg[0].out, cpu_SR[par[0]]);
    tcg_gen_mov_i32(cpu_SR[par[0]], tmp);
  } else {
    tcg_gen_movi_i32(arg[0].out, 0);
  }
}

static void translate_xsr_mask(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (sr_name[par[0]]) {
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_mov_i32(tmp, arg[0].in);
    tcg_gen_mov_i32(arg[0].out, cpu_SR[par[0]]);
    tcg_gen_andi_i32(cpu_SR[par[0]], tmp, par[2]);
  } else {
    tcg_gen_movi_i32(arg[0].out, 0);
  }
}

static void translate_xsr_ccount(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
#ifndef CONFIG_USER_ONLY
  TCGv_i32 tmp = tcg_temp_new_i32();

  translator_io_start(&dc->base);
  gen_helper_update_ccount(tcg_env);
  tcg_gen_mov_i32(tmp, cpu_SR[par[0]]);
  gen_helper_wsr_ccount(tcg_env, arg[0].in);
  tcg_gen_mov_i32(arg[0].out, tmp);

#endif
}

#define gen_translate_xsr(name)                                                                     \
  static void translate_xsr_##name(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) { \
    TCGv_i32 tmp = tcg_temp_new_i32();                                                              \
                                                                                                    \
    if (sr_name[par[0]]) {                                                                          \
      tcg_gen_mov_i32(tmp, cpu_SR[par[0]]);                                                         \
    } else {                                                                                        \
      tcg_gen_movi_i32(tmp, 0);                                                                     \
    }                                                                                               \
    translate_wsr_##name(dc, arg, par);                                                             \
    tcg_gen_mov_i32(arg[0].out, tmp);                                                               \
  }

gen_translate_xsr(acchi)
gen_translate_xsr(ccompare)
gen_translate_xsr(dbreaka)
gen_translate_xsr(dbreakc)
gen_translate_xsr(ibreaka)
gen_translate_xsr(ibreakenable)
gen_translate_xsr(icount)
gen_translate_xsr(memctl)
gen_translate_xsr(mpuenb)
gen_translate_xsr(ps)
gen_translate_xsr(rasid)
gen_translate_xsr(sar)
gen_translate_xsr(windowbase)
gen_translate_xsr(windowstart)

#undef gen_translate_xsr

static const XtensaOpcodeOps core_ops[] = {
    {
        .name = "abs",
        .translate = translate_abs,
    }, {
        .name = (const char * const[]) {
            "add", "add.n", NULL,
        },
        .translate = translate_add,
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "addi", "addi.n", NULL,
        },
        .translate = translate_addi,
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = "addmi",
        .translate = translate_addi,
    }, {
        .name = "addx2",
        .translate = translate_addx,
        .par = (const uint32_t[]){1},
    }, {
        .name = "addx4",
        .translate = translate_addx,
        .par = (const uint32_t[]){2},
    }, {
        .name = "addx8",
        .translate = translate_addx,
        .par = (const uint32_t[]){3},
    }, {
        .name = "all4",
        .translate = translate_all,
        .par = (const uint32_t[]){true, 4},
    }, {
        .name = "all8",
        .translate = translate_all,
        .par = (const uint32_t[]){true, 8},
    }, {
        .name = "and",
        .translate = translate_and,
    }, {
        .name = "andb",
        .translate = translate_boolean,
        .par = (const uint32_t[]){BOOLEAN_AND},
    }, {
        .name = "andbc",
        .translate = translate_boolean,
        .par = (const uint32_t[]){BOOLEAN_ANDC},
    }, {
        .name = "any4",
        .translate = translate_all,
        .par = (const uint32_t[]){false, 4},
    }, {
        .name = "any8",
        .translate = translate_all,
        .par = (const uint32_t[]){false, 8},
    }, {
        .name = (const char * const[]) {
            "ball", "ball.w15", "ball.w18", NULL,
        },
        .translate = translate_ball,
        .par = (const uint32_t[]){TCG_COND_EQ},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bany", "bany.w15", "bany.w18", NULL,
        },
        .translate = translate_bany,
        .par = (const uint32_t[]){TCG_COND_NE},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bbc", "bbc.w15", "bbc.w18", NULL,
        },
        .translate = translate_bb,
        .par = (const uint32_t[]){TCG_COND_EQ},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bbci", "bbci.w15", "bbci.w18", NULL,
        },
        .translate = translate_bbi,
        .par = (const uint32_t[]){TCG_COND_EQ},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bbs", "bbs.w15", "bbs.w18", NULL,
        },
        .translate = translate_bb,
        .par = (const uint32_t[]){TCG_COND_NE},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bbsi", "bbsi.w15", "bbsi.w18", NULL,
        },
        .translate = translate_bbi,
        .par = (const uint32_t[]){TCG_COND_NE},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "beq", "beq.w15", "beq.w18", NULL,
        },
        .translate = translate_b,
        .par = (const uint32_t[]){TCG_COND_EQ},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "beqi", "beqi.w15", "beqi.w18", NULL,
        },
        .translate = translate_bi,
        .par = (const uint32_t[]){TCG_COND_EQ},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "beqz", "beqz.n", "beqz.w15", "beqz.w18", NULL,
        },
        .translate = translate_bz,
        .par = (const uint32_t[]){TCG_COND_EQ},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = "bf",
        .translate = translate_bp,
        .par = (const uint32_t[]){TCG_COND_EQ},
    }, {
        .name = (const char * const[]) {
            "bge", "bge.w15", "bge.w18", NULL,
        },
        .translate = translate_b,
        .par = (const uint32_t[]){TCG_COND_GE},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bgei", "bgei.w15", "bgei.w18", NULL,
        },
        .translate = translate_bi,
        .par = (const uint32_t[]){TCG_COND_GE},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bgeu", "bgeu.w15", "bgeu.w18", NULL,
        },
        .translate = translate_b,
        .par = (const uint32_t[]){TCG_COND_GEU},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bgeui", "bgeui.w15", "bgeui.w18", NULL,
        },
        .translate = translate_bi,
        .par = (const uint32_t[]){TCG_COND_GEU},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bgez", "bgez.w15", "bgez.w18", NULL,
        },
        .translate = translate_bz,
        .par = (const uint32_t[]){TCG_COND_GE},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "blt", "blt.w15", "blt.w18", NULL,
        },
        .translate = translate_b,
        .par = (const uint32_t[]){TCG_COND_LT},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "blti", "blti.w15", "blti.w18", NULL,
        },
        .translate = translate_bi,
        .par = (const uint32_t[]){TCG_COND_LT},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bltu", "bltu.w15", "bltu.w18", NULL,
        },
        .translate = translate_b,
        .par = (const uint32_t[]){TCG_COND_LTU},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bltui", "bltui.w15", "bltui.w18", NULL,
        },
        .translate = translate_bi,
        .par = (const uint32_t[]){TCG_COND_LTU},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bltz", "bltz.w15", "bltz.w18", NULL,
        },
        .translate = translate_bz,
        .par = (const uint32_t[]){TCG_COND_LT},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bnall", "bnall.w15", "bnall.w18", NULL,
        },
        .translate = translate_ball,
        .par = (const uint32_t[]){TCG_COND_NE},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bne", "bne.w15", "bne.w18", NULL,
        },
        .translate = translate_b,
        .par = (const uint32_t[]){TCG_COND_NE},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bnei", "bnei.w15", "bnei.w18", NULL,
        },
        .translate = translate_bi,
        .par = (const uint32_t[]){TCG_COND_NE},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bnez", "bnez.n", "bnez.w15", "bnez.w18", NULL,
        },
        .translate = translate_bz,
        .par = (const uint32_t[]){TCG_COND_NE},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "bnone", "bnone.w15", "bnone.w18", NULL,
        },
        .translate = translate_bany,
        .par = (const uint32_t[]){TCG_COND_EQ},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = "break",
        .translate = translate_nop,
        .par = (const uint32_t[]){DEBUGCAUSE_BI},
        .op_flags = XTENSA_OP_DEBUG_BREAK,
    }, {
        .name = "break.n",
        .translate = translate_nop,
        .par = (const uint32_t[]){DEBUGCAUSE_BN},
        .op_flags = XTENSA_OP_DEBUG_BREAK,
    }, {
        .name = "bt",
        .translate = translate_bp,
        .par = (const uint32_t[]){TCG_COND_NE},
    }, {
        .name = "call0",
        .translate = translate_call0,
    }, {
        .name = "call12",
        .translate = translate_callw,
        .par = (const uint32_t[]){3},
    }, {
        .name = "call4",
        .translate = translate_callw,
        .par = (const uint32_t[]){1},
    }, {
        .name = "call8",
        .translate = translate_callw,
        .par = (const uint32_t[]){2},
    }, {
        .name = "callx0",
        .translate = translate_callx0,
    }, {
        .name = "callx12",
        .translate = translate_callxw,
        .par = (const uint32_t[]){3},
    }, {
        .name = "callx4",
        .translate = translate_callxw,
        .par = (const uint32_t[]){1},
    }, {
        .name = "callx8",
        .translate = translate_callxw,
        .par = (const uint32_t[]){2},
    }, {
        .name = "clamps",
        .translate = translate_clamps,
    }, {
        .name = "clrb_expstate",
        .translate = translate_clrb_expstate,
    }, {
        .name = "clrex",
        .translate = translate_clrex,
    }, {
        .name = "const16",
        .translate = translate_const16,
    }, {
        .name = "depbits",
        .translate = translate_depbits,
    }, {
        .name = "dhi",
        .translate = translate_dcache,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "dhi.b",
        .translate = translate_nop,
    }, {
        .name = "dhu",
        .translate = translate_dcache,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "dhwb",
        .translate = translate_dcache,
    }, {
        .name = "dhwb.b",
        .translate = translate_nop,
    }, {
        .name = "dhwbi",
        .translate = translate_dcache,
    }, {
        .name = "dhwbi.b",
        .translate = translate_nop,
    }, {
        .name = "dii",
        .translate = translate_nop,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "diu",
        .translate = translate_nop,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "diwb",
        .translate = translate_nop,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "diwbi",
        .translate = translate_nop,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "diwbui.p",
        .translate = translate_diwbuip,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "dpfl",
        .translate = translate_dcache,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "dpfm.b",
        .translate = translate_nop,
    }, {
        .name = "dpfm.bf",
        .translate = translate_nop,
    }, {
        .name = "dpfr",
        .translate = translate_nop,
    }, {
        .name = "dpfr.b",
        .translate = translate_nop,
    }, {
        .name = "dpfr.bf",
        .translate = translate_nop,
    }, {
        .name = "dpfro",
        .translate = translate_nop,
    }, {
        .name = "dpfw",
        .translate = translate_nop,
    }, {
        .name = "dpfw.b",
        .translate = translate_nop,
    }, {
        .name = "dpfw.bf",
        .translate = translate_nop,
    }, {
        .name = "dpfwo",
        .translate = translate_nop,
    }, {
        .name = "dsync",
        .translate = translate_nop,
    }, {
        .name = "entry",
        .translate = translate_entry,
        .test_exceptions = test_exceptions_entry,
        .test_overflow = test_overflow_entry,
        .op_flags = XTENSA_OP_EXIT_TB_M1 |
            XTENSA_OP_SYNC_REGISTER_WINDOW,
    }, {
        .name = "esync",
        .translate = translate_nop,
    }, {
        .name = "excw",
        .translate = translate_nop,
    }, {
        .name = "extui",
        .translate = translate_extui,
    }, {
        .name = "extw",
        .translate = translate_memw,
    }, {
        .name = "getex",
        .translate = translate_getex,
    }, {
        .name = "hwwdtlba",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "hwwitlba",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "idtlb",
        .translate = translate_itlb,
        .par = (const uint32_t[]){true},
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "ihi",
        .translate = translate_icache,
    }, {
        .name = "ihu",
        .translate = translate_icache,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "iii",
        .translate = translate_nop,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "iitlb",
        .translate = translate_itlb,
        .par = (const uint32_t[]){false},
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "iiu",
        .translate = translate_nop,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = (const char * const[]) {
            "ill", "ill.n", NULL,
        },
        .op_flags = XTENSA_OP_ILL | XTENSA_OP_NAME_ARRAY,
    }, {
        .name = "ipf",
        .translate = translate_nop,
    }, {
        .name = "ipfl",
        .translate = translate_icache,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "isync",
        .translate = translate_nop,
    }, {
        .name = "j",
        .translate = translate_j,
    }, {
        .name = "jx",
        .translate = translate_jx,
    }, {
        .name = "l16si",
        .translate = translate_ldst,
        .par = (const uint32_t[]){MO_TESW, false, false},
        .op_flags = XTENSA_OP_LOAD,
    }, {
        .name = "l16ui",
        .translate = translate_ldst,
        .par = (const uint32_t[]){MO_TEUW, false, false},
        .op_flags = XTENSA_OP_LOAD,
    }, {
        .name = "l32ai",
        .translate = translate_ldst,
        .par = (const uint32_t[]){MO_TEUL | MO_ALIGN, true, false},
        .op_flags = XTENSA_OP_LOAD,
    }, {
        .name = "l32e",
        .translate = translate_l32e,
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_LOAD,
    }, {
        .name = "l32ex",
        .translate = translate_l32ex,
        .op_flags = XTENSA_OP_LOAD,
    }, {
        .name = (const char * const[]) {
            "l32i", "l32i.n", NULL,
        },
        .translate = translate_ldst,
        .par = (const uint32_t[]){MO_TEUL, false, false},
        .op_flags = XTENSA_OP_NAME_ARRAY | XTENSA_OP_LOAD,
    }, {
        .name = "l32r",
        .translate = translate_l32r,
        .op_flags = XTENSA_OP_LOAD,
    }, {
        .name = "l8ui",
        .translate = translate_ldst,
        .par = (const uint32_t[]){MO_UB, false, false},
        .op_flags = XTENSA_OP_LOAD,
    }, {
        .name = "ldct",
        .translate = translate_lct,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "ldcw",
        .translate = translate_nop,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "lddec",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_NONE, 0, -4},
        .op_flags = XTENSA_OP_LOAD,
    }, {
        .name = "ldinc",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_NONE, 0, 4},
        .op_flags = XTENSA_OP_LOAD,
    }, {
        .name = "ldpte",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "lict",
        .translate = translate_lct,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "licw",
        .translate = translate_nop,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = (const char * const[]) {
            "loop", "loop.w15", NULL,
        },
        .translate = translate_loop,
        .par = (const uint32_t[]){TCG_COND_NEVER},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "loopgtz", "loopgtz.w15", NULL,
        },
        .translate = translate_loop,
        .par = (const uint32_t[]){TCG_COND_GT},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "loopnez", "loopnez.w15", NULL,
        },
        .translate = translate_loop,
        .par = (const uint32_t[]){TCG_COND_NE},
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = "max",
        .translate = translate_smax,
    }, {
        .name = "maxu",
        .translate = translate_umax,
    }, {
        .name = "memw",
        .translate = translate_memw,
    }, {
        .name = "min",
        .translate = translate_smin,
    }, {
        .name = "minu",
        .translate = translate_umin,
    }, {
        .name = (const char * const[]) {
            "mov", "mov.n", NULL,
        },
        .translate = translate_mov,
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = "moveqz",
        .translate = translate_movcond,
        .par = (const uint32_t[]){TCG_COND_EQ},
    }, {
        .name = "movf",
        .translate = translate_movp,
        .par = (const uint32_t[]){TCG_COND_EQ},
    }, {
        .name = "movgez",
        .translate = translate_movcond,
        .par = (const uint32_t[]){TCG_COND_GE},
    }, {
        .name = "movi",
        .translate = translate_movi,
    }, {
        .name = "movi.n",
        .translate = translate_movi,
    }, {
        .name = "movltz",
        .translate = translate_movcond,
        .par = (const uint32_t[]){TCG_COND_LT},
    }, {
        .name = "movnez",
        .translate = translate_movcond,
        .par = (const uint32_t[]){TCG_COND_NE},
    }, {
        .name = "movsp",
        .translate = translate_movsp,
        .op_flags = XTENSA_OP_ALLOCA,
    }, {
        .name = "movt",
        .translate = translate_movp,
        .par = (const uint32_t[]){TCG_COND_NE},
    }, {
        .name = "mul.aa.hh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_HH, 0},
    }, {
        .name = "mul.aa.hl",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_HL, 0},
    }, {
        .name = "mul.aa.lh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_LH, 0},
    }, {
        .name = "mul.aa.ll",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_LL, 0},
    }, {
        .name = "mul.ad.hh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_HH, 0},
    }, {
        .name = "mul.ad.hl",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_HL, 0},
    }, {
        .name = "mul.ad.lh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_LH, 0},
    }, {
        .name = "mul.ad.ll",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_LL, 0},
    }, {
        .name = "mul.da.hh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_HH, 0},
    }, {
        .name = "mul.da.hl",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_HL, 0},
    }, {
        .name = "mul.da.lh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_LH, 0},
    }, {
        .name = "mul.da.ll",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_LL, 0},
    }, {
        .name = "mul.dd.hh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_HH, 0},
    }, {
        .name = "mul.dd.hl",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_HL, 0},
    }, {
        .name = "mul.dd.lh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_LH, 0},
    }, {
        .name = "mul.dd.ll",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MUL, MAC16_LL, 0},
    }, {
        .name = "mul16s",
        .translate = translate_mul16,
        .par = (const uint32_t[]){true},
    }, {
        .name = "mul16u",
        .translate = translate_mul16,
        .par = (const uint32_t[]){false},
    }, {
        .name = "mula.aa.hh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HH, 0},
    }, {
        .name = "mula.aa.hl",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HL, 0},
    }, {
        .name = "mula.aa.lh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LH, 0},
    }, {
        .name = "mula.aa.ll",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LL, 0},
    }, {
        .name = "mula.ad.hh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HH, 0},
    }, {
        .name = "mula.ad.hl",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HL, 0},
    }, {
        .name = "mula.ad.lh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LH, 0},
    }, {
        .name = "mula.ad.ll",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LL, 0},
    }, {
        .name = "mula.da.hh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HH, 0},
    }, {
        .name = "mula.da.hh.lddec",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HH, -4},
    }, {
        .name = "mula.da.hh.ldinc",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HH, 4},
    }, {
        .name = "mula.da.hl",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HL, 0},
    }, {
        .name = "mula.da.hl.lddec",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HL, -4},
    }, {
        .name = "mula.da.hl.ldinc",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HL, 4},
    }, {
        .name = "mula.da.lh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LH, 0},
    }, {
        .name = "mula.da.lh.lddec",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LH, -4},
    }, {
        .name = "mula.da.lh.ldinc",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LH, 4},
    }, {
        .name = "mula.da.ll",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LL, 0},
    }, {
        .name = "mula.da.ll.lddec",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LL, -4},
    }, {
        .name = "mula.da.ll.ldinc",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LL, 4},
    }, {
        .name = "mula.dd.hh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HH, 0},
    }, {
        .name = "mula.dd.hh.lddec",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HH, -4},
    }, {
        .name = "mula.dd.hh.ldinc",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HH, 4},
    }, {
        .name = "mula.dd.hl",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HL, 0},
    }, {
        .name = "mula.dd.hl.lddec",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HL, -4},
    }, {
        .name = "mula.dd.hl.ldinc",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_HL, 4},
    }, {
        .name = "mula.dd.lh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LH, 0},
    }, {
        .name = "mula.dd.lh.lddec",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LH, -4},
    }, {
        .name = "mula.dd.lh.ldinc",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LH, 4},
    }, {
        .name = "mula.dd.ll",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LL, 0},
    }, {
        .name = "mula.dd.ll.lddec",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LL, -4},
    }, {
        .name = "mula.dd.ll.ldinc",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULA, MAC16_LL, 4},
    }, {
        .name = "mull",
        .translate = translate_mull,
    }, {
        .name = "muls.aa.hh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_HH, 0},
    }, {
        .name = "muls.aa.hl",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_HL, 0},
    }, {
        .name = "muls.aa.lh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_LH, 0},
    }, {
        .name = "muls.aa.ll",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_LL, 0},
    }, {
        .name = "muls.ad.hh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_HH, 0},
    }, {
        .name = "muls.ad.hl",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_HL, 0},
    }, {
        .name = "muls.ad.lh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_LH, 0},
    }, {
        .name = "muls.ad.ll",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_LL, 0},
    }, {
        .name = "muls.da.hh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_HH, 0},
    }, {
        .name = "muls.da.hl",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_HL, 0},
    }, {
        .name = "muls.da.lh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_LH, 0},
    }, {
        .name = "muls.da.ll",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_LL, 0},
    }, {
        .name = "muls.dd.hh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_HH, 0},
    }, {
        .name = "muls.dd.hl",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_HL, 0},
    }, {
        .name = "muls.dd.lh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_LH, 0},
    }, {
        .name = "muls.dd.ll",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_MULS, MAC16_LL, 0},
    }, {
        .name = "mulsh",
        .translate = translate_mulh,
        .par = (const uint32_t[]){true},
    }, {
        .name = "muluh",
        .translate = translate_mulh,
        .par = (const uint32_t[]){false},
    }, {
        .name = "neg",
        .translate = translate_neg,
    }, {
        .name = (const char * const[]) {
            "nop", "nop.n", NULL,
        },
        .translate = translate_nop,
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = "nsa",
        .translate = translate_nsa,
    }, {
        .name = "nsau",
        .translate = translate_nsau,
    }, {
        .name = "or",
        .translate = translate_or,
    }, {
        .name = "orb",
        .translate = translate_boolean,
        .par = (const uint32_t[]){BOOLEAN_OR},
    }, {
        .name = "orbc",
        .translate = translate_boolean,
        .par = (const uint32_t[]){BOOLEAN_ORC},
    }, {
        .name = "pdtlb",
        .translate = translate_ptlb,
        .par = (const uint32_t[]){true},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "pfend.a",
        .translate = translate_nop,
    }, {
        .name = "pfend.o",
        .translate = translate_nop,
    }, {
        .name = "pfnxt.f",
        .translate = translate_nop,
    }, {
        .name = "pfwait.a",
        .translate = translate_nop,
    }, {
        .name = "pfwait.r",
        .translate = translate_nop,
    }, {
        .name = "pitlb",
        .translate = translate_ptlb,
        .par = (const uint32_t[]){false},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "pptlb",
        .translate = translate_pptlb,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "quos",
        .translate = translate_quos,
        .par = (const uint32_t[]){true},
        .op_flags = XTENSA_OP_DIVIDE_BY_ZERO,
    }, {
        .name = "quou",
        .translate = translate_quou,
        .op_flags = XTENSA_OP_DIVIDE_BY_ZERO,
    }, {
        .name = "rdtlb0",
        .translate = translate_rtlb,
        .par = (const uint32_t[]){true, 0},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rdtlb1",
        .translate = translate_rtlb,
        .par = (const uint32_t[]){true, 1},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "read_impwire",
        .translate = translate_read_impwire,
    }, {
        .name = "rems",
        .translate = translate_quos,
        .par = (const uint32_t[]){false},
        .op_flags = XTENSA_OP_DIVIDE_BY_ZERO,
    }, {
        .name = "remu",
        .translate = translate_remu,
        .op_flags = XTENSA_OP_DIVIDE_BY_ZERO,
    }, {
        .name = "rer",
        .translate = translate_rer,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = (const char * const[]) {
            "ret", "ret.n", NULL,
        },
        .translate = translate_ret,
        .op_flags = XTENSA_OP_NAME_ARRAY,
    }, {
        .name = (const char * const[]) {
            "retw", "retw.n", NULL,
        },
        .translate = translate_retw,
        .test_exceptions = test_exceptions_retw,
        .op_flags = XTENSA_OP_UNDERFLOW | XTENSA_OP_NAME_ARRAY,
    }, {
        .name = "rfdd",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "rfde",
        .translate = translate_rfde,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rfdo",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "rfe",
        .translate = translate_rfe,
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_CHECK_INTERRUPTS,
    }, {
        .name = "rfi",
        .translate = translate_rfi,
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_CHECK_INTERRUPTS,
    }, {
        .name = "rfwo",
        .translate = translate_rfw,
        .par = (const uint32_t[]){true},
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_CHECK_INTERRUPTS,
    }, {
        .name = "rfwu",
        .translate = translate_rfw,
        .par = (const uint32_t[]){false},
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_CHECK_INTERRUPTS,
    }, {
        .name = "ritlb0",
        .translate = translate_rtlb,
        .par = (const uint32_t[]){false, 0},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "ritlb1",
        .translate = translate_rtlb,
        .par = (const uint32_t[]){false, 1},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rptlb0",
        .translate = translate_rptlb0,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rptlb1",
        .translate = translate_rptlb1,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rotw",
        .translate = translate_rotw,
        .op_flags = XTENSA_OP_PRIVILEGED |
            XTENSA_OP_EXIT_TB_M1 |
            XTENSA_OP_SYNC_REGISTER_WINDOW,
    }, {
        .name = "rsil",
        .translate = translate_rsil,
        .op_flags =
            XTENSA_OP_PRIVILEGED |
            XTENSA_OP_EXIT_TB_0 |
            XTENSA_OP_CHECK_INTERRUPTS,
    }, {
        .name = "rsr.176",
        .translate = translate_rsr,
        .par = (const uint32_t[]){176},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.208",
        .translate = translate_rsr,
        .par = (const uint32_t[]){208},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.acchi",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ACCHI,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "rsr.acclo",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ACCLO,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "rsr.atomctl",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ATOMCTL,
            XTENSA_OPTION_ATOMCTL,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.br",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            BR,
            XTENSA_OPTION_BOOLEAN,
        },
    }, {
        .name = "rsr.cacheadrdis",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            CACHEADRDIS,
            XTENSA_OPTION_MPU,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.cacheattr",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            CACHEATTR,
            XTENSA_OPTION_CACHEATTR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.ccompare0",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_ccompare,
        .par = (const uint32_t[]){
            CCOMPARE,
            XTENSA_OPTION_TIMER_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.ccompare1",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_ccompare,
        .par = (const uint32_t[]){
            CCOMPARE + 1,
            XTENSA_OPTION_TIMER_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.ccompare2",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_ccompare,
        .par = (const uint32_t[]){
            CCOMPARE + 2,
            XTENSA_OPTION_TIMER_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.ccount",
        .translate = translate_rsr_ccount,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            CCOUNT,
            XTENSA_OPTION_TIMER_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "rsr.configid0",
        .translate = translate_rsr,
        .par = (const uint32_t[]){CONFIGID0},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.configid1",
        .translate = translate_rsr,
        .par = (const uint32_t[]){CONFIGID1},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.cpenable",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            CPENABLE,
            XTENSA_OPTION_COPROCESSOR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.dbreaka0",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_dbreak,
        .par = (const uint32_t[]){
            DBREAKA,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.dbreaka1",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_dbreak,
        .par = (const uint32_t[]){
            DBREAKA + 1,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.dbreakc0",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_dbreak,
        .par = (const uint32_t[]){
            DBREAKC,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.dbreakc1",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_dbreak,
        .par = (const uint32_t[]){
            DBREAKC + 1,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.ddr",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            DDR,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.debugcause",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            DEBUGCAUSE,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.depc",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            DEPC,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.dtlbcfg",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            DTLBCFG,
            XTENSA_OPTION_MMU,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.epc1",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            EPC1,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.epc2",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 1,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.epc3",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 2,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.epc4",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 3,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.epc5",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 4,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.epc6",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 5,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.epc7",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 6,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.eps2",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.eps3",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 1,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.eps4",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 2,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.eps5",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 3,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.eps6",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 4,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.eps7",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 5,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.eraccess",
        .translate = translate_rsr,
        .par = (const uint32_t[]){ERACCESS},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.exccause",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            EXCCAUSE,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.excsave1",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            EXCSAVE1,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.excsave2",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 1,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.excsave3",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 2,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.excsave4",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 3,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.excsave5",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 4,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.excsave6",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 5,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.excsave7",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 6,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.excvaddr",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            EXCVADDR,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.ibreaka0",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_ibreak,
        .par = (const uint32_t[]){
            IBREAKA,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.ibreaka1",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_ibreak,
        .par = (const uint32_t[]){
            IBREAKA + 1,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.ibreakenable",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            IBREAKENABLE,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.icount",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ICOUNT,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.icountlevel",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ICOUNTLEVEL,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.intclear",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            INTCLEAR,
            XTENSA_OPTION_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.intenable",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            INTENABLE,
            XTENSA_OPTION_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.interrupt",
        .translate = translate_rsr_ccount,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            INTSET,
            XTENSA_OPTION_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "rsr.intset",
        .translate = translate_rsr_ccount,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            INTSET,
            XTENSA_OPTION_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "rsr.itlbcfg",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ITLBCFG,
            XTENSA_OPTION_MMU,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.lbeg",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            LBEG,
            XTENSA_OPTION_LOOP,
        },
    }, {
        .name = "rsr.lcount",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            LCOUNT,
            XTENSA_OPTION_LOOP,
        },
    }, {
        .name = "rsr.lend",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            LEND,
            XTENSA_OPTION_LOOP,
        },
    }, {
        .name = "rsr.litbase",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            LITBASE,
            XTENSA_OPTION_EXTENDED_L32R,
        },
    }, {
        .name = "rsr.m0",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MR,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "rsr.m1",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MR + 1,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "rsr.m2",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MR + 2,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "rsr.m3",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MR + 3,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "rsr.memctl",
        .translate = translate_rsr,
        .par = (const uint32_t[]){MEMCTL},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.mecr",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MECR,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.mepc",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MEPC,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.meps",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MEPS,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.mesave",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MESAVE,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.mesr",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MESR,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.mevaddr",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MESR,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.misc0",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MISC,
            XTENSA_OPTION_MISC_SR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.misc1",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MISC + 1,
            XTENSA_OPTION_MISC_SR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.misc2",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MISC + 2,
            XTENSA_OPTION_MISC_SR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.misc3",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MISC + 3,
            XTENSA_OPTION_MISC_SR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.mpucfg",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MPUCFG,
            XTENSA_OPTION_MPU,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.mpuenb",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MPUENB,
            XTENSA_OPTION_MPU,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.prefctl",
        .translate = translate_rsr,
        .par = (const uint32_t[]){PREFCTL},
    }, {
        .name = "rsr.prid",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            PRID,
            XTENSA_OPTION_PROCESSOR_ID,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.ps",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            PS,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.ptevaddr",
        .translate = translate_rsr_ptevaddr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            PTEVADDR,
            XTENSA_OPTION_MMU,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.rasid",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            RASID,
            XTENSA_OPTION_MMU,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.sar",
        .translate = translate_rsr,
        .par = (const uint32_t[]){SAR},
    }, {
        .name = "rsr.scompare1",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            SCOMPARE1,
            XTENSA_OPTION_CONDITIONAL_STORE,
        },
    }, {
        .name = "rsr.vecbase",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            VECBASE,
            XTENSA_OPTION_RELOCATABLE_VECTOR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.windowbase",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            WINDOW_BASE,
            XTENSA_OPTION_WINDOWED_REGISTER,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsr.windowstart",
        .translate = translate_rsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            WINDOW_START,
            XTENSA_OPTION_WINDOWED_REGISTER,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "rsync",
        .translate = translate_nop,
    }, {
        .name = "rur.expstate",
        .translate = translate_rur,
        .par = (const uint32_t[]){EXPSTATE},
    }, {
        .name = "rur.threadptr",
        .translate = translate_rur,
        .par = (const uint32_t[]){THREADPTR},
    }, {
        .name = "s16i",
        .translate = translate_ldst,
        .par = (const uint32_t[]){MO_TEUW, false, true},
        .op_flags = XTENSA_OP_STORE,
    }, {
        .name = "s32c1i",
        .translate = translate_s32c1i,
        .op_flags = XTENSA_OP_LOAD | XTENSA_OP_STORE,
    }, {
        .name = "s32e",
        .translate = translate_s32e,
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_STORE,
    }, {
        .name = "s32ex",
        .translate = translate_s32ex,
        .op_flags = XTENSA_OP_LOAD | XTENSA_OP_STORE,
    }, {
        .name = (const char * const[]) {
            "s32i", "s32i.n", "s32nb", NULL,
        },
        .translate = translate_ldst,
        .par = (const uint32_t[]){MO_TEUL, false, true},
        .op_flags = XTENSA_OP_NAME_ARRAY | XTENSA_OP_STORE,
    }, {
        .name = "s32ri",
        .translate = translate_ldst,
        .par = (const uint32_t[]){MO_TEUL | MO_ALIGN, true, true},
        .op_flags = XTENSA_OP_STORE,
    }, {
        .name = "s8i",
        .translate = translate_ldst,
        .par = (const uint32_t[]){MO_UB, false, true},
        .op_flags = XTENSA_OP_STORE,
    }, {
        .name = "salt",
        .translate = translate_salt,
        .par = (const uint32_t[]){TCG_COND_LT},
    }, {
        .name = "saltu",
        .translate = translate_salt,
        .par = (const uint32_t[]){TCG_COND_LTU},
    }, {
        .name = "sdct",
        .translate = translate_nop,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "sdcw",
        .translate = translate_nop,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "setb_expstate",
        .translate = translate_setb_expstate,
    }, {
        .name = "sext",
        .translate = translate_sext,
    }, {
        .name = "sict",
        .translate = translate_nop,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "sicw",
        .translate = translate_nop,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "simcall",
        .translate = translate_simcall,
        .test_exceptions = test_exceptions_simcall,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "sll",
        .translate = translate_sll,
    }, {
        .name = "slli",
        .translate = translate_slli,
    }, {
        .name = "sra",
        .translate = translate_sra,
    }, {
        .name = "srai",
        .translate = translate_srai,
    }, {
        .name = "src",
        .translate = translate_src,
    }, {
        .name = "srl",
        .translate = translate_srl,
    }, {
        .name = "srli",
        .translate = translate_srli,
    }, {
        .name = "ssa8b",
        .translate = translate_ssa8b,
    }, {
        .name = "ssa8l",
        .translate = translate_ssa8l,
    }, {
        .name = "ssai",
        .translate = translate_ssai,
    }, {
        .name = "ssl",
        .translate = translate_ssl,
    }, {
        .name = "ssr",
        .translate = translate_ssr,
    }, {
        .name = "sub",
        .translate = translate_sub,
    }, {
        .name = "subx2",
        .translate = translate_subx,
        .par = (const uint32_t[]){1},
    }, {
        .name = "subx4",
        .translate = translate_subx,
        .par = (const uint32_t[]){2},
    }, {
        .name = "subx8",
        .translate = translate_subx,
        .par = (const uint32_t[]){3},
    }, {
        .name = "syscall",
        .op_flags = XTENSA_OP_SYSCALL,
    }, {
        .name = "umul.aa.hh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_UMUL, MAC16_HH, 0},
    }, {
        .name = "umul.aa.hl",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_UMUL, MAC16_HL, 0},
    }, {
        .name = "umul.aa.lh",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_UMUL, MAC16_LH, 0},
    }, {
        .name = "umul.aa.ll",
        .translate = translate_mac16,
        .par = (const uint32_t[]){MAC16_UMUL, MAC16_LL, 0},
    }, {
        .name = "waiti",
        .translate = translate_waiti,
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "wdtlb",
        .translate = translate_wtlb,
        .par = (const uint32_t[]){true},
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "wer",
        .translate = translate_wer,
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "witlb",
        .translate = translate_wtlb,
        .par = (const uint32_t[]){false},
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "wptlb",
        .translate = translate_wptlb,
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "wrmsk_expstate",
        .translate = translate_wrmsk_expstate,
    }, {
        .name = "wsr.176",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "wsr.208",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "wsr.acchi",
        .translate = translate_wsr_acchi,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ACCHI,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "wsr.acclo",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ACCLO,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "wsr.atomctl",
        .translate = translate_wsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ATOMCTL,
            XTENSA_OPTION_ATOMCTL,
            0x3f,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.br",
        .translate = translate_wsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            BR,
            XTENSA_OPTION_BOOLEAN,
            0xffff,
        },
    }, {
        .name = "wsr.cacheadrdis",
        .translate = translate_wsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            CACHEADRDIS,
            XTENSA_OPTION_MPU,
            0xff,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.cacheattr",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            CACHEATTR,
            XTENSA_OPTION_CACHEATTR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.ccompare0",
        .translate = translate_wsr_ccompare,
        .test_exceptions = test_exceptions_ccompare,
        .par = (const uint32_t[]){
            CCOMPARE,
            XTENSA_OPTION_TIMER_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "wsr.ccompare1",
        .translate = translate_wsr_ccompare,
        .test_exceptions = test_exceptions_ccompare,
        .par = (const uint32_t[]){
            CCOMPARE + 1,
            XTENSA_OPTION_TIMER_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "wsr.ccompare2",
        .translate = translate_wsr_ccompare,
        .test_exceptions = test_exceptions_ccompare,
        .par = (const uint32_t[]){
            CCOMPARE + 2,
            XTENSA_OPTION_TIMER_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "wsr.ccount",
        .translate = translate_wsr_ccount,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            CCOUNT,
            XTENSA_OPTION_TIMER_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "wsr.configid0",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "wsr.configid1",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "wsr.cpenable",
        .translate = translate_wsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            CPENABLE,
            XTENSA_OPTION_COPROCESSOR,
            0xff,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "wsr.dbreaka0",
        .translate = translate_wsr_dbreaka,
        .test_exceptions = test_exceptions_dbreak,
        .par = (const uint32_t[]){
            DBREAKA,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.dbreaka1",
        .translate = translate_wsr_dbreaka,
        .test_exceptions = test_exceptions_dbreak,
        .par = (const uint32_t[]){
            DBREAKA + 1,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.dbreakc0",
        .translate = translate_wsr_dbreakc,
        .test_exceptions = test_exceptions_dbreak,
        .par = (const uint32_t[]){
            DBREAKC,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.dbreakc1",
        .translate = translate_wsr_dbreakc,
        .test_exceptions = test_exceptions_dbreak,
        .par = (const uint32_t[]){
            DBREAKC + 1,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.ddr",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            DDR,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.debugcause",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "wsr.depc",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            DEPC,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.dtlbcfg",
        .translate = translate_wsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            DTLBCFG,
            XTENSA_OPTION_MMU,
            0x01130000,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.epc1",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            EPC1,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.epc2",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 1,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.epc3",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 2,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.epc4",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 3,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.epc5",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 4,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.epc6",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 5,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.epc7",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 6,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.eps2",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.eps3",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 1,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.eps4",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 2,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.eps5",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 3,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.eps6",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 4,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.eps7",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 5,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.eraccess",
        .translate = translate_wsr_mask,
        .par = (const uint32_t[]){
            ERACCESS,
            0,
            0xffff,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.exccause",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            EXCCAUSE,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.excsave1",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            EXCSAVE1,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.excsave2",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 1,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.excsave3",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 2,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.excsave4",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 3,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.excsave5",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 4,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.excsave6",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 5,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.excsave7",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 6,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.excvaddr",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            EXCVADDR,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.ibreaka0",
        .translate = translate_wsr_ibreaka,
        .test_exceptions = test_exceptions_ibreak,
        .par = (const uint32_t[]){
            IBREAKA,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "wsr.ibreaka1",
        .translate = translate_wsr_ibreaka,
        .test_exceptions = test_exceptions_ibreak,
        .par = (const uint32_t[]){
            IBREAKA + 1,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "wsr.ibreakenable",
        .translate = translate_wsr_ibreakenable,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            IBREAKENABLE,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "wsr.icount",
        .translate = translate_wsr_icount,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ICOUNT,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.icountlevel",
        .translate = translate_wsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ICOUNTLEVEL,
            XTENSA_OPTION_DEBUG,
            0xf,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "wsr.intclear",
        .translate = translate_wsr_intclear,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            INTCLEAR,
            XTENSA_OPTION_INTERRUPT,
        },
        .op_flags =
            XTENSA_OP_PRIVILEGED |
            XTENSA_OP_EXIT_TB_0 |
            XTENSA_OP_CHECK_INTERRUPTS,
    }, {
        .name = "wsr.intenable",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            INTENABLE,
            XTENSA_OPTION_INTERRUPT,
        },
        .op_flags =
            XTENSA_OP_PRIVILEGED |
            XTENSA_OP_EXIT_TB_0 |
            XTENSA_OP_CHECK_INTERRUPTS,
    }, {
        .name = "wsr.interrupt",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            INTSET,
            XTENSA_OPTION_INTERRUPT,
        },
        .op_flags =
            XTENSA_OP_PRIVILEGED |
            XTENSA_OP_EXIT_TB_0 |
            XTENSA_OP_CHECK_INTERRUPTS,
    }, {
        .name = "wsr.intset",
        .translate = translate_wsr_intset,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            INTSET,
            XTENSA_OPTION_INTERRUPT,
        },
        .op_flags =
            XTENSA_OP_PRIVILEGED |
            XTENSA_OP_EXIT_TB_0 |
            XTENSA_OP_CHECK_INTERRUPTS,
    }, {
        .name = "wsr.itlbcfg",
        .translate = translate_wsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ITLBCFG,
            XTENSA_OPTION_MMU,
            0x01130000,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.lbeg",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            LBEG,
            XTENSA_OPTION_LOOP,
        },
        .op_flags = XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "wsr.lcount",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            LCOUNT,
            XTENSA_OPTION_LOOP,
        },
    }, {
        .name = "wsr.lend",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            LEND,
            XTENSA_OPTION_LOOP,
        },
        .op_flags = XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "wsr.litbase",
        .translate = translate_wsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            LITBASE,
            XTENSA_OPTION_EXTENDED_L32R,
            0xfffff001,
        },
        .op_flags = XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "wsr.m0",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MR,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "wsr.m1",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MR + 1,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "wsr.m2",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MR + 2,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "wsr.m3",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MR + 3,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "wsr.memctl",
        .translate = translate_wsr_memctl,
        .par = (const uint32_t[]){MEMCTL},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.mecr",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MECR,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.mepc",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MEPC,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.meps",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MEPS,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.mesave",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MESAVE,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.mesr",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MESR,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.mevaddr",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MESR,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.misc0",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MISC,
            XTENSA_OPTION_MISC_SR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.misc1",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MISC + 1,
            XTENSA_OPTION_MISC_SR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.misc2",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MISC + 2,
            XTENSA_OPTION_MISC_SR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.misc3",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MISC + 3,
            XTENSA_OPTION_MISC_SR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.mmid",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MMID,
            XTENSA_OPTION_TRACE_PORT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.mpuenb",
        .translate = translate_wsr_mpuenb,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MPUENB,
            XTENSA_OPTION_MPU,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "wsr.prefctl",
        .translate = translate_wsr,
        .par = (const uint32_t[]){PREFCTL},
    }, {
        .name = "wsr.prid",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "wsr.ps",
        .translate = translate_wsr_ps,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            PS,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags =
            XTENSA_OP_PRIVILEGED |
            XTENSA_OP_EXIT_TB_M1 |
            XTENSA_OP_CHECK_INTERRUPTS,
    }, {
        .name = "wsr.ptevaddr",
        .translate = translate_wsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            PTEVADDR,
            XTENSA_OPTION_MMU,
            0xffc00000,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.rasid",
        .translate = translate_wsr_rasid,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            RASID,
            XTENSA_OPTION_MMU,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "wsr.sar",
        .translate = translate_wsr_sar,
        .par = (const uint32_t[]){SAR},
    }, {
        .name = "wsr.scompare1",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            SCOMPARE1,
            XTENSA_OPTION_CONDITIONAL_STORE,
        },
    }, {
        .name = "wsr.vecbase",
        .translate = translate_wsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            VECBASE,
            XTENSA_OPTION_RELOCATABLE_VECTOR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "wsr.windowbase",
        .translate = translate_wsr_windowbase,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            WINDOW_BASE,
            XTENSA_OPTION_WINDOWED_REGISTER,
        },
        .op_flags = XTENSA_OP_PRIVILEGED |
            XTENSA_OP_EXIT_TB_M1 |
            XTENSA_OP_SYNC_REGISTER_WINDOW,
    }, {
        .name = "wsr.windowstart",
        .translate = translate_wsr_windowstart,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            WINDOW_START,
            XTENSA_OPTION_WINDOWED_REGISTER,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "wur.expstate",
        .translate = translate_wur,
        .par = (const uint32_t[]){EXPSTATE},
    }, {
        .name = "wur.f64r_lo",
        .translate = translate_xur_f64,
    }, {
        .name = "wur.f64r_hi",
        .translate = translate_xur_f64,
    }, {
        .name = "wur.f64s",
        .translate = translate_xur_f64,
    }, {
        .name = "rur.f64r_lo",
        .translate = translate_xur_f64,
    }, {
        .name = "rur.f64r_hi",
        .translate = translate_xur_f64,
    }, {
        .name = "rur.f64s",
        .translate = translate_xur_f64,
    }, {
        .name = "wur.threadptr",
        .translate = translate_wur,
        .par = (const uint32_t[]){THREADPTR},
    }, {
        .name = "xor",
        .translate = translate_xor,
    }, {
        .name = "xorb",
        .translate = translate_boolean,
        .par = (const uint32_t[]){BOOLEAN_XOR},
    }, {
        .name = "xsr.176",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "xsr.208",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "xsr.acchi",
        .translate = translate_xsr_acchi,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ACCHI,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "xsr.acclo",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ACCLO,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "xsr.atomctl",
        .translate = translate_xsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ATOMCTL,
            XTENSA_OPTION_ATOMCTL,
            0x3f,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.br",
        .translate = translate_xsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            BR,
            XTENSA_OPTION_BOOLEAN,
            0xffff,
        },
    }, {
        .name = "xsr.cacheadrdis",
        .translate = translate_xsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            CACHEADRDIS,
            XTENSA_OPTION_MPU,
            0xff,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.cacheattr",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            CACHEATTR,
            XTENSA_OPTION_CACHEATTR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.ccompare0",
        .translate = translate_xsr_ccompare,
        .test_exceptions = test_exceptions_ccompare,
        .par = (const uint32_t[]){
            CCOMPARE,
            XTENSA_OPTION_TIMER_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "xsr.ccompare1",
        .translate = translate_xsr_ccompare,
        .test_exceptions = test_exceptions_ccompare,
        .par = (const uint32_t[]){
            CCOMPARE + 1,
            XTENSA_OPTION_TIMER_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "xsr.ccompare2",
        .translate = translate_xsr_ccompare,
        .test_exceptions = test_exceptions_ccompare,
        .par = (const uint32_t[]){
            CCOMPARE + 2,
            XTENSA_OPTION_TIMER_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "xsr.ccount",
        .translate = translate_xsr_ccount,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            CCOUNT,
            XTENSA_OPTION_TIMER_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "xsr.configid0",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "xsr.configid1",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "xsr.cpenable",
        .translate = translate_xsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            CPENABLE,
            XTENSA_OPTION_COPROCESSOR,
            0xff,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "xsr.dbreaka0",
        .translate = translate_xsr_dbreaka,
        .test_exceptions = test_exceptions_dbreak,
        .par = (const uint32_t[]){
            DBREAKA,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.dbreaka1",
        .translate = translate_xsr_dbreaka,
        .test_exceptions = test_exceptions_dbreak,
        .par = (const uint32_t[]){
            DBREAKA + 1,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.dbreakc0",
        .translate = translate_xsr_dbreakc,
        .test_exceptions = test_exceptions_dbreak,
        .par = (const uint32_t[]){
            DBREAKC,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.dbreakc1",
        .translate = translate_xsr_dbreakc,
        .test_exceptions = test_exceptions_dbreak,
        .par = (const uint32_t[]){
            DBREAKC + 1,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.ddr",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            DDR,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.debugcause",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "xsr.depc",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            DEPC,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.dtlbcfg",
        .translate = translate_xsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            DTLBCFG,
            XTENSA_OPTION_MMU,
            0x01130000,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.epc1",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            EPC1,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.epc2",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 1,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.epc3",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 2,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.epc4",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 3,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.epc5",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 4,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.epc6",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 5,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.epc7",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPC1 + 6,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.eps2",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.eps3",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 1,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.eps4",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 2,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.eps5",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 3,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.eps6",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 4,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.eps7",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EPS2 + 5,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.eraccess",
        .translate = translate_xsr_mask,
        .par = (const uint32_t[]){
            ERACCESS,
            0,
            0xffff,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.exccause",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            EXCCAUSE,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.excsave1",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            EXCSAVE1,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.excsave2",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 1,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.excsave3",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 2,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.excsave4",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 3,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.excsave5",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 4,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.excsave6",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 5,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.excsave7",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_hpi,
        .par = (const uint32_t[]){
            EXCSAVE1 + 6,
            XTENSA_OPTION_HIGH_PRIORITY_INTERRUPT,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.excvaddr",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            EXCVADDR,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.ibreaka0",
        .translate = translate_xsr_ibreaka,
        .test_exceptions = test_exceptions_ibreak,
        .par = (const uint32_t[]){
            IBREAKA,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "xsr.ibreaka1",
        .translate = translate_xsr_ibreaka,
        .test_exceptions = test_exceptions_ibreak,
        .par = (const uint32_t[]){
            IBREAKA + 1,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "xsr.ibreakenable",
        .translate = translate_xsr_ibreakenable,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            IBREAKENABLE,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_0,
    }, {
        .name = "xsr.icount",
        .translate = translate_xsr_icount,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ICOUNT,
            XTENSA_OPTION_DEBUG,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.icountlevel",
        .translate = translate_xsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ICOUNTLEVEL,
            XTENSA_OPTION_DEBUG,
            0xf,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "xsr.intclear",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "xsr.intenable",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            INTENABLE,
            XTENSA_OPTION_INTERRUPT,
        },
        .op_flags =
            XTENSA_OP_PRIVILEGED |
            XTENSA_OP_EXIT_TB_0 |
            XTENSA_OP_CHECK_INTERRUPTS,
    }, {
        .name = "xsr.interrupt",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "xsr.intset",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "xsr.itlbcfg",
        .translate = translate_xsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            ITLBCFG,
            XTENSA_OPTION_MMU,
            0x01130000,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.lbeg",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            LBEG,
            XTENSA_OPTION_LOOP,
        },
        .op_flags = XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "xsr.lcount",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            LCOUNT,
            XTENSA_OPTION_LOOP,
        },
    }, {
        .name = "xsr.lend",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            LEND,
            XTENSA_OPTION_LOOP,
        },
        .op_flags = XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "xsr.litbase",
        .translate = translate_xsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            LITBASE,
            XTENSA_OPTION_EXTENDED_L32R,
            0xfffff001,
        },
        .op_flags = XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "xsr.m0",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MR,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "xsr.m1",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MR + 1,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "xsr.m2",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MR + 2,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "xsr.m3",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MR + 3,
            XTENSA_OPTION_MAC16,
        },
    }, {
        .name = "xsr.memctl",
        .translate = translate_xsr_memctl,
        .par = (const uint32_t[]){MEMCTL},
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.mecr",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MECR,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.mepc",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MEPC,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.meps",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MEPS,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.mesave",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MESAVE,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.mesr",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MESR,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.mevaddr",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MESR,
            XTENSA_OPTION_MEMORY_ECC_PARITY,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.misc0",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MISC,
            XTENSA_OPTION_MISC_SR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.misc1",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MISC + 1,
            XTENSA_OPTION_MISC_SR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.misc2",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MISC + 2,
            XTENSA_OPTION_MISC_SR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.misc3",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MISC + 3,
            XTENSA_OPTION_MISC_SR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.mpuenb",
        .translate = translate_xsr_mpuenb,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            MPUENB,
            XTENSA_OPTION_MPU,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "xsr.prefctl",
        .translate = translate_xsr,
        .par = (const uint32_t[]){PREFCTL},
    }, {
        .name = "xsr.prid",
        .op_flags = XTENSA_OP_ILL,
    }, {
        .name = "xsr.ps",
        .translate = translate_xsr_ps,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            PS,
            XTENSA_OPTION_EXCEPTION,
        },
        .op_flags =
            XTENSA_OP_PRIVILEGED |
            XTENSA_OP_EXIT_TB_M1 |
            XTENSA_OP_CHECK_INTERRUPTS,
    }, {
        .name = "xsr.ptevaddr",
        .translate = translate_xsr_mask,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            PTEVADDR,
            XTENSA_OPTION_MMU,
            0xffc00000,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.rasid",
        .translate = translate_xsr_rasid,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            RASID,
            XTENSA_OPTION_MMU,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    }, {
        .name = "xsr.sar",
        .translate = translate_xsr_sar,
        .par = (const uint32_t[]){SAR},
    }, {
        .name = "xsr.scompare1",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            SCOMPARE1,
            XTENSA_OPTION_CONDITIONAL_STORE,
        },
    }, {
        .name = "xsr.vecbase",
        .translate = translate_xsr,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            VECBASE,
            XTENSA_OPTION_RELOCATABLE_VECTOR,
        },
        .op_flags = XTENSA_OP_PRIVILEGED,
    }, {
        .name = "xsr.windowbase",
        .translate = translate_xsr_windowbase,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            WINDOW_BASE,
            XTENSA_OPTION_WINDOWED_REGISTER,
        },
        .op_flags = XTENSA_OP_PRIVILEGED |
            XTENSA_OP_EXIT_TB_M1 |
            XTENSA_OP_SYNC_REGISTER_WINDOW,
    }, {
        .name = "xsr.windowstart",
        .translate = translate_xsr_windowstart,
        .test_exceptions = test_exceptions_sr,
        .par = (const uint32_t[]){
            WINDOW_START,
            XTENSA_OPTION_WINDOWED_REGISTER,
        },
        .op_flags = XTENSA_OP_PRIVILEGED | XTENSA_OP_EXIT_TB_M1,
    },
};

const XtensaOpcodeTranslators xtensa_core_opcodes = {
    .num_opcodes = ARRAY_SIZE(core_ops),
    .opcode = core_ops,
};

static inline void get_f32_o1_i3(const OpcodeArg* arg, OpcodeArg* arg32, int o0, int i0, int i1, int i2) {
  if ((i0 >= 0 && arg[i0].num_bits == 64) || (o0 >= 0 && arg[o0].num_bits == 64)) {
    if (o0 >= 0) {
      arg32[o0].out = tcg_temp_new_i32();
    }
    if (i0 >= 0) {
      arg32[i0].in = tcg_temp_new_i32();
      tcg_gen_extrl_i64_i32(arg32[i0].in, arg[i0].in);
    }
    if (i1 >= 0) {
      arg32[i1].in = tcg_temp_new_i32();
      tcg_gen_extrl_i64_i32(arg32[i1].in, arg[i1].in);
    }
    if (i2 >= 0) {
      arg32[i2].in = tcg_temp_new_i32();
      tcg_gen_extrl_i64_i32(arg32[i2].in, arg[i2].in);
    }
  } else {
    if (o0 >= 0) {
      arg32[o0].out = arg[o0].out;
    }
    if (i0 >= 0) {
      arg32[i0].in = arg[i0].in;
    }
    if (i1 >= 0) {
      arg32[i1].in = arg[i1].in;
    }
    if (i2 >= 0) {
      arg32[i2].in = arg[i2].in;
    }
  }
}

static inline void put_f32_o1_i3(const OpcodeArg* arg, const OpcodeArg* arg32, int o0, int i0, int i1, int i2) {
  if ((i0 >= 0 && arg[i0].num_bits == 64) || (o0 >= 0 && arg[o0].num_bits == 64)) {
    if (o0 >= 0) {
      tcg_gen_extu_i32_i64(arg[o0].out, arg32[o0].out);
    }
  }
}

static inline void get_f32_o1_i2(const OpcodeArg* arg, OpcodeArg* arg32, int o0, int i0, int i1) {
  get_f32_o1_i3(arg, arg32, o0, i0, i1, -1);
}

static inline void put_f32_o1_i2(const OpcodeArg* arg, const OpcodeArg* arg32, int o0, int i0, int i1) {
  put_f32_o1_i3(arg, arg32, o0, i0, i1, -1);
}

static inline void get_f32_o1_i1(const OpcodeArg* arg, OpcodeArg* arg32, int o0, int i0) {
  get_f32_o1_i2(arg, arg32, o0, i0, -1);
}

static inline void put_f32_o1_i1(const OpcodeArg* arg, const OpcodeArg* arg32, int o0, int i0) {
  put_f32_o1_i2(arg, arg32, o0, i0, -1);
}

inline void get_f32_o1(const OpcodeArg* arg, OpcodeArg* arg32, int o0) { get_f32_o1_i1(arg, arg32, o0, -1); }

inline void put_f32_o1(const OpcodeArg* arg, const OpcodeArg* arg32, int o0) { put_f32_o1_i1(arg, arg32, o0, -1); }

static inline void get_f32_i2(const OpcodeArg* arg, OpcodeArg* arg32, int i0, int i1) {
  get_f32_o1_i2(arg, arg32, -1, i0, i1);
}

static inline void put_f32_i2(const OpcodeArg* arg, const OpcodeArg* arg32, int i0, int i1) {
  put_f32_o1_i2(arg, arg32, -1, i0, i1);
}

inline void get_f32_i1(const OpcodeArg* arg, OpcodeArg* arg32, int i0) { get_f32_i2(arg, arg32, i0, -1); }

inline void put_f32_i1(const OpcodeArg* arg, const OpcodeArg* arg32, int i0) { put_f32_i2(arg, arg32, i0, -1); }

static void translate_abs_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_abs_d(arg[0].out, arg[1].in);
}

static void translate_abs_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  OpcodeArg arg32[2];

  get_f32_o1_i1(arg, arg32, 0, 1);
  gen_helper_abs_s(arg32[0].out, arg32[1].in);
  put_f32_o1_i1(arg, arg32, 0, 1);
}

static void translate_fpu2k_add_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_fpu2k_add_s(arg[0].out, tcg_env, arg[1].in, arg[2].in);
}

enum {
  COMPARE_UN,
  COMPARE_OEQ,
  COMPARE_UEQ,
  COMPARE_OLT,
  COMPARE_ULT,
  COMPARE_OLE,
  COMPARE_ULE,
};

static void translate_compare_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  static void (*const helper[])(TCGv_i32 res, TCGv_env env, TCGv_i64 s, TCGv_i64 t) = {
      [COMPARE_UN] = gen_helper_un_d,   [COMPARE_OEQ] = gen_helper_oeq_d, [COMPARE_UEQ] = gen_helper_ueq_d,
      [COMPARE_OLT] = gen_helper_olt_d, [COMPARE_ULT] = gen_helper_ult_d, [COMPARE_OLE] = gen_helper_ole_d,
      [COMPARE_ULE] = gen_helper_ule_d,
  };
  TCGv_i32 zero = tcg_constant_i32(0);
  TCGv_i32 res = tcg_temp_new_i32();
  TCGv_i32 set_br = tcg_temp_new_i32();
  TCGv_i32 clr_br = tcg_temp_new_i32();

  tcg_gen_ori_i32(set_br, arg[0].in, 1 << arg[0].imm);
  tcg_gen_andi_i32(clr_br, arg[0].in, ~(1 << arg[0].imm));

  helper[par[0]](res, tcg_env, arg[1].in, arg[2].in);
  tcg_gen_movcond_i32(TCG_COND_NE, arg[0].out, res, zero, set_br, clr_br);
}

static void translate_compare_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  static void (*const helper[])(TCGv_i32 res, TCGv_env env, TCGv_i32 s, TCGv_i32 t) = {
      [COMPARE_UN] = gen_helper_un_s,   [COMPARE_OEQ] = gen_helper_oeq_s, [COMPARE_UEQ] = gen_helper_ueq_s,
      [COMPARE_OLT] = gen_helper_olt_s, [COMPARE_ULT] = gen_helper_ult_s, [COMPARE_OLE] = gen_helper_ole_s,
      [COMPARE_ULE] = gen_helper_ule_s,
  };
  OpcodeArg arg32[3];
  TCGv_i32 zero = tcg_constant_i32(0);
  TCGv_i32 res = tcg_temp_new_i32();
  TCGv_i32 set_br = tcg_temp_new_i32();
  TCGv_i32 clr_br = tcg_temp_new_i32();

  tcg_gen_ori_i32(set_br, arg[0].in, 1 << arg[0].imm);
  tcg_gen_andi_i32(clr_br, arg[0].in, ~(1 << arg[0].imm));

  get_f32_i2(arg, arg32, 1, 2);
  helper[par[0]](res, tcg_env, arg32[1].in, arg32[2].in);
  tcg_gen_movcond_i32(TCG_COND_NE, arg[0].out, res, zero, set_br, clr_br);
  put_f32_i2(arg, arg32, 1, 2);
}

static void translate_const_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  static const uint64_t v[] = {
      UINT64_C(0x0000000000000000),
      UINT64_C(0x3ff0000000000000),
      UINT64_C(0x4000000000000000),
      UINT64_C(0x3fe0000000000000),
  };

  tcg_gen_movi_i64(arg[0].out, v[arg[1].imm % ARRAY_SIZE(v)]);
  if (arg[1].imm >= ARRAY_SIZE(v)) {
    qemu_log_mask(LOG_GUEST_ERROR, "const.d f%d, #%d, immediate value is reserved\n", arg[0].imm, arg[1].imm);
  }
}

static void translate_const_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  static const uint32_t v[] = {
      0x00000000,
      0x3f800000,
      0x40000000,
      0x3f000000,
  };

  if (arg[0].num_bits == 32) {
    tcg_gen_movi_i32(arg[0].out, v[arg[1].imm % ARRAY_SIZE(v)]);
  } else {
    tcg_gen_movi_i64(arg[0].out, v[arg[1].imm % ARRAY_SIZE(v)]);
  }
  if (arg[1].imm >= ARRAY_SIZE(v)) {
    qemu_log_mask(LOG_GUEST_ERROR, "const.s f%d, #%d, immediate value is reserved\n", arg[0].imm, arg[1].imm);
  }
}

static void translate_float_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 scale = tcg_constant_i32(-arg[2].imm);

  if (par[0]) {
    gen_helper_uitof_d(arg[0].out, tcg_env, arg[1].in, scale);
  } else {
    gen_helper_itof_d(arg[0].out, tcg_env, arg[1].in, scale);
  }
}

static void translate_float_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 scale = tcg_constant_i32(-arg[2].imm);
  OpcodeArg arg32[1];

  get_f32_o1(arg, arg32, 0);
  if (par[0]) {
    gen_helper_uitof_s(arg32[0].out, tcg_env, arg[1].in, scale);
  } else {
    gen_helper_itof_s(arg32[0].out, tcg_env, arg[1].in, scale);
  }
  put_f32_o1(arg, arg32, 0);
}

static void translate_ftoi_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 rounding_mode = tcg_constant_i32(par[0]);
  TCGv_i32 scale = tcg_constant_i32(arg[2].imm);

  if (par[1]) {
    gen_helper_ftoui_d(arg[0].out, tcg_env, arg[1].in, rounding_mode, scale);
  } else {
    gen_helper_ftoi_d(arg[0].out, tcg_env, arg[1].in, rounding_mode, scale);
  }
}

static void translate_ftoi_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 rounding_mode = tcg_constant_i32(par[0]);
  TCGv_i32 scale = tcg_constant_i32(arg[2].imm);
  OpcodeArg arg32[2];

  get_f32_i1(arg, arg32, 1);
  if (par[1]) {
    gen_helper_ftoui_s(arg[0].out, tcg_env, arg32[1].in, rounding_mode, scale);
  } else {
    gen_helper_ftoi_s(arg[0].out, tcg_env, arg32[1].in, rounding_mode, scale);
  }
  put_f32_i1(arg, arg32, 1);
}

static void translate_ldsti(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 addr = tcg_temp_new_i32();
  MemOp mop;

  tcg_gen_addi_i32(addr, arg[1].in, arg[2].imm);
  mop = gen_load_store_alignment(dc, MO_TEUL, addr);
  if (par[0]) {
    tcg_gen_qemu_st_tl(arg[0].in, addr, dc->cring, mop);
  } else {
    tcg_gen_qemu_ld_tl(arg[0].out, addr, dc->cring, mop);
  }
  if (par[1]) {
    tcg_gen_mov_i32(arg[1].out, addr);
  }
}

static void translate_ldstx(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 addr = tcg_temp_new_i32();
  MemOp mop;

  tcg_gen_add_i32(addr, arg[1].in, arg[2].in);
  mop = gen_load_store_alignment(dc, MO_TEUL, addr);
  if (par[0]) {
    tcg_gen_qemu_st_tl(arg[0].in, addr, dc->cring, mop);
  } else {
    tcg_gen_qemu_ld_tl(arg[0].out, addr, dc->cring, mop);
  }
  if (par[1]) {
    tcg_gen_mov_i32(arg[1].out, addr);
  }
}

static void translate_fpu2k_madd_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_fpu2k_madd_s(arg[0].out, tcg_env, arg[0].in, arg[1].in, arg[2].in);
}

static void translate_mov_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_mov_i64(arg[0].out, arg[1].in);
}

static void translate_mov_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (arg[0].num_bits == 32) {
    tcg_gen_mov_i32(arg[0].out, arg[1].in);
  } else {
    tcg_gen_mov_i64(arg[0].out, arg[1].in);
  }
}

static void translate_movcond_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i64 zero = tcg_constant_i64(0);
  TCGv_i64 arg2 = tcg_temp_new_i64();

  tcg_gen_ext_i32_i64(arg2, arg[2].in);
  tcg_gen_movcond_i64(par[0], arg[0].out, arg2, zero, arg[1].in, arg[0].in);
}

static void translate_movcond_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (arg[0].num_bits == 32) {
    TCGv_i32 zero = tcg_constant_i32(0);

    tcg_gen_movcond_i32(par[0], arg[0].out, arg[2].in, zero, arg[1].in, arg[0].in);
  } else {
    translate_movcond_d(dc, arg, par);
  }
}

static void translate_movp_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i64 zero = tcg_constant_i64(0);
  TCGv_i32 tmp1 = tcg_temp_new_i32();
  TCGv_i64 tmp2 = tcg_temp_new_i64();

  tcg_gen_andi_i32(tmp1, arg[2].in, 1 << arg[2].imm);
  tcg_gen_extu_i32_i64(tmp2, tmp1);
  tcg_gen_movcond_i64(par[0], arg[0].out, tmp2, zero, arg[1].in, arg[0].in);
}

static void translate_movp_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (arg[0].num_bits == 32) {
    TCGv_i32 zero = tcg_constant_i32(0);
    TCGv_i32 tmp = tcg_temp_new_i32();

    tcg_gen_andi_i32(tmp, arg[2].in, 1 << arg[2].imm);
    tcg_gen_movcond_i32(par[0], arg[0].out, tmp, zero, arg[1].in, arg[0].in);
  } else {
    translate_movp_d(dc, arg, par);
  }
}

static void translate_fpu2k_mul_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_fpu2k_mul_s(arg[0].out, tcg_env, arg[1].in, arg[2].in);
}

static void translate_fpu2k_msub_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_fpu2k_msub_s(arg[0].out, tcg_env, arg[0].in, arg[1].in, arg[2].in);
}

static void translate_neg_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_neg_d(arg[0].out, arg[1].in);
}

static void translate_neg_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  OpcodeArg arg32[2];

  get_f32_o1_i1(arg, arg32, 0, 1);
  gen_helper_neg_s(arg32[0].out, arg32[1].in);
  put_f32_o1_i1(arg, arg32, 0, 1);
}

static void translate_rfr_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_extrh_i64_i32(arg[0].out, arg[1].in);
}

static void translate_rfr_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (arg[1].num_bits == 32) {
    tcg_gen_mov_i32(arg[0].out, arg[1].in);
  } else {
    tcg_gen_extrl_i64_i32(arg[0].out, arg[1].in);
  }
}

static void translate_fpu2k_sub_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_fpu2k_sub_s(arg[0].out, tcg_env, arg[1].in, arg[2].in);
}

static void translate_wfr_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_concat_i32_i64(arg[0].out, arg[2].in, arg[1].in);
}

static void translate_wfr_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (arg[0].num_bits == 32) {
    tcg_gen_mov_i32(arg[0].out, arg[1].in);
  } else {
    tcg_gen_ext_i32_i64(arg[0].out, arg[1].in);
  }
}

static void translate_wur_fpu2k_fcr(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_wur_fpu2k_fcr(tcg_env, arg[0].in);
}

static void translate_wur_fpu2k_fsr(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  tcg_gen_andi_i32(cpu_UR[par[0]], arg[0].in, 0xffffff80);
}

static const XtensaOpcodeOps fpu2000_ops[] = {
    {
        .name = "abs.s",
        .translate = translate_abs_s,
        .coprocessor = 0x1,
    },
    {
        .name = "add.s",
        .translate = translate_fpu2k_add_s,
        .coprocessor = 0x1,
    },
    {
        .name = "ceil.s",
        .translate = translate_ftoi_s,
        .par = (const uint32_t[]){float_round_up, false},
        .coprocessor = 0x1,
    },
    {
        .name = "float.s",
        .translate = translate_float_s,
        .par = (const uint32_t[]){false},
        .coprocessor = 0x1,
    },
    {
        .name = "floor.s",
        .translate = translate_ftoi_s,
        .par = (const uint32_t[]){float_round_down, false},
        .coprocessor = 0x1,
    },
    {
        .name = "lsi",
        .translate = translate_ldsti,
        .par = (const uint32_t[]){false, false},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "lsiu",
        .translate = translate_ldsti,
        .par = (const uint32_t[]){false, true},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "lsx",
        .translate = translate_ldstx,
        .par = (const uint32_t[]){false, false},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "lsxu",
        .translate = translate_ldstx,
        .par = (const uint32_t[]){false, true},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "madd.s",
        .translate = translate_fpu2k_madd_s,
        .coprocessor = 0x1,
    },
    {
        .name = "mov.s",
        .translate = translate_mov_s,
        .coprocessor = 0x1,
    },
    {
        .name = "moveqz.s",
        .translate = translate_movcond_s,
        .par = (const uint32_t[]){TCG_COND_EQ},
        .coprocessor = 0x1,
    },
    {
        .name = "movf.s",
        .translate = translate_movp_s,
        .par = (const uint32_t[]){TCG_COND_EQ},
        .coprocessor = 0x1,
    },
    {
        .name = "movgez.s",
        .translate = translate_movcond_s,
        .par = (const uint32_t[]){TCG_COND_GE},
        .coprocessor = 0x1,
    },
    {
        .name = "movltz.s",
        .translate = translate_movcond_s,
        .par = (const uint32_t[]){TCG_COND_LT},
        .coprocessor = 0x1,
    },
    {
        .name = "movnez.s",
        .translate = translate_movcond_s,
        .par = (const uint32_t[]){TCG_COND_NE},
        .coprocessor = 0x1,
    },
    {
        .name = "movt.s",
        .translate = translate_movp_s,
        .par = (const uint32_t[]){TCG_COND_NE},
        .coprocessor = 0x1,
    },
    {
        .name = "msub.s",
        .translate = translate_fpu2k_msub_s,
        .coprocessor = 0x1,
    },
    {
        .name = "mul.s",
        .translate = translate_fpu2k_mul_s,
        .coprocessor = 0x1,
    },
    {
        .name = "neg.s",
        .translate = translate_neg_s,
        .coprocessor = 0x1,
    },
    {
        .name = "oeq.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_OEQ},
        .coprocessor = 0x1,
    },
    {
        .name = "ole.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_OLE},
        .coprocessor = 0x1,
    },
    {
        .name = "olt.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_OLT},
        .coprocessor = 0x1,
    },
    {
        .name = "rfr",
        .translate = translate_rfr_s,
        .coprocessor = 0x1,
    },
    {
        .name = "round.s",
        .translate = translate_ftoi_s,
        .par = (const uint32_t[]){float_round_nearest_even, false},
        .coprocessor = 0x1,
    },
    {
        .name = "rur.fcr",
        .translate = translate_rur,
        .par = (const uint32_t[]){FCR},
        .coprocessor = 0x1,
    },
    {
        .name = "rur.fsr",
        .translate = translate_rur,
        .par = (const uint32_t[]){FSR},
        .coprocessor = 0x1,
    },
    {
        .name = "ssi",
        .translate = translate_ldsti,
        .par = (const uint32_t[]){true, false},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "ssiu",
        .translate = translate_ldsti,
        .par = (const uint32_t[]){true, true},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "ssx",
        .translate = translate_ldstx,
        .par = (const uint32_t[]){true, false},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "ssxu",
        .translate = translate_ldstx,
        .par = (const uint32_t[]){true, true},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "sub.s",
        .translate = translate_fpu2k_sub_s,
        .coprocessor = 0x1,
    },
    {
        .name = "trunc.s",
        .translate = translate_ftoi_s,
        .par = (const uint32_t[]){float_round_to_zero, false},
        .coprocessor = 0x1,
    },
    {
        .name = "ueq.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_UEQ},
        .coprocessor = 0x1,
    },
    {
        .name = "ufloat.s",
        .translate = translate_float_s,
        .par = (const uint32_t[]){true},
        .coprocessor = 0x1,
    },
    {
        .name = "ule.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_ULE},
        .coprocessor = 0x1,
    },
    {
        .name = "ult.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_ULT},
        .coprocessor = 0x1,
    },
    {
        .name = "un.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_UN},
        .coprocessor = 0x1,
    },
    {
        .name = "utrunc.s",
        .translate = translate_ftoi_s,
        .par = (const uint32_t[]){float_round_to_zero, true},
        .coprocessor = 0x1,
    },
    {
        .name = "wfr",
        .translate = translate_wfr_s,
        .coprocessor = 0x1,
    },
    {
        .name = "wur.fcr",
        .translate = translate_wur_fpu2k_fcr,
        .par = (const uint32_t[]){FCR},
        .coprocessor = 0x1,
    },
    {
        .name = "wur.fsr",
        .translate = translate_wur_fpu2k_fsr,
        .par = (const uint32_t[]){FSR},
        .coprocessor = 0x1,
    },
};

const XtensaOpcodeTranslators xtensa_fpu2000_opcodes = {
    .num_opcodes = ARRAY_SIZE(fpu2000_ops),
    .opcode = fpu2000_ops,
};

static void translate_add_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_add_d(arg[0].out, tcg_env, arg[1].in, arg[2].in);
}

static void translate_add_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (option_enabled(dc, XTENSA_OPTION_DFPU_SINGLE_ONLY)) {
    gen_helper_fpu2k_add_s(arg[0].out, tcg_env, arg[1].in, arg[2].in);
  } else {
    OpcodeArg arg32[3];

    get_f32_o1_i2(arg, arg32, 0, 1, 2);
    gen_helper_add_s(arg32[0].out, tcg_env, arg32[1].in, arg32[2].in);
    put_f32_o1_i2(arg, arg32, 0, 1, 2);
  }
}

static void translate_cvtd_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 v = tcg_temp_new_i32();

  tcg_gen_extrl_i64_i32(v, arg[1].in);
  gen_helper_cvtd_s(arg[0].out, tcg_env, v);
}

static void translate_cvts_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 v = tcg_temp_new_i32();

  gen_helper_cvts_d(v, tcg_env, arg[1].in);
  tcg_gen_extu_i32_i64(arg[0].out, v);
}

static void translate_ldsti_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 addr;
  MemOp mop;

  if (par[1]) {
    addr = tcg_temp_new_i32();
    tcg_gen_addi_i32(addr, arg[1].in, arg[2].imm);
  } else {
    addr = arg[1].in;
  }
  mop = gen_load_store_alignment(dc, MO_TEUQ, addr);
  if (par[0]) {
    tcg_gen_qemu_st_i64(arg[0].in, addr, dc->cring, mop);
  } else {
    tcg_gen_qemu_ld_i64(arg[0].out, addr, dc->cring, mop);
  }
  if (par[2]) {
    if (par[1]) {
      tcg_gen_mov_i32(arg[1].out, addr);
    } else {
      tcg_gen_addi_i32(arg[1].out, arg[1].in, arg[2].imm);
    }
  }
}

static void translate_ldsti_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 addr;
  OpcodeArg arg32[1];
  MemOp mop;

  if (par[1]) {
    addr = tcg_temp_new_i32();
    tcg_gen_addi_i32(addr, arg[1].in, arg[2].imm);
  } else {
    addr = arg[1].in;
  }
  mop = gen_load_store_alignment(dc, MO_TEUL, addr);
  if (par[0]) {
    get_f32_i1(arg, arg32, 0);
    tcg_gen_qemu_st_tl(arg32[0].in, addr, dc->cring, mop);
    put_f32_i1(arg, arg32, 0);
  } else {
    get_f32_o1(arg, arg32, 0);
    tcg_gen_qemu_ld_tl(arg32[0].out, addr, dc->cring, mop);
    put_f32_o1(arg, arg32, 0);
  }
  if (par[2]) {
    if (par[1]) {
      tcg_gen_mov_i32(arg[1].out, addr);
    } else {
      tcg_gen_addi_i32(arg[1].out, arg[1].in, arg[2].imm);
    }
  }
}

static void translate_ldstx_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 addr;
  MemOp mop;

  if (par[1]) {
    addr = tcg_temp_new_i32();
    tcg_gen_add_i32(addr, arg[1].in, arg[2].in);
  } else {
    addr = arg[1].in;
  }
  mop = gen_load_store_alignment(dc, MO_TEUQ, addr);
  if (par[0]) {
    tcg_gen_qemu_st_i64(arg[0].in, addr, dc->cring, mop);
  } else {
    tcg_gen_qemu_ld_i64(arg[0].out, addr, dc->cring, mop);
  }
  if (par[2]) {
    if (par[1]) {
      tcg_gen_mov_i32(arg[1].out, addr);
    } else {
      tcg_gen_add_i32(arg[1].out, arg[1].in, arg[2].in);
    }
  }
}

static void translate_ldstx_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  TCGv_i32 addr;
  OpcodeArg arg32[1];
  MemOp mop;

  if (par[1]) {
    addr = tcg_temp_new_i32();
    tcg_gen_add_i32(addr, arg[1].in, arg[2].in);
  } else {
    addr = arg[1].in;
  }
  mop = gen_load_store_alignment(dc, MO_TEUL, addr);
  if (par[0]) {
    get_f32_i1(arg, arg32, 0);
    tcg_gen_qemu_st_tl(arg32[0].in, addr, dc->cring, mop);
    put_f32_i1(arg, arg32, 0);
  } else {
    get_f32_o1(arg, arg32, 0);
    tcg_gen_qemu_ld_tl(arg32[0].out, addr, dc->cring, mop);
    put_f32_o1(arg, arg32, 0);
  }
  if (par[2]) {
    if (par[1]) {
      tcg_gen_mov_i32(arg[1].out, addr);
    } else {
      tcg_gen_add_i32(arg[1].out, arg[1].in, arg[2].in);
    }
  }
}

static void translate_madd_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_madd_d(arg[0].out, tcg_env, arg[0].in, arg[1].in, arg[2].in);
}

static void translate_madd_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (option_enabled(dc, XTENSA_OPTION_DFPU_SINGLE_ONLY)) {
    gen_helper_fpu2k_madd_s(arg[0].out, tcg_env, arg[0].in, arg[1].in, arg[2].in);
  } else {
    OpcodeArg arg32[3];

    get_f32_o1_i3(arg, arg32, 0, 0, 1, 2);
    gen_helper_madd_s(arg32[0].out, tcg_env, arg32[0].in, arg32[1].in, arg32[2].in);
    put_f32_o1_i3(arg, arg32, 0, 0, 1, 2);
  }
}

static void translate_mul_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_mul_d(arg[0].out, tcg_env, arg[1].in, arg[2].in);
}

static void translate_mul_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (option_enabled(dc, XTENSA_OPTION_DFPU_SINGLE_ONLY)) {
    gen_helper_fpu2k_mul_s(arg[0].out, tcg_env, arg[1].in, arg[2].in);
  } else {
    OpcodeArg arg32[3];

    get_f32_o1_i2(arg, arg32, 0, 1, 2);
    gen_helper_mul_s(arg32[0].out, tcg_env, arg32[1].in, arg32[2].in);
    put_f32_o1_i2(arg, arg32, 0, 1, 2);
  }
}

static void translate_msub_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_msub_d(arg[0].out, tcg_env, arg[0].in, arg[1].in, arg[2].in);
}

static void translate_msub_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (option_enabled(dc, XTENSA_OPTION_DFPU_SINGLE_ONLY)) {
    gen_helper_fpu2k_msub_s(arg[0].out, tcg_env, arg[0].in, arg[1].in, arg[2].in);
  } else {
    OpcodeArg arg32[3];

    get_f32_o1_i3(arg, arg32, 0, 0, 1, 2);
    gen_helper_msub_s(arg32[0].out, tcg_env, arg32[0].in, arg32[1].in, arg32[2].in);
    put_f32_o1_i3(arg, arg32, 0, 0, 1, 2);
  }
}

static void translate_sub_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_sub_d(arg[0].out, tcg_env, arg[1].in, arg[2].in);
}

static void translate_sub_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  if (option_enabled(dc, XTENSA_OPTION_DFPU_SINGLE_ONLY)) {
    gen_helper_fpu2k_sub_s(arg[0].out, tcg_env, arg[1].in, arg[2].in);
  } else {
    OpcodeArg arg32[3];

    get_f32_o1_i2(arg, arg32, 0, 1, 2);
    gen_helper_sub_s(arg32[0].out, tcg_env, arg32[1].in, arg32[2].in);
    put_f32_o1_i2(arg, arg32, 0, 1, 2);
  }
}

static void translate_mkdadj_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_mkdadj_d(arg[0].out, tcg_env, arg[0].in, arg[1].in);
}

static void translate_mkdadj_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  OpcodeArg arg32[2];

  get_f32_o1_i2(arg, arg32, 0, 0, 1);
  gen_helper_mkdadj_s(arg32[0].out, tcg_env, arg32[0].in, arg32[1].in);
  put_f32_o1_i2(arg, arg32, 0, 0, 1);
}

static void translate_mksadj_d(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_mksadj_d(arg[0].out, tcg_env, arg[1].in);
}

static void translate_mksadj_s(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  OpcodeArg arg32[2];

  get_f32_o1_i1(arg, arg32, 0, 1);
  gen_helper_mksadj_s(arg32[0].out, tcg_env, arg32[1].in);
  put_f32_o1_i1(arg, arg32, 0, 1);
}

static void translate_wur_fpu_fcr(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_wur_fpu_fcr(tcg_env, arg[0].in);
}

static void translate_rur_fpu_fsr(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_rur_fpu_fsr(arg[0].out, tcg_env);
}

static void translate_wur_fpu_fsr(DisasContext* dc, const OpcodeArg arg[], const uint32_t par[]) {
  gen_helper_wur_fpu_fsr(tcg_env, arg[0].in);
}

static const XtensaOpcodeOps fpu_ops[] = {
    {
        .name = "abs.d",
        .translate = translate_abs_d,
        .coprocessor = 0x1,
    },
    {
        .name = "abs.s",
        .translate = translate_abs_s,
        .coprocessor = 0x1,
    },
    {
        .name = "add.d",
        .translate = translate_add_d,
        .coprocessor = 0x1,
    },
    {
        .name = "add.s",
        .translate = translate_add_s,
        .coprocessor = 0x1,
    },
    {
        .name = "addexp.d",
        .translate = translate_nop,
        .coprocessor = 0x1,
    },
    {
        .name = "addexp.s",
        .translate = translate_nop,
        .coprocessor = 0x1,
    },
    {
        .name = "addexpm.d",
        .translate = translate_mov_s,
        .coprocessor = 0x1,
    },
    {
        .name = "addexpm.s",
        .translate = translate_mov_s,
        .coprocessor = 0x1,
    },
    {
        .name = "ceil.d",
        .translate = translate_ftoi_d,
        .par = (const uint32_t[]){float_round_up, false},
        .coprocessor = 0x1,
    },
    {
        .name = "ceil.s",
        .translate = translate_ftoi_s,
        .par = (const uint32_t[]){float_round_up, false},
        .coprocessor = 0x1,
    },
    {
        .name = "const.d",
        .translate = translate_const_d,
        .coprocessor = 0x1,
    },
    {
        .name = "const.s",
        .translate = translate_const_s,
        .coprocessor = 0x1,
    },
    {
        .name = "cvtd.s",
        .translate = translate_cvtd_s,
        .coprocessor = 0x1,
    },
    {
        .name = "cvts.d",
        .translate = translate_cvts_d,
        .coprocessor = 0x1,
    },
    {
        .name = "div0.d",
        .translate = translate_nop,
        .coprocessor = 0x1,
    },
    {
        .name = "div0.s",
        .translate = translate_nop,
        .coprocessor = 0x1,
    },
    {
        .name = "divn.d",
        .translate = translate_nop,
        .coprocessor = 0x1,
    },
    {
        .name = "divn.s",
        .translate = translate_nop,
        .coprocessor = 0x1,
    },
    {
        .name = "float.d",
        .translate = translate_float_d,
        .par = (const uint32_t[]){false},
        .coprocessor = 0x1,
    },
    {
        .name = "float.s",
        .translate = translate_float_s,
        .par = (const uint32_t[]){false},
        .coprocessor = 0x1,
    },
    {
        .name = "floor.d",
        .translate = translate_ftoi_d,
        .par = (const uint32_t[]){float_round_down, false},
        .coprocessor = 0x1,
    },
    {
        .name = "floor.s",
        .translate = translate_ftoi_s,
        .par = (const uint32_t[]){float_round_down, false},
        .coprocessor = 0x1,
    },
    {
        .name = "ldi",
        .translate = translate_ldsti_d,
        .par = (const uint32_t[]){false, true, false},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "ldip",
        .translate = translate_ldsti_d,
        .par = (const uint32_t[]){false, false, true},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "ldiu",
        .translate = translate_ldsti_d,
        .par = (const uint32_t[]){false, true, true},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "ldx",
        .translate = translate_ldstx_d,
        .par = (const uint32_t[]){false, true, false},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "ldxp",
        .translate = translate_ldstx_d,
        .par = (const uint32_t[]){false, false, true},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "ldxu",
        .translate = translate_ldstx_d,
        .par = (const uint32_t[]){false, true, true},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "lsi",
        .translate = translate_ldsti_s,
        .par = (const uint32_t[]){false, true, false},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "lsip",
        .translate = translate_ldsti_s,
        .par = (const uint32_t[]){false, false, true},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "lsiu",
        .translate = translate_ldsti_s,
        .par = (const uint32_t[]){false, true, true},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "lsx",
        .translate = translate_ldstx_s,
        .par = (const uint32_t[]){false, true, false},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "lsxp",
        .translate = translate_ldstx_s,
        .par = (const uint32_t[]){false, false, true},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "lsxu",
        .translate = translate_ldstx_s,
        .par = (const uint32_t[]){false, true, true},
        .op_flags = XTENSA_OP_LOAD,
        .coprocessor = 0x1,
    },
    {
        .name = "madd.d",
        .translate = translate_madd_d,
        .coprocessor = 0x1,
    },
    {
        .name = "madd.s",
        .translate = translate_madd_s,
        .coprocessor = 0x1,
    },
    {
        .name = "maddn.d",
        .translate = translate_nop,
        .coprocessor = 0x1,
    },
    {
        .name = "maddn.s",
        .translate = translate_nop,
        .coprocessor = 0x1,
    },
    {
        .name = "mkdadj.d",
        .translate = translate_mkdadj_d,
        .coprocessor = 0x1,
    },
    {
        .name = "mkdadj.s",
        .translate = translate_mkdadj_s,
        .coprocessor = 0x1,
    },
    {
        .name = "mksadj.d",
        .translate = translate_mksadj_d,
        .coprocessor = 0x1,
    },
    {
        .name = "mksadj.s",
        .translate = translate_mksadj_s,
        .coprocessor = 0x1,
    },
    {
        .name = "mov.d",
        .translate = translate_mov_d,
        .coprocessor = 0x1,
    },
    {
        .name = "mov.s",
        .translate = translate_mov_s,
        .coprocessor = 0x1,
    },
    {
        .name = "moveqz.d",
        .translate = translate_movcond_d,
        .par = (const uint32_t[]){TCG_COND_EQ},
        .coprocessor = 0x1,
    },
    {
        .name = "moveqz.s",
        .translate = translate_movcond_s,
        .par = (const uint32_t[]){TCG_COND_EQ},
        .coprocessor = 0x1,
    },
    {
        .name = "movf.d",
        .translate = translate_movp_d,
        .par = (const uint32_t[]){TCG_COND_EQ},
        .coprocessor = 0x1,
    },
    {
        .name = "movf.s",
        .translate = translate_movp_s,
        .par = (const uint32_t[]){TCG_COND_EQ},
        .coprocessor = 0x1,
    },
    {
        .name = "movgez.d",
        .translate = translate_movcond_d,
        .par = (const uint32_t[]){TCG_COND_GE},
        .coprocessor = 0x1,
    },
    {
        .name = "movgez.s",
        .translate = translate_movcond_s,
        .par = (const uint32_t[]){TCG_COND_GE},
        .coprocessor = 0x1,
    },
    {
        .name = "movltz.d",
        .translate = translate_movcond_d,
        .par = (const uint32_t[]){TCG_COND_LT},
        .coprocessor = 0x1,
    },
    {
        .name = "movltz.s",
        .translate = translate_movcond_s,
        .par = (const uint32_t[]){TCG_COND_LT},
        .coprocessor = 0x1,
    },
    {
        .name = "movnez.d",
        .translate = translate_movcond_d,
        .par = (const uint32_t[]){TCG_COND_NE},
        .coprocessor = 0x1,
    },
    {
        .name = "movnez.s",
        .translate = translate_movcond_s,
        .par = (const uint32_t[]){TCG_COND_NE},
        .coprocessor = 0x1,
    },
    {
        .name = "movt.d",
        .translate = translate_movp_d,
        .par = (const uint32_t[]){TCG_COND_NE},
        .coprocessor = 0x1,
    },
    {
        .name = "movt.s",
        .translate = translate_movp_s,
        .par = (const uint32_t[]){TCG_COND_NE},
        .coprocessor = 0x1,
    },
    {
        .name = "msub.d",
        .translate = translate_msub_d,
        .coprocessor = 0x1,
    },
    {
        .name = "msub.s",
        .translate = translate_msub_s,
        .coprocessor = 0x1,
    },
    {
        .name = "mul.d",
        .translate = translate_mul_d,
        .coprocessor = 0x1,
    },
    {
        .name = "mul.s",
        .translate = translate_mul_s,
        .coprocessor = 0x1,
    },
    {
        .name = "neg.d",
        .translate = translate_neg_d,
        .coprocessor = 0x1,
    },
    {
        .name = "neg.s",
        .translate = translate_neg_s,
        .coprocessor = 0x1,
    },
    {
        .name = "nexp01.d",
        .translate = translate_nop,
        .coprocessor = 0x1,
    },
    {
        .name = "nexp01.s",
        .translate = translate_nop,
        .coprocessor = 0x1,
    },
    {
        .name = "oeq.d",
        .translate = translate_compare_d,
        .par = (const uint32_t[]){COMPARE_OEQ},
        .coprocessor = 0x1,
    },
    {
        .name = "oeq.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_OEQ},
        .coprocessor = 0x1,
    },
    {
        .name = "ole.d",
        .translate = translate_compare_d,
        .par = (const uint32_t[]){COMPARE_OLE},
        .coprocessor = 0x1,
    },
    {
        .name = "ole.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_OLE},
        .coprocessor = 0x1,
    },
    {
        .name = "olt.d",
        .translate = translate_compare_d,
        .par = (const uint32_t[]){COMPARE_OLT},
        .coprocessor = 0x1,
    },
    {
        .name = "olt.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_OLT},
        .coprocessor = 0x1,
    },
    {
        .name = "rfr",
        .translate = translate_rfr_s,
        .coprocessor = 0x1,
    },
    {
        .name = "rfrd",
        .translate = translate_rfr_d,
        .coprocessor = 0x1,
    },
    {
        .name = "round.d",
        .translate = translate_ftoi_d,
        .par = (const uint32_t[]){float_round_nearest_even, false},
        .coprocessor = 0x1,
    },
    {
        .name = "round.s",
        .translate = translate_ftoi_s,
        .par = (const uint32_t[]){float_round_nearest_even, false},
        .coprocessor = 0x1,
    },
    {
        .name = "rur.fcr",
        .translate = translate_rur,
        .par = (const uint32_t[]){FCR},
        .coprocessor = 0x1,
    },
    {
        .name = "rur.fsr",
        .translate = translate_rur_fpu_fsr,
        .coprocessor = 0x1,
    },
    {
        .name = "sdi",
        .translate = translate_ldsti_d,
        .par = (const uint32_t[]){true, true, false},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "sdip",
        .translate = translate_ldsti_d,
        .par = (const uint32_t[]){true, false, true},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "sdiu",
        .translate = translate_ldsti_d,
        .par = (const uint32_t[]){true, true, true},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "sdx",
        .translate = translate_ldstx_d,
        .par = (const uint32_t[]){true, true, false},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "sdxp",
        .translate = translate_ldstx_d,
        .par = (const uint32_t[]){true, false, true},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "sdxu",
        .translate = translate_ldstx_d,
        .par = (const uint32_t[]){true, true, true},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "sqrt0.d",
        .translate = translate_nop,
        .coprocessor = 0x1,
    },
    {
        .name = "sqrt0.s",
        .translate = translate_nop,
        .coprocessor = 0x1,
    },
    {
        .name = "ssi",
        .translate = translate_ldsti_s,
        .par = (const uint32_t[]){true, true, false},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "ssip",
        .translate = translate_ldsti_s,
        .par = (const uint32_t[]){true, false, true},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "ssiu",
        .translate = translate_ldsti_s,
        .par = (const uint32_t[]){true, true, true},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "ssx",
        .translate = translate_ldstx_s,
        .par = (const uint32_t[]){true, true, false},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "ssxp",
        .translate = translate_ldstx_s,
        .par = (const uint32_t[]){true, false, true},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "ssxu",
        .translate = translate_ldstx_s,
        .par = (const uint32_t[]){true, true, true},
        .op_flags = XTENSA_OP_STORE,
        .coprocessor = 0x1,
    },
    {
        .name = "sub.d",
        .translate = translate_sub_d,
        .coprocessor = 0x1,
    },
    {
        .name = "sub.s",
        .translate = translate_sub_s,
        .coprocessor = 0x1,
    },
    {
        .name = "trunc.d",
        .translate = translate_ftoi_d,
        .par = (const uint32_t[]){float_round_to_zero, false},
        .coprocessor = 0x1,
    },
    {
        .name = "trunc.s",
        .translate = translate_ftoi_s,
        .par = (const uint32_t[]){float_round_to_zero, false},
        .coprocessor = 0x1,
    },
    {
        .name = "ueq.d",
        .translate = translate_compare_d,
        .par = (const uint32_t[]){COMPARE_UEQ},
        .coprocessor = 0x1,
    },
    {
        .name = "ueq.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_UEQ},
        .coprocessor = 0x1,
    },
    {
        .name = "ufloat.d",
        .translate = translate_float_d,
        .par = (const uint32_t[]){true},
        .coprocessor = 0x1,
    },
    {
        .name = "ufloat.s",
        .translate = translate_float_s,
        .par = (const uint32_t[]){true},
        .coprocessor = 0x1,
    },
    {
        .name = "ule.d",
        .translate = translate_compare_d,
        .par = (const uint32_t[]){COMPARE_ULE},
        .coprocessor = 0x1,
    },
    {
        .name = "ule.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_ULE},
        .coprocessor = 0x1,
    },
    {
        .name = "ult.d",
        .translate = translate_compare_d,
        .par = (const uint32_t[]){COMPARE_ULT},
        .coprocessor = 0x1,
    },
    {
        .name = "ult.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_ULT},
        .coprocessor = 0x1,
    },
    {
        .name = "un.d",
        .translate = translate_compare_d,
        .par = (const uint32_t[]){COMPARE_UN},
        .coprocessor = 0x1,
    },
    {
        .name = "un.s",
        .translate = translate_compare_s,
        .par = (const uint32_t[]){COMPARE_UN},
        .coprocessor = 0x1,
    },
    {
        .name = "utrunc.d",
        .translate = translate_ftoi_d,
        .par = (const uint32_t[]){float_round_to_zero, true},
        .coprocessor = 0x1,
    },
    {
        .name = "utrunc.s",
        .translate = translate_ftoi_s,
        .par = (const uint32_t[]){float_round_to_zero, true},
        .coprocessor = 0x1,
    },
    {
        .name = "wfr",
        .translate = translate_wfr_s,
        .coprocessor = 0x1,
    },
    {
        .name = "wfrd",
        .translate = translate_wfr_d,
        .coprocessor = 0x1,
    },
    {
        .name = "wur.fcr",
        .translate = translate_wur_fpu_fcr,
        .par = (const uint32_t[]){FCR},
        .coprocessor = 0x1,
    },
    {
        .name = "wur.fsr",
        .translate = translate_wur_fpu_fsr,
        .coprocessor = 0x1,
    },
};

const XtensaOpcodeTranslators xtensa_fpu_opcodes = {
    .num_opcodes = ARRAY_SIZE(fpu_ops),
    .opcode = fpu_ops,
};
