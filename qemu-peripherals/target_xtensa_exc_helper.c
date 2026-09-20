/*
 * Copyright (c) 2011 - 2019, Max Filippov, Open Source and Linux Lab.
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
#include "qemu/log.h"
#include "qemu/main-loop.h"
#ifndef CONFIG_USER_ONLY
#include "sysemu/runstate.h"
#endif

/* Dynamic simulator addresses — resolved from firmware ELF at load time. */
#include "exec/address-spaces.h"
#include "exec/cpu_ldst.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "gdbstub/helpers.h"
#include "hw/i2c/esp32_i2c.h"
#include "hw/irq.h"
#include "hw/ssi/lilygo_sx1262.h"
#include "hw/xtensa/mofei-sim-addrs.h"
#include "qemu/atomic.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/qemu-print.h"
#include "qemu/timer.h"

static struct XtensaConfigList* xtensa_cores;
static uint32_t mofei_murphy_last_framebuffer_object;
static uint32_t mofei_murphy_last_framebuffer_data;

static bool mofei_guest_addr_in_sim_ram(uint32_t guest_addr);
static bool mofei_guest_range_in_sim_ram(uint32_t guest_addr, uint32_t len);
bool mofei_read_guest_memory(uint32_t guest_addr, uint8_t* dest, uint32_t len);
void HELPER(mofei_inject_framebuffer)(CPUXtensaState* env);
static void mofei_write_guest_u32(CPUXtensaState* env, uint32_t addr, uint32_t value);
static uint32_t mofei_read_mmio_u32(uint32_t addr);
static void mofei_write_mmio_u32(uint32_t addr, uint32_t value);

bool esp32s3_gpspi2_radio_selected(void);
int esp32s3_gpspi2_direct_radio_transfer(const uint8_t* tx, uint8_t* rx, size_t len);

#define MOFEI_SDMMC_BASE 0x60028000u
#define MOFEI_SDMMC_CMD_ADDR (MOFEI_SDMMC_BASE + 0x2cu)
#define MOFEI_SDMMC_RINTSTS_ADDR (MOFEI_SDMMC_BASE + 0x44u)
#define MOFEI_SDMMC_IDSTS_ADDR (MOFEI_SDMMC_BASE + 0x8cu)
#define MOFEI_SDMMC_INTMASK_CMD_DONE BIT(2)
#define MOFEI_SDMMC_INTMASK_DATA_OVER BIT(3)
#define MOFEI_SDMMC_SD_EVENT_MASK 0x0000f3ffu
#define MOFEI_SDMMC_DMA_EVENT_MASK 0x0000001fu
#define MOFEI_SDMMC_DMA_DONE_MASK 0x00000003u
#define MOFEI_SDMMC_SHADOW_BYTES 0x200u

#define MOFEI_ESP32S3_GPIO_BASE 0x60004000u
#define MOFEI_ESP32S3_GPIO_OUT_W1TS (MOFEI_ESP32S3_GPIO_BASE + 0x08u)
#define MOFEI_ESP32S3_GPIO_OUT_W1TC (MOFEI_ESP32S3_GPIO_BASE + 0x0cu)
#define MOFEI_ESP32S3_GPIO_OUT1_W1TS (MOFEI_ESP32S3_GPIO_BASE + 0x14u)
#define MOFEI_ESP32S3_GPIO_OUT1_W1TC (MOFEI_ESP32S3_GPIO_BASE + 0x18u)

static uint8_t mofei_sdmmc_shadow[MOFEI_SDMMC_SHADOW_BYTES];

static bool mofei_sdmmc_shadow_range(uint32_t addr, uint32_t len) {
  (void)addr;
  (void)len;
  /* SDMMC 已由机器模型中的 DWC 控制器承载。ESP-IDF 通过被加速的
   * memcpy 写寄存器时也必须落到真实 MMIO，否则命令只进入影子数组，
   * SD 卡永远收不到 CMD0/CMD8，初始化会得到空响应。 */
  return false;
}

static uint32_t mofei_load_le32(const uint8_t* bytes) {
  return ((uint32_t)bytes[0]) | ((uint32_t)bytes[1] << 8u) | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static void mofei_store_le32(uint8_t* bytes, uint32_t value) {
  bytes[0] = (uint8_t)(value & 0xffu);
  bytes[1] = (uint8_t)((value >> 8u) & 0xffu);
  bytes[2] = (uint8_t)((value >> 16u) & 0xffu);
  bytes[3] = (uint8_t)((value >> 24u) & 0xffu);
}

static bool mofei_sdmmc_shadow_read(uint32_t addr, uint8_t* dest, uint32_t len) {
  if (!dest || len == 0 || !mofei_sdmmc_shadow_range(addr, len)) {
    return false;
  }
  memcpy(dest, &mofei_sdmmc_shadow[addr - MOFEI_SDMMC_BASE], len);
  return true;
}

static bool mofei_sdmmc_shadow_write(uint32_t addr, const uint8_t* src, uint32_t len) {
  if (!src || len == 0 || !mofei_sdmmc_shadow_range(addr, len)) {
    return false;
  }

  if (len == sizeof(uint32_t) && (addr == MOFEI_SDMMC_RINTSTS_ADDR || addr == MOFEI_SDMMC_IDSTS_ADDR)) {
    const uint32_t value = mofei_load_le32(src);
    uint8_t* reg = &mofei_sdmmc_shadow[addr - MOFEI_SDMMC_BASE];
    mofei_store_le32(reg, mofei_load_le32(reg) & ~value);
    return true;
  }

  memcpy(&mofei_sdmmc_shadow[addr - MOFEI_SDMMC_BASE], src, len);
  if (len == sizeof(uint32_t) && addr == MOFEI_SDMMC_CMD_ADDR) {
    uint8_t* rintsts = &mofei_sdmmc_shadow[MOFEI_SDMMC_RINTSTS_ADDR - MOFEI_SDMMC_BASE];
    uint8_t* idsts = &mofei_sdmmc_shadow[MOFEI_SDMMC_IDSTS_ADDR - MOFEI_SDMMC_BASE];
    mofei_store_le32(rintsts, mofei_load_le32(rintsts) | MOFEI_SDMMC_INTMASK_CMD_DONE | MOFEI_SDMMC_INTMASK_DATA_OVER);
    mofei_store_le32(idsts, mofei_load_le32(idsts) | MOFEI_SDMMC_DMA_DONE_MASK);
  }
  return true;
}

static uint32_t mofei_sdmmc_shadow_read_u32(uint32_t addr) {
  uint8_t bytes[4] = {0};
  mofei_sdmmc_shadow_read(addr, bytes, sizeof(bytes));
  return mofei_load_le32(bytes);
}

static void mofei_sdmmc_shadow_write_u32(uint32_t addr, uint32_t value) {
  uint8_t bytes[4] = {0};
  mofei_store_le32(bytes, value);
  mofei_sdmmc_shadow_write(addr, bytes, sizeof(bytes));
}

static bool mofei_murphy_context_pointer_plausible(uint32_t addr) {
  return (addr >= 0x3c800000u && addr < 0x3d000000u) || (addr >= 0x3fc80000u && addr < 0x3fda0000u);
}

static uint32_t mofei_read_guest_u32(uint32_t guest_addr) {
  uint8_t bytes[4] = {0};
  if (guest_addr != 0 && mofei_read_guest_memory(guest_addr, bytes, sizeof(bytes))) {
    return ((uint32_t)bytes[0]) | ((uint32_t)bytes[1] << 8u) | ((uint32_t)bytes[2] << 16u) |
           ((uint32_t)bytes[3] << 24u);
  }
  return 0;
}

static bool mofei_read_guest_data_u32_checked(CPUXtensaState* env, uint32_t guest_addr, uint32_t* value) {
  if (!value || guest_addr == 0) {
    return false;
  }
  *value = 0;
  if (mofei_guest_range_in_sim_ram(guest_addr, sizeof(*value))) {
    for (uint32_t i = 0; i < sizeof(*value); ++i) {
      *value |= ((uint32_t)cpu_ldub_data(env, guest_addr + i)) << (i * 8u);
    }
    return true;
  }
  uint8_t bytes[4] = {0};
  if (!mofei_read_guest_memory(guest_addr, bytes, sizeof(bytes))) {
    return false;
  }
  *value =
      ((uint32_t)bytes[0]) | ((uint32_t)bytes[1] << 8u) | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
  return true;
}

static uint32_t mofei_read_guest_data_u32(CPUXtensaState* env, uint32_t guest_addr) {
  uint32_t value = 0;
  mofei_read_guest_data_u32_checked(env, guest_addr, &value);
  return value;
}

static bool mofei_write_guest_data_u32(CPUXtensaState* env, uint32_t guest_addr, uint32_t value) {
  if (guest_addr == 0) {
    return false;
  }
  if (mofei_guest_range_in_sim_ram(guest_addr, sizeof(value))) {
    for (uint32_t i = 0; i < sizeof(value); ++i) {
      cpu_stb_data(env, guest_addr + i, (uint8_t)(value >> (i * 8u)));
    }
    return true;
  }
  const uint8_t bytes[4] = {
      (uint8_t)(value & 0xffu),
      (uint8_t)((value >> 8u) & 0xffu),
      (uint8_t)((value >> 16u) & 0xffu),
      (uint8_t)((value >> 24u) & 0xffu),
  };
  if (mofei_sdmmc_shadow_write(guest_addr, bytes, sizeof(bytes))) {
    return true;
  }
  return address_space_write(&address_space_memory, guest_addr, MEMTXATTRS_UNSPECIFIED, bytes, sizeof(bytes)) ==
         MEMTX_OK;
}

static void mofei_repair_murphy_input_chain(CPUXtensaState* env, uint32_t runner, uint32_t input, uint32_t window,
                                            uint32_t now_ms) {
  if (runner == 0 || input == 0 || window == 0 || now_ms == 0) {
    return;
  }

  const uint32_t input_esp = mofei_sim_addrs.murphyDeviceInput_addr;
  const uint32_t mapped_input = mofei_sim_addrs.murphyDeviceMappedInput_addr;
  const uint32_t sleep_input = mofei_sim_addrs.murphyDeviceSleepInput_addr;
  const uint32_t remote_input = mofei_sim_addrs.murphyDeviceRemoteInput_addr;
  const uint32_t debug_input = mofei_sim_addrs.murphyDeviceDebugInput_addr;
  const uint32_t input_esp_vptr = mofei_sim_addrs.murphyInputEspVtable_addr + 8u;
  const uint32_t mapped_input_vptr = mofei_sim_addrs.murphyPortraitInputVtable_addr + 8u;
  const uint32_t sleep_input_vptr = mofei_sim_addrs.murphySleepHoldInputVtable_addr + 8u;
  const uint32_t remote_input_vptr = mofei_sim_addrs.murphyRemoteInputVtable_addr + 8u;
  const uint32_t synthetic_input_vptr = mofei_sim_addrs.murphySyntheticInputVtable_addr + 8u;

  if (input_esp != 0 && mofei_sim_addrs.murphyInputEspVtable_addr != 0) {
    cpu_stl_data(env, input_esp + 0u, input_esp_vptr);
  }
  if (mapped_input != 0 && mofei_sim_addrs.murphyPortraitInputVtable_addr != 0) {
    cpu_stl_data(env, mapped_input + 0u, mapped_input_vptr);
    cpu_stl_data(env, mapped_input + 4u, input_esp);
    cpu_stl_data(env, mapped_input + 8u, 800u);
    cpu_stl_data(env, mapped_input + 12u, 480u);
    cpu_stl_data(env, mapped_input + 16u, 800u);
    cpu_stl_data(env, mapped_input + 20u, 480u);
  }
  if (sleep_input != 0 && mofei_sim_addrs.murphySleepHoldInputVtable_addr != 0) {
    cpu_stl_data(env, sleep_input + 0u, sleep_input_vptr);
    cpu_stl_data(env, sleep_input + 4u, mapped_input ? mapped_input : input_esp);
    cpu_stl_data(env, sleep_input + 8u, now_ms);
  }
  if (remote_input != 0 && mofei_sim_addrs.murphyRemoteInputVtable_addr != 0) {
    cpu_stl_data(env, remote_input + 0u, remote_input_vptr);
    cpu_stl_data(env, remote_input + 4u, sleep_input ? sleep_input : (mapped_input ? mapped_input : input_esp));
  }
  if (debug_input != 0 && mofei_sim_addrs.murphySyntheticInputVtable_addr != 0) {
    cpu_stl_data(env, debug_input + 0u, synthetic_input_vptr);
    cpu_stl_data(env, debug_input + 4u, remote_input ? remote_input : (sleep_input ? sleep_input : input_esp));
    cpu_stl_data(env, debug_input + 8u, now_ms);
  }

  cpu_stl_data(env, runner + 0u, input);
  cpu_stl_data(env, runner + 4u, window);
  cpu_stl_data(env, runner + 8u, now_ms);
}

static void mofei_log_murphy_runner_layout(CPUXtensaState* env, uint32_t runner, uint32_t input, uint32_t window,
                                           uint32_t now_ms) {
  static unsigned log_count;
  if (log_count >= 4) {
    return;
  }
  log_count++;

  const uint32_t runner_input = mofei_read_guest_data_u32(env, runner + 0u);
  const uint32_t runner_window = mofei_read_guest_data_u32(env, runner + 4u);
  const uint32_t runner_now = mofei_read_guest_data_u32(env, runner + 8u);
  const uint32_t input_vtable = mofei_read_guest_data_u32(env, input + 0u);
  const uint32_t input_poll = mofei_read_guest_data_u32(env, input_vtable + 8u);
  const uint32_t window_word0 = mofei_read_guest_data_u32(env, window + 0u);
  const uint32_t window_word4 = mofei_read_guest_data_u32(env, window + 4u);
  fprintf(stderr,
          "[MURPHY] runner layout runner=0x%08x fields(input=0x%08x window=0x%08x now=0x%08x) "
          "expected(input=0x%08x window=0x%08x now=0x%08x) input.vtable=0x%08x input.poll=0x%08x "
          "window[0]=0x%08x window[4]=0x%08x\n",
          runner, runner_input, runner_window, runner_now, input, window, now_ms, input_vtable, input_poll,
          window_word0, window_word4);
}

static bool mofei_vptr_matches(uint32_t vptr, uint32_t table_addr) {
  if (table_addr == 0 || vptr == 0) {
    return false;
  }
  return vptr == table_addr || vptr == table_addr + 8u;
}

static const char* mofei_murphy_activity_for_vptr(uint32_t vptr) {
  struct MurphyActivityVtable {
    uint32_t table;
    const char* activity;
  } entries[] = {
      {mofei_sim_addrs.murphyVtableHomeScene_addr, "Dashboard"},
      {mofei_sim_addrs.murphyVtableSettingsScene_addr, "Settings"},
      {mofei_sim_addrs.murphyVtableWifiSelectionScene_addr, "WifiSelection"},
      {mofei_sim_addrs.murphyVtableFileBrowserScene_addr, "FileBrowser"},
      {mofei_sim_addrs.murphyVtableLibraryScene_addr, "Library"},
      {mofei_sim_addrs.murphyVtableRecentBooksScene_addr, "RecentBooks"},
      {mofei_sim_addrs.murphyVtableArcadeHubScene_addr, "ArcadeHub"},
      {mofei_sim_addrs.murphyVtableGame2048Scene_addr, "Game2048"},
      {mofei_sim_addrs.murphyVtableWeatherScene_addr, "WeatherClock"},
      {mofei_sim_addrs.murphyVtableCalendarScene_addr, "Calendar"},
      {mofei_sim_addrs.murphyVtableStudyHubScene_addr, "Study"},
      {mofei_sim_addrs.murphyVtableCardsTodayScene_addr, "StudyCardsToday"},
      {mofei_sim_addrs.murphyVtableStudyQueueScene_addr, "StudyQueue"},
      {mofei_sim_addrs.murphyVtableStudyReviewQueueScene_addr, "ReviewQueue"},
      {mofei_sim_addrs.murphyVtableStudyImportStatusScene_addr, "DeckImportStatus"},
      {mofei_sim_addrs.murphyVtableStudyQuizScene_addr, "StudyQuiz"},
      {mofei_sim_addrs.murphyVtableStudyReportScene_addr, "LearningReport"},
      {mofei_sim_addrs.murphyVtableStudyRecoveryScene_addr, "StudyRecovery"},
      {mofei_sim_addrs.murphyVtableKeyboardScene_addr, "KeyboardEntry"},
      {mofei_sim_addrs.murphyVtableConfirmationScene_addr, "Confirmation"},
      {mofei_sim_addrs.murphyVtableReaderScene_addr, "Reader"},
      {mofei_sim_addrs.murphyVtableSleepScene_addr, "SleepWallpaper"},
      {mofei_sim_addrs.murphyVtablePandaHubScene_addr, "Applets"},
      {mofei_sim_addrs.murphyVtablePandaLuaAppScene_addr, "LuaApp"},
      {mofei_sim_addrs.murphyVtableButtonRemapScene_addr, "ButtonRemap"},
      {mofei_sim_addrs.murphyVtableDiagnosticsScene_addr, "DeviceDiagnostics"},
      {mofei_sim_addrs.murphyVtableFontPickerScene_addr, "TtfFontSelect"},
      {mofei_sim_addrs.murphyVtableEnumEditorScene_addr, "TimeZoneSelect"},
      {mofei_sim_addrs.murphyVtableToggleGroupScene_addr, "StatusBarSettings"},
      {mofei_sim_addrs.murphyVtableSearchResultsScene_addr, "TxtSearchResults"},
      {mofei_sim_addrs.murphyVtableBookmarkListScene_addr, "TxtBookmarks"},
      {mofei_sim_addrs.murphyVtableChapterListScene_addr, "EpubReaderChapterSelection"},
      {mofei_sim_addrs.murphyVtablePercentJumpScene_addr, "EpubReaderPercentSelection"},
      {mofei_sim_addrs.murphyVtableDictionaryScene_addr, "Dictionary"},
      {mofei_sim_addrs.murphyVtableSudokuScene_addr, "Sudoku"},
      {mofei_sim_addrs.murphyVtableVirtualPetScene_addr, "VirtualPet"},
  };
  for (int i = 0; i < (int)(sizeof(entries) / sizeof(entries[0])); ++i) {
    if (mofei_vptr_matches(vptr, entries[i].table)) {
      return entries[i].activity;
    }
  }
  return NULL;
}

static bool mofei_find_murphy_stack_context(CPUXtensaState* env, uint32_t sp, uint32_t* context_out,
                                            uint32_t* runner_out, uint32_t* fb_out, uint32_t* display_out) {
  const uint32_t fixed_context = sp + 164u;
  uint32_t fixed_runner = 0;
  uint32_t fixed_fb = 0;
  uint32_t fixed_display = 0;
  mofei_read_guest_memory(fixed_context + 0u, (uint8_t*)&fixed_runner, sizeof(fixed_runner));
  mofei_read_guest_memory(fixed_context + 4u, (uint8_t*)&fixed_fb, sizeof(fixed_fb));
  mofei_read_guest_memory(fixed_context + 8u, (uint8_t*)&fixed_display, sizeof(fixed_display));
  if (mofei_murphy_context_pointer_plausible(fixed_runner) && mofei_murphy_context_pointer_plausible(fixed_fb) &&
      mofei_murphy_context_pointer_plausible(fixed_display)) {
    *context_out = fixed_context;
    *runner_out = fixed_runner;
    *fb_out = fixed_fb;
    *display_out = fixed_display;
    return true;
  }

  const uint32_t scan_start = MOFEI_SIM_FAKE_APP_STACK_TOP > 8192u ? MOFEI_SIM_FAKE_APP_STACK_TOP - 8192u : 0;
  const uint32_t scan_end = MOFEI_SIM_FAKE_APP_STACK_TOP + 256u;

  for (uint32_t addr = scan_start; addr + 12u <= scan_end; addr += 4u) {
    uint32_t runner = 0;
    uint32_t fb = 0;
    uint32_t display = 0;
    mofei_read_guest_memory(addr + 0u, (uint8_t*)&runner, sizeof(runner));
    mofei_read_guest_memory(addr + 4u, (uint8_t*)&fb, sizeof(fb));
    mofei_read_guest_memory(addr + 8u, (uint8_t*)&display, sizeof(display));
    if (mofei_murphy_context_pointer_plausible(runner) && mofei_murphy_context_pointer_plausible(fb) &&
        mofei_murphy_context_pointer_plausible(display)) {
      *context_out = addr;
      *runner_out = runner;
      *fb_out = fb;
      *display_out = display;
      return true;
    }
  }

  return false;
}

static inline unsigned mofei_windowbase_bound(unsigned a, const CPUXtensaState* env) {
  return a & (env->config->nareg / 4 - 1);
}

static void add_translator_to_hash(GHashTable* translator, const char* name, const XtensaOpcodeOps* opcode) {
  if (!g_hash_table_insert(translator, (void*)name, (void*)opcode)) {
    error_report("Multiple definitions of '%s' opcode in a single table", name);
  }
}

static GHashTable* hash_opcode_translators(const XtensaOpcodeTranslators* t) {
  unsigned i, j;
  GHashTable* translator = g_hash_table_new(g_str_hash, g_str_equal);

  for (i = 0; i < t->num_opcodes; ++i) {
    if (t->opcode[i].op_flags & XTENSA_OP_NAME_ARRAY) {
      const char* const* name = t->opcode[i].name;

      for (j = 0; name[j]; ++j) {
        add_translator_to_hash(translator, (void*)name[j], (void*)(t->opcode + i));
      }
    } else {
      add_translator_to_hash(translator, (void*)t->opcode[i].name, (void*)(t->opcode + i));
    }
  }
  return translator;
}

static XtensaOpcodeOps* xtensa_find_opcode_ops(const XtensaOpcodeTranslators* t, const char* name) {
  static GHashTable* translators;
  GHashTable* translator;

  if (translators == NULL) {
    translators = g_hash_table_new(g_direct_hash, g_direct_equal);
  }
  translator = g_hash_table_lookup(translators, t);
  if (translator == NULL) {
    translator = hash_opcode_translators(t);
    g_hash_table_insert(translators, (void*)t, translator);
  }
  return g_hash_table_lookup(translator, name);
}

static void init_libisa(XtensaConfig* config) {
  unsigned i, j;
  unsigned opcodes;
  unsigned formats;
  unsigned regfiles;

  config->isa = xtensa_isa_init(config->isa_internal, NULL, NULL);
  assert(xtensa_isa_maxlength(config->isa) <= MAX_INSN_LENGTH);
  assert(xtensa_insnbuf_size(config->isa) <= MAX_INSNBUF_LENGTH);
  opcodes = xtensa_isa_num_opcodes(config->isa);
  formats = xtensa_isa_num_formats(config->isa);
  regfiles = xtensa_isa_num_regfiles(config->isa);
  config->opcode_ops = g_new(XtensaOpcodeOps*, opcodes);

  for (i = 0; i < formats; ++i) {
    assert(xtensa_format_num_slots(config->isa, i) <= MAX_INSN_SLOTS);
  }

  for (i = 0; i < opcodes; ++i) {
    const char* opc_name = xtensa_opcode_name(config->isa, i);
    XtensaOpcodeOps* ops = NULL;

    assert(xtensa_opcode_num_operands(config->isa, i) <= MAX_OPCODE_ARGS);
    if (!config->opcode_translators) {
      ops = xtensa_find_opcode_ops(&xtensa_core_opcodes, opc_name);
    } else {
      for (j = 0; !ops && config->opcode_translators[j]; ++j) {
        ops = xtensa_find_opcode_ops(config->opcode_translators[j], opc_name);
      }
    }
#ifdef DEBUG
    if (ops == NULL) {
      fprintf(stderr, "opcode translator not found for %s's opcode '%s'\n", config->name, opc_name);
    }
#endif
    config->opcode_ops[i] = ops;
  }
  config->a_regfile = xtensa_regfile_lookup(config->isa, "AR");

  config->regfile = g_new(void**, regfiles);
  for (i = 0; i < regfiles; ++i) {
    const char* name = xtensa_regfile_name(config->isa, i);
    int entries = xtensa_regfile_num_entries(config->isa, i);
    int bits = xtensa_regfile_num_bits(config->isa, i);

    config->regfile[i] = xtensa_get_regfile_by_name(name, entries, bits);
#ifdef DEBUG
    if (config->regfile[i] == NULL) {
      fprintf(stderr, "regfile '%s' not found for %s\n", name, config->name);
    }
#endif
  }
  xtensa_collect_sr_names(config);
}

static void xtensa_finalize_config(XtensaConfig* config) {
  if (config->isa_internal) {
    init_libisa(config);
  }

  if (config->gdb_regmap.num_regs == 0 || config->gdb_regmap.num_core_regs == 0) {
    unsigned n_regs = 0;
    unsigned n_core_regs = 0;

    xtensa_count_regs(config, &n_regs, &n_core_regs);
    if (config->gdb_regmap.num_regs == 0) {
      config->gdb_regmap.num_regs = n_regs;
    }
    if (config->gdb_regmap.num_core_regs == 0) {
      config->gdb_regmap.num_core_regs = n_core_regs;
    }
  }
}

static void xtensa_core_class_init(ObjectClass* oc, void* data) {
  CPUClass* cc = CPU_CLASS(oc);
  XtensaCPUClass* xcc = XTENSA_CPU_CLASS(oc);
  XtensaConfig* config = data;

  xtensa_finalize_config(config);
  xcc->config = config;

  /*
   * Use num_core_regs to see only non-privileged registers in an unmodified
   * gdb. Use num_regs to see all registers. gdb modification is required
   * for that: reset bit 0 in the 'flags' field of the registers definitions
   * in the gdb/xtensa-config.c inside gdb source tree or inside gdb overlay.
   */
  cc->gdb_num_core_regs = config->gdb_regmap.num_regs;

  /* Espressif local: allow changing the behavior here using
   * QEMU_XTENSA_CORE_REGS_ONLY environment variable, to support different
   * GDB builds
   */
  const char* core_regs_only = getenv("QEMU_XTENSA_CORE_REGS_ONLY");
  if (core_regs_only != NULL && strcmp(core_regs_only, "0") != 0) {
    cc->gdb_num_core_regs = config->gdb_regmap.num_core_regs;
  }
}

void xtensa_register_core(XtensaConfigList* node) {
  TypeInfo type = {
      .parent = TYPE_XTENSA_CPU,
      .class_init = xtensa_core_class_init,
      .class_data = (void*)node->config,
  };

  node->next = xtensa_cores;
  xtensa_cores = node;
  type.name = g_strdup_printf(XTENSA_CPU_TYPE_NAME("%s"), node->config->name);
  type_register(&type);
  g_free((gpointer)type.name);
}

static uint32_t check_hw_breakpoints(CPUXtensaState* env) {
  unsigned i;

  for (i = 0; i < env->config->ndbreak; ++i) {
    if (env->cpu_watchpoint[i] && env->cpu_watchpoint[i]->flags & BP_WATCHPOINT_HIT) {
      return DEBUGCAUSE_DB | (i << DEBUGCAUSE_DBNUM_SHIFT);
    }
  }
  return 0;
}

void xtensa_breakpoint_handler(CPUState* cs) {
  CPUXtensaState* env = cpu_env(cs);

  if (cs->watchpoint_hit) {
    if (cs->watchpoint_hit->flags & BP_CPU) {
      uint32_t cause;

      cs->watchpoint_hit = NULL;
      cause = check_hw_breakpoints(env);
      if (cause) {
        debug_exception_env(env, cause);
      }
      cpu_loop_exit_noexc(cs);
    }
  } else {
    if (cpu_breakpoint_test(cs, env->pc, BP_GDB) || !cpu_breakpoint_test(cs, env->pc, BP_CPU)) {
      return;
    }
    if (env->sregs[ICOUNT] == 0xffffffff && xtensa_get_cintlevel(env) < env->sregs[ICOUNTLEVEL]) {
      debug_exception_env(env, DEBUGCAUSE_IC);
    } else {
      debug_exception_env(env, DEBUGCAUSE_IB);
    }
    cpu_loop_exit_noexc(cs);
  }
}

void HELPER(exception)(CPUXtensaState* env, uint32_t excp) {
  CPUState* cs = env_cpu(env);

  cs->exception_index = excp;
  if (excp == EXCP_YIELD) {
    env->yield_needed = 0;
  }
  cpu_loop_exit(cs);
}

static bool mofei_read_pc_bytes(CPUState* cs, uint32_t pc, uint8_t* bytes, size_t len) {
  bool read_from_system_memory = false;
  if (((pc >= MOFEI_SIM_DCACHE_BASE && pc < MOFEI_SIM_DCACHE_LIMIT && len <= MOFEI_SIM_DCACHE_LIMIT - pc) ||
       (pc >= MOFEI_SIM_ICACHE_BASE && pc < MOFEI_SIM_ICACHE_LIMIT && len <= MOFEI_SIM_ICACHE_LIMIT - pc)) &&
      address_space_read(&address_space_memory, pc, MEMTXATTRS_UNSPECIFIED, bytes, len) == MEMTX_OK) {
    bool all_zero = true;
    read_from_system_memory = true;
    for (size_t i = 0; i < len; ++i) {
      if (bytes[i] != 0) {
        all_zero = false;
        break;
      }
    }
    if (!all_zero) {
      return true;
    }
  }
  if (cpu_memory_rw_debug(cs, pc, bytes, len, 0) == 0) {
    bool all_zero = true;
    for (size_t i = 0; i < len; ++i) {
      if (bytes[i] != 0) {
        all_zero = false;
        break;
      }
    }
    if (!all_zero) {
      return true;
    }
  }
  if (read_from_system_memory) {
    return true;
  }
  return false;
}

static bool mofei_bytes_are_all_zero(const uint8_t* bytes, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (bytes[i] != 0) {
      return false;
    }
  }
  return true;
}

static bool mofei_pc_is_firmware_codeish(uint32_t pc) {
  return (pc >= 0x40000000u && pc < 0x40400000u) || (pc >= 0x42000000u && pc < 0x44000000u) ||
         (pc >= 0x02000000u && pc < 0x04000000u);
}

static bool mofei_raw_addr_has_calln_marker(uint32_t raw_addr) { return (raw_addr >> 30) != 0; }

static bool mofei_pc_is_exception_vector_region(CPUXtensaState* env, uint32_t pc) {
  uint32_t vecbase = env->sregs[VECBASE];
  if (vecbase == 0) {
    return false;
  }
  return pc >= vecbase && pc < vecbase + 0x100u;
}

static uint32_t mofei_canonical_calln_return_pc(uint32_t raw_addr) { return (raw_addr & 0x3fffffffu) | 0x40000000u; }

typedef struct MofeiExceptionResume {
  uint32_t pc;
  uint32_t raw_a0;
  bool restores_calln_window;
} MofeiExceptionResume;

static uint32_t mofei_restore_calln_window_common(CPUXtensaState* env, uint32_t pc, uint32_t ret_addr,
                                                  unsigned* out_caller_ret_reg, const char* log_prefix);

static bool mofei_trace_enabled(const char* env_name) {
  const char* value = getenv(env_name);
  return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void mofei_log_safe_resume_check(uint32_t pc, const char* reason, const uint8_t* bytes, size_t len) {
  static unsigned log_count;
  if (log_count >= 96) {
    return;
  }
  log_count++;
  fprintf(stderr, "[SAFE-RESUME] pc=%08x %s", pc, reason);
  if (bytes) {
    fprintf(stderr, " bytes=");
    for (size_t i = 0; i < len; ++i) {
      fprintf(stderr, "%s%02x", i == 0 ? "" : " ", bytes[i]);
    }
  }
  fprintf(stderr, "\n");
}

static bool mofei_pc_is_safe_resume(CPUXtensaState* env, CPUState* cs, uint32_t pc, uint32_t bad_pc) {
  uint8_t bytes[6] = {0};

  if (pc == 0) {
    mofei_log_safe_resume_check(pc, "reject:zero", NULL, 0);
    return false;
  }
  if (pc == 0x4037D000u) {
    mofei_log_safe_resume_check(pc, "reject:simulator-halt-stub", NULL, 0);
    return false;
  }
  if (pc == bad_pc) {
    mofei_log_safe_resume_check(pc, "reject:bad-pc", NULL, 0);
    return false;
  }
  if (mofei_pc_is_exception_vector_region(env, pc)) {
    mofei_log_safe_resume_check(pc, "reject:exception-vector", NULL, 0);
    return false;
  }
  if (!mofei_pc_is_firmware_codeish(pc)) {
    mofei_log_safe_resume_check(pc, "reject:not-codeish", NULL, 0);
    return false;
  }
  if (!mofei_read_pc_bytes(cs, pc, bytes, sizeof(bytes))) {
    mofei_log_safe_resume_check(pc, "reject:read-failed", NULL, 0);
    return false;
  }
  if (mofei_bytes_are_all_zero(bytes, sizeof(bytes))) {
    mofei_log_safe_resume_check(pc, "reject:all-zero", bytes, sizeof(bytes));
    return false;
  }
  mofei_log_safe_resume_check(pc, "accept", bytes, sizeof(bytes));
  return true;
}

static bool mofei_pc_is_safe_epc_resume(CPUXtensaState* env, CPUState* cs, uint32_t pc, uint32_t bad_pc) {
  if (env->regs[0] != 0 && !mofei_raw_addr_has_calln_marker(env->regs[0])) {
    mofei_log_safe_resume_check(pc, "reject:bad-a0-return-state", NULL, 0);
    return false;
  }
  return mofei_pc_is_safe_resume(env, cs, pc, bad_pc);
}

static bool mofei_pc_is_safe_a0_resume(CPUXtensaState* env, CPUState* cs, uint32_t pc, uint32_t bad_pc,
                                       uint32_t raw_a0) {
  if (!mofei_raw_addr_has_calln_marker(raw_a0)) {
    mofei_log_safe_resume_check(pc, "reject:a0-no-calln-marker", NULL, 0);
    return false;
  }
  if (pc != mofei_canonical_calln_return_pc(raw_a0)) {
    mofei_log_safe_resume_check(pc, "reject:a0-mismatch", NULL, 0);
    return false;
  }
  return mofei_pc_is_safe_resume(env, cs, pc, bad_pc);
}

static MofeiExceptionResume mofei_make_pc_exception_resume(uint32_t pc) {
  return (MofeiExceptionResume){
      .pc = pc,
      .raw_a0 = 0,
      .restores_calln_window = false,
  };
}

static MofeiExceptionResume mofei_make_a0_exception_resume(uint32_t pc, uint32_t raw_a0) {
  return (MofeiExceptionResume){
      .pc = pc,
      .raw_a0 = raw_a0,
      .restores_calln_window = true,
  };
}

static void mofei_apply_exception_resume(CPUXtensaState* env, const MofeiExceptionResume* resume,
                                         const char* log_prefix) {
  env->sregs[PS] &= ~PS_EXCM;
  if (resume->restores_calln_window) {
    env->pc = mofei_restore_calln_window_common(env, resume->pc, resume->raw_a0, NULL, log_prefix);
  } else {
    env->pc = resume->pc;
  }
}

static MofeiExceptionResume mofei_find_zero_vector_resume(CPUXtensaState* env, CPUState* cs, uint32_t bad_pc) {
  /* VECBASE+0x10 is the user exception vector, so EPC1 is the faulting
   * instruction. DEPC may still hold older double-exception context. */
  const uint32_t epc_candidates[] = {
      env->sregs[EPC1],
      env->sregs[DEPC],
  };

  for (size_t i = 0; i < ARRAY_SIZE(epc_candidates); ++i) {
    uint32_t epc = epc_candidates[i];
    if (epc >= 0x02000000u && epc < 0x04000000u) {
      epc |= 0x40000000u;
    }
    if (mofei_pc_is_safe_epc_resume(env, cs, epc, bad_pc)) {
      return mofei_make_pc_exception_resume(epc);
    }
  }

  if (mofei_raw_addr_has_calln_marker(env->regs[0])) {
    uint32_t a0_resume = mofei_canonical_calln_return_pc(env->regs[0]);
    if (mofei_pc_is_safe_a0_resume(env, cs, a0_resume, bad_pc, env->regs[0])) {
      return mofei_make_a0_exception_resume(a0_resume, env->regs[0]);
    }
  }

  return (MofeiExceptionResume){0};
}

static bool mofei_handle_zero_exception_vector(CPUXtensaState* env, uint32_t pc, uint32_t cause) {
  CPUState* cs = env_cpu(env);
  uint8_t bytes[6] = {0};
  uint32_t expected_pc = env->sregs[VECBASE] + 0x10u;
  static unsigned zero_vector_log_count;
  static bool shutdown_requested;

  if (cause != 0 || pc != expected_pc || !mofei_read_pc_bytes(cs, pc, bytes, sizeof(bytes)) ||
      !mofei_bytes_are_all_zero(bytes, sizeof(bytes))) {
    return false;
  }

  MofeiExceptionResume resume = mofei_find_zero_vector_resume(env, cs, pc);
  if (resume.pc) {
    if (zero_vector_log_count < 8) {
      fprintf(stderr,
              "[EXC-ZERO-VECTOR] pc=%08x is blank VECBASE+0x10; resuming at %08x (EPC1=%08x DEPC=%08x A0=%08x)\n", pc,
              resume.pc, env->sregs[EPC1], env->sregs[DEPC], env->regs[0]);
      zero_vector_log_count++;
    }
    mofei_apply_exception_resume(env, &resume, "EXC-ZERO-VECTOR");
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cs);
    return true;
  }

  if (!shutdown_requested) {
    fprintf(stderr,
            "[EXC-ZERO-VECTOR] pc=%08x is blank VECBASE+0x10 and no safe resume PC was found; requesting simulator "
            "shutdown\n",
            pc);
    shutdown_requested = true;
  }
  cs->halted = 1;
#ifndef CONFIG_USER_ONLY
  qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
#endif
  cpu_loop_exit(cs);
  return true;
}

static MofeiExceptionResume mofei_find_fetch_exception_resume(CPUXtensaState* env, CPUState* cs, uint32_t bad_pc) {
  uint32_t raw_a0 = env->regs[0];
  if (mofei_raw_addr_has_calln_marker(raw_a0)) {
    uint32_t a0_resume = mofei_canonical_calln_return_pc(raw_a0);
    if (mofei_pc_is_safe_a0_resume(env, cs, a0_resume, bad_pc, raw_a0)) {
      return mofei_make_a0_exception_resume(a0_resume, raw_a0);
    }
  }

  const uint32_t epc_candidates[] = {
      env->sregs[EPC1],
      env->sregs[DEPC],
  };
  for (size_t i = 0; i < ARRAY_SIZE(epc_candidates); ++i) {
    if (mofei_pc_is_safe_epc_resume(env, cs, epc_candidates[i], bad_pc)) {
      return mofei_make_pc_exception_resume(epc_candidates[i]);
    }
  }

  return (MofeiExceptionResume){0};
}

static bool mofei_handle_fetch_exception_artifact(CPUXtensaState* env, uint32_t pc, uint32_t cause) {
  CPUState* cs = env_cpu(env);
  uint8_t bytes[6] = {0};
  bool readable;
  bool unusable_pc;
  MofeiExceptionResume resume;
  static unsigned resume_log_count;
  static bool shutdown_requested;

  if (cause != 15) {
    return false;
  }

  readable = mofei_read_pc_bytes(cs, pc, bytes, sizeof(bytes));
  unusable_pc = mofei_pc_is_exception_vector_region(env, pc) || !mofei_pc_is_firmware_codeish(pc) || !readable ||
                mofei_bytes_are_all_zero(bytes, sizeof(bytes));
  if (!unusable_pc) {
    return false;
  }

  resume = mofei_find_fetch_exception_resume(env, cs, pc);
  if (resume.pc) {
    if (resume_log_count < 8) {
      fprintf(stderr,
              "[EXC-FETCH-ARTIFACT] pc=%08x is unusable for simulator fetch; resuming at %08x (EPC1=%08x "
              "DEPC=%08x A0=%08x)\n",
              pc, resume.pc, env->sregs[EPC1], env->sregs[DEPC], env->regs[0]);
      resume_log_count++;
    }
    mofei_apply_exception_resume(env, &resume, "EXC-FETCH-ARTIFACT");
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cs);
    return true;
  }

  if (!shutdown_requested) {
    fprintf(stderr,
            "[EXC-FETCH-ARTIFACT] pc=%08x is unusable for simulator fetch and no safe resume PC was found; "
            "requesting simulator shutdown (EPC1=%08x DEPC=%08x A0=%08x)\n",
            pc, env->sregs[EPC1], env->sregs[DEPC], env->regs[0]);
    shutdown_requested = true;
  }
  cs->halted = 1;
#ifndef CONFIG_USER_ONLY
  qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
#endif
  cpu_loop_exit(cs);
  return true;
}

static bool mofei_handle_illegal_instruction_artifact(CPUXtensaState* env, uint32_t pc, uint32_t cause) {
  CPUState* cs = env_cpu(env);
  uint8_t bytes[6] = {0};
  MofeiExceptionResume resume;
  static unsigned resume_log_count;
  static bool shutdown_requested;

  if (cause != 0) {
    return false;
  }

  mofei_read_pc_bytes(cs, pc, bytes, sizeof(bytes));

  /* Do not redirect illegal-instruction traps to loop().  If the PC has been
   * corrupted (for example into the middle of a firmware instruction), jumping
   * to loop() preserves stale register/window state and can recursively fault
   * through the blank exception vector.  Prefer the call return address when it
   * is safe; otherwise stop the simulator instead of hiding the root cause in an
   * endless recovery loop. */
  resume = mofei_find_fetch_exception_resume(env, cs, pc);
  if (resume.pc) {
    if (resume_log_count < 8) {
      fprintf(stderr,
              "[EXC-ILLEGAL] bytes at PC: %02x %02x %02x %02x %02x %02x; resuming at caller 0x%08x "
              "(EPC1=%08x DEPC=%08x A0=%08x)\n",
              bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], resume.pc, env->sregs[EPC1], env->sregs[DEPC],
              env->regs[0]);
      resume_log_count++;
    }
    mofei_apply_exception_resume(env, &resume, "EXC-ILLEGAL");
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cs);
    return true;
  }

  if (!shutdown_requested) {
    fprintf(stderr,
            "[EXC-ILLEGAL] bytes at PC: %02x %02x %02x %02x %02x %02x; no safe resume PC found; requesting "
            "simulator shutdown (EPC1=%08x DEPC=%08x A0=%08x)\n",
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], env->sregs[EPC1], env->sregs[DEPC],
            env->regs[0]);
    shutdown_requested = true;
  }
  cs->halted = 1;
#ifndef CONFIG_USER_ONLY
  qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
#endif
  cpu_loop_exit(cs);
  return true;
}

void HELPER(exception_cause)(CPUXtensaState* env, uint32_t pc, uint32_t cause) {
  /* ESP32-S3 QEMU: Skip divide-by-zero exceptions instead of crashing.
   * The firmware's peripheral calibration returns 0 (retw-patched),
   * causing division by zero in clock/timestamp calculations. */
  if (cause == INTEGER_DIVIDE_BY_ZERO_CAUSE) {
    qemu_log_mask(CPU_LOG_INT, "%s: skipping divide-by-zero at pc=%08x\n", __func__, pc);
    return;
  }

  /* CPU1 is permanently powered off in mofei-sim. Any exception on
   * CPU1 is just the idle loop hitting zero-fill ROM. Halt it to
   * silence the noise and stop the spinning. */
  {
    CPUState* cs = env_cpu(env);
    if (cs->cpu_index > 0) {
      cs->halted = 1;
      cpu_loop_exit(cs);
      return;
    }
  }

  uint32_t vector;

  env->pc = pc;

  if (mofei_handle_zero_exception_vector(env, pc, cause)) {
    return;
  }

  if (mofei_handle_fetch_exception_artifact(env, pc, cause)) {
    return;
  }

  if (mofei_handle_illegal_instruction_artifact(env, pc, cause)) {
    return;
  }

  /* Log only serious exceptions (cause 0 = illegal instruction,
   * cause 15 =_instr/da-fetch, cause 29 = register window).
   * Skip frequent benign exceptions to reduce noise. */
  static int exc_log_count = 0;
  if ((cause == 0 || cause == 15 || cause == 29) && exc_log_count < 20) {
    fprintf(stderr, "[EXC] cause=%u pc=%08x PS=%08x A0=%08x\n", cause, pc, env->sregs[PS], env->regs[0]);
    exc_log_count++;
  }

  static int exc_detail_log_count = 0;
  if ((pc == 0 || cause == 0 || cause == 15) && exc_detail_log_count < 20) {
    fprintf(stderr, "[EXC-DETAIL] WINDOWBASE=%d WINDOWSTART=0x%x SP=0x%08x\n", env->sregs[WINDOW_BASE],
            env->sregs[WINDOW_START], env->regs[1]);
    fprintf(stderr, "[EXC-DETAIL] regs[0-7]=%08x %08x %08x %08x %08x %08x %08x %08x\n", env->regs[0], env->regs[1],
            env->regs[2], env->regs[3], env->regs[4], env->regs[5], env->regs[6], env->regs[7]);
    fprintf(stderr, "[EXC-DETAIL] regs[8-15]=%08x %08x %08x %08x %08x %08x %08x %08x\n", env->regs[8], env->regs[9],
            env->regs[10], env->regs[11], env->regs[12], env->regs[13], env->regs[14], env->regs[15]);
    fprintf(stderr, "[EXC-DETAIL] EXCVADDR=0x%08x LBEG=0x%08x LEND=0x%08x LCOUNT=0x%08x\n", env->sregs[EXCVADDR],
            env->sregs[LBEG], env->sregs[LEND], env->sregs[LCOUNT]);
    fprintf(stderr, "[EXC-DETAIL] SAR=%d VECBASE=0x%08x\n", env->sregs[SAR], env->sregs[VECBASE]);
    exc_detail_log_count++;
  }

  if (env->sregs[PS] & PS_EXCM) {
    if (env->config->ndepc) {
      env->sregs[DEPC] = pc;
    } else {
      env->sregs[EPC1] = pc;
    }
    vector = EXC_DOUBLE;
  } else {
    env->sregs[EPC1] = pc;
    env->sregs[EXCCAUSE] = cause;
    env->sregs[PS] |= PS_EXCM;
    vector = EXC_USER;
  }

  CPUState* cs = env_cpu(env);
  cs->exception_index = EXCP_INTERRUPT;
  env->pc = env->sregs[VECBASE] + vector;
  cpu_loop_exit(cs);
}

void HELPER(exception_cause_vaddr)(CPUXtensaState* env, uint32_t pc, uint32_t cause, uint32_t vaddr) {
  env->sregs[EXCVADDR] = vaddr;
  HELPER(exception_cause)(env, pc, cause);
}

void debug_exception_env(CPUXtensaState* env, uint32_t cause) {
  if (xtensa_get_cintlevel(env) < env->config->debug_level) {
    HELPER(debug_exception)(env, env->pc, cause);
  }
}

void HELPER(debug_exception)(CPUXtensaState* env, uint32_t pc, uint32_t cause) {
  unsigned level = env->config->debug_level;

  env->pc = pc;
  env->sregs[DEBUGCAUSE] = cause;
  env->sregs[EPC1 + level - 1] = pc;
  env->sregs[EPS2 + level - 2] = env->sregs[PS];
  env->sregs[PS] = (env->sregs[PS] & ~PS_INTLEVEL) | PS_EXCM | (level << PS_INTLEVEL_SHIFT);
  HELPER(exception)(env, EXC_DEBUG);
}

#ifndef CONFIG_USER_ONLY

void HELPER(waiti)(CPUXtensaState* env, uint32_t pc, uint32_t intlevel) {
  CPUState* cpu = env_cpu(env);

  env->pc = pc;
  env->sregs[PS] = (env->sregs[PS] & ~PS_INTLEVEL) | (intlevel << PS_INTLEVEL_SHIFT);

  bql_lock();
  check_interrupts(env);
  bql_unlock();

  if (env->pending_irq_level) {
    cpu_loop_exit(cpu);
    return;
  }

  cpu->halted = 1;
  HELPER(exception)(env, EXCP_HLT);
}

void HELPER(check_interrupts)(CPUXtensaState* env) {
  bql_lock();
  check_interrupts(env);
  bql_unlock();
}

void HELPER(intset)(CPUXtensaState* env, uint32_t v) {
  qatomic_or(&env->sregs[INTSET], v & env->config->inttype_mask[INTTYPE_SOFTWARE]);
}

static void intclear(CPUXtensaState* env, uint32_t v) { qatomic_and(&env->sregs[INTSET], ~v); }

void HELPER(intclear)(CPUXtensaState* env, uint32_t v) {
  intclear(env, v & (env->config->inttype_mask[INTTYPE_SOFTWARE] | env->config->inttype_mask[INTTYPE_EDGE]));
}

static uint32_t relocated_vector(CPUXtensaState* env, uint32_t vector) {
  if (xtensa_option_enabled(env->config, XTENSA_OPTION_RELOCATABLE_VECTOR)) {
    return vector - env->config->vecbase + env->sregs[VECBASE];
  } else {
    return vector;
  }
}

/*!
 * Handle penging IRQ.
 * For the high priority interrupt jump to the corresponding interrupt vector.
 * For the level-1 interrupt convert it to either user, kernel or double
 * exception with the 'level-1 interrupt' exception cause.
 */
static void handle_interrupt(CPUXtensaState* env) {
  int level = env->pending_irq_level;

  if ((level > xtensa_get_cintlevel(env) && level <= env->config->nlevel &&
       (env->config->level_mask[level] & env->sregs[INTSET] & env->sregs[INTENABLE])) ||
      level == env->config->nmi_level) {
    CPUState* cs = env_cpu(env);

    if (level > 1) {
      /* env->config->nlevel check should have ensured this */
      assert(level < ARRAY_SIZE(env->config->interrupt_vector));

      env->sregs[EPC1 + level - 1] = env->pc;
      env->sregs[EPS2 + level - 2] = env->sregs[PS];
      env->sregs[PS] = (env->sregs[PS] & ~PS_INTLEVEL) | level | PS_EXCM;
      env->pc = relocated_vector(env, env->config->interrupt_vector[level]);
      if (level == env->config->nmi_level) {
        intclear(env, env->config->inttype_mask[INTTYPE_NMI]);
      }
    } else {
      env->sregs[EXCCAUSE] = LEVEL1_INTERRUPT_CAUSE;

      if (env->sregs[PS] & PS_EXCM) {
        if (env->config->ndepc) {
          env->sregs[DEPC] = env->pc;
        } else {
          env->sregs[EPC1] = env->pc;
        }
        cs->exception_index = EXC_DOUBLE;
      } else {
        env->sregs[EPC1] = env->pc;
        cs->exception_index = (env->sregs[PS] & PS_UM) ? EXC_USER : EXC_KERNEL;
      }
      env->sregs[PS] |= PS_EXCM;
    }
  }
}

static void mofei_store_window_overflow(CPUXtensaState* env, unsigned save_count) {
  const uint32_t next_sp = env->regs[save_count + 1];
  const uint32_t previous_sp = save_count > 4 ? cpu_ldl_data(env, env->regs[1] - 12) : 0;

  for (unsigned i = 0; i < 4; ++i) {
    cpu_stl_data(env, next_sp - 16 + i * 4, env->regs[i]);
  }

  if (save_count > 4) {
    const int base_offset = save_count == 8 ? -32 : -48;
    for (unsigned i = 4; i < save_count; ++i) {
      cpu_stl_data(env, previous_sp + base_offset + (i - 4) * 4, env->regs[i]);
    }
  }
}

static void mofei_load_window_underflow(CPUXtensaState* env, unsigned save_count) {
  const uint32_t next_sp = env->regs[save_count + 1];
  uint32_t before_regs[16];
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_WINDOW")) {
    memcpy(before_regs, env->regs, sizeof(before_regs));
  }

  env->regs[0] = cpu_ldl_data(env, next_sp - 16);
  env->regs[1] = cpu_ldl_data(env, next_sp - 12);
  env->regs[2] = cpu_ldl_data(env, next_sp - 8);

  if (save_count == 4) {
    env->regs[3] = cpu_ldl_data(env, next_sp - 4);
    return;
  }

  const uint32_t previous_sp = cpu_ldl_data(env, env->regs[1] - 12);
  const int base_offset = save_count == 8 ? -32 : -48;

  env->regs[3] = cpu_ldl_data(env, next_sp - 4);
  for (unsigned i = 4; i < save_count; ++i) {
    env->regs[i] = cpu_ldl_data(env, previous_sp + base_offset + (i - 4) * 4);
  }

  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_WINDOW")) {
    static unsigned underflow_log_count;
    if (underflow_log_count < 64) {
      fprintf(stderr,
              "[MOFEI-WINDOW-DBG] underflow save=%u wb=%u owb=%u next_sp=0x%08x before_a0=0x%08x "
              "before_a1=0x%08x before_ret=0x%08x after_a0=0x%08x after_a1=0x%08x after_ret=0x%08x\n",
              save_count, env->sregs[WINDOW_BASE], (env->sregs[PS] & PS_OWB) >> PS_OWB_SHIFT, next_sp, before_regs[0],
              before_regs[1], before_regs[save_count + 2], env->regs[0], env->regs[1], env->regs[save_count + 2]);
      underflow_log_count++;
    }
  }
}

static bool mofei_handle_window_exception(CPUXtensaState* env, int exception_index) {
  bool overflow;
  uint32_t window_bit;
  unsigned save_count;
  static unsigned log_count;

  switch (exception_index) {
    case EXC_WINDOW_OVERFLOW4:
      overflow = true;
      save_count = 4;
      break;
    case EXC_WINDOW_OVERFLOW8:
      overflow = true;
      save_count = 8;
      break;
    case EXC_WINDOW_OVERFLOW12:
      overflow = true;
      save_count = 12;
      break;
    case EXC_WINDOW_UNDERFLOW4:
      overflow = false;
      save_count = 4;
      break;
    case EXC_WINDOW_UNDERFLOW8:
      overflow = false;
      save_count = 8;
      break;
    case EXC_WINDOW_UNDERFLOW12:
      overflow = false;
      save_count = 12;
      break;
    default:
      return false;
  }

  if (overflow) {
    mofei_store_window_overflow(env, save_count);
  } else {
    mofei_load_window_underflow(env, save_count);
  }
  xtensa_sync_phys_from_window(env);

  window_bit = 1u << mofei_windowbase_bound(env->sregs[WINDOW_BASE], env);
  if (overflow) {
    env->sregs[WINDOW_START] &= ~window_bit;
  } else {
    env->sregs[WINDOW_START] |= window_bit;
  }

  if (log_count < 16) {
    fprintf(stderr, "[MOFEI-WINDOW] %s exc=%d wb=%u owb=%u pc=%08x epc1=%08x ws=0x%x\n",
            overflow ? "overflow" : "underflow", exception_index, env->sregs[WINDOW_BASE],
            (env->sregs[PS] & PS_OWB) >> PS_OWB_SHIFT, env->pc, env->sregs[EPC1], env->sregs[WINDOW_START]);
    log_count++;
  }

  env->sregs[PS] &= ~PS_EXCM;
  xtensa_restore_owb(env);
  env->pc = env->sregs[EPC1];
  return true;
}

/* Called from cpu_handle_interrupt with BQL held */
void xtensa_cpu_do_interrupt(CPUState* cs) {
  CPUXtensaState* env = cpu_env(cs);

  if (cs->exception_index == EXC_IRQ) {
    qemu_log_mask(CPU_LOG_INT,
                  "%s(EXC_IRQ) level = %d, cintlevel = %d, "
                  "pc = %08x, a0 = %08x, ps = %08x, "
                  "intset = %08x, intenable = %08x, "
                  "ccount = %08x\n",
                  __func__, env->pending_irq_level, xtensa_get_cintlevel(env), env->pc, env->regs[0], env->sregs[PS],
                  env->sregs[INTSET], env->sregs[INTENABLE], env->sregs[CCOUNT]);
    handle_interrupt(env);
  }

  switch (cs->exception_index) {
    case EXC_WINDOW_OVERFLOW4:
    case EXC_WINDOW_UNDERFLOW4:
    case EXC_WINDOW_OVERFLOW8:
    case EXC_WINDOW_UNDERFLOW8:
    case EXC_WINDOW_OVERFLOW12:
    case EXC_WINDOW_UNDERFLOW12:
      if (mofei_handle_window_exception(env, cs->exception_index)) {
        break;
      }
      g_assert_not_reached();
    case EXC_KERNEL:
    case EXC_USER:
    case EXC_DOUBLE:
    case EXC_DEBUG:
      qemu_log_mask(CPU_LOG_INT,
                    "%s(%d) "
                    "pc = %08x, a0 = %08x, a1 = %08x, ps = %08x, ccount = %08x\n",
                    __func__, cs->exception_index, env->pc, env->regs[0], env->regs[1], env->sregs[PS],
                    env->sregs[CCOUNT]);
      if (env->config->exception_vector[cs->exception_index]) {
        uint32_t vector;

        vector = env->config->exception_vector[cs->exception_index];
        env->pc = relocated_vector(env, vector);
      } else {
        qemu_log_mask(CPU_LOG_INT, "%s(pc = %08x) bad exception_index: %d\n", __func__, env->pc, cs->exception_index);
      }
      break;

    case EXC_IRQ:
      break;

    default:
      qemu_log("%s(pc = %08x) unknown exception_index: %d\n", __func__, env->pc, cs->exception_index);
      break;
  }
  check_interrupts(env);
}

bool xtensa_cpu_exec_interrupt(CPUState* cs, int interrupt_request) {
  if (interrupt_request & CPU_INTERRUPT_HARD) {
    cs->exception_index = EXC_IRQ;
    xtensa_cpu_do_interrupt(cs);
    return true;
  }
  return false;
}

/* Bump allocator for QEMU simulator: intercepts allocator RETW patches.
 * Called from translate.c when executing at the patched RETW address.
 * Returns guest pointers from capability-specific simulator pools and keeps
 * heap_caps_* query answers tied to the same allocation ledger. */
typedef enum MofeiSimHeapClass {
  MOFEI_SIM_HEAP_INTERNAL = 1,
  MOFEI_SIM_HEAP_PSRAM = 2,
} MofeiSimHeapClass;

typedef enum MofeiSimHeapMode {
  MOFEI_SIM_HEAP_MODE_DEVICE = 1,
  MOFEI_SIM_HEAP_MODE_TEST = 2,
} MofeiSimHeapMode;

typedef struct MofeiSimHeapPool {
  uint32_t base;
  uint32_t limit;
  uint32_t ptr;
  uint32_t total;
  MofeiSimHeapClass heap_class;
  const char* label;
} MofeiSimHeapPool;

static MofeiSimHeapPool mofei_internal_pool = {0, 0, 0, 0, MOFEI_SIM_HEAP_INTERNAL, "internal"};
static MofeiSimHeapPool mofei_internal_pool_ext = {0, 0, 0, 0, MOFEI_SIM_HEAP_INTERNAL, "internal-ext"};
static MofeiSimHeapPool mofei_psram_pool = {MOFEI_SIM_ALLOC_HEAP_BASE, MOFEI_SIM_PSRAM_HEAP_LIMIT,
                                            MOFEI_SIM_ALLOC_HEAP_BASE, MOFEI_SIM_PSRAM_HEAP_BYTES,
                                            MOFEI_SIM_HEAP_PSRAM,      "psram"};
static uint32_t mofei_bump_count = 0;
#define MOFEI_BUMP_ALLOC_RECORD_MAX 32768
typedef struct MofeiBumpAllocRecord {
  uint32_t addr;
  uint32_t requested_size;
  uint32_t aligned_size;
  uint32_t caps;
  MofeiSimHeapClass heap_class;
  bool active;
} MofeiBumpAllocRecord;
static MofeiBumpAllocRecord mofei_bump_alloc_records[MOFEI_BUMP_ALLOC_RECORD_MAX];
static uint32_t mofei_bump_alloc_record_count;
static bool mofei_sim_heap_initialized;
static MofeiSimHeapMode mofei_sim_heap_mode = MOFEI_SIM_HEAP_MODE_DEVICE;

static const char* mofei_sim_heap_mode_name(MofeiSimHeapMode mode) {
  return mode == MOFEI_SIM_HEAP_MODE_TEST ? "test" : "device";
}

static void mofei_configure_heap_pool(MofeiSimHeapPool* pool, uint32_t base, uint32_t limit) {
  pool->base = base;
  pool->limit = limit;
  pool->ptr = base;
  pool->total = limit > base ? limit - base : 0;
}

static uint32_t mofei_heap_pool_remaining_capacity(uint32_t heap_budget, uint32_t used_bytes) {
  return used_bytes < heap_budget ? heap_budget - used_bytes : 0;
}

static uint32_t mofei_align_up_u32(uint32_t value, uint32_t alignment) {
  return alignment > 0 ? (value + alignment - 1u) & ~(alignment - 1u) : value;
}

static uint32_t mofei_min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static uint32_t mofei_internal_heap_start_addr(void) {
  uint32_t heap_start = mofei_sim_addrs.heap_start_addr;
  const bool before_rom_stub = heap_start >= MOFEI_SIM_INTERNAL_DRAM_BASE && heap_start < MOFEI_SIM_ROM_SAFE_STUB_BASE;
  const bool after_rom_stub = heap_start >= MOFEI_SIM_ROM_SAFE_STUB_LIMIT && heap_start < MOFEI_SIM_EXTERNAL_RAM_BASE;
  if (before_rom_stub || after_rom_stub) {
    return mofei_align_up_u32(heap_start, MOFEI_SIM_INTERNAL_HEAP_ALIGNMENT);
  }
  return MOFEI_SIM_INTERNAL_HEAP_FALLBACK_BASE;
}

static void mofei_configure_internal_heap_pool(void) {
  uint32_t base = MOFEI_SIM_INTERNAL_HEAP_FALLBACK_BASE;
  uint32_t limit = MOFEI_SIM_EXTERNAL_RAM_BASE;
  uint32_t ext_base = 0;
  uint32_t ext_limit = 0;

  if (mofei_sim_heap_mode == MOFEI_SIM_HEAP_MODE_TEST) {
    limit = MOFEI_SIM_INTERNAL_HEAP_TEST_LIMIT;
  } else {
    base = mofei_internal_heap_start_addr();
    if (base < MOFEI_SIM_ROM_SAFE_STUB_BASE) {
      limit = MOFEI_SIM_ROM_SAFE_STUB_BASE;
      const uint32_t primary_bytes = limit > base ? limit - base : 0;
      const uint32_t remaining_bytes = mofei_heap_pool_remaining_capacity(MOFEI_SIM_INTERNAL_HEAP_BYTES, primary_bytes);
      ext_base = MOFEI_SIM_INTERNAL_HEAP_FALLBACK_BASE;
      ext_limit = mofei_min_u32(MOFEI_SIM_EXTERNAL_RAM_BASE, ext_base + remaining_bytes);
    }
    limit = mofei_min_u32(limit, base + MOFEI_SIM_INTERNAL_HEAP_BYTES);
  }

  base = mofei_align_up_u32(base, MOFEI_SIM_INTERNAL_HEAP_ALIGNMENT);
  if (limit <= base || base < MOFEI_SIM_INTERNAL_DRAM_BASE || limit > MOFEI_SIM_INTERNAL_HEAP_TEST_LIMIT) {
    base = MOFEI_SIM_INTERNAL_HEAP_FALLBACK_BASE;
    limit = mofei_sim_heap_mode == MOFEI_SIM_HEAP_MODE_TEST ? MOFEI_SIM_INTERNAL_HEAP_TEST_LIMIT
                                                            : MOFEI_SIM_EXTERNAL_RAM_BASE;
  }
  mofei_configure_heap_pool(&mofei_internal_pool, base, limit);
  ext_base = mofei_align_up_u32(ext_base, MOFEI_SIM_INTERNAL_HEAP_ALIGNMENT);
  if (mofei_sim_heap_mode != MOFEI_SIM_HEAP_MODE_DEVICE || ext_limit <= ext_base ||
      ext_base < MOFEI_SIM_ROM_SAFE_STUB_LIMIT || ext_limit > MOFEI_SIM_EXTERNAL_RAM_BASE) {
    ext_base = 0;
    ext_limit = 0;
  }
  mofei_configure_heap_pool(&mofei_internal_pool_ext, ext_base, ext_limit);
}

static void mofei_sim_heap_init(void) {
  if (mofei_sim_heap_initialized) {
    return;
  }

  const char* mode = getenv("MOFEI_SIM_HEAP_MODE");
  if (mode && g_ascii_strcasecmp(mode, "test") == 0) {
    mofei_sim_heap_mode = MOFEI_SIM_HEAP_MODE_TEST;
  } else if (mode && mode[0] != '\0' && g_ascii_strcasecmp(mode, "device") != 0) {
    fprintf(stderr, "[MOFEI-SIM] invalid MOFEI_SIM_HEAP_MODE=%s; using device\n", mode);
  }

  mofei_configure_internal_heap_pool();
  if (mofei_sim_heap_mode == MOFEI_SIM_HEAP_MODE_TEST) {
    mofei_configure_heap_pool(&mofei_psram_pool, MOFEI_SIM_ALLOC_HEAP_BASE, MOFEI_SIM_ALLOC_HEAP_LIMIT);
  } else {
    mofei_configure_heap_pool(&mofei_psram_pool, MOFEI_SIM_ALLOC_HEAP_BASE, MOFEI_SIM_PSRAM_HEAP_LIMIT);
  }

  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_ALLOC")) {
    fprintf(stderr,
            "[MOFEI-SIM] heap_mode=%s heap_start=0x%08x internal=%u internal_base=0x%08x internal_limit=0x%08x "
            "internal_ext=%u internal_ext_base=0x%08x internal_ext_limit=0x%08x psram=%u psram_base=0x%08x "
            "psram_limit=0x%08x\n",
            mofei_sim_heap_mode_name(mofei_sim_heap_mode), mofei_sim_addrs.heap_start_addr, mofei_internal_pool.total,
            mofei_internal_pool.base, mofei_internal_pool.limit, mofei_internal_pool_ext.total,
            mofei_internal_pool_ext.base, mofei_internal_pool_ext.limit, mofei_psram_pool.total, mofei_psram_pool.base,
            mofei_psram_pool.limit);
  }
  mofei_sim_heap_initialized = true;
}

static uint32_t mofei_skip_fn_common(CPUXtensaState* env, uint32_t* out_ret_addr, unsigned* out_callinc);

typedef struct MofeiTouchSample {
  uint16_t x;
  uint16_t y;
  uint8_t action;
  uint8_t touch_id;
} MofeiTouchSample;

typedef struct MofeiButtonSample {
  uint8_t button_id;
  bool released;
} MofeiButtonSample;

#define MOFEI_TOUCH_QUEUE_CAP 16
#define MOFEI_BUTTON_QUEUE_CAP 16
#define MOFEI_PANDA_DEBUG_MAILBOX_BYTES 4096

static MofeiTouchSample g_touch_queue[MOFEI_TOUCH_QUEUE_CAP];
static unsigned g_touch_queue_head;
static unsigned g_touch_queue_count;
static MofeiButtonSample g_button_queue[MOFEI_BUTTON_QUEUE_CAP];
static unsigned g_button_queue_head;
static unsigned g_button_queue_count;

void mofeiSimulatorTouchQueue(uint16_t x, uint16_t y, uint8_t action, uint8_t touch_id) {
  if (x >= 480) x = 479;
  if (y >= 800) y = 799;
  if (g_touch_queue_count == MOFEI_TOUCH_QUEUE_CAP) {
    g_touch_queue_head = (g_touch_queue_head + 1) % MOFEI_TOUCH_QUEUE_CAP;
    g_touch_queue_count--;
  }
  unsigned tail = (g_touch_queue_head + g_touch_queue_count) % MOFEI_TOUCH_QUEUE_CAP;
  g_touch_queue[tail] = (MofeiTouchSample){x, y, action, touch_id};
  g_touch_queue_count++;
  fprintf(stderr, "[MOFEI-SIM] touch_queue raw=%u,%u action=%u touch_id=%u depth=%u\n", x, y, action, touch_id,
          g_touch_queue_count);
}

static void mofeiSimulatorButtonQueue(uint8_t button_id, bool released) {
  if (button_id > 6) {
    return;
  }
  if (g_button_queue_count == MOFEI_BUTTON_QUEUE_CAP) {
    g_button_queue_head = (g_button_queue_head + 1) % MOFEI_BUTTON_QUEUE_CAP;
    g_button_queue_count--;
  }
  unsigned tail = (g_button_queue_head + g_button_queue_count) % MOFEI_BUTTON_QUEUE_CAP;
  g_button_queue[tail] = (MofeiButtonSample){button_id, released};
  g_button_queue_count++;
  fprintf(stderr, "[MOFEI-SIM] button_queue id=%u released=%d depth=%u\n", button_id, released ? 1 : 0,
          g_button_queue_count);
}

static bool mofeiSimulatorButtonEventWrite(hwaddr addr, uint8_t mask) {
  if (addr < MOFEI_SIM_INTERNAL_DRAM_BASE || addr >= MOFEI_SIM_EXTERNAL_RAM_LIMIT) {
    return false;
  }
  uint8_t cur = 0;
  if (address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &cur, 1) != MEMTX_OK) {
    cur = 0;
  }
  cur |= mask;
  return address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &cur, 1) == MEMTX_OK;
}

/* Write button state bits to the firmware's g_mofeiSimButtonBits variable.
 * Called from ft6336u_inject_button (main thread) when button events arrive
 * over the IPC chardev.  Writes directly to guest DRAM so the firmware's
 * readMofeiButtonState() sees the injected state on its next poll.  Also
 * latches edge events so a fast down/up cannot be lost between firmware polls. */
void mofeiSimulatorButtonStateWrite(uint8_t button_id, uint8_t pressed) {
  /* Button IDs are HalGPIO button ids after the legacy front-key range:
   *   0 = Back, 1 = Confirm, 2 = Left, 3 = Right, 4 = Up, 5 = Down, 6 = Power.
   * Active-low host events are normalized here as pressed = bit set. */
  if (button_id > 6) {
    return;
  }
  const uint8_t mask = (uint8_t)(1u << button_id);
  mofeiSimulatorButtonQueue(button_id, pressed == 0);

  uint8_t cur = 0;
  const hwaddr addr = mofei_sim_addrs.g_mofeiSimButtonBits_addr;
  bool wrote_state = false;
  if (addr >= MOFEI_SIM_INTERNAL_DRAM_BASE && addr < MOFEI_SIM_EXTERNAL_RAM_LIMIT) {
    /* Read-modify-write: read current, update bit, write back.
     * address_space_read/write operate on the default system AS. */
    MemTxResult mr = address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &cur, 1);
    if (mr == MEMTX_OK) {
      cur = pressed ? (cur | mask) : (cur & ~mask);
      wrote_state = address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &cur, 1) == MEMTX_OK;
    }
  }

  const hwaddr event_addr = pressed ? mofei_sim_addrs.g_mofeiSimButtonPressedEvents_addr
                                    : mofei_sim_addrs.g_mofeiSimButtonReleasedEvents_addr;
  bool wrote_event = false;
  if (event_addr != 0) {
    wrote_event = mofeiSimulatorButtonEventWrite(event_addr, mask);
  }
  fprintf(stderr,
          "[MOFEI-SIM] button_state btn=%u pressed=%u bits=0x%02x state_addr=0x%lx state_written=%d event_addr=0x%lx "
          "event_written=%d\n",
          button_id, pressed, cur, (unsigned long)addr, wrote_state ? 1 : 0, (unsigned long)event_addr,
          wrote_event ? 1 : 0);
}

static bool mofeiSimulatorWriteU8(hwaddr addr, uint8_t value) {
  if (addr < MOFEI_SIM_INTERNAL_DRAM_BASE || addr >= MOFEI_SIM_EXTERNAL_RAM_LIMIT) {
    return false;
  }
  return address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &value, sizeof(value)) == MEMTX_OK;
}

static bool mofeiSimulatorWriteU16(hwaddr addr, uint16_t value) {
  if (addr < MOFEI_SIM_INTERNAL_DRAM_BASE || addr >= MOFEI_SIM_EXTERNAL_RAM_LIMIT) {
    return false;
  }
  return address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &value, sizeof(value)) == MEMTX_OK;
}

static bool mofeiSimulatorReadU8(hwaddr addr, uint8_t* value) {
  if (!value || addr < MOFEI_SIM_INTERNAL_DRAM_BASE || addr >= MOFEI_SIM_EXTERNAL_RAM_LIMIT) {
    return false;
  }
  return address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, value, sizeof(*value)) == MEMTX_OK;
}

static bool mofeiSimulatorReadU32(hwaddr addr, uint32_t* value) {
  if (!value || addr < MOFEI_SIM_INTERNAL_DRAM_BASE || addr >= MOFEI_SIM_EXTERNAL_RAM_LIMIT) {
    return false;
  }
  return address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, (uint8_t*)value, sizeof(*value)) ==
         MEMTX_OK;
}

static bool mofeiSimulatorWriteU32(hwaddr addr, uint32_t value) {
  if (addr < MOFEI_SIM_INTERNAL_DRAM_BASE || addr >= MOFEI_SIM_EXTERNAL_RAM_LIMIT) {
    return false;
  }
  return address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, (const uint8_t*)&value,
                             sizeof(value)) == MEMTX_OK;
}

static bool mofeiSimulatorGuestRangeWritable(hwaddr addr, uint32_t len) {
  if (len == 0) {
    return true;
  }
  return (addr >= MOFEI_SIM_INTERNAL_DRAM_BASE && addr < MOFEI_SIM_INTERNAL_DRAM_LIMIT &&
          len <= MOFEI_SIM_INTERNAL_DRAM_LIMIT - addr) ||
         (addr >= MOFEI_SIM_EXTERNAL_RAM_BASE && addr < MOFEI_SIM_EXTERNAL_RAM_LIMIT &&
          len <= MOFEI_SIM_EXTERNAL_RAM_LIMIT - addr);
}

static bool mofeiSimulatorWriteBytes(hwaddr addr, const uint8_t* data, uint32_t len) {
  if (len == 0) {
    return true;
  }
  if (!data || !mofeiSimulatorGuestRangeWritable(addr, len)) {
    return false;
  }
  return address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, data, len) == MEMTX_OK;
}

bool mofeiSimulatorConsoleWrite(const uint8_t* payload, uint32_t len);
bool mofeiSimulatorConsolePollResponse(void);

void mofeiSimulatorTouchEventWrite(uint8_t type, uint16_t x, uint16_t y) {
  if (!mofei_sim_addrs.resolved || mofei_sim_addrs.g_mofeiSimTouchEventPending_addr == 0 ||
      mofei_sim_addrs.g_mofeiSimTouchEventType_addr == 0 || mofei_sim_addrs.g_mofeiSimTouchEventX_addr == 0 ||
      mofei_sim_addrs.g_mofeiSimTouchEventY_addr == 0) {
    return;
  }
  if (type == 0 || type > 6) {
    return;
  }
  if (x >= 480) x = 479;
  if (y >= 800) y = 799;

  const bool ok = mofeiSimulatorWriteU8(mofei_sim_addrs.g_mofeiSimTouchEventType_addr, type) &&
                  mofeiSimulatorWriteU16(mofei_sim_addrs.g_mofeiSimTouchEventX_addr, x) &&
                  mofeiSimulatorWriteU16(mofei_sim_addrs.g_mofeiSimTouchEventY_addr, y) &&
                  mofeiSimulatorWriteU8(mofei_sim_addrs.g_mofeiSimTouchEventPending_addr, 1);
  if (ok) {
    fprintf(stderr, "[MOFEI-SIM] touch_event type=%u x=%u y=%u\n", type, x, y);
  }
}

bool mofeiSimulatorConsoleWrite(const uint8_t* payload, uint32_t len) {
  /* Must equal g_mofeiSimConsoleBuffer's size in
   * apps/panda-os/device/main/SerialConsoleEsp.cpp (and its extern in main.cpp)
   * and the same enum in mofeiSimulatorConsolePollResponse below. */
  enum { MOFEI_SIM_CONSOLE_MAILBOX_BYTES = 1024 };
  if (mofei_sim_addrs.g_mofeiSimConsolePending_addr == 0 || mofei_sim_addrs.g_mofeiSimConsoleLength_addr == 0 ||
      mofei_sim_addrs.g_mofeiSimConsoleBuffer_addr == 0) {
    fprintf(stderr, "[MOFEI-SIM] console mailbox symbols unresolved\n");
    return false;
  }
  if (!payload || len == 0 || len > MOFEI_SIM_CONSOLE_MAILBOX_BYTES ||
      !mofeiSimulatorGuestRangeWritable(mofei_sim_addrs.g_mofeiSimConsoleBuffer_addr, len)) {
    qemu_log_mask(LOG_GUEST_ERROR, "mofei-console: invalid request len=%u\n", len);
    return false;
  }

  uint8_t pending = 0;
  const bool pending_read = mofeiSimulatorReadU8(mofei_sim_addrs.g_mofeiSimConsolePending_addr, &pending);
  if (!pending_read || pending != 0) {
    fprintf(stderr, "[MOFEI-SIM] console mailbox rejected pending_read=%d pending=%u len=%u\n", pending_read ? 1 : 0,
            pending, len);
    return false;
  }

  const bool buffer_written = mofeiSimulatorWriteBytes(mofei_sim_addrs.g_mofeiSimConsoleBuffer_addr, payload, len);
  const bool length_written = mofeiSimulatorWriteU32(mofei_sim_addrs.g_mofeiSimConsoleLength_addr, len);
  const bool pending_written = mofeiSimulatorWriteU8(mofei_sim_addrs.g_mofeiSimConsolePending_addr, 1);
  const bool ok = buffer_written && length_written && pending_written;
  if (!ok) {
    fprintf(stderr, "[MOFEI-SIM] console mailbox write failed buffer=%d length=%d pending=%d len=%u\n",
            buffer_written ? 1 : 0, length_written ? 1 : 0, pending_written ? 1 : 0, len);
  }
  if (ok) fprintf(stderr, "[MOFEI-SIM] console request len=%u\n", len);
  return ok;
}

bool mofeiSimulatorConsolePollResponse(void) {
  /* Keep in sync with mofeiSimulatorConsoleWrite and the firmware buffer. */
  enum { MOFEI_SIM_CONSOLE_MAILBOX_BYTES = 1024, MOFEI_SIM_CONSOLE_RESPONSE_PENDING = 2 };
  if (mofei_sim_addrs.g_mofeiSimConsolePending_addr == 0 || mofei_sim_addrs.g_mofeiSimConsoleLength_addr == 0 ||
      mofei_sim_addrs.g_mofeiSimConsoleBuffer_addr == 0) {
    return false;
  }

  uint8_t pending = 0;
  if (!mofeiSimulatorReadU8(mofei_sim_addrs.g_mofeiSimConsolePending_addr, &pending) ||
      pending != MOFEI_SIM_CONSOLE_RESPONSE_PENDING) {
    return false;
  }

  uint32_t len = 0;
  uint8_t response[MOFEI_SIM_CONSOLE_MAILBOX_BYTES];
  const bool valid = mofeiSimulatorReadU32(mofei_sim_addrs.g_mofeiSimConsoleLength_addr, &len) && len > 0 &&
                     len <= sizeof(response) &&
                     address_space_read(&address_space_memory, mofei_sim_addrs.g_mofeiSimConsoleBuffer_addr,
                                        MEMTXATTRS_UNSPECIFIED, response, len) == MEMTX_OK;
  mofeiSimulatorWriteU8(mofei_sim_addrs.g_mofeiSimConsolePending_addr, 0);
  if (!valid) {
    fprintf(stderr, "[MOFEI-SIM] console response invalid len=%u\n", len);
    return false;
  }

  fwrite(response, 1, len, stderr);
  if (response[len - 1] != '\n') fputc('\n', stderr);
  fflush(stderr);
  return true;
}

bool mofeiSimulatorPandaDebugWriteRequest(const uint8_t* payload, uint32_t len) {
  if (!mofei_sim_addrs.resolved || mofei_sim_addrs.g_mofeiSimPandaDebugRequestPending_addr == 0 ||
      mofei_sim_addrs.g_mofeiSimPandaDebugRequestLength_addr == 0 ||
      mofei_sim_addrs.g_mofeiSimPandaDebugRequestBuffer_addr == 0) {
    qemu_log_mask(LOG_GUEST_ERROR, "mofei-debug: firmware request mailbox symbols are unresolved\n");
    return false;
  }
  if (len > MOFEI_PANDA_DEBUG_MAILBOX_BYTES) {
    qemu_log_mask(LOG_GUEST_ERROR, "mofei-debug: request payload %u exceeds mailbox cap\n", len);
    return false;
  }
  if (!mofeiSimulatorGuestRangeWritable(mofei_sim_addrs.g_mofeiSimPandaDebugRequestBuffer_addr, len)) {
    qemu_log_mask(LOG_GUEST_ERROR, "mofei-debug: request buffer address 0x%08x is outside simulator RAM\n",
                  mofei_sim_addrs.g_mofeiSimPandaDebugRequestBuffer_addr);
    return false;
  }

  uint8_t pending = 0;
  if (!mofeiSimulatorReadU8(mofei_sim_addrs.g_mofeiSimPandaDebugRequestPending_addr, &pending)) {
    return false;
  }
  if (pending != 0) {
    qemu_log_mask(LOG_GUEST_ERROR, "mofei-debug: request mailbox busy, dropping payload len=%u\n", len);
    return false;
  }

  const bool ok = mofeiSimulatorWriteBytes(mofei_sim_addrs.g_mofeiSimPandaDebugRequestBuffer_addr, payload, len) &&
                  mofeiSimulatorWriteU32(mofei_sim_addrs.g_mofeiSimPandaDebugRequestLength_addr, len) &&
                  mofeiSimulatorWriteU8(mofei_sim_addrs.g_mofeiSimPandaDebugRequestPending_addr, 1);
  if (ok) {
    fprintf(stderr, "[MOFEI-SIM] panda_debug request len=%u\n", len);
  }
  return ok;
}

extern void ssd1677_emit_debug_frame(const uint8_t* payload, uint32_t len);
extern void uc8253c_emit_debug_frame(const uint8_t* payload, uint32_t len);
bool mofei_read_guest_memory(uint32_t guest_addr, uint8_t* dest, uint32_t len);

bool mofeiSimulatorPandaDebugPollResponse(void) {
  if (!mofei_sim_addrs.resolved || mofei_sim_addrs.g_mofeiSimPandaDebugResponsePending_addr == 0 ||
      mofei_sim_addrs.g_mofeiSimPandaDebugResponseLength_addr == 0 ||
      mofei_sim_addrs.g_mofeiSimPandaDebugResponseBuffer_addr == 0) {
    return false;
  }

  uint8_t pending = 0;
  if (!mofeiSimulatorReadU8(mofei_sim_addrs.g_mofeiSimPandaDebugResponsePending_addr, &pending) || pending == 0) {
    return false;
  }

  uint32_t len = 0;
  if (!mofeiSimulatorReadU32(mofei_sim_addrs.g_mofeiSimPandaDebugResponseLength_addr, &len)) {
    return false;
  }
  if (len > MOFEI_PANDA_DEBUG_MAILBOX_BYTES ||
      !mofeiSimulatorGuestRangeWritable(mofei_sim_addrs.g_mofeiSimPandaDebugResponseBuffer_addr, len)) {
    qemu_log_mask(LOG_GUEST_ERROR, "mofei-debug: invalid response len=%u buffer=0x%08x\n", len,
                  mofei_sim_addrs.g_mofeiSimPandaDebugResponseBuffer_addr);
    mofeiSimulatorWriteU8(mofei_sim_addrs.g_mofeiSimPandaDebugResponsePending_addr, 0);
    return false;
  }

  uint8_t* response = g_malloc0(len > 0 ? len : 1);
  if (len > 0) {
    mofei_read_guest_memory(mofei_sim_addrs.g_mofeiSimPandaDebugResponseBuffer_addr, response, len);
  }
  mofeiSimulatorWriteU8(mofei_sim_addrs.g_mofeiSimPandaDebugResponsePending_addr, 0);

  if (mofei_sim_board_is_s37uc()) {
    uc8253c_emit_debug_frame(response, len);
  } else {
    ssd1677_emit_debug_frame(response, len);
  }
  fprintf(stderr, "[MOFEI-SIM] panda_debug response len=%u\n", len);
  g_free(response);
  return true;
}

static uint32_t mofei_bump_align_size(uint32_t size) {
  if (size == 0) {
    size = 64;
  }
  if (size > MOFEI_SIM_ALLOC_HEAP_LIMIT - MOFEI_SIM_ALLOC_HEAP_BASE) {
    return 0;
  }
  return (size + 63u) & ~63u;
}

static MofeiBumpAllocRecord* mofei_reserve_bump_alloc_record(void) {
  for (uint32_t i = 0; i < mofei_bump_alloc_record_count; i++) {
    if (mofei_bump_alloc_records[i].aligned_size == 0) {
      return &mofei_bump_alloc_records[i];
    }
  }
  if (mofei_bump_alloc_record_count >= MOFEI_BUMP_ALLOC_RECORD_MAX) {
    return NULL;
  }
  return &mofei_bump_alloc_records[mofei_bump_alloc_record_count++];
}

static MofeiBumpAllocRecord* mofei_record_bump_alloc(uint32_t addr, uint32_t requested_size, uint32_t aligned_size,
                                                     uint32_t caps, MofeiSimHeapClass heap_class) {
  if (addr == 0) {
    return NULL;
  }
  MofeiBumpAllocRecord* record = mofei_reserve_bump_alloc_record();
  if (!record) {
    return NULL;
  }
  *record = (MofeiBumpAllocRecord){addr, requested_size, aligned_size, caps, heap_class, true};
  return record;
}

static MofeiBumpAllocRecord* mofei_record_free_bump_span(uint32_t addr, uint32_t aligned_size, uint32_t caps,
                                                         MofeiSimHeapClass heap_class) {
  if (addr == 0 || aligned_size == 0) {
    return NULL;
  }
  MofeiBumpAllocRecord* record = mofei_reserve_bump_alloc_record();
  if (!record) {
    return NULL;
  }
  *record = (MofeiBumpAllocRecord){addr, aligned_size, aligned_size, caps, heap_class, false};
  return record;
}

static bool mofei_bump_record_is_free(const MofeiBumpAllocRecord* record) {
  return record && !record->active && record->aligned_size > 0;
}

static MofeiBumpAllocRecord* mofei_find_bump_alloc_record(uint32_t addr) {
  for (uint32_t i = 0; i < mofei_bump_alloc_record_count; i++) {
    if (mofei_bump_alloc_records[i].addr == addr && mofei_bump_alloc_records[i].active) {
      return &mofei_bump_alloc_records[i];
    }
  }
  for (uint32_t i = 0; i < mofei_bump_alloc_record_count; i++) {
    if (mofei_bump_alloc_records[i].addr == addr && mofei_bump_alloc_records[i].aligned_size > 0) {
      return &mofei_bump_alloc_records[i];
    }
  }
  return NULL;
}

static uint32_t mofei_lookup_bump_alloc_size(uint32_t addr) {
  MofeiBumpAllocRecord* record = mofei_find_bump_alloc_record(addr);
  return record && record->active ? record->requested_size : 0;
}

/* Mirrors ESP-IDF heap capability bits used by firmware heap preflights. */
#define MOFEI_MALLOC_CAP_8BIT (1u << 2)
#define MOFEI_MALLOC_CAP_DMA (1u << 3)
#define MOFEI_MALLOC_CAP_SPIRAM (1u << 10)
#define MOFEI_MALLOC_CAP_INTERNAL (1u << 11)
#define MOFEI_MALLOC_CAP_DEFAULT (1u << 12)

static MofeiSimHeapPool* mofei_pool_for_caps(uint32_t caps) {
  mofei_sim_heap_init();
  if ((caps & MOFEI_MALLOC_CAP_SPIRAM) != 0 && (caps & (MOFEI_MALLOC_CAP_INTERNAL | MOFEI_MALLOC_CAP_DMA)) == 0) {
    return &mofei_psram_pool;
  }
  return &mofei_internal_pool;
}

static MofeiSimHeapPool* mofei_next_internal_pool(const MofeiSimHeapPool* pool) {
  return pool == &mofei_internal_pool && mofei_internal_pool_ext.total > 0 ? &mofei_internal_pool_ext : NULL;
}

static bool mofei_caps_use_default_malloc_policy(uint32_t caps) { return (caps & MOFEI_MALLOC_CAP_DEFAULT) != 0; }

static uint32_t mofei_record_end_addr(const MofeiBumpAllocRecord* record) {
  if (!record || record->aligned_size > UINT32_MAX - record->addr) {
    return 0;
  }
  return record->addr + record->aligned_size;
}

static MofeiBumpAllocRecord* mofei_find_reusable_bump_record(MofeiSimHeapClass heap_class, uint32_t aligned_size) {
  MofeiBumpAllocRecord* best = NULL;
  for (uint32_t i = 0; i < mofei_bump_alloc_record_count; i++) {
    MofeiBumpAllocRecord* record = &mofei_bump_alloc_records[i];
    if (mofei_bump_record_is_free(record) && record->heap_class == heap_class && record->aligned_size >= aligned_size &&
        (!best || record->aligned_size < best->aligned_size)) {
      best = record;
    }
  }
  return best;
}

static MofeiBumpAllocRecord* mofei_find_reusable_bump_record_in_pool(const MofeiSimHeapPool* pool,
                                                                     uint32_t aligned_size) {
  MofeiBumpAllocRecord* best = NULL;
  if (!pool) {
    return NULL;
  }
  for (uint32_t i = 0; i < mofei_bump_alloc_record_count; i++) {
    MofeiBumpAllocRecord* record = &mofei_bump_alloc_records[i];
    if (mofei_bump_record_is_free(record) && record->heap_class == pool->heap_class && record->addr >= pool->base &&
        record->addr < pool->limit && record->aligned_size >= aligned_size &&
        (!best || record->aligned_size < best->aligned_size)) {
      best = record;
    }
  }
  return best;
}

static MofeiBumpAllocRecord* mofei_split_reusable_bump_record(MofeiBumpAllocRecord* record, uint32_t aligned_size,
                                                              uint32_t caps, MofeiBumpAllocRecord** out_tail) {
  if (out_tail) {
    *out_tail = NULL;
  }
  if (!mofei_bump_record_is_free(record) || aligned_size == 0 || record->aligned_size <= aligned_size) {
    return record;
  }

  const uint32_t tail_size = record->aligned_size - aligned_size;
  const uint32_t tail_addr = record->addr + aligned_size;
  MofeiBumpAllocRecord* tail = mofei_record_free_bump_span(tail_addr, tail_size, caps, record->heap_class);
  if (!tail) {
    return record;
  }
  record->aligned_size = aligned_size;
  if (out_tail) {
    *out_tail = tail;
  }
  return record;
}

static void mofei_clear_free_bump_record(MofeiBumpAllocRecord* record) {
  if (!record) {
    return;
  }
  record->requested_size = 0;
  record->aligned_size = 0;
  record->caps = 0;
  record->active = false;
}

static MofeiBumpAllocRecord* mofei_coalesce_free_record(MofeiBumpAllocRecord* record) {
  if (!mofei_bump_record_is_free(record)) {
    return record;
  }

  bool merged = true;
  while (merged) {
    merged = false;
    for (uint32_t i = 0; i < mofei_bump_alloc_record_count; i++) {
      MofeiBumpAllocRecord* other = &mofei_bump_alloc_records[i];
      if (other == record || !mofei_bump_record_is_free(other) || other->heap_class != record->heap_class) {
        continue;
      }

      const uint32_t record_end = mofei_record_end_addr(record);
      const uint32_t other_end = mofei_record_end_addr(other);
      if (record_end != 0 && record_end == other->addr) {
        record->aligned_size += other->aligned_size;
        record->requested_size = record->aligned_size;
        mofei_clear_free_bump_record(other);
        merged = true;
        break;
      }
      if (other_end != 0 && other_end == record->addr) {
        other->aligned_size += record->aligned_size;
        other->requested_size = other->aligned_size;
        mofei_clear_free_bump_record(record);
        record = other;
        merged = true;
        break;
      }
    }
  }
  return record;
}

static uint32_t mofei_pool_unallocated_bytes(const MofeiSimHeapPool* pool) {
  if (!pool || pool->ptr >= pool->limit) {
    return 0;
  }
  return pool->limit - pool->ptr;
}

static uint32_t mofei_pool_free_bytes(const MofeiSimHeapPool* pool) {
  uint32_t free_bytes = mofei_pool_unallocated_bytes(pool);
  if (!pool) {
    return free_bytes;
  }
  for (uint32_t i = 0; i < mofei_bump_alloc_record_count; i++) {
    const MofeiBumpAllocRecord* record = &mofei_bump_alloc_records[i];
    if (mofei_bump_record_is_free(record) && record->heap_class == pool->heap_class && record->addr >= pool->base &&
        record->addr < pool->limit) {
      free_bytes += record->aligned_size;
    }
  }
  return free_bytes;
}

static uint32_t mofei_pool_largest_free_block(const MofeiSimHeapPool* pool) {
  uint32_t largest = mofei_pool_unallocated_bytes(pool);
  if (!pool) {
    return largest;
  }
  for (uint32_t i = 0; i < mofei_bump_alloc_record_count; i++) {
    const MofeiBumpAllocRecord* record = &mofei_bump_alloc_records[i];
    if (mofei_bump_record_is_free(record) && record->heap_class == pool->heap_class && record->addr >= pool->base &&
        record->addr < pool->limit && record->aligned_size > largest) {
      largest = record->aligned_size;
    }
  }
  return largest;
}

static MofeiSimHeapPool* mofei_resolve_alloc_pool(MofeiSimHeapPool* preferred_pool, uint32_t aligned_size) {
  for (MofeiSimHeapPool* pool = preferred_pool; pool; pool = mofei_next_internal_pool(pool)) {
    if (mofei_find_reusable_bump_record_in_pool(pool, aligned_size) ||
        aligned_size <= mofei_pool_unallocated_bytes(pool)) {
      return pool;
    }
  }
  return preferred_pool;
}

static uint32_t mofei_bump_alloc(uint32_t size, uint32_t caps) {
  static uint32_t allocation_failure_log_count;
  const uint32_t requested_size = size;
  size = mofei_bump_align_size(size);
  MofeiSimHeapPool* pool = mofei_pool_for_caps(caps);
  pool = mofei_resolve_alloc_pool(pool, size);
  if (size == 0 || !pool || pool->ptr < pool->base || pool->ptr > pool->limit) {
    if (allocation_failure_log_count++ < 64) {
      fprintf(stderr,
              "[MALLOC] exhausted/invalid pool=%s caps=0x%08x requested=%u aligned=%u ptr=0x%08x limit=0x%08x\n",
              pool ? pool->label : "?", caps, requested_size, size, pool ? pool->ptr : 0, pool ? pool->limit : 0);
    }
    return 0;
  }
  MofeiBumpAllocRecord* record = mofei_find_reusable_bump_record_in_pool(pool, size);
  const bool reused_record = record != NULL;
  MofeiBumpAllocRecord* split_tail = NULL;
  const uint32_t original_record_aligned_size = record ? record->aligned_size : 0;
  record = mofei_split_reusable_bump_record(record, size, caps, &split_tail);
  uint32_t result = record ? record->addr : pool->ptr;
  if (record) {
    record->requested_size = requested_size;
    record->caps = caps;
    record->active = true;
  } else {
    if (size > pool->limit - pool->ptr) {
      if (allocation_failure_log_count++ < 64) {
        fprintf(stderr,
                "[MALLOC] exhausted/invalid pool=%s caps=0x%08x requested=%u aligned=%u ptr=0x%08x limit=0x%08x\n",
                pool->label, caps, requested_size, size, pool->ptr, pool->limit);
      }
      return 0;
    }
    record = mofei_record_bump_alloc(result, requested_size, size, caps, pool->heap_class);
    if (!record) {
      if (allocation_failure_log_count++ < 64) {
        fprintf(stderr, "[MALLOC] record table full pool=%s caps=0x%08x requested=%u aligned=%u result=0x%08x\n",
                pool->label, caps, requested_size, size, result);
      }
      return 0;
    }
    pool->ptr += size;
  }
  mofei_bump_count++;

  /* QEMU's allocator bypass returns addresses from the DCACHE/PSRAM range.
   * The firmware expects malloc/calloc buffers to start zeroed in several
   * boot-time data structures (ArduinoJson pools, std::string storage,
   * display framebuffers that are immediately memset later).  Keep the
   * simulator heap deterministic by clearing the guest backing memory here;
   * this also makes direct framebuffer reads see CPU writes against the same
   * initialized backing store. */
  uint8_t* zero = g_malloc0(size);
  MemTxResult mr = address_space_write(&address_space_memory, result, MEMTXATTRS_UNSPECIFIED, zero, size);
  g_free(zero);
  if (mr != MEMTX_OK) {
    fprintf(stderr, "[MALLOC] zero-write failed pool=%s requested=%u aligned=%u result=0x%08x end=0x%08x status=%d\n",
            pool->label, requested_size, size, result, result + size, mr);
    if (record) {
      record->active = false;
      if (split_tail) {
        record->aligned_size = original_record_aligned_size;
        mofei_clear_free_bump_record(split_tail);
      }
    }
    if (!reused_record && pool->ptr == result + size) {
      pool->ptr = result;
    }
    return 0;
  }

  if (mofei_bump_count <= 20) {
    fprintf(stderr, "[MALLOC] #%u pool=%s caps=0x%08x requested=%u aligned=%u result=0x%08x end=0x%08x free=%u\n",
            mofei_bump_count, pool->label, caps, requested_size, size, result, result + size,
            mofei_pool_free_bytes(pool));
  }
  if (requested_size == 48000u && (caps & MOFEI_MALLOC_CAP_SPIRAM) != 0) {
    mofei_murphy_last_framebuffer_data = result;
  }
  return result;
}

static uint32_t mofei_bump_malloc_default(uint32_t size, uint32_t caps) {
  if (!mofei_caps_use_default_malloc_policy(caps)) {
    return mofei_bump_alloc(size, caps);
  }
  const uint32_t internal_result = mofei_bump_alloc(size, MOFEI_MALLOC_CAP_8BIT);
  if (internal_result != 0) {
    return internal_result;
  }
  return mofei_bump_alloc(size, MOFEI_MALLOC_CAP_SPIRAM | MOFEI_MALLOC_CAP_8BIT);
}

static bool mofei_caps_are_8bit_only(uint32_t caps) { return caps == MOFEI_MALLOC_CAP_8BIT; }

static uint32_t mofei_bump_heap_caps_alloc(uint32_t size, uint32_t caps) {
  if (mofei_caps_use_default_malloc_policy(caps)) {
    return mofei_bump_malloc_default(size, caps);
  }
  if (!mofei_caps_are_8bit_only(caps)) {
    return mofei_bump_alloc(size, caps);
  }
  const uint32_t internal_result = mofei_bump_alloc(size, caps);
  if (internal_result != 0) {
    return internal_result;
  }
  return mofei_bump_alloc(size, MOFEI_MALLOC_CAP_SPIRAM | MOFEI_MALLOC_CAP_8BIT);
}

static bool mofei_bump_free(uint32_t addr) {
  static uint32_t ignored_free_log_count;
  static uint32_t successful_free_log_count;
  mofei_sim_heap_init();
  if (addr == 0) {
    return true;
  }
  MofeiBumpAllocRecord* record = mofei_find_bump_alloc_record(addr);
  if (!record || !record->active) {
    if (mofei_trace_enabled("MOFEI_SIM_DEBUG_ALLOC") && ignored_free_log_count++ < 128) {
      fprintf(stderr, "[FREE] ignored unknown/inactive ptr=0x%08x\n", addr);
    }
    return false;
  }
  record->active = false;
  record->requested_size = record->aligned_size;
  record = mofei_coalesce_free_record(record);
  MofeiSimHeapPool* pool =
      record->heap_class == MOFEI_SIM_HEAP_PSRAM
          ? &mofei_psram_pool
          : (addr >= mofei_internal_pool_ext.base && addr < mofei_internal_pool_ext.limit ? &mofei_internal_pool_ext
                                                                                          : &mofei_internal_pool);
  bool trimmed = true;
  while (trimmed) {
    trimmed = false;
    for (uint32_t i = 0; i < mofei_bump_alloc_record_count; i++) {
      MofeiBumpAllocRecord* top = &mofei_bump_alloc_records[i];
      if (!mofei_bump_record_is_free(top) || top->heap_class != pool->heap_class || top->addr < pool->base ||
          top->addr >= pool->limit || mofei_record_end_addr(top) != pool->ptr) {
        continue;
      }
      pool->ptr = top->addr;
      mofei_clear_free_bump_record(top);
      trimmed = true;
      break;
    }
  }
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_ALLOC") && successful_free_log_count++ < 256) {
    fprintf(stderr, "[FREE] pool=%s ptr=0x%08x requested=%u aligned=%u free=%u largest=%u\n", pool->label, addr,
            record ? record->requested_size : 0, record ? record->aligned_size : 0, mofei_pool_free_bytes(pool),
            mofei_pool_largest_free_block(pool));
  }
  return true;
}

static uint32_t mofei_bump_realloc(uint32_t old_addr, uint32_t size, uint32_t caps) {
  if (size == 0) {
    mofei_bump_free(old_addr);
    return 0;
  }
  if (old_addr == 0) {
    return mofei_bump_heap_caps_alloc(size, caps);
  }
  if (caps == 0) {
    MofeiBumpAllocRecord* old_record = mofei_find_bump_alloc_record(old_addr);
    if (old_record) {
      caps = old_record->caps;
    }
  }
  const uint32_t result = mofei_bump_heap_caps_alloc(size, caps);
  if (result == 0 || old_addr == 0) {
    return result;
  }

  MofeiBumpAllocRecord* old_record = mofei_find_bump_alloc_record(old_addr);
  const uint32_t old_size = old_record ? old_record->requested_size : 0;
  const uint32_t copy_size = old_size < size ? old_size : size;
  if (copy_size == 0) {
    mofei_bump_free(old_addr);
    return result;
  }

  uint8_t* buffer = g_malloc(copy_size);
  MemTxResult mr = address_space_read(&address_space_memory, old_addr, MEMTXATTRS_UNSPECIFIED, buffer, copy_size);
  if (mr == MEMTX_OK) {
    mr = address_space_write(&address_space_memory, result, MEMTXATTRS_UNSPECIFIED, buffer, copy_size);
  }
  g_free(buffer);
  if (mr != MEMTX_OK) {
    fprintf(stderr, "[REALLOC] copy failed old=0x%08x new=0x%08x old_size=%u requested=%u copied=%u status=%d\n",
            old_addr, result, old_size, size, copy_size, mr);
    return 0;
  }
  mofei_bump_free(old_addr);
  return result;
}

static uint32_t mofei_mul_size_or_zero(uint32_t a, uint32_t b) {
  if (a == 0 || b == 0) {
    return 0;
  }
  if (a > UINT32_MAX / b) {
    return 0;
  }
  return a * b;
}

static uint32_t mofei_arg_value(uint32_t arg, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
  switch (arg) {
    case MOFEI_ALLOC_ARG_DEFAULT_POLICY:
      return MOFEI_MALLOC_CAP_DEFAULT;
    case MOFEI_ALLOC_ARG_A2:
      return a2;
    case MOFEI_ALLOC_ARG_A3:
      return a3;
    case MOFEI_ALLOC_ARG_A4:
      return a4;
    case MOFEI_ALLOC_ARG_A5:
      return a5;
    default:
      return 0;
  }
}

static uint32_t mofei_logical_reg_or_zero(CPUXtensaState* env, unsigned reg) { return reg < 16 ? env->regs[reg] : 0; }

static uint32_t mofei_alloc_size_from_args(uint32_t size_kind, uint32_t a2, uint32_t a3, uint32_t a4) {
  switch (size_kind) {
    case MOFEI_ALLOC_SIZE_A2:
      return a2;
    case MOFEI_ALLOC_SIZE_A3:
      return a3;
    case MOFEI_ALLOC_SIZE_A4:
      return a4;
    case MOFEI_ALLOC_SIZE_MUL_A2_A3:
      return mofei_mul_size_or_zero(a2, a3);
    case MOFEI_ALLOC_SIZE_MUL_A3_A4:
      return mofei_mul_size_or_zero(a3, a4);
    case MOFEI_ALLOC_SIZE_FREE:
      return 0;
    default:
      return a2;
  }
}

void HELPER(mofei_bump_malloc)(CPUXtensaState* env) {
  /* Read size from env->regs[2] (architectural register), not phys_regs.
   * The RETW helper will call xtensa_sync_phys_from_window which copies
   * env->regs[] → phys_regs[], so writing back to env->regs[2] ensures
   * the value survives the window rotation. */
  uint32_t result = mofei_bump_alloc(env->regs[2], MOFEI_MALLOC_CAP_8BIT);
  env->regs[2] = result;
}

uint32_t HELPER(mofei_bump_alloc_kind)(CPUXtensaState* env, uint32_t size_kind, uint32_t old_ptr_arg, uint32_t caps_arg,
                                       uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
  const uint32_t size = mofei_alloc_size_from_args(size_kind, a2, a3, a4);
  const uint32_t old_addr = mofei_arg_value(old_ptr_arg, a2, a3, a4, a5);
  if (size_kind == MOFEI_ALLOC_SIZE_FREE) {
    mofei_bump_free(old_addr);
    env->regs[2] = 0;
    return 0;
  }
  uint32_t caps = mofei_arg_value(caps_arg, a2, a3, a4, a5);
  if (caps == 0) {
    caps = MOFEI_MALLOC_CAP_8BIT;
  }
  const uint32_t result = old_ptr_arg != MOFEI_ALLOC_ARG_NONE ? mofei_bump_realloc(old_addr, size, caps)
                                                              : mofei_bump_heap_caps_alloc(size, caps);
  env->regs[2] = result;
  return result;
}

uint32_t HELPER(mofei_skip_fn_bump_malloc)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  unsigned arg_reg = callinc * 4 + 2;
  uint32_t result = mofei_bump_alloc(env->regs[arg_reg], MOFEI_MALLOC_CAP_8BIT);
  env->regs[arg_reg] = result;

  static int skip_malloc_log = 0;
  if (skip_malloc_log < 20) {
    fprintf(stderr, "[SKIP-MALLOC] raw=0x%08x clean=0x%08x callinc=%u arg_ret_reg=%u ret=0x%08x\n", raw_addr,
            clean_addr, callinc, arg_reg, result);
    skip_malloc_log++;
  }

  return clean_addr;
}

uint32_t HELPER(mofei_skip_fn_wrapped_malloc)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  unsigned arg_reg = callinc * 4 + 2;
  uint32_t result = mofei_bump_malloc_default(env->regs[arg_reg], MOFEI_MALLOC_CAP_DEFAULT);
  env->regs[arg_reg] = result;

  static int skip_wrapped_malloc_log = 0;
  if (skip_wrapped_malloc_log < 20) {
    fprintf(stderr, "[SKIP-WRAP-MALLOC] raw=0x%08x clean=0x%08x callinc=%u arg_ret_reg=%u ret=0x%08x\n", raw_addr,
            clean_addr, callinc, arg_reg, result);
    skip_wrapped_malloc_log++;
  }

  return clean_addr;
}

uint32_t HELPER(mofei_skip_fn_bump_alloc_kind)(CPUXtensaState* env, uint32_t size_kind, uint32_t old_ptr_arg,
                                               uint32_t caps_arg) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned base_reg = callinc * 4;
  const uint32_t a2 = mofei_logical_reg_or_zero(env, base_reg + 2);
  const uint32_t a3 = mofei_logical_reg_or_zero(env, base_reg + 3);
  const uint32_t a4 = mofei_logical_reg_or_zero(env, base_reg + 4);
  const uint32_t a5 = mofei_logical_reg_or_zero(env, base_reg + 5);
  const uint32_t size = mofei_alloc_size_from_args(size_kind, a2, a3, a4);
  const uint32_t old_addr = mofei_arg_value(old_ptr_arg, a2, a3, a4, a5);
  if (size_kind == MOFEI_ALLOC_SIZE_FREE) {
    mofei_bump_free(old_addr);
    env->regs[base_reg + 2] = 0;
    return clean_addr;
  }
  uint32_t caps = mofei_arg_value(caps_arg, a2, a3, a4, a5);
  if (caps == 0) {
    caps = MOFEI_MALLOC_CAP_8BIT;
  }
  const uint32_t result = old_ptr_arg != MOFEI_ALLOC_ARG_NONE ? mofei_bump_realloc(old_addr, size, caps)
                                                              : mofei_bump_heap_caps_alloc(size, caps);
  env->regs[base_reg + 2] = result;

  static int skip_alloc_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_ALLOC") && skip_alloc_log < 64) {
    fprintf(stderr,
            "[SKIP-ALLOC] raw=0x%08x clean=0x%08x callinc=%u ret_reg=%u size_kind=%u old_arg=%u caps_arg=%u "
            "size=%u caps=0x%08x ret=0x%08x\n",
            raw_addr, clean_addr, callinc, base_reg + 2, size_kind, old_ptr_arg, caps_arg, size, caps, result);
    skip_alloc_log++;
  }

  return clean_addr;
}

uint32_t HELPER(mofei_skip_fn_atomic_exchange_1)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned base_reg = callinc * 4;
  const uint32_t addr = mofei_logical_reg_or_zero(env, base_reg + 2);
  const uint8_t replacement = (uint8_t)mofei_logical_reg_or_zero(env, base_reg + 3);
  uint8_t previous = 0;

  if (mofei_guest_addr_in_sim_ram(addr)) {
    previous = (uint8_t)cpu_ldub_data(env, addr);
    cpu_stb_data(env, addr, replacement);
  } else {
    if (address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &previous, sizeof(previous)) ==
        MEMTX_OK) {
      address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &replacement, sizeof(replacement));
    }
  }

  env->regs[base_reg + 2] = previous;

  static int atomic_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_ATOMIC") && atomic_log < 64) {
    fprintf(stderr, "[SKIP-ATOMIC-XCHG1] raw=0x%08x clean=0x%08x callinc=%u addr=0x%08x old=%u new=%u ret_reg=%u\n",
            raw_addr, clean_addr, callinc, addr, previous, replacement, base_reg + 2);
    atomic_log++;
  }

  return clean_addr;
}

uint32_t HELPER(mofei_skip_fn_atomic_fetch_add_2)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned base_reg = callinc * 4;
  const uint32_t addr = mofei_logical_reg_or_zero(env, base_reg + 2);
  const uint16_t increment = (uint16_t)mofei_logical_reg_or_zero(env, base_reg + 3);
  uint16_t previous = 0;

  if (mofei_guest_addr_in_sim_ram(addr)) {
    previous = cpu_lduw_data(env, addr);
    cpu_stw_data(env, addr, (uint16_t)(previous + increment));
  } else if (address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &previous, sizeof(previous)) ==
             MEMTX_OK) {
    const uint16_t replacement = (uint16_t)(previous + increment);
    address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &replacement, sizeof(replacement));
  }

  env->regs[base_reg + 2] = previous;
  return clean_addr;
}

uint32_t HELPER(mofei_skip_fn_atomic_fetch_add_4)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned base_reg = callinc * 4;
  const uint32_t addr = mofei_logical_reg_or_zero(env, base_reg + 2);
  const uint32_t increment = mofei_logical_reg_or_zero(env, base_reg + 3);
  uint32_t previous = 0;

  if (mofei_read_guest_data_u32_checked(env, addr, &previous)) {
    mofei_write_guest_data_u32(env, addr, previous + increment);
  }

  env->regs[base_reg + 2] = previous;
  return clean_addr;
}

uint32_t HELPER(mofei_skip_fn_atomic_compare_exchange_1)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned base_reg = callinc * 4;
  const uint32_t addr = mofei_logical_reg_or_zero(env, base_reg + 2);
  const uint32_t expected_addr = mofei_logical_reg_or_zero(env, base_reg + 3);
  const uint8_t desired = (uint8_t)mofei_logical_reg_or_zero(env, base_reg + 4);
  uint8_t observed = 0;
  uint8_t expected = 0;
  bool read_observed = false;
  bool read_expected = false;
  bool exchanged = false;

  if (mofei_guest_addr_in_sim_ram(addr)) {
    observed = (uint8_t)cpu_ldub_data(env, addr);
    read_observed = true;
  } else {
    read_observed = address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &observed,
                                       sizeof(observed)) == MEMTX_OK;
  }

  if (mofei_guest_addr_in_sim_ram(expected_addr)) {
    expected = (uint8_t)cpu_ldub_data(env, expected_addr);
    read_expected = true;
  } else {
    read_expected = address_space_read(&address_space_memory, expected_addr, MEMTXATTRS_UNSPECIFIED, &expected,
                                       sizeof(expected)) == MEMTX_OK;
  }

  if (read_observed && read_expected && observed == expected) {
    if (mofei_guest_addr_in_sim_ram(addr)) {
      cpu_stb_data(env, addr, desired);
      exchanged = true;
    } else {
      exchanged = address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &desired, sizeof(desired)) ==
                  MEMTX_OK;
    }
  } else if (read_observed) {
    if (mofei_guest_addr_in_sim_ram(expected_addr)) {
      cpu_stb_data(env, expected_addr, observed);
    } else {
      address_space_write(&address_space_memory, expected_addr, MEMTXATTRS_UNSPECIFIED, &observed, sizeof(observed));
    }
  }

  env->regs[base_reg + 2] = exchanged ? 1 : 0;

  static int atomic_compare_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_ATOMIC") && atomic_compare_log < 64) {
    fprintf(stderr,
            "[SKIP-ATOMIC-CMPXCHG1] raw=0x%08x clean=0x%08x callinc=%u addr=0x%08x expected_addr=0x%08x "
            "observed=%u expected=%u desired=%u exchanged=%d ret_reg=%u\n",
            raw_addr, clean_addr, callinc, addr, expected_addr, observed, expected, desired, exchanged ? 1 : 0,
            base_reg + 2);
    atomic_compare_log++;
  }

  return clean_addr;
}

uint32_t HELPER(mofei_skip_fn_atomic_compare_exchange_4)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned base_reg = callinc * 4;
  const uint32_t addr = mofei_logical_reg_or_zero(env, base_reg + 2);
  const uint32_t expected_addr = mofei_logical_reg_or_zero(env, base_reg + 3);
  const uint32_t desired = mofei_logical_reg_or_zero(env, base_reg + 4);
  uint32_t observed = 0;
  uint32_t expected = 0;
  bool read_observed = false;
  bool read_expected = false;
  bool exchanged = false;

  read_observed = mofei_read_guest_data_u32_checked(env, addr, &observed);
  read_expected = mofei_read_guest_data_u32_checked(env, expected_addr, &expected);

  if (read_observed && read_expected && observed == expected) {
    exchanged = mofei_write_guest_data_u32(env, addr, desired);
  } else if (read_observed) {
    mofei_write_guest_data_u32(env, expected_addr, observed);
  }

  env->regs[base_reg + 2] = exchanged ? 1 : 0;

  static int atomic_compare4_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_ATOMIC") && atomic_compare4_log < 64) {
    fprintf(stderr,
            "[SKIP-ATOMIC-CMPXCHG4] raw=0x%08x clean=0x%08x callinc=%u addr=0x%08x expected_addr=0x%08x "
            "observed=%u expected=%u desired=%u exchanged=%d ret_reg=%u\n",
            raw_addr, clean_addr, callinc, addr, expected_addr, observed, expected, desired, exchanged ? 1 : 0,
            base_reg + 2);
    atomic_compare4_log++;
  }

  return clean_addr;
}

uint32_t HELPER(mofei_s32c1i)(CPUXtensaState* env, uint32_t addr, uint32_t compare, uint32_t replacement) {
  uint32_t previous = 0;
  const bool read_previous = mofei_read_guest_data_u32_checked(env, addr, &previous);

  if (read_previous && previous == compare) {
    mofei_write_guest_data_u32(env, addr, replacement);
  }

  static int s32c1i_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_ATOMIC") && s32c1i_log < 64) {
    fprintf(stderr, "[S32C1I] addr=0x%08x compare=0x%08x old=0x%08x new=0x%08x exchanged=%d\n", addr, compare, previous,
            replacement, read_previous && previous == compare ? 1 : 0);
    s32c1i_log++;
  }

  return previous;
}

uint32_t HELPER(mofei_s32ex)(CPUXtensaState* env, uint32_t addr, uint32_t compare, uint32_t replacement) {
  uint32_t previous = 0;
  const bool read_previous = mofei_read_guest_data_u32_checked(env, addr, &previous);

  if (read_previous && previous == compare) {
    mofei_write_guest_data_u32(env, addr, replacement);
  }

  static int s32ex_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_ATOMIC") && s32ex_log < 64) {
    fprintf(stderr, "[S32EX] addr=0x%08x compare=0x%08x old=0x%08x new=0x%08x exchanged=%d\n", addr, compare, previous,
            replacement, read_previous && previous == compare ? 1 : 0);
    s32ex_log++;
  }

  return previous;
}

static uint32_t mofei_heap_pool_query_value(const MofeiSimHeapPool* pool, bool total_size, bool largest_block) {
  if (!pool) {
    return 0;
  }
  if (total_size) {
    return pool->total;
  }
  return largest_block ? mofei_pool_largest_free_block(pool) : mofei_pool_free_bytes(pool);
}

static uint32_t mofei_sim_heap_query_value(uint32_t caps, bool total_size, bool largest_block) {
  mofei_sim_heap_init();
  const bool wants_spiram = (caps & MOFEI_MALLOC_CAP_SPIRAM) != 0;
  const bool wants_internal = (caps & MOFEI_MALLOC_CAP_INTERNAL) != 0 || !wants_spiram;
  if (wants_spiram && !wants_internal) {
    return mofei_heap_pool_query_value(&mofei_psram_pool, total_size, largest_block);
  }

  if (wants_internal && !wants_spiram) {
    if (largest_block) {
      const uint32_t primary_largest = mofei_heap_pool_query_value(&mofei_internal_pool, total_size, true);
      const uint32_t ext_largest = mofei_heap_pool_query_value(&mofei_internal_pool_ext, total_size, true);
      return primary_largest > ext_largest ? primary_largest : ext_largest;
    }
    return mofei_heap_pool_query_value(&mofei_internal_pool, total_size, false) +
           mofei_heap_pool_query_value(&mofei_internal_pool_ext, total_size, false);
  }

  if (largest_block) {
    const uint32_t internal_largest = mofei_heap_pool_query_value(&mofei_internal_pool, total_size, true);
    const uint32_t internal_ext_largest = mofei_heap_pool_query_value(&mofei_internal_pool_ext, total_size, true);
    const uint32_t psram_largest = mofei_heap_pool_query_value(&mofei_psram_pool, total_size, true);
    const uint32_t internal_max = internal_largest > internal_ext_largest ? internal_largest : internal_ext_largest;
    return internal_max > psram_largest ? internal_max : psram_largest;
  }
  return mofei_heap_pool_query_value(&mofei_internal_pool, total_size, false) +
         mofei_heap_pool_query_value(&mofei_internal_pool_ext, total_size, false) +
         mofei_heap_pool_query_value(&mofei_psram_pool, total_size, false);
}

static uint32_t mofei_heap_caps_query_common(CPUXtensaState* env, bool total_size, bool largest_block) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned caps_reg = callinc * 4 + 2;
  const uint32_t caps = env->regs[caps_reg];
  const uint32_t result = mofei_sim_heap_query_value(caps, total_size, largest_block);
  env->regs[caps_reg] = result;

  static int heap_query_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_ALLOC") && heap_query_log < 64) {
    fprintf(stderr, "[HEAP-QUERY] caps=0x%08x total=%u largest=%u result=%u ret=0x%08x\n", caps, total_size ? 1 : 0,
            largest_block ? 1 : 0, result, clean_addr);
    heap_query_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_heap_caps_get_free_size)(CPUXtensaState* env) {
  return mofei_heap_caps_query_common(env, false, false);
}

uint32_t HELPER(mofei_heap_caps_get_largest_free_block)(CPUXtensaState* env) {
  return mofei_heap_caps_query_common(env, false, true);
}

uint32_t HELPER(mofei_heap_caps_get_total_size)(CPUXtensaState* env) {
  return mofei_heap_caps_query_common(env, true, false);
}

uint32_t HELPER(mofei_heap_caps_get_allocated_size)(CPUXtensaState* env) {
  mofei_sim_heap_init();
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned ptr_reg = callinc * 4 + 2;
  const uint32_t ptr = env->regs[ptr_reg];
  uint32_t size = mofei_lookup_bump_alloc_size(ptr);
  if (size == 0 && ((ptr >= mofei_psram_pool.base && ptr < mofei_psram_pool.limit) ||
                    (ptr >= mofei_internal_pool.base && ptr < mofei_internal_pool.limit) ||
                    (ptr >= mofei_internal_pool_ext.base && ptr < mofei_internal_pool_ext.limit))) {
    size = 64;
  }
  env->regs[ptr_reg] = size;

  static int alloc_size_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_ALLOC") && alloc_size_log < 64) {
    fprintf(stderr, "[ALLOC-SIZE] ptr=0x%08x size=%u ret=0x%08x\n", ptr, size, clean_addr);
    alloc_size_log++;
  }
  return clean_addr;
}

static uint32_t mofei_esp_psram_query_common(CPUXtensaState* env, bool total_size, bool largest_block) {
  mofei_sim_heap_init();
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned ret_reg = callinc * 4 + 2;
  const uint32_t result = mofei_heap_pool_query_value(&mofei_psram_pool, total_size, largest_block);
  env->regs[ret_reg] = result;

  static int esp_psram_query_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_ALLOC") && esp_psram_query_log < 64) {
    fprintf(stderr, "[ESP-PSRAM] total=%u largest=%u result=%u ret=0x%08x\n", total_size ? 1 : 0, largest_block ? 1 : 0,
            result, clean_addr);
    esp_psram_query_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_esp_get_psram_size)(CPUXtensaState* env) {
  return mofei_esp_psram_query_common(env, true, false);
}

uint32_t HELPER(mofei_esp_get_free_psram)(CPUXtensaState* env) {
  return mofei_esp_psram_query_common(env, false, false);
}

uint32_t HELPER(mofei_esp_get_max_alloc_psram)(CPUXtensaState* env) {
  return mofei_esp_psram_query_common(env, false, true);
}

static uint32_t mofei_queue_ptr = 0x3FFE0000;
static uint32_t mofei_queue_count = 0;
#define MAX_QUEUE_ALLOCS 4000

static uint32_t mofei_next_queue_handle(void) {
  /* Return a valid queue handle (non-NULL) so that mutex/queue operations
   * succeed. Use bump-allocated memory so each handle is unique. */
  uint32_t handle = mofei_queue_ptr;
  mofei_queue_ptr += 64; /* Each "queue" gets 64 bytes */
  mofei_queue_count++;
  if (mofei_queue_count <= 20 || (mofei_queue_count % 500) == 0) {
    fprintf(stderr, "[QUEUE] #%u create=0x%08x (success)\n", mofei_queue_count, handle);
  }
  return handle;
}

uint32_t HELPER(mofei_next_queue_handle)(void) { return mofei_next_queue_handle(); }

void HELPER(mofei_bump_queue_create)(CPUXtensaState* env) {
  /* RETW fallback for queue-create paths that entered the patched callee.
   * Write to env->regs[2] so the value survives xtensa_sync_phys_from_window
   * inside the subsequent gen_helper_retw. */
  env->regs[2] = mofei_next_queue_handle();
}

static uint32_t mofei_murphy_or_app_main_entry(void) {
  if (mofei_sim_addrs.murphySimulatorMain_addr) {
    return mofei_sim_addrs.murphySimulatorMain_addr;
  }
  return mofei_sim_addrs.app_main_addr;
}

static const char* mofei_murphy_or_app_main_entry_name(void) {
  if (mofei_sim_addrs.murphySimulatorMain_addr) {
    return "murphySimulatorMain";
  }
  return "app_main";
}

/* Redirect vTaskStartScheduler to the firmware UI entry — ONE TIME ONLY.
 * After the first redirect, the helper becomes a no-op so the RETW
 * executes normally. This prevents infinite redirect loops if
 * app_main's code path re-enters vTaskStartScheduler. */
static int mofei_redirected_to_app_main;

static void mofei_prepare_boot_firmware_frame(CPUXtensaState* env) {
  if (mofei_sim_addrs.murphySimulatorHooksEnabled_addr) {
    cpu_stb_data(env, mofei_sim_addrs.murphySimulatorHooksEnabled_addr, 1);
    fprintf(stderr, "[BOOT-FRAME] Murphy OS simulator hooks enabled at 0x%08x\n",
            mofei_sim_addrs.murphySimulatorHooksEnabled_addr);
  }

  const uint32_t stack_top = MOFEI_SIM_FAKE_APP_STACK_TOP;
  const uint32_t safe_ret = 0x4037D000u;
  const unsigned callinc = 2;
  const uint32_t marked_ret = (callinc << 30) | (safe_ret & 0x3fffffffu);

  memset(env->regs, 0, sizeof(env->regs));
  memset(env->phys_regs, 0, sizeof(env->phys_regs));

  env->sregs[WINDOW_BASE] = 0;
  env->windowbase_next = 0;
  env->sregs[WINDOW_START] = 1;
  env->sregs[PS] &= ~(PS_CALLINC | PS_EXCM);
  env->sregs[PS] |= PS_WOE | (callinc << PS_CALLINC_SHIFT);

  env->regs[1] = stack_top;
  env->regs[callinc * 4] = marked_ret;
  xtensa_sync_phys_from_window(env);

  cpu_stl_data(env, stack_top - 16, marked_ret);
  cpu_stl_data(env, stack_top - 12, stack_top);
  cpu_stl_data(env, stack_top - 8, 0);
  cpu_stl_data(env, stack_top - 4, 0);

  fprintf(stderr, "[BOOT-FRAME] %s fake call8 WB=%u WS=0x%x PS=0x%x SP=0x%08x A8=0x%08x\n",
          mofei_murphy_or_app_main_entry_name(), env->sregs[WINDOW_BASE], env->sregs[WINDOW_START], env->sregs[PS],
          env->regs[1], env->regs[8]);
}

static bool mofei_publish_murphy_boot_frame(CPUXtensaState* env) {
  if (!mofei_sim_addrs.murphySimulatorFramebuffer_addr || !mofei_sim_addrs.murphySimulatorFramebufferStorage_addr) {
    return false;
  }

  cpu_stl_data(env, mofei_sim_addrs.murphySimulatorFramebuffer_addr,
               mofei_sim_addrs.murphySimulatorFramebufferStorage_addr);
  HELPER(mofei_inject_framebuffer)(env);
  return true;
}

void HELPER(mofei_jump_to_app_main)(CPUXtensaState* env) {
  if (mofei_redirected_to_app_main) {
    return;
  }
  mofei_redirected_to_app_main = 1;

  fprintf(stderr, "[REDIRECT] WB=%d WS=0x%x PS=0x%x SP=0x%x A0=0x%x\n", env->sregs[WINDOW_BASE],
          env->sregs[WINDOW_START], env->sregs[PS], env->regs[1], env->regs[0]);

  extern MofeiSimAddrs mofei_sim_addrs;
  env->pc = mofei_murphy_or_app_main_entry();
  if (!env->pc) {
    fprintf(stderr, "[REDIRECT] WARNING: firmware UI entry not resolved, using fallback\n");
    env->pc = 0x4037d000; /* safe halt */
  }

  mofei_prepare_boot_firmware_frame(env);
}

uint32_t HELPER(mofei_enter_murphy_run_loop)(CPUXtensaState* env, uint32_t callinc) {
  const uint32_t target = mofei_sim_addrs.murphyDeviceShellRunSimulatorLoop_addr;
  const uint32_t shell_reg = ((callinc << 2) + 2u) & 0xfu;
  const uint32_t context_reg = ((callinc << 2) + 3u) & 0xfu;
  const uint32_t shell = env->regs[shell_reg];
  uint32_t context = env->regs[context_reg];
  if (!target) {
    fprintf(stderr, "[MURPHY] run-loop entry missing; staying at marker\n");
    return env->pc;
  }
  if (!mofei_murphy_context_pointer_plausible(context) && mofei_sim_addrs.murphySimulatorShellContext_addr) {
    context = mofei_sim_addrs.murphySimulatorShellContext_addr;
  }
  memset(env->regs, 0, sizeof(env->regs));
  memset(env->phys_regs, 0, sizeof(env->phys_regs));
  env->sregs[WINDOW_BASE] = 0;
  env->windowbase_next = 0;
  env->sregs[WINDOW_START] = 1;
  env->sregs[PS] &= ~(PS_CALLINC | PS_EXCM);
  env->sregs[PS] |= PS_WOE;
  env->regs[0] = 0x4037D000u;
  env->regs[1] = MOFEI_SIM_FAKE_APP_STACK_TOP - 80u;
  env->regs[2] = shell;
  env->regs[3] = context ? context : shell;
  xtensa_sync_phys_from_window(env);
  fprintf(stderr,
          "[MURPHY] enter run loop callinc=%u shell_reg=%u context_reg=%u shell=0x%08x context=0x%08x target=0x%08x\n",
          callinc, shell_reg, context_reg, shell, context, target);
  return target;
}

uint32_t HELPER(mofei_start_murphy_simulator_loop)(CPUXtensaState* env) {
  const uint32_t callinc = (env->sregs[PS] & PS_CALLINC) >> PS_CALLINC_SHIFT;
  const uint32_t arg_base = callinc << 2;
  uint32_t shell = env->regs[(arg_base + 2) & 0xf];
  uint32_t context = env->regs[(arg_base + 3) & 0xf];
  if (shell == 0 || context == 0) {
    shell = env->regs[10];
    context = env->regs[11];
  }
  bool used_static_shell = false;
  if (shell == 0 && mofei_sim_addrs.murphyDeviceShell_addr) {
    shell = mofei_sim_addrs.murphyDeviceShell_addr;
    used_static_shell = true;
  }
  const uint32_t target = mofei_sim_addrs.murphyDeviceShellRunSimulatorLoop_addr;
  if (!target || shell == 0) {
    fprintf(stderr, "[MURPHY] start loop missing shell/context callinc=%u target=0x%08x shell=0x%08x context=0x%08x\n",
            callinc, target, shell, context);
    return env->pc;
  }

  if (context != 0) {
    for (uint32_t i = 0; i < 48; i++) {
      uint8_t value = 0;
      mofei_read_guest_memory(context + i, &value, 1);
      cpu_stb_data(env, shell + i, value);
    }
  }

  env->regs[2] = shell;
  env->regs[10] = shell;

  fprintf(stderr, "[MURPHY] start simulator loop callinc=%u shell=0x%08x context=0x%08x target=0x%08x\n", callinc,
          shell, context, target);
  return target;
}

uint32_t HELPER(mofei_prepare_murphy_simulator_loop_frame)(CPUXtensaState* env) {
  const uint32_t method = mofei_sim_addrs.murphyDeviceShellRunSimulatorLoop_addr;
  const uint32_t target = method;
  const uint32_t shell = mofei_sim_addrs.murphyDeviceShell_addr;
  if (!method || !target || !shell) {
    fprintf(stderr, "[MURPHY] simulator loop frame missing method=0x%08x target=0x%08x shell=0x%08x\n", method, target,
            shell);
    return env->pc;
  }

  const uint32_t stack_top = MOFEI_SIM_FAKE_APP_STACK_TOP;
  const uint32_t frame_size = 80u;
  const uint32_t frame_sp = stack_top - frame_size;
  const uint32_t safe_ret = 0x4037D000u;
  const unsigned callinc = 2;
  const uint32_t marked_ret = (callinc << 30) | (safe_ret & 0x3fffffffu);
  const uint32_t caller_sp = env->regs[1];
  const uint32_t exported_context = mofei_sim_addrs.murphySimulatorShellContext_addr;

  uint32_t stack_context = 0;
  uint32_t stack_runner = 0;
  uint32_t stack_fb = 0;
  uint32_t stack_display = 0;
  uint32_t exported_runner = 0;
  uint32_t exported_fb = 0;
  uint32_t exported_display = 0;
  bool found_stack_context = false;
  if (exported_context != 0) {
    stack_runner = mofei_read_guest_data_u32(env, exported_context + 0u);
    stack_fb = mofei_read_guest_data_u32(env, exported_context + 4u);
    stack_display = mofei_read_guest_data_u32(env, exported_context + 8u);
    if (mofei_murphy_context_pointer_plausible(stack_runner) && mofei_murphy_context_pointer_plausible(stack_fb) &&
        mofei_murphy_context_pointer_plausible(stack_display)) {
      stack_context = exported_context;
      found_stack_context = true;
    }
  }
  if (!found_stack_context) {
    found_stack_context =
        mofei_find_murphy_stack_context(env, caller_sp, &stack_context, &stack_runner, &stack_fb, &stack_display);
  }
  if (!found_stack_context) {
    exported_runner = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorRunner_addr);
    exported_fb = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorFramebufferObject_addr);
    exported_display = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorDisplay_addr);
    if (mofei_murphy_context_pointer_plausible(exported_runner) &&
        mofei_murphy_context_pointer_plausible(exported_fb) &&
        mofei_murphy_context_pointer_plausible(exported_display)) {
      stack_runner = exported_runner;
      stack_fb = exported_fb;
      stack_display = exported_display;
      found_stack_context = true;
    }
  }
  if (!found_stack_context && mofei_murphy_context_pointer_plausible(mofei_sim_addrs.murphyDeviceRunner_addr) &&
      mofei_murphy_context_pointer_plausible(mofei_sim_addrs.murphyDeviceFramebuffer_addr) &&
      mofei_murphy_context_pointer_plausible(mofei_sim_addrs.murphyDeviceDisplay_addr)) {
    stack_runner = mofei_sim_addrs.murphyDeviceRunner_addr;
    stack_fb = mofei_sim_addrs.murphyDeviceFramebuffer_addr;
    stack_display = mofei_sim_addrs.murphyDeviceDisplay_addr;
    found_stack_context = true;
  }
  fprintf(stderr,
          "[MURPHY] simulator loop fallback original WB=%u WS=0x%x SP=0x%08x exported_ctx=0x%08x found=%d "
          "stack_ctx=0x%08x runner=0x%08x fb=0x%08x display=0x%08x exported_runner=0x%08x exported_fb=0x%08x "
          "exported_display=0x%08x\n",
          env->sregs[WINDOW_BASE], env->sregs[WINDOW_START], caller_sp, exported_context, found_stack_context ? 1 : 0,
          stack_context, stack_runner, stack_fb, stack_display, exported_runner, exported_fb, exported_display);

  if (stack_context == 0 && exported_context != 0) {
    stack_context = exported_context;
    found_stack_context = true;
  }
  if (stack_context == 0) {
    stack_context = shell;
  }

  if (stack_runner == 0) {
    stack_runner = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorRunner_addr)
                       ?: mofei_sim_addrs.murphyDeviceRunner_addr;
  }
  if (stack_fb == 0) {
    stack_fb = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorFramebufferObject_addr)
                   ?: mofei_sim_addrs.murphyDeviceFramebuffer_addr;
  }
  if (stack_display == 0) {
    stack_display = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorDisplay_addr)
                        ?: mofei_sim_addrs.murphyDeviceDisplay_addr;
  }
  if (stack_fb != 0) {
    mofei_murphy_last_framebuffer_object = stack_fb;
  }
  if (stack_fb != 0 && mofei_murphy_last_framebuffer_data != 0) {
    const uint32_t fb_width = mofei_read_guest_data_u32(env, stack_fb + 0u);
    const uint32_t fb_height = mofei_read_guest_data_u32(env, stack_fb + 4u);
    const uint32_t fb_stride = mofei_read_guest_data_u32(env, stack_fb + 8u);
    const uint32_t fb_bits = mofei_read_guest_data_u32(env, stack_fb + 12u);
    fprintf(stderr, "[MURPHY] framebuffer candidate object=0x%08x data=0x%08x size=%ux%u stride=%u bits=0x%08x\n",
            stack_fb, mofei_murphy_last_framebuffer_data, fb_width, fb_height, fb_stride, fb_bits);
    if (fb_bits == 0 && ((fb_width == 800u && fb_height == 480u && fb_stride == 100u) ||
                         (fb_width == 0u && fb_height == 0u && fb_stride == 0u))) {
      cpu_stl_data(env, stack_fb + 0u, 800u);
      cpu_stl_data(env, stack_fb + 4u, 480u);
      cpu_stl_data(env, stack_fb + 8u, 100u);
      cpu_stl_data(env, stack_fb + 12u, mofei_murphy_last_framebuffer_data);
      cpu_stl_data(env, mofei_sim_addrs.murphySimulatorFramebuffer_addr, mofei_murphy_last_framebuffer_data);
      for (uint32_t i = 0; i < 48000u; ++i) {
        cpu_stb_data(env, mofei_murphy_last_framebuffer_data + i, 0xffu);
      }
      fprintf(stderr, "[MURPHY] repaired framebuffer bits object=0x%08x data=0x%08x size=%ux%u stride=%u\n", stack_fb,
              mofei_murphy_last_framebuffer_data, fb_width, fb_height, fb_stride);
    }
  }

  if (found_stack_context) {
    cpu_stl_data(env, stack_context + 0u, stack_runner);
    cpu_stl_data(env, stack_context + 4u, stack_fb);
    cpu_stl_data(env, stack_context + 8u, stack_display);
    const uint32_t sleep_input = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorSleepInput_addr)
                                     ?: mofei_sim_addrs.murphyDeviceSleepInput_addr;
    const uint32_t power = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorPower_addr)
                               ?: mofei_sim_addrs.murphyDevicePower_addr;
    const uint32_t frontlight = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorFrontlight_addr)
                                    ?: mofei_sim_addrs.murphyDeviceFrontlight_addr;
    const uint32_t storage = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorStorage_addr)
                                 ?: mofei_sim_addrs.murphyDeviceStorage_addr;
    const uint32_t cast_store = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorCastStore_addr)
                                    ?: mofei_sim_addrs.murphyDeviceCastStore_addr;
    cpu_stl_data(env, stack_context + 12u, sleep_input);
    cpu_stl_data(env, stack_context + 16u, power);
    cpu_stl_data(env, stack_context + 20u, frontlight);
    cpu_stl_data(env, stack_context + 24u, storage);
    cpu_stl_data(env, stack_context + 28u, mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorUiFont_addr));
    cpu_stl_data(env, stack_context + 32u,
                 mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorBodyFont_addr));
    cpu_stl_data(env, stack_context + 36u, cast_store);
    cpu_stl_data(env, stack_context + 40u, 0);
    cpu_stl_data(env, stack_context + 44u, 0);
  }

  memset(env->regs, 0, sizeof(env->regs));
  memset(env->phys_regs, 0, sizeof(env->phys_regs));

  env->sregs[WINDOW_BASE] = 0;
  env->windowbase_next = 0;
  env->sregs[WINDOW_START] = 1;
  env->sregs[PS] &= ~(PS_CALLINC | PS_EXCM);
  env->sregs[PS] |= PS_WOE | (callinc << PS_CALLINC_SHIFT);

  env->regs[1] = stack_top;
  env->regs[callinc * 4] = marked_ret;
  env->regs[callinc * 4 + 2] = shell;
  env->regs[callinc * 4 + 3] = stack_context ? stack_context : shell;
  xtensa_sync_phys_from_window(env);

  uint32_t ctx_runner = 0;
  uint32_t ctx_fb = 0;
  uint32_t ctx_display = 0;
  uint32_t runner_input = 0;
  uint32_t runner_window = 0;
  uint32_t runner_now = 0;
  mofei_read_guest_memory(stack_context + 0, (uint8_t*)&ctx_runner, sizeof(ctx_runner));
  mofei_read_guest_memory(stack_context + 4, (uint8_t*)&ctx_fb, sizeof(ctx_fb));
  mofei_read_guest_memory(stack_context + 8, (uint8_t*)&ctx_display, sizeof(ctx_display));
  const uint32_t exported_runner_input = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorRunnerInput_addr)
                                             ?: mofei_sim_addrs.murphyDeviceDebugInput_addr;
  const uint32_t exported_runner_window =
      mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorRunnerWindow_addr)
          ?: mofei_sim_addrs.murphyDeviceWindow_addr;
  const uint32_t exported_runner_now = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorRunnerNowMs_addr)
                                           ?: mofei_sim_addrs.murphyDeviceNowMs_addr;
  if (ctx_runner != 0 && exported_runner_input != 0 && exported_runner_window != 0 && exported_runner_now != 0) {
    mofei_repair_murphy_input_chain(env, ctx_runner, exported_runner_input, exported_runner_window,
                                    exported_runner_now);
  }
  if (ctx_runner != 0) {
    mofei_read_guest_memory(ctx_runner + 0, (uint8_t*)&runner_input, sizeof(runner_input));
    mofei_read_guest_memory(ctx_runner + 4, (uint8_t*)&runner_window, sizeof(runner_window));
    mofei_read_guest_memory(ctx_runner + 8, (uint8_t*)&runner_now, sizeof(runner_now));
  }
  mofei_log_murphy_runner_layout(env, ctx_runner, runner_input, runner_window, runner_now);

  cpu_stl_data(env, frame_sp, marked_ret);
  cpu_stl_data(env, frame_sp + 4, stack_top);
  cpu_stl_data(env, frame_sp + 8, shell);
  cpu_stl_data(env, frame_sp + 12, stack_context ? stack_context : shell);

  fprintf(stderr,
          "[MURPHY] prepared simulator loop call frame method=0x%08x target=0x%08x shell=0x%08x context=0x%08x "
          "WB=%u WS=0x%x PS=0x%x A8=0x%08x A9=0x%08x A10=0x%08x A11=0x%08x\n",
          method, target, shell, stack_context ? stack_context : shell, env->sregs[WINDOW_BASE],
          env->sregs[WINDOW_START], env->sregs[PS], env->regs[8], env->regs[9], env->regs[10], env->regs[11]);
  fprintf(stderr,
          "[MURPHY] simulator loop guest context runner=0x%08x fb=0x%08x display=0x%08x runner.input=0x%08x "
          "runner.window=0x%08x runner.now=0x%08x\n",
          ctx_runner, ctx_fb, ctx_display, runner_input, runner_window, runner_now);
  return target;
}

/* When xTaskCreateUniversal is RETW-patched, it returns without creating
 * a task.  app_main then returns and the firmware halts.
 *
 * This helper intercepts xTaskCreateUniversal's RETW: we redirect
 * execution to loopTask so that setup() and loop() actually run.
 * The address is hardcoded for now (resolved from ELF nm output). */
static int mofei_loop_task_entered;

void HELPER(mofei_maybe_jump_to_loop_task)(CPUXtensaState* env) {
  if (mofei_loop_task_entered) {
    return;
  }
  mofei_loop_task_entered = 1;

  /* loopTask address from firmware ELF symbol table. */
  extern MofeiSimAddrs mofei_sim_addrs;
  const uint32_t loop_task_addr = mofei_sim_addrs.loopTask_addr;
  if (!loop_task_addr) {
    fprintf(stderr, "[LOOP-TASK] WARNING: loopTask_addr not resolved, skipping\n");
    return;
  }

  fprintf(stderr, "[LOOP-TASK] Entering loopTask @ 0x%08x\n", loop_task_addr);

  /* We are at xTaskCreateUniversal's RETW.  ENTRY has rotated the
   * window by callinc (from the call8 that reached this function).
   * RETW will undo that rotation via gen_helper_retw.
   *
   * After RETW, WB will be restored to app_main's window.
   * We want loopTask to start as if app_main called it with call8.
   * loopTask has its own ENTRY that rotates by the current CALLINC
   * in PS.  Since the original call8 to xTaskCreateUniversal set
   * CALLINC=2 in PS, and RETW doesn't clear CALLINC, loopTask's
   * ENTRY will rotate by 2 — exactly what call8 would do.
   *
   * We just need to set up A0 (return address) and A1 (stack pointer)
   * in the window that loopTask's ENTRY will rotate to.
   *
   * Current WB is inside xTaskCreateUniversal's ENTRY.
   * After RETW: WB = (current_wb - callinc) % nwb = app_main's WB.
   * Then loopTask's ENTRY: WB = (app_main's WB + callinc) % nwb
   *   = (current_wb - callinc + callinc) % nwb = current_wb.
   * So loopTask's ENTRY target window is the same WB we're in now.
   * But RETW will clear WINDOWSTART for current WB, and loopTask's
   * ENTRY will set it again.  We need A0 in the window that ENTRY
   * will rotate to — which is the current WB. */
  unsigned wb = env->sregs[WINDOW_BASE];
  unsigned callinc = (env->sregs[PS] & PS_CALLINC) >> PS_CALLINC_SHIFT;
  if (callinc == 0) callinc = 1;

  /* Set up A0 and A1 in the target window for loopTask's ENTRY.
   * The target is the same WB (see analysis above). */
  uint32_t safe_ret = 0x4037d000;
  env->phys_regs[wb * 4 + 0] = safe_ret;
  /* Keep A1 (SP) as-is — it's already valid from app_main's frame */

  env->pc = loop_task_addr;

  fprintf(stderr, "[LOOP-TASK] WB=%u callinc=%u WS=0x%x SP=0x%08x A0=0x%08x\n", wb, callinc, env->sregs[WINDOW_START],
          env->phys_regs[wb * 4 + 1], safe_ret);
}

/* 仅当调度器尚未进入固件 UI 时，才把 __assert_func 作为启动回退入口。
 * UI 运行后的真实断言不能再次触发启动重定向；后续 RETW 仍由 TCG 处理。 */
void HELPER(mofei_assert_redirect)(CPUXtensaState* env) {
  if (mofei_redirected_to_app_main) return;

  fprintf(stderr, "[ASSERT-REDIRECT] Bootstrap assert → %s\n", mofei_murphy_or_app_main_entry_name());
  mofei_redirected_to_app_main = 1;
  env->pc = mofei_murphy_or_app_main_entry();
  if (!env->pc) {
    fprintf(stderr, "[ASSERT-REDIRECT] WARNING: firmware UI entry not resolved\n");
    env->pc = 0x4037d000; /* safe halt */
  }
  mofei_prepare_boot_firmware_frame(env);
  CPUState* cs = env_cpu(env);
  cs->exception_index = EXCP_INTERRUPT;
  cpu_loop_exit(cs);
}

/* Make RETW-patched FreeRTOS functions return pdTRUE (1) instead of 0.
 * This prevents callers from asserting on "failure" and retrying in a loop.
 * Write to env->regs[2] so the value survives xtensa_sync_phys_from_window
 * inside the subsequent gen_helper_retw. */
void HELPER(mofei_retw_success)(CPUXtensaState* env) { env->regs[2] = 1; /* A2 = pdTRUE = 1 */ }

void HELPER(mofei_retw_queue_receive)(CPUXtensaState* env) {
  const uint32_t receive_buffer = env->regs[3];
  const uint32_t tick_count = env->regs[4];
  const bool buffer_in_ram = (receive_buffer >= 0x3c800000u && receive_buffer < 0x3d000000u) ||
                             (receive_buffer >= 0x3fc80000u && receive_buffer < 0x3fda0000u);
  if (buffer_in_ram && mofei_sim_board_is_lilygo_t5s3_pro()) {
    const uint32_t latched_event = esp32s3_i2c_consume_completion_event();
    static uint32_t i2c_queue_receive_log_count;
    if (mofei_trace_enabled("MOFEI_SIM_DEBUG_I2C") && i2c_queue_receive_log_count++ < 64) {
      fprintf(stderr, "[ESP32S3-I2C-DIAG] queue_receive handle=0x%08x out=0x%08x ticks=%u event=%u\n", env->regs[2],
              receive_buffer, tick_count, latched_event);
    }
    if (latched_event != 0) {
      mofei_write_guest_u32(env, receive_buffer, latched_event);
      env->regs[2] = 1;
      return;
    }
    const uint32_t i2c_int_raw = mofei_read_mmio_u32(0x60013020u);
    const uint32_t completion_mask = BIT(7) | BIT(3);
    if (i2c_int_raw & (BIT(10) | completion_mask)) {
      const uint32_t event = (i2c_int_raw & BIT(10)) ? 2u : 1u;
      mofei_write_guest_u32(env, receive_buffer, event);
      mofei_write_mmio_u32(0x60013024u, i2c_int_raw & (BIT(10) | completion_mask));
      env->regs[2] = 1;
      return;
    }
  }
  if (buffer_in_ram) {
    uint32_t sdmmc_status = mofei_read_mmio_u32(MOFEI_SDMMC_RINTSTS_ADDR) & MOFEI_SDMMC_SD_EVENT_MASK;
    uint32_t dma_status_raw = mofei_read_mmio_u32(MOFEI_SDMMC_IDSTS_ADDR);
    uint32_t dma_status = dma_status_raw & MOFEI_SDMMC_DMA_EVENT_MASK;
    if (sdmmc_status == 0 && dma_status == 0) {
      if (tick_count == 0) {
        env->regs[2] = 0; /* A2 = pdFALSE */
        return;
      }
      sdmmc_status = MOFEI_SDMMC_INTMASK_CMD_DONE | MOFEI_SDMMC_INTMASK_DATA_OVER;
      dma_status = MOFEI_SDMMC_DMA_DONE_MASK;
    }
    mofei_write_guest_u32(env, receive_buffer, sdmmc_status);
    mofei_write_guest_u32(env, receive_buffer + 4u, dma_status);
    if (sdmmc_status != 0) {
      mofei_write_mmio_u32(MOFEI_SDMMC_RINTSTS_ADDR, sdmmc_status);
    }
    if (dma_status_raw != 0) {
      mofei_write_mmio_u32(MOFEI_SDMMC_IDSTS_ADDR, dma_status_raw);
    }
    static int queue_receive_log = 0;
    if (mofei_trace_enabled("MOFEI_SIM_DEBUG_SDMMC") && queue_receive_log < 64) {
      fprintf(stderr, "[SDMMC-EVT] xQueueReceive ticks=%u out=0x%08x sd=0x%08x dma=0x%08x\n", tick_count,
              receive_buffer, sdmmc_status, dma_status);
      queue_receive_log++;
    }
  }
  env->regs[2] = 1; /* A2 = pdTRUE = 1 */
}

/* Make RETW-patched partition/search functions return NULL (0).
 * esp_partition_find/next return a pointer or NULL.
 * A2=0 signals "no partition found" so callers skip partition I/O.
 * Write to env->regs[2] so the value survives xtensa_sync_phys_from_window. */
void HELPER(mofei_retw_null)(CPUXtensaState* env) { env->regs[2] = 0; }

uint64_t HELPER(mofei_time_us)(void) { return qemu_clock_get_us(QEMU_CLOCK_VIRTUAL); }

/* esp_timer_get_time() 与 systimer HAL 通过 A2:A3 返回 64 位值。
 * 绕过模拟器不兼容的计时代码时必须同步写入高低半字。 */
void HELPER(mofei_retw_time_us)(CPUXtensaState* env) {
  const uint64_t now_us = HELPER(mofei_time_us)();
  env->regs[2] = (uint32_t)now_us;
  env->regs[3] = (uint32_t)(now_us >> 32);
}

/* Make RETW-patched NVS functions return ESP_FAIL (-1).
 * NVS functions return esp_err_t; ESP_FAIL = -1 tells callers
 * that NVS is not available, so they skip NVS operations.
 * Write to env->regs[2] so the value survives xtensa_sync_phys_from_window. */
void HELPER(mofei_retw_fail)(CPUXtensaState* env) { env->regs[2] = 0xFFFFFFFF; }

static uint32_t mofei_intr_handle_ptr = 0x3FFE8000;
static uint32_t mofei_intr_handle_count;

static void mofei_write_guest_u32(CPUXtensaState* env, uint32_t addr, uint32_t value) {
  if (addr == 0) {
    return;
  }
  if (mofei_guest_addr_in_sim_ram(addr)) {
    cpu_stl_data(env, addr, value);
    return;
  }
  uint8_t bytes[4] = {0};
  mofei_store_le32(bytes, value);
  if (mofei_sdmmc_shadow_write(addr, bytes, sizeof(bytes))) {
    return;
  }
  address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, bytes, sizeof(bytes));
}

static void mofei_retw_intr_alloc_common(CPUXtensaState* env, uint32_t ret_handle_ptr) {
  const uint32_t handle = mofei_intr_handle_ptr;
  mofei_intr_handle_ptr += 64;
  mofei_intr_handle_count++;
  mofei_write_guest_u32(env, ret_handle_ptr, handle);
  env->regs[2] = 0; /* ESP_OK */
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_INTR") && mofei_intr_handle_count <= 8) {
    fprintf(stderr, "[INTR] #%u handle=0x%08x out=0x%08x (ESP_OK)\n", mofei_intr_handle_count, handle, ret_handle_ptr);
  }
}

void HELPER(mofei_retw_intr_alloc)(CPUXtensaState* env) { mofei_retw_intr_alloc_common(env, env->regs[6]); }

void HELPER(mofei_retw_intr_alloc_intrstatus)(CPUXtensaState* env) { mofei_retw_intr_alloc_common(env, env->regs[8]); }

#define MOFEI_ESP_OK 0x00000000u
#define MOFEI_ESP_ERR_INVALID_ARG 0x00000102u
#define MOFEI_ESP_ERR_TIMEOUT 0x00000107u

static uint32_t mofei_read_mmio_u32(uint32_t addr) {
  if (mofei_sdmmc_shadow_range(addr, sizeof(uint32_t))) {
    return mofei_sdmmc_shadow_read_u32(addr);
  }
  uint8_t bytes[4] = {0};
  if (address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, bytes, sizeof(bytes)) != MEMTX_OK) {
    return 0;
  }
  return mofei_load_le32(bytes);
}

static void mofei_write_mmio_u32(uint32_t addr, uint32_t value) {
  if (mofei_sdmmc_shadow_range(addr, sizeof(uint32_t))) {
    mofei_sdmmc_shadow_write_u32(addr, value);
    return;
  }
  uint8_t bytes[4] = {0};
  mofei_store_le32(bytes, value);
  address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, bytes, sizeof(bytes));
}

uint32_t HELPER(mofei_sdmmc_wait_for_event)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned ret_reg = callinc * 4 + 2;
  const unsigned tick_reg = callinc * 4 + 2;
  const unsigned event_reg = callinc * 4 + 3;
  const int32_t tick_count = (int32_t)env->regs[tick_reg];
  const uint32_t out_event = env->regs[event_reg];

  if (out_event == 0) {
    env->regs[ret_reg] = MOFEI_ESP_ERR_INVALID_ARG;
    return clean_addr;
  }

  uint32_t sdmmc_status = mofei_read_mmio_u32(MOFEI_SDMMC_RINTSTS_ADDR) & MOFEI_SDMMC_SD_EVENT_MASK;
  const uint32_t dma_status_raw = mofei_read_mmio_u32(MOFEI_SDMMC_IDSTS_ADDR);
  uint32_t dma_status = dma_status_raw & MOFEI_SDMMC_DMA_EVENT_MASK;

  if (sdmmc_status == 0 && dma_status == 0) {
    if (tick_count == 0) {
      env->regs[ret_reg] = MOFEI_ESP_ERR_TIMEOUT;
      return clean_addr;
    }
    sdmmc_status = MOFEI_SDMMC_INTMASK_CMD_DONE | MOFEI_SDMMC_INTMASK_DATA_OVER;
    dma_status = MOFEI_SDMMC_DMA_DONE_MASK;
  }

  mofei_write_guest_u32(env, out_event, sdmmc_status);
  mofei_write_guest_u32(env, out_event + 4, dma_status);

  if (sdmmc_status != 0) {
    mofei_write_mmio_u32(MOFEI_SDMMC_RINTSTS_ADDR, sdmmc_status);
  }
  if (dma_status_raw != 0) {
    mofei_write_mmio_u32(MOFEI_SDMMC_IDSTS_ADDR, dma_status_raw);
  }

  static int sdmmc_event_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_SDMMC") && sdmmc_event_log < 64) {
    fprintf(stderr, "[SDMMC-EVT] tick=%d out=0x%08x sd=0x%08x dma=0x%08x ret=0x%08x\n", tick_count, out_event,
            sdmmc_status, dma_status, clean_addr);
    sdmmc_event_log++;
  }

  env->regs[ret_reg] = MOFEI_ESP_OK;
  return clean_addr;
}

/* Skip an ADC/eFuse function entirely by returning 0 immediately.
 * Called from disas_xtensa_insn when PC matches a known blocking function.
 *
 * Since entry has NOT run yet, the window has NOT rotated. The return
 * address is in A(callinc*4) of the current window:
 *   call0 → A0
 *   call4 → A4
 *   call8 → A8
 *   call12 → A12
 *
 * The return value goes in A2 (caller's A2, same window since no rotation).
 * We return the target PC so the TCG code can jump to it.
 */
/* Skip a void function entirely. Same as skip_fn_return_zero but doesn't touch A2.
 * Since the CALL has already rotated the window but ENTRY hasn't run, we must:
 *  1. Save the return address from the current window's A[callinc*4]
 *  2. Return the cleaned PC (mask off the callinc marker bits). */
static uint32_t mofei_skip_fn_common(CPUXtensaState* env, uint32_t* out_ret_addr, unsigned* out_callinc) {
  /* Extract callinc directly from PS.CALLINC. */
  unsigned callinc = (env->sregs[PS] & PS_CALLINC) >> PS_CALLINC_SHIFT;
  uint32_t raw_addr = env->regs[callinc * 4];

  static int skip_full_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_SKIP") && skip_full_log < 50) {
    fprintf(stderr, "[SKIP-DBG] pc=%08x callinc=%u raw_addr=%08x A0=%08x A4=%08x A8=%08x A12=%08x PS=%08x\n", env->pc,
            callinc, raw_addr, env->regs[0], env->regs[4], env->regs[8], env->regs[12], env->sregs[PS]);
    skip_full_log++;
  }

  /* The translate.c intercept matches dc->pc against the function's symbol
   * address, which is its first instruction — the ENTRY opcode.  Because we
   * `return` immediately after gen_jump, ENTRY is never translated, never
   * executed.  CALLN (gen_callw_slot) only stores the return address and sets
   * PS.CALLINC — it does NOT change WINDOW_BASE.  So at this point
   * WINDOW_BASE is still the caller's value and env->regs[0..15] contain
   * the caller's register frame.  We must NOT rotate the window. */

  /* Strip the CALLN marker bits (bits 31:30) and restore the ESP32-S3 flash
   * virtual mapping.  CALLN stores return PCs as marker bits + low 30 bits;
   * jumping to only the low bits (for example 0x020a9bd2) loses the executable
   * 0x42000000 mapping and stalls/faults after skipped simulator stubs. */
  uint32_t clean_addr = mofei_canonical_calln_return_pc(raw_addr);
  if (out_ret_addr) *out_ret_addr = raw_addr;
  if (out_callinc) *out_callinc = callinc;
  return clean_addr;
}

uint32_t HELPER(mofei_skip_fn_void)(CPUXtensaState* env) {
  uint32_t raw_addr;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, NULL);

  static int skip_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_SKIP") && skip_log < 30) {
    fprintf(stderr, "[SKIP-FN-VOID] raw=0x%08x clean=0x%08x\n", raw_addr, clean_addr);
    skip_log++;
  }

  return clean_addr;
}

uint32_t HELPER(mofei_publish_hook_return)(CPUXtensaState* env, uint32_t raw_addr) {
  static int publish_ret_log = 0;
  const uint32_t clean_addr = mofei_canonical_calln_return_pc(raw_addr);
  if (publish_ret_log < 8) {
    fprintf(stderr, "[MOFEI] publish return raw=0x%08x clean=0x%08x wb=%u ps=0x%08x\n", raw_addr, clean_addr,
            env->sregs[WINDOW_BASE], env->sregs[PS]);
    publish_ret_log++;
  }
  return clean_addr;
}

bool mofei_read_guest_memory(uint32_t guest_addr, uint8_t* dest, uint32_t len);

static bool mofei_guest_addr_in_sim_ram(uint32_t guest_addr) {
  return (guest_addr >= MOFEI_SIM_INTERNAL_DRAM_BASE && guest_addr < MOFEI_SIM_INTERNAL_DRAM_LIMIT) ||
         (guest_addr >= MOFEI_SIM_EXTERNAL_RAM_BASE && guest_addr < MOFEI_SIM_EXTERNAL_RAM_LIMIT);
}

static bool mofei_guest_range_in_sim_ram(uint32_t guest_addr, uint32_t len) {
  return (guest_addr >= MOFEI_SIM_INTERNAL_DRAM_BASE && guest_addr < MOFEI_SIM_INTERNAL_DRAM_LIMIT &&
          len <= MOFEI_SIM_INTERNAL_DRAM_LIMIT - guest_addr) ||
         (guest_addr >= MOFEI_SIM_EXTERNAL_RAM_BASE && guest_addr < MOFEI_SIM_EXTERNAL_RAM_LIMIT &&
          len <= MOFEI_SIM_EXTERNAL_RAM_LIMIT - guest_addr);
}

static bool mofei_guest_range_in_flash_cache_overlay(uint32_t guest_addr, uint32_t len) {
  return (guest_addr >= MOFEI_SIM_DCACHE_BASE && guest_addr < MOFEI_SIM_DCACHE_LIMIT &&
          len <= MOFEI_SIM_DCACHE_LIMIT - guest_addr) ||
         (guest_addr >= MOFEI_SIM_ICACHE_BASE && guest_addr < MOFEI_SIM_ICACHE_LIMIT &&
          len <= MOFEI_SIM_ICACHE_LIMIT - guest_addr);
}

static bool mofei_guest_range_overlaps_icache(uint32_t guest_addr, uint32_t len) {
  if (len == 0) {
    return false;
  }
  const uint64_t start = guest_addr;
  const uint64_t end = start + len;
  return start < MOFEI_SIM_ICACHE_LIMIT && end > MOFEI_SIM_ICACHE_BASE;
}

static void mofei_write_guest_u8(CPUXtensaState* env, uint32_t addr, uint8_t value) {
  if (mofei_guest_range_overlaps_icache(addr, 1)) {
    return;
  }
  if (mofei_guest_addr_in_sim_ram(addr)) {
    cpu_stb_data(env, addr, value);
    return;
  }
  if (mofei_sdmmc_shadow_write(addr, &value, sizeof(value))) {
    return;
  }
  address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &value, sizeof(value));
}

static void mofei_memset_guest(CPUXtensaState* env, uint32_t dest, uint8_t value, uint32_t len) {
  if (len > 0) {
    if (mofei_guest_range_overlaps_icache(dest, len)) {
      static int log_count = 0;
      if (log_count < 8) {
        fprintf(stderr, "[MOFEI] blocked runtime memset into ICACHE dest=0x%08x value=0x%02x len=%u\n", dest, value,
                len);
        log_count++;
      }
      return;
    }
    if (mofei_guest_addr_in_sim_ram(dest)) {
      for (uint32_t i = 0; i < len; ++i) {
        cpu_stb_data(env, dest + i, value);
      }
    } else {
      uint8_t* buf = g_malloc(len);
      memset(buf, value, len);
      if (!mofei_sdmmc_shadow_write(dest, buf, len)) {
        address_space_write(&address_space_memory, dest, MEMTXATTRS_UNSPECIFIED, buf, len);
      }
      g_free(buf);
    }
  }
}

static void mofei_memcpy_guest(CPUXtensaState* env, uint32_t dest, uint32_t src, uint32_t len) {
  if (len > 0) {
    if (mofei_guest_range_overlaps_icache(dest, len)) {
      static int log_count = 0;
      if (log_count < 8) {
        fprintf(stderr, "[MOFEI] blocked runtime memcpy into ICACHE dest=0x%08x src=0x%08x len=%u\n", dest, src, len);
        log_count++;
      }
      return;
    }
    uint8_t* buf = g_malloc0(len);
    mofei_read_guest_memory(src, buf, len);
    if (mofei_guest_addr_in_sim_ram(dest)) {
      for (uint32_t i = 0; i < len; ++i) {
        cpu_stb_data(env, dest + i, buf[i]);
      }
    } else if (!mofei_sdmmc_shadow_write(dest, buf, len)) {
      address_space_write(&address_space_memory, dest, MEMTXATTRS_UNSPECIFIED, buf, len);
    }
    g_free(buf);
  }
}

static int32_t mofei_strcmp_guest(uint32_t left, uint32_t right) {
  int32_t result = 0;

  for (uint32_t offset = 0; offset < 4096; ++offset) {
    uint8_t left_ch = 0;
    uint8_t right_ch = 0;
    if (left != 0) {
      mofei_read_guest_memory(left + offset, &left_ch, 1);
    }
    if (right != 0) {
      mofei_read_guest_memory(right + offset, &right_ch, 1);
    }
    if (left_ch != right_ch || left_ch == 0 || right_ch == 0) {
      result = (int32_t)left_ch - (int32_t)right_ch;
      break;
    }
  }

  return result;
}

static uint32_t mofei_strlen_guest(uint32_t str) {
  uint32_t len = 0;
  uint8_t ch = 0;

  if (str != 0) {
    for (; len < 4096; ++len) {
      mofei_read_guest_memory(str + len, &ch, 1);
      if (ch == 0) {
        break;
      }
    }
  }

  return len;
}

static uint32_t mofei_strdup_guest(CPUXtensaState* env, uint32_t src, uint32_t* out_len) {
  if (out_len) {
    *out_len = 0;
  }
  if (src == 0) {
    return 0;
  }

  const uint32_t len = mofei_strlen_guest(src);
  if (out_len) {
    *out_len = len;
  }

  const uint32_t bytes = len + 1;
  const uint32_t dest = mofei_bump_alloc(bytes, MOFEI_MALLOC_CAP_8BIT);
  if (dest == 0) {
    return 0;
  }

  for (uint32_t i = 0; i < len; ++i) {
    uint8_t ch = 0;
    mofei_read_guest_memory(src + i, &ch, 1);
    mofei_write_guest_u8(env, dest + i, ch);
  }
  mofei_write_guest_u8(env, dest + len, 0);
  return dest;
}

uint32_t HELPER(mofei_memset)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned dest_reg = callinc * 4 + 2;
  const unsigned value_reg = callinc * 4 + 3;
  const unsigned len_reg = callinc * 4 + 4;
  const uint32_t dest = env->regs[dest_reg];
  const uint8_t value = (uint8_t)env->regs[value_reg];
  const uint32_t len = env->regs[len_reg];

  mofei_memset_guest(env, dest, value, len);
  env->regs[dest_reg] = dest;
  static int memset_log = 0;
  if (memset_log < 12) {
    fprintf(stderr, "[MOFEI] memset skip dest=0x%08x value=0x%02x len=%u ret=0x%08x\n", dest, value, len, clean_addr);
    memset_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_memset_direct)(CPUXtensaState* env, uint32_t dest, uint32_t value, uint32_t len) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);

  mofei_memset_guest(env, dest, (uint8_t)value, len);
  env->regs[callinc * 4 + 2] = dest;
  static int memset_direct_log = 0;
  if (memset_direct_log < 12) {
    fprintf(stderr, "[MOFEI] memset direct dest=0x%08x value=0x%02x len=%u ret=0x%08x\n", dest, (uint8_t)value, len,
            clean_addr);
    memset_direct_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_memcpy)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned dest_reg = callinc * 4 + 2;
  const unsigned src_reg = callinc * 4 + 3;
  const unsigned len_reg = callinc * 4 + 4;
  const uint32_t dest = env->regs[dest_reg];
  const uint32_t src = env->regs[src_reg];
  const uint32_t len = env->regs[len_reg];

  mofei_memcpy_guest(env, dest, src, len);
  env->regs[dest_reg] = dest;
  static int memcpy_log = 0;
  if (memcpy_log < 12) {
    fprintf(stderr, "[MOFEI] memcpy skip dest=0x%08x src=0x%08x len=%u ret=0x%08x\n", dest, src, len, clean_addr);
    memcpy_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_memcpy_direct)(CPUXtensaState* env, uint32_t dest, uint32_t src, uint32_t len) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);

  mofei_memcpy_guest(env, dest, src, len);
  env->regs[callinc * 4 + 2] = dest;
  static int memcpy_direct_log = 0;
  if (memcpy_direct_log < 12) {
    fprintf(stderr, "[MOFEI] memcpy direct dest=0x%08x src=0x%08x len=%u ret=0x%08x\n", dest, src, len, clean_addr);
    memcpy_direct_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_strcmp)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned left_reg = callinc * 4 + 2;
  const unsigned right_reg = callinc * 4 + 3;
  const uint32_t left = env->regs[left_reg];
  const uint32_t right = env->regs[right_reg];
  const int32_t result = mofei_strcmp_guest(left, right);

  env->regs[left_reg] = (uint32_t)result;
  static int strcmp_log = 0;
  if (strcmp_log < 12) {
    fprintf(stderr, "[MOFEI] strcmp skip left=0x%08x right=0x%08x result=%d ret=0x%08x\n", left, right, result,
            clean_addr);
    strcmp_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_strcmp_direct)(CPUXtensaState* env, uint32_t left, uint32_t right) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const int32_t result = mofei_strcmp_guest(left, right);

  env->regs[callinc * 4 + 2] = (uint32_t)result;
  static int strcmp_direct_log = 0;
  if (strcmp_direct_log < 12) {
    fprintf(stderr, "[MOFEI] strcmp direct left=0x%08x right=0x%08x result=%d ret=0x%08x\n", left, right, result,
            clean_addr);
    strcmp_direct_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_strlen)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned str_reg = callinc * 4 + 2;
  const uint32_t str = env->regs[str_reg];
  const uint32_t len = mofei_strlen_guest(str);

  env->regs[str_reg] = len;
  static int strlen_log = 0;
  if (strlen_log < 12) {
    fprintf(stderr, "[MOFEI] strlen skip str=0x%08x len=%u ret=0x%08x\n", str, len, clean_addr);
    strlen_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_strlen_direct)(CPUXtensaState* env, uint32_t str) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const uint32_t len = mofei_strlen_guest(str);

  env->regs[callinc * 4 + 2] = len;
  static int strlen_direct_log = 0;
  if (strlen_direct_log < 12) {
    fprintf(stderr, "[MOFEI] strlen direct str=0x%08x len=%u ret=0x%08x\n", str, len, clean_addr);
    strlen_direct_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_strdup)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned str_reg = callinc * 4 + 2;
  const uint32_t src = env->regs[str_reg];
  uint32_t len = 0;
  const uint32_t dest = mofei_strdup_guest(env, src, &len);

  env->regs[str_reg] = dest;

  static int strdup_log = 0;
  if (strdup_log < 12) {
    fprintf(stderr, "[MOFEI] strdup skip src=0x%08x len=%u dest=0x%08x ret=0x%08x\n", src, len, dest, clean_addr);
    strdup_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_strdup_direct)(CPUXtensaState* env, uint32_t src) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  uint32_t len = 0;
  const uint32_t dest = mofei_strdup_guest(env, src, &len);

  env->regs[callinc * 4 + 2] = dest;
  static int strdup_direct_log = 0;
  if (strdup_direct_log < 12) {
    fprintf(stderr, "[MOFEI] strdup direct src=0x%08x len=%u dest=0x%08x ret=0x%08x\n", src, len, dest, clean_addr);
    strdup_direct_log++;
  }
  return clean_addr;
}

static int64_t mofei_make_i64(uint32_t lo, uint32_t hi) { return (int64_t)(((uint64_t)hi << 32) | (uint64_t)lo); }

static uint64_t mofei_make_u64(uint32_t lo, uint32_t hi) { return ((uint64_t)hi << 32) | (uint64_t)lo; }

static void mofei_write_i64_return(CPUXtensaState* env, unsigned callinc, uint64_t value) {
  env->regs[callinc * 4 + 2] = (uint32_t)value;
  env->regs[callinc * 4 + 3] = (uint32_t)(value >> 32);
}

uint32_t HELPER(mofei_divdi3_direct)(CPUXtensaState* env, uint32_t dividend_lo, uint32_t dividend_hi,
                                     uint32_t divisor_lo, uint32_t divisor_hi) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const int64_t dividend = mofei_make_i64(dividend_lo, dividend_hi);
  const int64_t divisor = mofei_make_i64(divisor_lo, divisor_hi);
  int64_t result = 0;
  if (divisor != 0) {
    result = (dividend == INT64_MIN && divisor == -1) ? INT64_MIN : dividend / divisor;
  }

  mofei_write_i64_return(env, callinc, (uint64_t)result);
  static int divdi_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_LIBGCC") && divdi_log < 32) {
    fprintf(stderr, "[LIBGCC] __divdi3 %lld / %lld = %lld ret=0x%08x\n", (long long)dividend, (long long)divisor,
            (long long)result, clean_addr);
    divdi_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_moddi3_direct)(CPUXtensaState* env, uint32_t dividend_lo, uint32_t dividend_hi,
                                     uint32_t divisor_lo, uint32_t divisor_hi) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const int64_t dividend = mofei_make_i64(dividend_lo, dividend_hi);
  const int64_t divisor = mofei_make_i64(divisor_lo, divisor_hi);
  int64_t result = 0;
  if (divisor != 0) {
    result = (dividend == INT64_MIN && divisor == -1) ? 0 : dividend % divisor;
  }

  mofei_write_i64_return(env, callinc, (uint64_t)result);
  static int moddi_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_LIBGCC") && moddi_log < 32) {
    fprintf(stderr, "[LIBGCC] __moddi3 %lld %% %lld = %lld ret=0x%08x\n", (long long)dividend, (long long)divisor,
            (long long)result, clean_addr);
    moddi_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_udivdi3_direct)(CPUXtensaState* env, uint32_t dividend_lo, uint32_t dividend_hi,
                                      uint32_t divisor_lo, uint32_t divisor_hi) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const uint64_t dividend = mofei_make_u64(dividend_lo, dividend_hi);
  const uint64_t divisor = mofei_make_u64(divisor_lo, divisor_hi);
  const uint64_t result = divisor == 0 ? 0 : dividend / divisor;

  mofei_write_i64_return(env, callinc, result);
  static int udivdi_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_LIBGCC") && udivdi_log < 32) {
    fprintf(stderr, "[LIBGCC] __udivdi3 %llu / %llu = %llu ret=0x%08x\n", (unsigned long long)dividend,
            (unsigned long long)divisor, (unsigned long long)result, clean_addr);
    udivdi_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_umoddi3_direct)(CPUXtensaState* env, uint32_t dividend_lo, uint32_t dividend_hi,
                                      uint32_t divisor_lo, uint32_t divisor_hi) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const uint64_t dividend = mofei_make_u64(dividend_lo, dividend_hi);
  const uint64_t divisor = mofei_make_u64(divisor_lo, divisor_hi);
  const uint64_t result = divisor == 0 ? 0 : dividend % divisor;

  mofei_write_i64_return(env, callinc, result);
  static int umoddi_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_LIBGCC") && umoddi_log < 32) {
    fprintf(stderr, "[LIBGCC] __umoddi3 %llu %% %llu = %llu ret=0x%08x\n", (unsigned long long)dividend,
            (unsigned long long)divisor, (unsigned long long)result, clean_addr);
    umoddi_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_cxa_guard_acquire)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned guard_reg = callinc * 4 + 2;
  const uint32_t guard = env->regs[guard_reg];
  uint8_t value = 0;
  if (guard != 0) {
    mofei_read_guest_memory(guard, &value, 1);
  }
  env->regs[guard_reg] = value == 0 ? 1u : 0u;

  static int guard_acquire_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_GUARD") && guard_acquire_log < 64) {
    fprintf(stderr, "[GUARD] acquire guard=0x%08x value=0x%02x result=%u ret=0x%08x\n", guard, value,
            env->regs[guard_reg], clean_addr);
    guard_acquire_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_cxa_guard_release)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned guard_reg = callinc * 4 + 2;
  const uint32_t guard = env->regs[guard_reg];
  if (guard != 0) {
    mofei_write_guest_u8(env, guard, 1);
  }
  env->regs[guard_reg] = 0;

  static int guard_release_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_GUARD") && guard_release_log < 64) {
    fprintf(stderr, "[GUARD] release guard=0x%08x ret=0x%08x\n", guard, clean_addr);
    guard_release_log++;
  }
  return clean_addr;
}

#define MOFEI_PTHREAD_TLS_KEY_MAX 32u

static uint32_t mofei_pthread_tls_values[MOFEI_PTHREAD_TLS_KEY_MAX];
static uint32_t mofei_pthread_tls_next_key;

uint32_t HELPER(mofei_pthread_key_create)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const uint32_t key_ptr = env->regs[callinc * 4 + 2];
  const uint32_t destructor_ptr = env->regs[callinc * 4 + 3];
  uint32_t key = 0;
  bool ok = false;

  (void)destructor_ptr;
  if (key_ptr != 0 && mofei_pthread_tls_next_key < MOFEI_PTHREAD_TLS_KEY_MAX) {
    key = mofei_pthread_tls_next_key++;
    mofei_write_guest_u32(env, key_ptr, key);
    ok = true;
  }

  env->regs[callinc * 4 + 2] = ok ? 0 : MOFEI_ESP_ERR_INVALID_ARG;

  static int key_create_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_PTHREAD_TLS") && key_create_log < 16) {
    fprintf(stderr, "[PTHREAD-TLS] key_create key_ptr=0x%08x key=%u ret=0x%08x\n", key_ptr, key, clean_addr);
    key_create_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_pthread_getspecific)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const uint32_t key = env->regs[callinc * 4 + 2];
  uint32_t value = 0;

  if (key < MOFEI_PTHREAD_TLS_KEY_MAX) {
    value = mofei_pthread_tls_values[key];
  }
  env->regs[callinc * 4 + 2] = value;

  static int get_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_PTHREAD_TLS") && get_log < 32) {
    fprintf(stderr, "[PTHREAD-TLS] getspecific key=%u value=0x%08x ret=0x%08x\n", key, value, clean_addr);
    get_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_pthread_setspecific)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const uint32_t key = env->regs[callinc * 4 + 2];
  const uint32_t value = env->regs[callinc * 4 + 3];

  if (key < MOFEI_PTHREAD_TLS_KEY_MAX) {
    mofei_pthread_tls_values[key] = value;
    env->regs[callinc * 4 + 2] = 0;
  } else {
    env->regs[callinc * 4 + 2] = MOFEI_ESP_ERR_INVALID_ARG;
  }

  static int set_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_PTHREAD_TLS") && set_log < 32) {
    fprintf(stderr, "[PTHREAD-TLS] setspecific key=%u value=0x%08x ret=0x%08x\n", key, value, clean_addr);
    set_log++;
  }
  return clean_addr;
}

uint32_t HELPER(mofei_skip_fn_return_zero)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  env->regs[callinc * 4 + 2] = 0;

  static int skip_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_SKIP") && skip_log < 30) {
    fprintf(stderr, "[SKIP-FN] raw=0x%08x clean=0x%08x callinc=%u ret_reg=%u\n", raw_addr, clean_addr, callinc,
            callinc * 4 + 2);
    skip_log++;
  }

  return clean_addr;
}

uint32_t HELPER(mofei_skip_fn_return_one)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  env->regs[callinc * 4 + 2] = 1;

  static int skip_one_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_SKIP") && skip_one_log < 30) {
    fprintf(stderr, "[SKIP-FN] raw=0x%08x clean=0x%08x callinc=%u RET=1 (true)\n", raw_addr, clean_addr, callinc);
    skip_one_log++;
  }

  return clean_addr;
}

uint32_t HELPER(mofei_skip_fn_i2c_transfer)(CPUXtensaState* env, uint32_t transfer_kind) {
  enum { MOFEI_I2C_DIRECT_MAX_BYTES = 4096, MOFEI_I2C_DIRECT_MAX_BUFFERS = 8 };
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned base_reg = callinc * 4;
  const uint32_t device = mofei_logical_reg_or_zero(env, base_reg + 2);
  uint8_t device_address_bytes[2] = {};
  uint32_t write_ptr = 0;
  uint32_t write_size = 0;
  uint32_t read_ptr = 0;
  uint32_t read_size = 0;
  uint8_t* write_data = NULL;
  uint8_t* read_data = NULL;
  int result = -1;

  if (device == 0 || !mofei_read_guest_memory(device + 4u, device_address_bytes, sizeof(device_address_bytes))) {
    goto done;
  }
  const uint16_t device_address = (uint16_t)device_address_bytes[0] | ((uint16_t)device_address_bytes[1] << 8u);

  switch (transfer_kind) {
    case MOFEI_I2C_TRANSFER_TRANSMIT:
      write_ptr = mofei_logical_reg_or_zero(env, base_reg + 3);
      write_size = mofei_logical_reg_or_zero(env, base_reg + 4);
      break;
    case MOFEI_I2C_TRANSFER_RECEIVE:
      read_ptr = mofei_logical_reg_or_zero(env, base_reg + 3);
      read_size = mofei_logical_reg_or_zero(env, base_reg + 4);
      break;
    case MOFEI_I2C_TRANSFER_TRANSMIT_RECEIVE:
      write_ptr = mofei_logical_reg_or_zero(env, base_reg + 3);
      write_size = mofei_logical_reg_or_zero(env, base_reg + 4);
      read_ptr = mofei_logical_reg_or_zero(env, base_reg + 5);
      read_size = mofei_logical_reg_or_zero(env, base_reg + 6);
      break;
    case MOFEI_I2C_TRANSFER_MULTI_TRANSMIT: {
      const uint32_t buffers = mofei_logical_reg_or_zero(env, base_reg + 3);
      const uint32_t buffer_count = mofei_logical_reg_or_zero(env, base_reg + 4);
      if (buffers == 0 || buffer_count == 0 || buffer_count > MOFEI_I2C_DIRECT_MAX_BUFFERS) goto done;
      uint8_t descriptors[MOFEI_I2C_DIRECT_MAX_BUFFERS * 8] = {};
      if (!mofei_read_guest_memory(buffers, descriptors, buffer_count * 8u)) goto done;
      for (uint32_t i = 0; i < buffer_count; ++i) {
        const uint32_t segment_size = mofei_load_le32(&descriptors[i * 8u + 4u]);
        if (segment_size > MOFEI_I2C_DIRECT_MAX_BYTES - write_size) goto done;
        write_size += segment_size;
      }
      write_data = g_malloc0(write_size > 0 ? write_size : 1u);
      uint32_t offset = 0;
      for (uint32_t i = 0; i < buffer_count; ++i) {
        const uint32_t segment_ptr = mofei_load_le32(&descriptors[i * 8u]);
        const uint32_t segment_size = mofei_load_le32(&descriptors[i * 8u + 4u]);
        if (segment_size > 0 &&
            (segment_ptr == 0 || !mofei_read_guest_memory(segment_ptr, write_data + offset, segment_size))) {
          goto done;
        }
        offset += segment_size;
      }
      break;
    }
    default:
      goto done;
  }

  if (write_size > MOFEI_I2C_DIRECT_MAX_BYTES || read_size > MOFEI_I2C_DIRECT_MAX_BYTES ||
      (write_size > 0 && write_ptr == 0 && transfer_kind != MOFEI_I2C_TRANSFER_MULTI_TRANSMIT) ||
      (read_size > 0 && read_ptr == 0)) {
    goto done;
  }
  if (write_size > 0 && write_data == NULL) {
    write_data = g_malloc(write_size);
    if (!mofei_read_guest_memory(write_ptr, write_data, write_size)) goto done;
  }
  if (read_size > 0) read_data = g_malloc0(read_size);

  result = esp32s3_i2c_direct_transfer(device_address, write_data, write_size, read_data, read_size);
  if (result == 0 && read_size > 0 &&
      address_space_write(&address_space_memory, read_ptr, MEMTXATTRS_UNSPECIFIED, read_data, read_size) != MEMTX_OK) {
    result = -1;
  }

done:
  g_free(write_data);
  g_free(read_data);
  env->regs[base_reg + 2] = result == 0 ? 0u : UINT32_MAX;
  static uint32_t i2c_direct_log_count;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_I2C") && i2c_direct_log_count++ < 64) {
    fprintf(stderr, "[ESP32S3-I2C-DIAG] direct kind=%u device=0x%08x write=%u read=%u result=%d ret=0x%08x\n",
            transfer_kind, device, write_size, read_size, result, clean_addr);
  }
  return clean_addr;
}

uint32_t HELPER(mofei_try_spi_radio_transfer)(CPUXtensaState* env) {
  enum {
    MOFEI_SPI_DIRECT_MAX_BYTES = 4096,
    MOFEI_SPI_TRANS_USE_RXDATA = BIT(2),
    MOFEI_SPI_TRANS_USE_TXDATA = BIT(3),
  };
  if (!esp32s3_gpspi2_radio_selected()) return 0;

  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned base_reg = callinc * 4;
  const uint32_t transaction = mofei_logical_reg_or_zero(env, base_reg + 3);
  uint8_t descriptor[40] = {0};
  uint8_t* tx_data = NULL;
  uint8_t* rx_data = NULL;
  uint32_t tx_guest = 0;
  uint32_t rx_guest = 0;
  uint32_t flags = 0;
  uint32_t transfer_bits = 0;
  uint32_t transfer_bytes = 0;
  uint32_t receive_bits = 0;
  uint32_t receive_bytes = 0;
  uint8_t tx_first = 0;
  int result = -1;

  if (transaction == 0 || !mofei_read_guest_memory(transaction, descriptor, sizeof(descriptor))) goto done;
  flags = mofei_load_le32(&descriptor[0]);
  transfer_bits = mofei_load_le32(&descriptor[16]);
  receive_bits = mofei_load_le32(&descriptor[20]);
  if ((transfer_bits & 7u) != 0 || (receive_bits & 7u) != 0) goto done;
  transfer_bytes = transfer_bits / 8u;
  receive_bytes = receive_bits == 0 ? transfer_bytes : receive_bits / 8u;
  if (transfer_bytes == 0 || transfer_bytes > MOFEI_SPI_DIRECT_MAX_BYTES) goto done;
  if (receive_bytes > transfer_bytes) goto done;

  tx_data = g_malloc(transfer_bytes);
  if ((flags & MOFEI_SPI_TRANS_USE_TXDATA) != 0) {
    if (transfer_bytes > 4u) goto done;
    memcpy(tx_data, &descriptor[32], transfer_bytes);
  } else {
    tx_guest = mofei_load_le32(&descriptor[32]);
    if (tx_guest == 0 || !mofei_read_guest_memory(tx_guest, tx_data, transfer_bytes)) goto done;
  }
  tx_first = tx_data[0];

  rx_data = g_malloc0(transfer_bytes);
  if ((flags & MOFEI_SPI_TRANS_USE_RXDATA) != 0) {
    if (receive_bytes > 4u) goto done;
    rx_guest = transaction + 36u;
  } else {
    rx_guest = mofei_load_le32(&descriptor[36]);
  }

  result = esp32s3_gpspi2_direct_radio_transfer(tx_data, rx_data, transfer_bytes);
  if (result == 0 && rx_guest != 0 && receive_bytes > 0 &&
      address_space_write(&address_space_memory, rx_guest, MEMTXATTRS_UNSPECIFIED, rx_data, receive_bytes) !=
          MEMTX_OK) {
    result = -1;
  }

done:
  g_free(tx_data);
  g_free(rx_data);
  env->regs[base_reg + 2] = result == 0 ? 0u : UINT32_MAX;
  static uint32_t spi_direct_log_count;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_SPI") && spi_direct_log_count++ < 64) {
    fprintf(stderr,
            "[ESP32S3-SPI-DIAG] direct-radio transaction=0x%08x flags=0x%08x bits=%u tx=0x%08x "
            "rx=0x%08x tx0=0x%02x bytes=%u rx_bytes=%u result=%d ret=0x%08x\n",
            transaction, flags, transfer_bits, tx_guest, rx_guest, tx_first, transfer_bytes, receive_bytes, result,
            clean_addr);
  }
  return clean_addr;
}

uint32_t HELPER(mofei_gpio_set_level)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned return_reg = callinc * 4 + 2;
  const uint32_t gpio_num = env->regs[return_reg];
  const uint32_t level = env->regs[return_reg + 1];
  if ((mofei_sim_board_is_lilygo_t5s3_pro() &&
       (gpio_num == 12u || gpio_num == 39u || gpio_num == 40u || gpio_num == 46u)) ||
      (mofei_sim_board_is_m5papers3() && gpio_num == 47u)) {
    const bool upper_bank = gpio_num >= 32u;
    const uint32_t bit = BIT(gpio_num & 31u);
    const uint32_t set_addr = upper_bank ? MOFEI_ESP32S3_GPIO_OUT1_W1TS : MOFEI_ESP32S3_GPIO_OUT_W1TS;
    const uint32_t clear_addr = upper_bank ? MOFEI_ESP32S3_GPIO_OUT1_W1TC : MOFEI_ESP32S3_GPIO_OUT_W1TC;
    mofei_write_mmio_u32(level != 0 ? set_addr : clear_addr, bit);
  }
  /* gpio_set_level() 對所有有效 GPIO 都回傳 esp_err_t。板級 MMIO 副作用
   * 可以不發生，但回傳暫存器不能保留 pin 編號，否則一般 reset/interrupt
   * 腳位會被 firmware 誤判為驅動錯誤。 */
  env->regs[return_reg] = 0;
  return clean_addr;
}

uint32_t HELPER(mofei_gpio_get_level)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned base_reg = callinc * 4;
  const uint32_t pin = mofei_logical_reg_or_zero(env, base_reg + 2);
  uint32_t level = 1;
  if (mofei_sim_board_is_lilygo_t5s3_pro()) {
    if (pin == LILYGO_SX1262_BUSY_GPIO) {
      level = lilygo_sx1262_board_busy() ? 1u : 0u;
    } else if (pin == LILYGO_SX1262_DIO1_GPIO) {
      level = lilygo_sx1262_board_dio1() ? 1u : 0u;
    }
  }
  env->regs[base_reg + 2] = level;
  return clean_addr;
}

uint32_t HELPER(mofei_esp_efuse_read_field_blob)(CPUXtensaState* env) {
  uint32_t raw_addr;
  unsigned callinc;
  const uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const unsigned arg_base = callinc * 4;
  const uint32_t field = mofei_logical_reg_or_zero(env, arg_base + 2);
  const uint32_t dest = mofei_logical_reg_or_zero(env, arg_base + 3);
  const uint32_t dest_size_bits = mofei_logical_reg_or_zero(env, arg_base + 4);
  uint32_t dest_size_bytes = (dest_size_bits + 7u) / 8u;

  if (dest != 0 && dest_size_bytes > 0) {
    if (dest_size_bytes > 1024u) {
      dest_size_bytes = 1024u;
    }
    mofei_memset_guest(env, dest, 0, dest_size_bytes);
  }
  env->regs[arg_base + 2] = 0;

  static int efuse_log = 0;
  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_EFUSE") && efuse_log < 32) {
    fprintf(stderr, "[EFUSE] read_field_blob field=0x%08x dest=0x%08x bits=%u bytes=%u ret=0x%08x\n", field, dest,
            dest_size_bits, dest_size_bytes, clean_addr);
    efuse_log++;
  }

  return clean_addr;
}

void HELPER(mofei_throttle_loop)(CPUXtensaState* env) {
  static int loop_throttle_ms = -1;
  static int log_count = 0;
  (void)env;

  if (loop_throttle_ms < 0) {
    const char* value = getenv("MOFEI_SIM_LOOP_THROTTLE_MS");
    loop_throttle_ms = 2;
    if (value && value[0] != '\0') {
      char* end = NULL;
      const long parsed = strtol(value, &end, 10);
      if (end != value) {
        loop_throttle_ms = parsed < 0 ? 0 : (parsed > 50 ? 50 : (int)parsed);
      }
    }
    if (loop_throttle_ms > 0) {
      fprintf(stderr, "[QEMU-SIM] loop throttle: %d ms per firmware loop()\n", loop_throttle_ms);
    } else {
      fprintf(stderr, "[QEMU-SIM] loop throttle disabled\n");
    }
  }

  if (loop_throttle_ms <= 0) {
    return;
  }

  if (mofei_trace_enabled("MOFEI_SIM_DEBUG_LOOP_THROTTLE") && log_count < 8) {
    fprintf(stderr, "[QEMU-SIM] loop throttle sleep %d ms\n", loop_throttle_ms);
    log_count++;
  }
  g_usleep((gulong)loop_throttle_ms * 1000);
}

uint32_t HELPER(mofei_touch_read)(CPUXtensaState* env, uint32_t x_ptr, uint32_t y_ptr, uint32_t action_ptr,
                                  uint32_t touch_id_ptr) {
  uint32_t raw_addr;
  unsigned callinc;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const bool ok = g_touch_queue_count > 0;
  if (ok) {
    MofeiTouchSample sample = g_touch_queue[g_touch_queue_head];
    g_touch_queue_head = (g_touch_queue_head + 1) % MOFEI_TOUCH_QUEUE_CAP;
    g_touch_queue_count--;
    if (x_ptr != 0) {
      cpu_stw_data(env, x_ptr, sample.x);
    }
    if (y_ptr != 0) {
      cpu_stw_data(env, y_ptr, sample.y);
    }
    if (action_ptr != 0) {
      cpu_stb_data(env, action_ptr, sample.action);
    }
    if (touch_id_ptr != 0) {
      cpu_stb_data(env, touch_id_ptr, sample.touch_id);
    }
    fprintf(stderr, "[MOFEI-SIM] touch_read raw=%u,%u action=%u touch_id=%u depth=%u ptrs=%08x,%08x,%08x,%08x\n",
            sample.x, sample.y, sample.action, sample.touch_id, g_touch_queue_count, x_ptr, y_ptr, action_ptr,
            touch_id_ptr);
  }
  /* The firmware touch hook is intercepted at the callee ENTRY PC and jumps
   * directly back to the caller, so ENTRY/RETW never run. Match the other
   * skipped-CALLN helpers by writing the return value into the caller-visible
   * return register for the active callinc. */
  env->regs[callinc * 4 + 2] = ok ? 1 : 0;
  return clean_addr;
}

uint32_t HELPER(mofei_button_read)(CPUXtensaState* env, uint32_t button_id_ptr, uint32_t released_ptr) {
  uint32_t raw_addr;
  unsigned callinc;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  const bool ok = g_button_queue_count > 0;
  if (ok) {
    MofeiButtonSample sample = g_button_queue[g_button_queue_head];
    g_button_queue_head = (g_button_queue_head + 1) % MOFEI_BUTTON_QUEUE_CAP;
    g_button_queue_count--;
    if (button_id_ptr != 0) {
      cpu_stb_data(env, button_id_ptr, sample.button_id);
    }
    if (released_ptr != 0) {
      cpu_stb_data(env, released_ptr, sample.released ? 1 : 0);
    }
    fprintf(stderr, "[MOFEI-SIM] button_read id=%u released=%d depth=%u ptrs=%08x,%08x\n", sample.button_id,
            sample.released ? 1 : 0, g_button_queue_count, button_id_ptr, released_ptr);
  }
  env->regs[callinc * 4 + 2] = ok ? 1 : 0;
  if (ok) {
    fprintf(stderr, "[MOFEI-SIM] button_ret ok=1 callinc=%u raw=%08x clean=%08x retreg=A%u value=%u\n", callinc,
            raw_addr, clean_addr, callinc * 4 + 2, env->regs[callinc * 4 + 2]);
  }
  return clean_addr;
}

uint32_t HELPER(mofei_trace_file_browser_directory)(CPUXtensaState* env, uint32_t path_ptr, uint32_t entry_count) {
  uint32_t raw_addr;
  unsigned callinc;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  char path[160] = {0};

  if (path_ptr != 0) {
    mofei_read_guest_memory(path_ptr, (uint8_t*)path, sizeof(path) - 1);
  }
  fprintf(stderr, "E2E:FILE_BROWSER:path=%s:entries=%u\n", path, entry_count);
  env->regs[callinc * 4 + 2] = 0;
  return clean_addr;
}

uint32_t HELPER(mofei_trace_rpipe)(CPUXtensaState* env, uint32_t line_ptr) {
  uint32_t raw_addr;
  unsigned callinc;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  char line[1536] = {0};

  if (line_ptr != 0) {
    mofei_read_guest_memory(line_ptr, (uint8_t*)line, sizeof(line) - 1);
  }
  fprintf(stderr, "[SIM-RPIPE] %s\n", line);
  env->regs[callinc * 4 + 2] = 0;
  return clean_addr;
}

uint32_t HELPER(mofei_trace_opds_fetch)(CPUXtensaState* env, uint32_t url_ptr, uint32_t entry_count,
                                        uint32_t has_search) {
  uint32_t raw_addr;
  unsigned callinc;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  char url[192] = {0};

  if (url_ptr != 0) {
    mofei_read_guest_memory(url_ptr, (uint8_t*)url, sizeof(url) - 1);
  }
  fprintf(stderr, "E2E:OPDS_FETCH:url=%s entries=%u search=%u\n", url, entry_count, has_search ? 1u : 0u);
  env->regs[callinc * 4 + 2] = 0;
  return clean_addr;
}

uint32_t HELPER(mofei_trace_opds_search)(CPUXtensaState* env, uint32_t url_ptr, uint32_t query_ptr) {
  uint32_t raw_addr;
  unsigned callinc;
  uint32_t clean_addr = mofei_skip_fn_common(env, &raw_addr, &callinc);
  char url[192] = {0};
  char query[96] = {0};

  if (url_ptr != 0) {
    mofei_read_guest_memory(url_ptr, (uint8_t*)url, sizeof(url) - 1);
  }
  if (query_ptr != 0) {
    mofei_read_guest_memory(query_ptr, (uint8_t*)query, sizeof(query) - 1);
  }
  fprintf(stderr, "E2E:OPDS_SEARCH:url=%s query=%s\n", url, query);
  env->regs[callinc * 4 + 2] = 0;
  return clean_addr;
}

/* Intercept callx8 to xTaskCreatePinnedToCore (0x40381750).
 * Called from translate_callxw at translation time when PC matches
 * ActivityManager::begin's callx8 instruction (PC 0x42061993).
 * Writes a non-NULL sentinel to *a15 (pxCreatedTask output param)
 * and sets the return value register (a10) to pdPASS (1). */
void HELPER(mofei_callx_intercept)(CPUXtensaState* env, uint32_t target) {
  /* a15 = pxCreatedTask output pointer in current window */
  uint32_t pxCreatedTask = env->regs[15];
  if (pxCreatedTask != 0 && pxCreatedTask > 0x3FC00000 && pxCreatedTask < 0x40000000) {
    cpu_stl_data(env, pxCreatedTask, 0x3FFE0100);
  }
  /* Return pdPASS in a10 (return register for callinc=2).
   * For callx8 (callinc=2), caller's A10 = callee's A2 in the rotated window.
   * We write to env->regs[2] so it survives window sync. */
  env->regs[2] = 1;
  fprintf(stderr, "[CALLX-INTERCEPT] xTaskCreatePinnedToCore(0x%x) → pdPASS, handle_ptr=0x%x\n", target, pxCreatedTask);
}

void HELPER(mofei_startup_sync_flags)(CPUXtensaState* env, uint32_t call_pc) {
  static int log_count = 0;
  uint8_t one = 1;
  uint8_t system0 = 0;
  uint8_t system1 = 0;

  if (mofei_sim_addrs.start_cpu0_addr && call_pc == mofei_sim_addrs.start_cpu0_addr + 0x67) {
    cpu_stb_data(env, env->regs[1], one);
  }

  if (mofei_sim_addrs.s_cpu_up_1_addr) {
    cpu_stb_data(env, mofei_sim_addrs.s_cpu_up_1_addr - 1, one);
    cpu_stb_data(env, mofei_sim_addrs.s_cpu_up_1_addr, one);
  }
  if (mofei_sim_addrs.s_cpu_inited_1_addr) {
    cpu_stb_data(env, mofei_sim_addrs.s_cpu_inited_1_addr - 1, one);
    cpu_stb_data(env, mofei_sim_addrs.s_cpu_inited_1_addr, one);
  }
  if (mofei_sim_addrs.s_system_inited_1_addr) {
    cpu_stb_data(env, mofei_sim_addrs.s_system_inited_1_addr - 1, one);
    cpu_stb_data(env, mofei_sim_addrs.s_system_inited_1_addr, one);
    system0 = (uint8_t)cpu_ldub_data(env, mofei_sim_addrs.s_system_inited_1_addr - 1);
    system1 = (uint8_t)cpu_ldub_data(env, mofei_sim_addrs.s_system_inited_1_addr);
  }
  if (mofei_sim_addrs.s_system_full_inited_addr) {
    cpu_stb_data(env, mofei_sim_addrs.s_system_full_inited_addr, one);
  }
  if (log_count < 4) {
    fprintf(stderr, "[QEMU] startup sync flags refreshed system=[%u,%u]\n", system0, system1);
    log_count++;
  }
}

/* Safe RETW for patched functions: perform the useful RETW state transition
 * without invoking the guest window-underflow vector.
 *
 * A normal QEMU RETW calls xtensa_rotate_window(), which first copies the
 * current TCG register window to phys_regs, then switches WINDOW_BASE, then
 * reloads env->regs[] from the caller's physical window.  If WINDOW_START says
 * that caller window is spilled, mirror the Xtensa underflow vector and reload
 * the caller registers from the stack before resuming. */
static uint32_t mofei_restore_calln_window_common(CPUXtensaState* env, uint32_t pc, uint32_t ret_addr,
                                                  unsigned* out_caller_ret_reg, const char* log_prefix) {
  unsigned wb = env->sregs[WINDOW_BASE];
  uint32_t clean_ret_addr = mofei_canonical_calln_return_pc(ret_addr);
  unsigned callinc = (ret_addr >> 30) & 0x3;
  const char* prefix = log_prefix ? log_prefix : "SAFE-RETW";
  if (callinc == 0) {
    static int illegal_log_count = 0;
    if (illegal_log_count < 8) {
      fprintf(stderr, "[%s] illegal marker pc=0x%08x A0=0x%08x ret_pc=0x%08x PS=0x%08x\n", prefix, pc, ret_addr,
              clean_ret_addr, env->sregs[PS]);
      illegal_log_count++;
    }
    HELPER(exception_cause)(env, pc, ILLEGAL_INSTRUCTION_CAUSE);
    return env->pc;
  }
  unsigned nwb = env->config->nareg / 4;
  unsigned target_wb = (wb - callinc) % nwb;
  unsigned caller_ret_reg = callinc * 4 + 2;
  bool needs_stack_restore = (env->sregs[WINDOW_START] & (1u << target_wb)) == 0;
  static int log_count = 0;
  if (log_count < 20) {
    fprintf(stderr, "[%s] wb=%u callinc=%u target_wb=%u A0=0x%08x ret_pc=0x%08x stack=%u WS_old=0x%x\n", prefix, wb,
            callinc, target_wb, ret_addr, clean_ret_addr, needs_stack_restore ? 1 : 0, env->sregs[WINDOW_START]);
    log_count++;
  }

  /* Match xtensa_rotate_window(): sync the current (callee) logical register
   * window into phys_regs before selecting the caller window.  This preserves
   * A0 at the caller's A4/A8/A12 slot and A2 at the caller's return register
   * (A6/A10/A14), including values just written by malloc/queue helpers. */
  xtensa_sync_phys_from_window(env);

  env->sregs[WINDOW_START] &= ~(1u << wb);
  env->sregs[WINDOW_BASE] = target_wb;
  env->windowbase_next = target_wb;
  xtensa_sync_window_from_phys(env);

  if (needs_stack_restore) {
    mofei_load_window_underflow(env, callinc * 4);
  }

  env->sregs[WINDOW_START] |= (1u << target_wb);
  xtensa_sync_phys_from_window(env);

  if (log_count < 20) {
    fprintf(stderr, "[%s] WS_new=0x%x new_A0=0x%08x ret_reg=A%u ret=0x%08x\n", prefix, env->sregs[WINDOW_START],
            env->regs[0], caller_ret_reg, env->regs[caller_ret_reg]);
  }

  if (out_caller_ret_reg) {
    *out_caller_ret_reg = caller_ret_reg;
  }

  return clean_ret_addr;
}

static uint32_t mofei_retw_safe_common(CPUXtensaState* env, uint32_t pc, uint32_t ret_addr, uint32_t return_value) {
  unsigned caller_ret_reg;
  uint32_t clean_ret_addr = mofei_restore_calln_window_common(env, pc, ret_addr, &caller_ret_reg, "SAFE-RETW");
  env->regs[caller_ret_reg] = return_value;
  xtensa_sync_phys_from_window(env);
  return clean_ret_addr;
}

uint32_t HELPER(mofei_retw_safe)(CPUXtensaState* env, uint32_t pc) {
  return mofei_retw_safe_common(env, pc, env->regs[0], env->regs[2]);
}

/* Provide CPU env access for the SSD1677 inject timer's guest-memory reads. */
CPUXtensaState* mofei_get_cpu_env(void) {
  CPUState* cpu0 = first_cpu;
  if (!cpu0) {
    return NULL;
  }
  return &XTENSA_CPU(cpu0)->env;
}

/* Read guest memory into a host buffer.  Used by the SSD1677 inject timer
 * to re-read the framebuffer from guest memory without needing CPU types. */
bool mofei_read_guest_memory(uint32_t guest_addr, uint8_t* dest, uint32_t len) {
  CPUState* cpu0 = first_cpu;
  if (!cpu0 || !dest || len == 0) {
    return false;
  }

  if (mofei_guest_range_in_flash_cache_overlay(guest_addr, len) &&
      address_space_read(&address_space_memory, guest_addr, MEMTXATTRS_UNSPECIFIED, dest, len) == MEMTX_OK) {
    return true;
  }

  if (mofei_guest_range_in_sim_ram(guest_addr, len)) {
    CPUXtensaState* env = &XTENSA_CPU(cpu0)->env;
    for (uint32_t i = 0; i < len; ++i) {
      dest[i] = (uint8_t)cpu_ldub_data(env, guest_addr + i);
    }
    return true;
  }

  if (mofei_sdmmc_shadow_read(guest_addr, dest, len)) {
    return true;
  }

  if (cpu_memory_rw_debug(cpu0, guest_addr, dest, len, 0) == 0) {
    return true;
  }

  if (address_space_read(&address_space_memory, guest_addr, MEMTXATTRS_UNSPECIFIED, dest, len) == MEMTX_OK) {
    return true;
  }
  return false;
}

/* Display-bypass: start the periodic framebuffer push timer and wire the
 * SSD1677 to read directly from the guest's MofeiDisplay framebuffer.
 *
 * Strategy: the SPI FSM path in SSD1677 produces structured-but-garbled
 * output during bulk WRITE_RAM (0x24) transfers, because the FSM cannot
 * cleanly handle the high-throughput pixel stream. Instead, we let the
 * firmware do its full rendering into its own PSRAM framebuffer, then have
 * the SSD1677 inject path copy that buffer over to its internal fb[] on
 * publish hooks, with a short realtime timer as a fallback refresh path.
 *
 * The simulator firmware routes GfxRenderer writes through gfxSimulatorFrameBuffer().
 * Prefer the exported static framebuffer symbol so QEMU samples the same storage
 * that clearScreen/fillRect/drawPixel mutate; fall back to the renderer field for
 * older builds where the symbol was unavailable.
 */
void HELPER(mofei_inject_framebuffer)(CPUXtensaState* env) {
  /* Forward declarations — defined in hw/display/ssd1677_gdeq0426t82.c */
  extern void ssd1677_start_inject_timer(void);
  extern void ssd1677_publish_injected_framebuffer(void);
  extern void ssd1677_set_fb_guest_addr(uint32_t addr);
  extern void ssd1677_set_fb_data_guest_addr(uint32_t addr);
  extern void ssd1677_publish_direct_framebuffer(const uint8_t* payload, uint32_t len);
  extern void uc8253c_publish_injected_framebuffer(void);
  extern void uc8253c_set_fb_guest_addr(uint32_t addr);
  extern void uc8253c_set_fb_data_guest_addr(uint32_t addr);
  extern MofeiSimAddrs mofei_sim_addrs;
  static bool injection_wired;
  static uint32_t wired_framebuffer_addr;
  static bool timer_started;

  if (mofei_sim_board_is_lilygo_t5s3_pro() || mofei_sim_board_is_m5papers3()) {
    static uint8_t lilygo_framebuffer[MOFEI_LILYGO_FRAMEBUFFER_BYTES];
    static uint32_t lilygo_publish_count;
    uint32_t framebuffer_addr = 0;
    uint32_t framebuffer_object = 0;
    uint32_t object_framebuffer_addr = 0;
    if (mofei_sim_addrs.murphySimulatorFramebuffer_addr) {
      framebuffer_addr = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorFramebuffer_addr);
    }
    if (mofei_sim_addrs.murphySimulatorFramebufferObject_addr) {
      framebuffer_object = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorFramebufferObject_addr);
      if (framebuffer_object != 0) {
        /* Framebuffer ABI on Xtensa: Size(8), stride(4), format(1)+padding(3), bits(4). */
        object_framebuffer_addr = mofei_read_guest_data_u32(env, framebuffer_object + 16u);
      }
    }
    if (framebuffer_addr == 0) {
      framebuffer_addr = object_framebuffer_addr;
    }

    const bool read_ok = framebuffer_addr != 0 &&
                         framebuffer_addr != mofei_sim_addrs.murphySimulatorFramebufferStorage_addr &&
                         mofei_read_guest_memory(framebuffer_addr, lilygo_framebuffer, sizeof(lilygo_framebuffer));
    uint32_t non_white_bytes = 0;
    if (read_ok) {
      for (uint32_t i = 0; i < sizeof(lilygo_framebuffer); ++i) {
        non_white_bytes += lilygo_framebuffer[i] != 0;
      }
    }
    if (lilygo_publish_count < 8) {
      fprintf(stderr,
              "[LILYGO-FB] publish=%u exported=0x%08x object=0x%08x object_bits=0x%08x selected=0x%08x "
              "read=%u non_white=%u/%u sample=%02x%02x%02x%02x\n",
              lilygo_publish_count + 1u,
              mofei_sim_addrs.murphySimulatorFramebuffer_addr
                  ? mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorFramebuffer_addr)
                  : 0,
              framebuffer_object, object_framebuffer_addr, framebuffer_addr, read_ok ? 1u : 0u, non_white_bytes,
              (uint32_t)sizeof(lilygo_framebuffer), read_ok ? lilygo_framebuffer[0] : 0,
              read_ok ? lilygo_framebuffer[1] : 0, read_ok ? lilygo_framebuffer[2] : 0,
              read_ok ? lilygo_framebuffer[3] : 0);
    }
    lilygo_publish_count++;
    if (read_ok) {
      ssd1677_publish_direct_framebuffer(lilygo_framebuffer, sizeof(lilygo_framebuffer));
    }
    return;
  }

  if (mofei_sim_addrs.murphySimulatorFramebuffer_addr) {
    uint32_t framebuffer_addr = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorFramebuffer_addr);
    if (framebuffer_addr == 0 && mofei_sim_addrs.murphySimulatorShellContext_addr) {
      const uint32_t framebuffer_object =
          mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorShellContext_addr + 4u);
      if (framebuffer_object != 0) {
        framebuffer_addr = mofei_read_guest_data_u32(env, framebuffer_object + 12u);
      }
    }
    if (framebuffer_addr == 0 && mofei_sim_addrs.murphySimulatorFramebufferObject_addr) {
      const uint32_t framebuffer_object =
          mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorFramebufferObject_addr);
      if (framebuffer_object != 0) {
        framebuffer_addr = mofei_read_guest_data_u32(env, framebuffer_object + 12u);
      }
    }
    if (framebuffer_addr == 0 && mofei_murphy_last_framebuffer_object != 0) {
      framebuffer_addr = mofei_read_guest_data_u32(env, mofei_murphy_last_framebuffer_object + 12u);
    }
    if (framebuffer_addr != 0 && framebuffer_addr != wired_framebuffer_addr) {
      if (mofei_sim_board_is_s37uc()) {
        uc8253c_set_fb_guest_addr(0);
        uc8253c_set_fb_data_guest_addr(framebuffer_addr);
      } else {
        ssd1677_set_fb_guest_addr(0);
        ssd1677_set_fb_data_guest_addr(framebuffer_addr);
      }
      fprintf(stderr, "[%s] inject: updated Murphy OS framebuffer data to 0x%08x via 0x%08x\n",
              mofei_sim_board_is_s37uc() ? "UC8253C" : "SSD1677", framebuffer_addr,
              mofei_sim_addrs.murphySimulatorFramebuffer_addr);
      wired_framebuffer_addr = framebuffer_addr;
      injection_wired = true;
    }
  }

  if (!injection_wired) {
    if (mofei_sim_addrs.murphySimulatorFramebuffer_addr) {
      uint32_t framebuffer_addr = 0;
      framebuffer_addr = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorFramebuffer_addr);
      if (framebuffer_addr == 0 && mofei_sim_addrs.murphySimulatorShellContext_addr) {
        const uint32_t framebuffer_object =
            mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorShellContext_addr + 4u);
        if (framebuffer_object != 0) {
          framebuffer_addr = mofei_read_guest_data_u32(env, framebuffer_object + 12u);
        }
      }
      if (framebuffer_addr == 0 && mofei_sim_addrs.murphySimulatorFramebufferObject_addr) {
        const uint32_t framebuffer_object =
            mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorFramebufferObject_addr);
        if (framebuffer_object != 0) {
          framebuffer_addr = mofei_read_guest_data_u32(env, framebuffer_object + 12u);
        }
      }
      if (framebuffer_addr == 0 && mofei_murphy_last_framebuffer_object != 0) {
        framebuffer_addr = mofei_read_guest_data_u32(env, mofei_murphy_last_framebuffer_object + 12u);
      }
      if (framebuffer_addr != 0) {
        if (mofei_sim_board_is_s37uc()) {
          uc8253c_set_fb_guest_addr(0);
          uc8253c_set_fb_data_guest_addr(framebuffer_addr);
        } else {
          ssd1677_set_fb_guest_addr(0);
          ssd1677_set_fb_data_guest_addr(framebuffer_addr);
        }
        fprintf(stderr, "[%s] inject: wired Murphy OS framebuffer data at 0x%08x via 0x%08x\n",
                mofei_sim_board_is_s37uc() ? "UC8253C" : "SSD1677", framebuffer_addr,
                mofei_sim_addrs.murphySimulatorFramebuffer_addr);
        injection_wired = true;
      } else {
        static unsigned null_log_count;
        if (null_log_count < 4) {
          const uint32_t exported_ptr = mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorFramebuffer_addr);
          const uint32_t ctx_object =
              mofei_sim_addrs.murphySimulatorShellContext_addr
                  ? mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorShellContext_addr + 4u)
                  : 0;
          const uint32_t ctx_bits = ctx_object ? mofei_read_guest_data_u32(env, ctx_object + 12u) : 0;
          const uint32_t global_object =
              mofei_sim_addrs.murphySimulatorFramebufferObject_addr
                  ? mofei_read_guest_data_u32(env, mofei_sim_addrs.murphySimulatorFramebufferObject_addr)
                  : 0;
          const uint32_t global_bits = global_object ? mofei_read_guest_data_u32(env, global_object + 12u) : 0;
          const uint32_t last_bits = mofei_murphy_last_framebuffer_object
                                         ? mofei_read_guest_data_u32(env, mofei_murphy_last_framebuffer_object + 12u)
                                         : 0;
          fprintf(stderr,
                  "[%s] inject: Murphy framebuffer candidates exported_ptr=0x%08x ctx_object=0x%08x "
                  "ctx_bits=0x%08x global_object=0x%08x global_bits=0x%08x last_object=0x%08x last_bits=0x%08x\n",
                  mofei_sim_board_is_s37uc() ? "UC8253C" : "SSD1677", exported_ptr, ctx_object, ctx_bits, global_object,
                  global_bits, mofei_murphy_last_framebuffer_object, last_bits);
          null_log_count++;
        }
        fprintf(stderr, "[%s] inject: Murphy OS framebuffer pointer is null at 0x%08x\n",
                mofei_sim_board_is_s37uc() ? "UC8253C" : "SSD1677", mofei_sim_addrs.murphySimulatorFramebuffer_addr);
      }
    } else if (mofei_sim_addrs.gfxSimulatorFrameBuffer_addr) {
      if (mofei_sim_board_is_s37uc()) {
        uc8253c_set_fb_guest_addr(0);
        uc8253c_set_fb_data_guest_addr(mofei_sim_addrs.gfxSimulatorFrameBuffer_addr);
        fprintf(stderr, "[UC8253C] inject: wired static simulator framebuffer at 0x%08x\n",
                mofei_sim_addrs.gfxSimulatorFrameBuffer_addr);
      } else {
        ssd1677_set_fb_guest_addr(0);
        ssd1677_set_fb_data_guest_addr(mofei_sim_addrs.gfxSimulatorFrameBuffer_addr);
        fprintf(stderr, "[SSD1677] inject: wired static simulator framebuffer at 0x%08x\n",
                mofei_sim_addrs.gfxSimulatorFrameBuffer_addr);
      }
      injection_wired = true;
    } else if (mofei_sim_addrs.renderer_addr) {
      uint32_t fb_ptr_addr = mofei_sim_addrs.renderer_addr + MOFEI_GFX_RENDERER_FRAMEBUFFER_PTR_OFFSET;
      if (mofei_sim_board_is_s37uc()) {
        uc8253c_set_fb_guest_addr(fb_ptr_addr);
        uc8253c_set_fb_data_guest_addr(0);
      } else {
        ssd1677_set_fb_guest_addr(fb_ptr_addr);
        ssd1677_set_fb_data_guest_addr(0);
      }
      /* Do not cache the pointed-to framebuffer address here. The firmware can
       * repair GfxRenderer::frameBuffer after this hook runs, so the SSD1677
       * timer must re-read the renderer field each sample instead of keeping a
       * stale PSRAM value from setupDisplayAndFonts(). */
      fprintf(stderr, "[%s] inject: wired direct-read at fb_ptr_addr=0x%08x (renderer=0x%08x + %u)\n",
              mofei_sim_board_is_s37uc() ? "UC8253C" : "SSD1677", fb_ptr_addr, mofei_sim_addrs.renderer_addr,
              MOFEI_GFX_RENDERER_FRAMEBUFFER_PTR_OFFSET);
      injection_wired = true;
    } else {
      fprintf(stderr, "[%s] inject: framebuffer symbols not resolved; direct-read disabled, falling back to SPI FSM\n",
              mofei_sim_board_is_s37uc() ? "UC8253C" : "SSD1677");
    }
  }

  if (mofei_sim_board_is_s37uc()) {
    uc8253c_publish_injected_framebuffer();
  } else {
    ssd1677_publish_injected_framebuffer();
  }

  /* Do NOT start a periodic push timer — every displayBuffer() call in
   * the firmware triggers gfxSimulatorPublishFrameBuffer → RETW inject →
   * this helper → publish.  A periodic timer would push frames between
   * renders, racing with touch-event processing and causing flicker. */
  static bool timer_never_started;  // kept for struct compatibility
  (void)timer_never_started;
  (void)timer_started;
}

void HELPER(mofei_trace_activity)(CPUXtensaState* env, uint32_t pc, uint32_t kind) {
  (void)env;
  const char* label = "unknown";
  switch (kind) {
    case 1:
      label = "Dashboard::render";
      break;
    case 2:
      label = "Settings::render";
      break;
    case 3:
      label = "renderActivitySync";
      break;
    case 24:
      label = "ButtonRemapActivity::render";
      break;
    case 25:
      label = "DeviceDiagnosticsActivity::render";
      break;
    case 26:
      label = "StatusBarSettingsActivity::render";
      break;
    case 4:
      label = "CalendarActivity::render";
      break;
    case 5:
      label = "WeatherClockActivity::render";
      break;
    case 6:
      label = "ArcadeHubActivity::render";
      break;
    case 29:
      label = "Game2048Activity::render";
      break;
    case 7:
      label = "RecentBooksActivity::render";
      break;
    case 8:
      label = "FileBrowserActivity::render";
      break;
    case 38:
      label = "AppletsActivity::render";
      break;
    case 39:
      label = "LuaAppActivity::render";
      break;
    case 9:
      label = "TtfFontSelectActivity::render";
      break;
    case 30:
      label = "TimeZoneSelectActivity::render";
      break;
    case 31:
      label = "TraditionalChineseFontsActivity::render";
      break;
    case 32:
      label = "LanguageSelectActivity::render";
      break;
    case 33:
      label = "SleepWallpaperActivity::render";
      break;
    case 10:
      label = "ReaderActivity::render";
      break;
    case 11:
      label = "EpubReaderActivity::render";
      break;
    case 12:
      label = "TxtReaderActivity::render";
      break;
    case 13:
      label = "XtcReaderActivity::render";
      break;
    case 14:
      label = "ReadingHubActivity::render";
      break;
    case 15:
      label = "StudyHubActivity::render";
      break;
    case 16:
      label = "OpdsServerListActivity::render";
      break;
    case 17:
      label = "OpdsBookBrowserActivity::render";
      break;
    case 18:
      label = "OpdsSettingsActivity::render";
      break;
    case 37:
      label = "DictionaryActivity::render";
      break;
    case 19:
      label = "KeyboardEntryActivity::render";
      break;
    case 20:
      label = "EpubReaderChapterSelectionActivity::render";
      break;
    case 21:
      label = "EpubSearchResultsActivity::render";
      break;
    case 22:
      label = "TxtSearchResultsActivity::render";
      break;
    case 34:
      label = "TxtBookmarksActivity::render";
      break;
    case 35:
      label = "EpubBookmarksActivity::render";
      break;
    case 36:
      label = "EpubReaderFootnotesActivity::render";
      break;
    case 23:
      label = "ReaderFrontlightSelectionActivity::render";
      break;
    case 27:
      label = "StudyCardsTodayActivity::render";
      break;
    case 28:
      label = "EpubReaderPercentSelectionActivity::render";
      break;
    default:
      break;
  }
  fprintf(stderr, "[MOFEI-TRACE] %-24s pc=0x%08x\n", label, pc);
}

void HELPER(mofei_trace_murphy_activity)(CPUXtensaState* env, uint32_t pc, uint32_t scene, uint32_t mode,
                                         uint32_t total_cards, uint32_t active_cards, uint32_t active_window_start) {
  enum {
    MOFEI_FONT_FALLBACK_TRACE_MODE = 0xFE,
    MOFEI_FONT_FALLBACK_SAME_PIXEL_SIZE = 1U << 0,
    MOFEI_FONT_FALLBACK_EMBEDDED_METRICS = 1U << 1,
    MOFEI_FONT_FALLBACK_EMBEDDED_BITMAP = 1U << 2,
    MOFEI_FONT_FALLBACK_EMBEDDED_CACHED_BITMAP = 1U << 3,
    MOFEI_FONT_FALLBACK_FALLBACK_METRICS = 1U << 4,
    MOFEI_FONT_FALLBACK_FALLBACK_BITMAP = 1U << 5,
    MOFEI_FONT_FALLBACK_FALLBACK_CACHED_BITMAP = 1U << 6,
    MOFEI_FONT_FALLBACK_DYNAMIC_METRICS = 1U << 7,
    MOFEI_FONT_FALLBACK_DYNAMIC_BITMAP = 1U << 8,
    MOFEI_FONT_FALLBACK_DYNAMIC_CACHED_BITMAP = 1U << 9,
  };
  if (scene == 0 && mode == MOFEI_FONT_FALLBACK_TRACE_MODE) {
    fprintf(stderr,
            "E2E:FONT_FALLBACK:codepoint=U+%04X:px=%u:same_size=%u:embedded_metrics=%u:embedded_bitmap=%u:"
            "embedded_cached=%u:fallback_metrics=%u:fallback_bitmap=%u:fallback_cached=%u:dynamic_metrics=%u:"
            "dynamic_bitmap=%u:dynamic_cached=%u\n",
            total_cards, active_window_start, (active_cards & MOFEI_FONT_FALLBACK_SAME_PIXEL_SIZE) != 0,
            (active_cards & MOFEI_FONT_FALLBACK_EMBEDDED_METRICS) != 0,
            (active_cards & MOFEI_FONT_FALLBACK_EMBEDDED_BITMAP) != 0,
            (active_cards & MOFEI_FONT_FALLBACK_EMBEDDED_CACHED_BITMAP) != 0,
            (active_cards & MOFEI_FONT_FALLBACK_FALLBACK_METRICS) != 0,
            (active_cards & MOFEI_FONT_FALLBACK_FALLBACK_BITMAP) != 0,
            (active_cards & MOFEI_FONT_FALLBACK_FALLBACK_CACHED_BITMAP) != 0,
            (active_cards & MOFEI_FONT_FALLBACK_DYNAMIC_METRICS) != 0,
            (active_cards & MOFEI_FONT_FALLBACK_DYNAMIC_BITMAP) != 0,
            (active_cards & MOFEI_FONT_FALLBACK_DYNAMIC_CACHED_BITMAP) != 0);
    return;
  }
  const uint32_t vptr = mofei_read_guest_data_u32(env, scene);
  const char* activity = mofei_murphy_activity_for_vptr(vptr);
  if (activity == NULL) {
    static unsigned unknown_count;
    if (unknown_count < 16) {
      unknown_count++;
      fprintf(stderr, "[MURPHY-TRACE] unknown activity scene=0x%08x vptr=0x%08x pc=0x%08x mode=%u\n", scene, vptr, pc,
              mode);
    }
    return;
  }
  fprintf(stderr, "UIREFRESH activity=%s mode=%s\n", activity, mode == 0 ? "full" : "fast");
  if (strcmp(activity, "Study") == 0) {
    fprintf(stderr, "STUDY: catalog cards=%u active=%u start=%u\n", total_cards, active_cards, active_window_start);
  }
}

void HELPER(mofei_trace_epub_pipeline)(CPUXtensaState* env, uint32_t pc, uint32_t kind) {
  (void)env;
  const char* label = "unknown";
  switch (kind) {
    case 1:
      label = "Epub::load";
      break;
    case 2:
      label = "Epub::setupCacheDir";
      break;
    case 3:
      label = "ZipFile::open";
      break;
    case 4:
      label = "BookMetadataCache::beginWrite";
      break;
    case 5:
      label = "BookMetadataCache::beginContentOpfPass";
      break;
    case 6:
      label = "Epub::parseContentOpf";
      break;
    case 7:
      label = "Epub::findContentOpfFile";
      break;
    case 8:
      label = "Epub::readItemContentsToStream";
      break;
    case 9:
      label = "Epub::readItemContentsToUtf8Stream";
      break;
    case 10:
      label = "ZipFile::readFileToStream";
      break;
    case 11:
      label = "ContentOpfParser::write";
      break;
    case 12:
      label = "ContentOpfParser::flush";
      break;
    case 13:
      label = "BookMetadataCache::createSpineEntry";
      break;
    case 14:
      label = "BookMetadataCache::endContentOpfPass";
      break;
    case 15:
      label = "epub::expat_psram::xmlRealloc";
      break;
    case 16:
      label = "BookMetadataCache::beginTocPass";
      break;
    case 17:
      label = "BookMetadataCache::endWrite";
      break;
    case 18:
      label = "BookMetadataCache::buildBookBin";
      break;
    case 19:
      label = "BookMetadataCache::cleanupBuildArtifacts";
      break;
    default:
      break;
  }
  fprintf(stderr, "[MOFEI-EPUB] %-40s pc=0x%08x\n", label, pc);
}

/* Set up goToBoot execution after setupDisplayAndFonts completes.
 *
 * setupDisplayAndFonts was called from setup() via call8 (CALLINC=2).
 * We undo that window rotation (return to setup()'s frame), then set up
 * a new call8 frame for goToBoot with:
 *   A2 (caller frame) = activityManager (this pointer)
 *   A0 (caller frame) = loop_addr with callinc=2 (goToBoot's return target)
 *
 * The translate.c code then jumps to goToBoot's entry point. goToBoot's
 * own entry instruction rotates the window by CALLINC=2, making the
 * caller's A2 visible as A10 in goToBoot's window (standard call8 ABI).
 */
void HELPER(mofei_redirect_to_goToBoot)(CPUXtensaState* env) {
  extern MofeiSimAddrs mofei_sim_addrs;
  if (!mofei_sim_addrs.resolved) {
    fprintf(stderr, "[MOFEI] WARNING: sim addresses not resolved, skipping goToBoot redirect\n");
    return;
  }
  if (!mofei_sim_addrs.goToBoot_addr || !mofei_sim_addrs.activityManager_addr || !mofei_sim_addrs.loop_addr) {
    fprintf(stderr, "[MOFEI] WARNING: goToBoot/activityManager/loop not resolved\n");
    return;
  }

  unsigned wb = env->sregs[WINDOW_BASE];
  unsigned nwb = env->config->nareg / 4;
  unsigned callinc = (env->sregs[PS] & PS_CALLINC) >> PS_CALLINC_SHIFT;
  if (callinc == 0) callinc = 2;
  unsigned caller_wb = (wb - callinc) % nwb;
  unsigned target_wb = (caller_wb + 2) % nwb;  // call8 rotates by 2

  fprintf(stderr, "[GOTOBOOT] WB=%u callinc=%u caller_wb=%u target_wb=%u WS=0x%x\n", wb, callinc, caller_wb, target_wb,
          env->sregs[WINDOW_START]);

  /* We are exiting setupDisplayAndFonts (which was in 'wb').
     We want to return to 'caller_wb' (setup()), then simulate a call8 to goToBoot
     which rotates the window to 'target_wb'.
     So we clear 'wb', ensure 'caller_wb' is set, and set 'target_wb'. */
  env->sregs[WINDOW_START] &= ~(1u << wb);
  env->sregs[WINDOW_START] |= (1u << caller_wb);
  env->sregs[WINDOW_START] |= (1u << target_wb);

  env->sregs[WINDOW_BASE] = target_wb;
  env->windowbase_next = target_wb;
  /* Do NOT sync from phys_regs here — phys_regs for target_wb may be stale.
   * The sync after writing correct values (below) is sufficient. */

  uint32_t loop_addr = mofei_sim_addrs.loop_addr;
  uint32_t a0_loop = (2u << 30) | (loop_addr & 0x3FFFFFFF);

  /* In the new target_wb, A0 is the return address and A2 is the first argument.
     BUT wait, before 'entry' executes, these registers are in the caller's frame
     at A8 and A10 (because callinc=2).
     So we must place them in phys_regs for the CALLEE's window, which is target_wb + 2. */
  unsigned callee_wb = (target_wb + 2) % nwb;
  env->phys_regs[callee_wb * 4 + 0] = a0_loop;
  env->phys_regs[callee_wb * 4 + 2] = mofei_sim_addrs.activityManager_addr;

  xtensa_sync_window_from_phys(env);

  fprintf(stderr, "[GOTOBOOT] Set up target frame: A0=0x%08x (loop@0x%x), A2=0x%08x (activityManager)\n", a0_loop,
          loop_addr, mofei_sim_addrs.activityManager_addr);
  fprintf(stderr, "[GOTOBOOT] PS.CALLINC=%u, goToBoot will entry with callinc=2\n",
          (env->sregs[PS] & PS_CALLINC) >> PS_CALLINC_SHIFT);

  env->sregs[PS] = (env->sregs[PS] & ~PS_CALLINC) | (2 << PS_CALLINC_SHIFT);
}

void HELPER(mofei_debug_print_a0)(CPUXtensaState* env) {
  fprintf(stderr, "[DEBUG] Exception caught: EXCCAUSE=%u, EXCVADDR=0x%08x, EPC1=0x%08x\n", env->sregs[EXCCAUSE],
          env->sregs[EXCVADDR], env->sregs[EPC1]);
}

/* Intercept MofeiDisplay::sendCommand(uint8_t cmd) at ENTRY.
 * The function is entered via call8, so entry has NOT run yet.
 * A3 holds the command byte. Push it directly to the SSD1677
 * and skip the function body entirely (no SPI hardware access).
 * Returns the caller's return address so TCG can jump there. */
uint32_t HELPER(mofei_spi_cmd)(CPUXtensaState* env, uint32_t byte_val) {
  extern void ssd1677_spi_byte(uint8_t byte);
  extern void mofei_set_dc_pin(bool high);
  extern void uc8253c_spi_byte(uint8_t byte);
  extern void uc8253c_set_dc_pin(bool high);
  if (mofei_sim_board_is_s37uc()) {
    uc8253c_set_dc_pin(false);
    uc8253c_spi_byte((uint8_t)byte_val);
  } else {
    mofei_set_dc_pin(false);
    ssd1677_spi_byte((uint8_t)byte_val);
  }

  uint32_t clean_addr = mofei_skip_fn_common(env, NULL, NULL);

  static int cmd_log = 0;
  cmd_log++;
  if (cmd_log <= 200) {
    fprintf(stderr, "[SPI-CMD] #%d byte=0x%02X ret=0x%08x\n", cmd_log, (uint8_t)byte_val, clean_addr);
  }

  return clean_addr;
}

/* Intercept MofeiDisplay::sendData(uint8_t data) at ENTRY.
 * Same mechanism as mofei_spi_cmd but for data bytes. */
uint32_t HELPER(mofei_spi_data)(CPUXtensaState* env, uint32_t byte_val) {
  extern void ssd1677_spi_byte(uint8_t byte);
  extern void mofei_set_dc_pin(bool high);
  extern void uc8253c_spi_byte(uint8_t byte);
  extern void uc8253c_set_dc_pin(bool high);
  if (mofei_sim_board_is_s37uc()) {
    uc8253c_set_dc_pin(true);
    uc8253c_spi_byte((uint8_t)byte_val);
  } else {
    mofei_set_dc_pin(true);
    ssd1677_spi_byte((uint8_t)byte_val);
  }

  uint32_t clean_addr = mofei_skip_fn_common(env, NULL, NULL);

  static int data_log = 0;
  data_log++;
  if (data_log <= 200) {
    fprintf(stderr, "[SPI-DATA] #%d byte=0x%02X ret=0x%08x\n", data_log, (uint8_t)byte_val, clean_addr);
  }

  return clean_addr;
}

/* Intercept MofeiDisplay::sendData(const uint8_t* data, size_t len) at ENTRY.
 * A3 = data pointer, A4 = length. Read the buffer from guest memory
 * and push all bytes to the SSD1677. */
uint32_t HELPER(mofei_spi_data_buf)(CPUXtensaState* env, uint32_t data_ptr, uint32_t len) {
  extern void ssd1677_spi_bytes(const uint8_t* data, uint32_t len);
  extern void mofei_set_dc_pin(bool high);
  extern void uc8253c_spi_bytes(const uint8_t* data, uint32_t len);
  extern void uc8253c_set_dc_pin(bool high);

  if (data_ptr != 0 && len > 0 && len <= 65536) {
    uint8_t* buf = g_malloc(len);
    if (cpu_memory_rw_debug(env_cpu(env), data_ptr, buf, len, 0) != 0) {
      memset(buf, 0xFF, len);
    }
    if (mofei_sim_board_is_s37uc()) {
      uc8253c_set_dc_pin(true);
      uc8253c_spi_bytes(buf, len);
    } else {
      mofei_set_dc_pin(true);
      ssd1677_spi_bytes(buf, len);
    }
    g_free(buf);
  }

  uint32_t clean_addr = mofei_skip_fn_common(env, NULL, NULL);

  static int buf_log = 0;
  buf_log++;
  if (buf_log <= 50) {
    fprintf(stderr, "[SPI-BUF] #%d ptr=0x%08x len=%u ret=0x%08x\n", buf_log, data_ptr, len, clean_addr);
  }

  return clean_addr;
}

void xtensa_cpu_do_unaligned_access(CPUState* cs, vaddr addr, MMUAccessType access_type, int mmu_idx,
                                    uintptr_t retaddr) {
  XtensaCPU* cpu = XTENSA_CPU(cs);
  CPUXtensaState* env = &cpu->env;

  assert(xtensa_option_enabled(env->config, XTENSA_OPTION_UNALIGNED_EXCEPTION));
  cpu_restore_state(CPU(cpu), retaddr);
  HELPER(exception_cause_vaddr)(env, env->pc, LOAD_STORE_ALIGNMENT_CAUSE, addr);
}

bool xtensa_cpu_tlb_fill(CPUState* cs, vaddr address, int size, MMUAccessType access_type, int mmu_idx, bool probe,
                         uintptr_t retaddr) {
  CPUXtensaState* env = cpu_env(cs);
  uint32_t paddr;
  uint32_t page_size;
  unsigned access;
  int ret = xtensa_get_physical_addr(env, true, address, access_type, mmu_idx, &paddr, &page_size, &access);

  qemu_log_mask(CPU_LOG_MMU, "%s(%08" VADDR_PRIx ", %d, %d) -> %08x, ret = %d\n", __func__, address, access_type,
                mmu_idx, paddr, ret);

  if (ret == 0) {
    tlb_set_page(cs, address & TARGET_PAGE_MASK, paddr & TARGET_PAGE_MASK, access, mmu_idx, page_size);
    return true;
  } else if (probe) {
    return false;
  } else {
    cpu_restore_state(cs, retaddr);
    HELPER(exception_cause_vaddr)(env, env->pc, ret, address);
  }
}

void xtensa_cpu_do_transaction_failed(CPUState* cs, hwaddr physaddr, vaddr addr, unsigned size,
                                      MMUAccessType access_type, int mmu_idx, MemTxAttrs attrs, MemTxResult response,
                                      uintptr_t retaddr) {
  CPUXtensaState* env = cpu_env(cs);

  cpu_restore_state(cs, retaddr);
  HELPER(exception_cause_vaddr)(
      env, env->pc, access_type == MMU_INST_FETCH ? INSTR_PIF_ADDR_ERROR_CAUSE : LOAD_STORE_PIF_ADDR_ERROR_CAUSE, addr);
}

void xtensa_runstall(CPUXtensaState* env, bool runstall) {
  CPUState* cpu = env_cpu(env);

  env->runstall = runstall;
  cpu->halted = runstall;
  if (runstall) {
    cpu_interrupt(cpu, CPU_INTERRUPT_HALT);
  } else {
    qemu_cpu_kick(cpu);
  }
}

#endif /* !CONFIG_USER_ONLY */
