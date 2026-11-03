import test from "node:test";
import assert from "node:assert/strict";
import {readFileSync, mkdtempSync, writeFileSync, rmSync} from "node:fs";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {spawnSync} from "node:child_process";

test("first illegal fault register diagnostic runs before fallback under the existing eight-log gate",()=>{
  const source=readFileSync(new URL("../qemu-peripherals/target_xtensa_exc_helper.c",import.meta.url),"utf8");
  const start=source.indexOf("static void mofei_log_illegal_registers(");
  const end=source.indexOf("static bool mofei_handle_illegal_instruction_artifact",start);
  assert.ok(start>=0 && end>start);
  const fn=source.slice(start,end);
  assert.doesNotMatch(fn,/address_space_read|cpu_ld|cpu_st|mofei_read_pc_bytes/);
  const handler=source.slice(end,source.indexOf("void HELPER(exception_cause)",end));
  const log=handler.indexOf("if (resume_log_count < 8) mofei_log_illegal_registers(env, pc);");
  assert.ok(log>handler.indexOf("if (cause != 0)"));
  assert.ok(log<handler.indexOf("resume = mofei_find_fetch_exception_resume"));
  assert.equal((handler.match(/resume_log_count\+\+/g)||[]).length,1);
  const dir=mkdtempSync(join(tmpdir(),"panda-illegal-regs-"));
  try {
    writeFileSync(join(dir,"illegal.c"),`
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
enum {PS,WINDOW_BASE,WINDOW_START,EPC1,DEPC};
typedef struct { uint32_t regs[16],sregs[5]; } CPUXtensaState;
${fn}
int main(void) {
 CPUXtensaState env={0};for(unsigned i=0;i<16;i++)env.regs[i]=0x1000+i;
 env.regs[0]=0;env.regs[1]=0x3fd87570;
 env.sregs[PS]=0x60000;env.sregs[WINDOW_BASE]=6;env.sregs[WINDOW_START]=0x40;
 env.sregs[EPC1]=0x421fc8bf;env.sregs[DEPC]=0x4200ecef;
 CPUXtensaState before=env;mofei_log_illegal_registers(&env,0x421fc8bf);
 assert(!memcmp(&before,&env,sizeof(env)));
}
`);
    const build=spawnSync("cc",["-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",join(dir,"illegal.c"),"-o",join(dir,"illegal")],{encoding:"utf8"});
    assert.equal(build.status,0,build.stderr);
    const run=spawnSync(join(dir,"illegal"),[],{encoding:"utf8",timeout:5000});
    assert.equal(run.status,0,run.stderr);
    assert.match(run.stderr,/fault_pc=421fc8bf A0=00000000 SP=3fd87570 PS=00060000 WB=6 WS=00000040/);
    assert.match(run.stderr,/EPC1=421fc8bf DEPC=4200ecef/);
    assert.match(run.stderr,/A2=00001002.*A8=00001008.*A15=0000100f/);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
