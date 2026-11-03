import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const root = fileURLToPath(new URL("../", import.meta.url));
const fixture = readFileSync(new URL("fixtures/xtensa-check-atomctl.c", import.meta.url), "utf8");
function shellFunction(script, name) {
  const start = script.indexOf(`${name}() {`);
  assert.ok(start >= 0);
  return script.slice(start, script.indexOf("\n}", start) + 2);
}
function cFunction(source, name) {
  const start = source.indexOf(`uint32_t HELPER(${name})(`);
  assert.ok(start >= 0);
  let end = source.indexOf("{", start) + 1;
  for (let depth = 1; depth; ++end) {
    if (source[end] === "{") ++depth;
    if (source[end] === "}") --depth;
  }
  return source.slice(start, end);
}

test("native and WASM setup apply the same local-memory atomic patch idempotently", () => {
  const dir = mkdtempSync(join(tmpdir(), "panda-local-atomic-"));
  try {
    const outputs = [];
    for (const [scriptName, functionName, variable] of [
      ["build-qemu-wasm.sh", "patch_qemu_wasm_local_memory_atomics", "QEMU_WASM_SRC_DIR"],
      ["integrate-peripherals.sh", "apply_xtensa_local_memory_atomics", "QEMU_DIR"],
    ]) {
      const inputRoot = join(dir, scriptName);
      const target = join(inputRoot, "target/xtensa/op_helper.c");
      mkdirSync(join(inputRoot, "target/xtensa"), { recursive: true });
      writeFileSync(target, fixture);
      const script = readFileSync(join(root, "scripts", scriptName), "utf8");
      const driver = `set -e\nrequire_tool() { command -v "$1" >/dev/null; }\ndie() { echo "$1" >&2; exit 1; }\n${shellFunction(script, functionName)}\n${functionName}\n${functionName}\n`;
      const run = spawnSync("bash", ["-c", driver], { encoding: "utf8", env: { ...process.env, SIM_ROOT: root, SRC_DIR: join(root, "qemu-peripherals"), [variable]: inputRoot } });
      assert.equal(run.status, 0, run.stderr + run.stdout);
      outputs.push(readFileSync(target, "utf8"));
    }
    assert.equal(outputs[0], outputs[1]);
    assert.match(outputs[0], /paddr - region->addr <= region->size - sizeof\(uint32_t\)/);
    const wasm = readFileSync(join(root, "scripts/build-qemu-wasm.sh"), "utf8");
    assert.match(wasm, /\npatch_qemu_wasm_browser_crypto_compat\npatch_qemu_wasm_local_memory_atomics\n/);
    const native = readFileSync(join(root, "scripts/integrate-peripherals.sh"), "utf8");
    assert.match(shellFunction(native, "sync_simulator_qemu_sources"), /apply_xtensa_local_memory_atomics/);
    const atomic = cFunction(readFileSync(join(root, "qemu-peripherals/target_xtensa_exc_helper.c"), "utf8"), "mofei_s32c1i");
    for (const [name, code, localExpected] of [["before", fixture, 3], ["after", outputs[0], 0]]) {
      const source = `
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#define HELPER(name) helper_##name
#define PAGE_READ 1u
#define PAGE_WRITE 2u
#define PAGE_CACHE_MASK 0x300u
#define PAGE_CACHE_BYPASS 0u
#define PAGE_CACHE_WT 0x100u
#define PAGE_CACHE_WB 0x200u
#define PAGE_CACHE_ISOLATE 0x300u
#define XTENSA_OPTION_DCACHE 1
#define STORE_PROHIBITED_CAUSE 29
#define LOAD_STORE_ERROR_CAUSE 3
enum {ATOMCTL};
struct Config { bool cache; struct { unsigned num; struct XtensaMemoryRegion {uint32_t addr,size;} location[2]; } dataram; };
typedef struct {uint32_t sregs[1];struct Config* config;} CPUXtensaState;
static jmp_buf fault;
static unsigned fault_code,access=3,translation_code,alias;
static uint32_t memory_value;
static bool xtensa_option_enabled(const struct Config* c,int option) {(void)option;return c->cache;}
static int xtensa_get_cring(CPUXtensaState* env) {(void)env;return 0;}
static int xtensa_get_physical_addr(CPUXtensaState* env,bool update,uint32_t vaddr,int rw,int ring,uint32_t* paddr,uint32_t* page,uint32_t* flags) {
 (void)env;(void)update;(void)rw;(void)ring;*paddr=alias?alias:vaddr;*page=4096;*flags=access;return translation_code;
}
static void helper_exception_cause_vaddr(CPUXtensaState* env,uint32_t pc,uint32_t cause,uint32_t vaddr) {
 (void)env;assert(pc==0x4038482e);assert(vaddr);fault_code=cause;longjmp(fault,1);
}
${code}
static bool mofei_read_guest_data_u32_checked(CPUXtensaState* env,uint32_t addr,uint32_t* value) {(void)env;assert(addr==0x3fca06a8);*value=memory_value;return true;}
static void mofei_write_guest_data_u32(CPUXtensaState* env,uint32_t addr,uint32_t value) {(void)env;assert(addr==0x3fca06a8);memory_value=value;}
static bool mofei_trace_enabled(const char* name) {(void)name;return false;}
${atomic}
static void check(CPUXtensaState* env,uint32_t addr,unsigned expected) {
 fault_code=0;
 if(setjmp(fault)==0) {helper_check_atomctl(env,0x4038482e,addr);assert(expected==0);}
 else assert(fault_code==expected);
 assert(env->sregs[ATOMCTL]==0x28);
}
int main(void) {
 struct Config config={.cache=true,.dataram={.num=1,.location={{0x3c000000,0x04000000}}}};
 CPUXtensaState env={.sregs={0x28},.config=&config};
 check(&env,0x3fca06a8,${localExpected});
 config.cache=false;check(&env,0x3fca06a8,${localExpected});config.cache=true;
 check(&env,0x3c000000,${localExpected});check(&env,0x3ffffffc,${localExpected});
 check(&env,0x3ffffffd,3);check(&env,0x40000000,3);check(&env,0x3bfffffc,3);
 alias=0x3fca06a8;check(&env,0x1000,${localExpected});alias=0;
 access=PAGE_READ;check(&env,0x3fca06a8,29);
 access=3;translation_code=24;check(&env,0x3fca06a8,24);translation_code=0;
 access=3|PAGE_CACHE_WB;check(&env,0x50000000,0);
 access=3|PAGE_CACHE_WT;check(&env,0x50000000,0);
 access=3|PAGE_CACHE_ISOLATE;check(&env,0x50000000,3);
 access=3;check(&env,0x50000000,3);
 config.dataram.num=0;check(&env,0x3fca06a8,3);config.dataram.num=1;
 if(${localExpected}==0) {
  check(&env,0x3fca06a8,0);
  assert(helper_mofei_s32c1i(&env,0x3fca06a8,0,1)==0 && memory_value==1);
  assert(helper_mofei_s32c1i(&env,0x3fca06a8,0,2)==1 && memory_value==1);
 }
 puts("local physical memory + permission/cache boundaries + actual CAS PASS");
}
`;
      const file = join(dir, `${name}.c`), binary = join(dir, name);
      writeFileSync(file, source);
      const compiled = spawnSync("cc", ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", file, "-o", binary], { encoding: "utf8" });
      assert.equal(compiled.status, 0, compiled.stderr);
      const run = spawnSync(binary, [], { encoding: "utf8", timeout: 5000 });
      assert.equal(run.status, 0, run.stderr);
      assert.match(run.stdout, /actual CAS PASS/);
    }
  } finally { rmSync(dir, { recursive: true, force: true }); }
});
