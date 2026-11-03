import test from "node:test";
import assert from "node:assert/strict";
import {readFileSync, mkdtempSync, writeFileSync, rmSync} from "node:fs";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {spawnSync} from "node:child_process";

test("existing bounded TICK diagnostics expose current and caller queue arguments without guest reads",()=>{
  const source=readFileSync(new URL("../qemu-peripherals/hw_xtensa_esp32s3.c",import.meta.url),"utf8");
  const start=source.indexOf("static void mofei_log_tick_registers(");
  const end=source.indexOf("static void mofei_cpu_tick_timer_cb",start);
  assert.ok(start>=0&&end>start);
  const fn=source.slice(start,end);
  assert.doesNotMatch(fn,/address_space_read|cpu_ld|cpu_st/);
  const call=source.indexOf("mofei_log_tick_registers(env);",end);
  assert.ok(call>source.indexOf("(mofei_cpu_tick_count % 1000) == 0",end));
  const dir=mkdtempSync(join(tmpdir(),"panda-tick-regs-"));
  try {
    writeFileSync(join(dir,"tick.c"),`
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#define WINDOW_BASE 0
typedef struct { unsigned nareg; } Config;
typedef struct { uint32_t regs[16],phys_regs[64],sregs[1]; const Config* config; } CPUXtensaState;
${fn}
int main(void) {
 Config config={64}; CPUXtensaState env={.config=&config};
 env.sregs[WINDOW_BASE]=12; env.regs[0]=0x8238d485;
 env.regs[2]=0x3c394198; env.regs[3]=1713; env.regs[4]=0x3c3fcae9;
 env.phys_regs[40]=0x8202813f; env.phys_regs[41]=0x3fd87000; env.phys_regs[42]=0x3ffe12c0;
 mofei_log_tick_registers(&env);
 env.sregs[WINDOW_BASE]=0;env.regs[0]=0xc238d485;env.phys_regs[54]=0x3ffe0040;
 mofei_log_tick_registers(&env);
 env.regs[0]=0; mofei_log_tick_registers(&env);
}
`);
    const build=spawnSync("cc",["-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",join(dir,"tick.c"),"-o",join(dir,"tick")],{encoding:"utf8"});
    assert.equal(build.status,0,build.stderr);
    const run=spawnSync(join(dir,"tick"),[],{encoding:"utf8",timeout:5000});
    assert.equal(run.status,0,run.stderr);
    assert.match(run.stderr,/a2=0x3c394198 a3=0x000006b1/);
    assert.match(run.stderr,/callinc=2 caller_valid=1 caller_wb=10 caller_a0=0x8202813f caller_sp=0x3fd87000 caller_a2=0x3ffe12c0/);
    assert.match(run.stderr,/callinc=3 caller_valid=1 caller_wb=13.*caller_a2=0x3ffe0040/);
    assert.match(run.stderr,/callinc=0 caller_valid=0.*caller_a2=0x00000000/);
  } finally { rmSync(dir,{recursive:true,force:true}); }
});
