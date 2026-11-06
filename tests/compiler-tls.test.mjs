import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { homedir, tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const root = new URL("../", import.meta.url);
const read = name => readFileSync(new URL(`qemu-peripherals/${name}`, root), "utf8");
function fn(source, signature) {
  const start = source.indexOf(signature);
  assert.ok(start >= 0, signature);
  let end = source.indexOf("{", start) + 1;
  for (let depth = 1; depth; end++) {
    if (source[end] === "{") depth++;
    if (source[end] === "}") depth--;
  }
  return source.slice(start, end);
}
function compileAndRun(code, args = []) {
  const dir = mkdtempSync(join(tmpdir(), "panda-compiler-tls-"));
  try {
    writeFileSync(join(dir, "tls.c"), code);
    const compiled = spawnSync(process.env.CC || "cc", ["-std=c11", "-Wall", "-Wextra", "-Werror",
      "-fsanitize=address,undefined", join(dir, "tls.c"), "-o", join(dir, "tls")], { encoding: "utf8" });
    assert.equal(compiled.status, 0, compiled.stderr);
    const run = spawnSync(join(dir, "tls"), args, { encoding: "utf8", timeout: 15000 });
    assert.equal(run.status, 0, run.stderr);
    return run.stdout;
  } finally { rmSync(dir, { recursive: true, force: true }); }
}
const preamble = `
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "${new URL("qemu-peripherals/mofei-sim-addrs.h", root).pathname}"
`;

test("actual serialized main TLS copies template, zeroes BSS and owns its existing stack budget across continuations/reset", () => {
  const helper = read("target_xtensa_exc_helper.c");
  const init = fn(helper, "static bool mofei_initialize_main_tls(");
  const reset = fn(helper, "void mofei_sim_reset_serialized_task(");
  const top = fn(helper, "static uint32_t mofei_main_stack_top(");
  const boot = fn(helper, "static void mofei_prepare_boot_firmware_frame(");
  const continuation = fn(helper, "uint32_t HELPER(mofei_enter_murphy_run_loop)(");
  const code = `${preamble}
#define HELPER(name) helper_##name
#define g_malloc0(n) calloc(1,n)
#define g_free free
#define MEMTXATTRS_UNSPECIFIED 0
#define MEMTX_OK 0
typedef int MemTxResult;
enum {THREADPTR,WINDOW_BASE,WINDOW_START,PS};
#define PS_CALLINC 0x30000u
#define PS_CALLINC_SHIFT 16
#define PS_EXCM 0x10u
#define PS_UM 0x20u
#define PS_WOE 0x40000u
typedef struct { uint32_t pc,regs[16],phys_regs[64],sregs[4],uregs[4],windowbase_next; } CPUXtensaState;
MofeiSimAddrs mofei_sim_addrs;
static int mofei_redirected_to_app_main,mofei_loop_task_entered;
static uint32_t mofei_main_tls_base,mofei_main_tls_threadptr,address_space_memory;
static uint8_t ram[MOFEI_SIM_FAKE_TASK_STACK_BYTES],image[256],adjacent_globals[128];
static unsigned reads,writes;static bool fail_read,fail_write;
static uint8_t* at(uint32_t addr,size_t len) {
 assert(addr>=MOFEI_SIM_FAKE_APP_STACK_BASE && addr<=MOFEI_SIM_FAKE_APP_STACK_TOP);
 assert(len<=MOFEI_SIM_FAKE_APP_STACK_TOP-addr);return ram+addr-MOFEI_SIM_FAKE_APP_STACK_BASE;
}
static bool mofei_read_guest_memory(uint32_t addr,uint8_t* dest,uint32_t len) {
 ++reads;assert(addr==0x3c400000u && len==16);
 if(fail_read)return false;memcpy(dest,image,len);return true;
}
static MemTxResult address_space_write(void* space,uint32_t addr,int attrs,const void* data,size_t len) {
 (void)space;(void)attrs;++writes;if(fail_write)return 1;memcpy(at(addr,len),data,len);return MEMTX_OK;
}
static void cpu_stb_data(CPUXtensaState* env,uint32_t addr,uint8_t value) {(void)env;*at(addr,1)=value;}
static void cpu_stl_data(CPUXtensaState* env,uint32_t addr,uint32_t value) {(void)env;memcpy(at(addr,4),&value,4);}
static void xtensa_sync_phys_from_window(CPUXtensaState* env) {memcpy(env->phys_regs,env->regs,sizeof(env->regs));}
static const char* mofei_murphy_or_app_main_entry_name(void) {return "test_main";}
static bool mofei_murphy_context_pointer_plausible(uint32_t p) {return p!=0;}
static CPUXtensaState* env_cpu(CPUXtensaState* env) {return env;}
static void cpu_abort(CPUXtensaState* cpu,const char* message) {(void)cpu;(void)message;abort();}
${reset}
${top}
${init}
${boot}
${continuation}
static void metadata(void) {
 mofei_sim_addrs=(MofeiSimAddrs){.tls_data_start_addr=0x3c400000,.tls_data_end_addr=0x3c400010,
   .tls_bss_start_addr=0x3c400010,.tls_bss_end_addr=0x3c400045,.tls_section_alignment=4,
   .murphyDeviceShellRunSimulatorLoop_addr=0x42002000};
}
int main(void) {
 CPUXtensaState env={0};metadata();
 for(unsigned i=0;i<sizeof(image);i++)image[i]=(uint8_t)(i+1);
 memset(ram,0x9b,sizeof(ram));memset(adjacent_globals,0x67,sizeof(adjacent_globals));
 uint8_t globals_before[sizeof(adjacent_globals)];memcpy(globals_before,adjacent_globals,sizeof(adjacent_globals));
 env.uregs[THREADPTR]=0x3fca7228u;
 mofei_prepare_boot_firmware_frame(&env);
 const uint32_t base=MOFEI_SIM_FAKE_APP_STACK_TOP-80u;
 assert(mofei_main_tls_base==base && mofei_main_stack_top()==base && env.regs[1]==base);
 assert(env.uregs[THREADPTR]==base-8 && !memcmp(at(base,16),image,16));
 for(unsigned i=16;i<80;i++)assert(*at(base+i,1)==0);
 assert(reads==1 && writes==1 && !memcmp(adjacent_globals,globals_before,sizeof(adjacent_globals)));
 assert(env.regs[1]-MOFEI_SIM_FAKE_APP_STACK_BASE==MOFEI_SIM_FAKE_TASK_STACK_BYTES-80);
 *at(base+4,1)=0x4b;*at(base+60,1)=0xa1;
 unsigned old_reads=reads,old_writes=writes;
 mofei_prepare_boot_firmware_frame(&env);
 assert(reads==old_reads && writes==old_writes && *at(base+4,1)==0x4b && *at(base+60,1)==0xa1);
 env.regs[10]=0x3fd01000;env.regs[11]=0x3fd02000;
 assert(helper_mofei_enter_murphy_run_loop(&env,2)==0x42002000);
 assert(env.regs[1]==base-80 && env.uregs[THREADPTR]==base-8 && *at(base+60,1)==0xa1);
 mofei_redirected_to_app_main=1;mofei_loop_task_entered=1;
 mofei_sim_reset_serialized_task();
 assert(!mofei_main_tls_base && !mofei_redirected_to_app_main && !mofei_loop_task_entered);
 assert(mofei_initialize_main_tls(&env) && *at(base+4,1)==image[4] && !*at(base+60,1));
 for(unsigned align=1;align<=256;align*=2) {
  mofei_sim_reset_serialized_task();mofei_sim_addrs.tls_section_alignment=align;
  assert(mofei_initialize_main_tls(&env));assert(!(mofei_main_tls_base&15));
  unsigned expected_bias=align>8?align:8;assert(env.uregs[THREADPTR]==base-expected_bias);
 }
 for(unsigned bad=0;bad<8;bad++) {
  mofei_sim_reset_serialized_task();metadata();
  if(bad==0)mofei_sim_addrs.tls_section_alignment=0;
  if(bad==1)mofei_sim_addrs.tls_section_alignment=3;
  if(bad==2)mofei_sim_addrs.tls_data_start_addr=0;
  if(bad==3)mofei_sim_addrs.tls_bss_start_addr++;
  if(bad==4)mofei_sim_addrs.tls_data_end_addr--;
  if(bad==5)mofei_sim_addrs.tls_bss_end_addr=mofei_sim_addrs.tls_bss_start_addr-1;
  if(bad==6)mofei_sim_addrs.tls_bss_end_addr=mofei_sim_addrs.tls_data_start_addr+MOFEI_SIM_FAKE_TASK_STACK_BYTES;
  if(bad==7)mofei_sim_addrs.tls_section_alignment=0x80000000u;
  CPUXtensaState saved=env;old_reads=reads;old_writes=writes;
  assert(!mofei_initialize_main_tls(&env));assert(!memcmp(&saved,&env,sizeof(env)) && writes==old_writes && reads==old_reads);
 }
 metadata();fail_read=true;CPUXtensaState saved=env;old_writes=writes;
 assert(!mofei_initialize_main_tls(&env) && !mofei_main_tls_base && writes==old_writes && !memcmp(&saved,&env,sizeof(env)));
 fail_read=false;fail_write=true;
 assert(!mofei_initialize_main_tls(&env) && !mofei_main_tls_base && !memcmp(&saved,&env,sizeof(env)));
 fail_write=false;assert(mofei_initialize_main_tls(&env));
 puts("actual TLS copy/zero/bias, same-owner continuation, reset, stack accounting and failure paths PASS");
}
`;
  assert.match(compileAndRun(code), /failure paths PASS/);
});

test("all serialized main frame ceilings use the TLS reservation; kernel reset discards prior ownership", () => {
  const helper = read("target_xtensa_exc_helper.c"), machine = read("hw_xtensa_esp32s3.c");
  for (const name of ["static void mofei_prepare_boot_firmware_frame(", "uint32_t HELPER(mofei_enter_murphy_run_loop)(",
    "uint32_t HELPER(mofei_prepare_murphy_simulator_loop_frame)("]) {
    const body = fn(helper, name);
    assert.match(body, /mofei_main_stack_top\(\)/);
    assert.doesNotMatch(body, /MOFEI_SIM_FAKE_APP_STACK_TOP/);
  }
  assert.match(fn(machine, "static void mofei_prepare_kernel_call_frame("), /mofei_sim_reset_serialized_task\(\)/);
  assert.equal((helper.match(/mofei_initialize_main_tls\(env\)/g) || []).length, 1);
  const idf = process.env.IDF_PATH || join(homedir(), ".espressif/v6.0.1/esp-idf");
  const port = readFileSync(join(idf, "components/freertos/FreeRTOS-Kernel/portable/xtensa/port.c"), "utf8");
  const sdk = fn(port, "FORCE_INLINE_ATTR UBaseType_t uxInitialiseStackTLS(");
  assert.match(sdk, /memcpy\(\(void \*\)uxStackPointer, &_thread_local_data_start, tls_data_size\)/);
  assert.match(sdk, /memset\(\(void \*\)\(uxStackPointer \+ tls_data_size\), 0, tls_bss_size\)/);
  assert.match(sdk, /#define TCB_SIZE 8/);
});

test("actual C symbol resolver binds complete TLS metadata from the retained guest ELF", { skip: !process.env.PANDA_OBSERVER_ELF }, () => {
  const machine = read("hw_xtensa_esp32s3.c");
  const names = ["_thread_local_data_start", "_thread_local_data_end", "_thread_local_bss_start", "_thread_local_bss_end", "_tls_section_alignment"];
  const nm = process.env.NM || join(homedir(), ".espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm");
  const symbols = spawnSync(nm, ["-n", process.env.PANDA_OBSERVER_ELF], { encoding: "utf8", maxBuffer: 32 * 1024 * 1024 });
  assert.equal(symbols.status, 0, symbols.stderr);
  const expected = names.map(name => {
    const match = new RegExp(`^([0-9a-f]+) [a-zA-Z] ${name}$`, "m").exec(symbols.stdout);
    assert.ok(match, name); return Number.parseInt(match[1], 16);
  });
  const result = compileAndRun(`${preamble}
#define ELFMAG "\\177ELF"
typedef size_t gsize;
typedef struct { uint32_t address; } MofeiFirmwareSymbol;
static const MofeiFirmwareSymbol* mofei_find_firmware_symbol(const char* name) {(void)name;return NULL;}
MofeiSimAddrs mofei_sim_addrs;
${fn(machine, "static uint32_t mofei_resolve_elf_symbol(")}
${fn(machine, "static void mofei_resolve_main_tls_metadata(")}
int main(int argc,char** argv) {
 assert(argc==2);FILE* file=fopen(argv[1],"rb");assert(file);assert(!fseek(file,0,SEEK_END));
 long size=ftell(file);assert(size>0);rewind(file);uint8_t* bytes=malloc((size_t)size);assert(bytes);
 assert(fread(bytes,1,(size_t)size,file)==(size_t)size);fclose(file);
 mofei_resolve_main_tls_metadata(bytes,(size_t)size);
 printf("[%u,%u,%u,%u,%u]\\n",mofei_sim_addrs.tls_data_start_addr,mofei_sim_addrs.tls_data_end_addr,
   mofei_sim_addrs.tls_bss_start_addr,mofei_sim_addrs.tls_bss_end_addr,mofei_sim_addrs.tls_section_alignment);
 free(bytes);
}
`, [process.env.PANDA_OBSERVER_ELF]);
  assert.deepEqual(JSON.parse(result), expected);
  assert.equal(expected[2], expected[1]);
  assert.ok(expected[3] >= expected[2] && expected[4] > 0);
});
