import test from "node:test";
import assert from "node:assert/strict";
import {readFileSync, mkdtempSync, writeFileSync, rmSync} from "node:fs";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {spawnSync} from "node:child_process";

function body(source,signature) {
  const start=source.indexOf(signature),open=source.indexOf("{",start);
  assert.ok(start>=0);
  let end=open+1,depth=1;
  while(depth) {if(source[end]==="{")depth++;if(source[end]==="}")depth--;end++;}
  return source.slice(start,end);
}
test("real syscall exceptions follow canonical USER/KERNEL/DOUBLE delivery and configured vector relocation",()=>{
  const source=readFileSync(new URL("../qemu-peripherals/target_xtensa_exc_helper.c",import.meta.url),"utf8");
  const cause=body(source,"void HELPER(exception_cause)(");
  const relocated=body(source,"static uint32_t relocated_vector(");
  const delivered=body(source,"void HELPER(exception)(");
  const bootFrame=body(source,"static void mofei_prepare_boot_firmware_frame(");
  assert.doesNotMatch(cause,/env->pc = env->sregs\[VECBASE\] \+ vector/);
  assert.match(cause,/HELPER\(exception\)\(env, vector\)/);
  const dispatcher=body(source,"void xtensa_cpu_do_interrupt(");
  const routeStart=dispatcher.indexOf("if (env->config->exception_vector[cs->exception_index])");
  const route=body(dispatcher.slice(routeStart),"if (env->config->exception_vector[cs->exception_index])");
  const dir=mkdtempSync(join(tmpdir(),"panda-exception-route-"));
  try {
    writeFileSync(join(dir,"route.c"),`
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HELPER(name) helper_##name
enum {PS,EPC1,DEPC,EXCCAUSE,VECBASE,WINDOW_BASE,WINDOW_START,EXCVADDR,LBEG,LEND,LCOUNT,SAR};
enum {EXC_KERNEL=10,EXC_USER=11,EXC_DOUBLE=12,EXC_MAX=14};
#define EXCP_YIELD 100
#define PS_EXCM 0x10u
#define PS_UM 0x20u
#define PS_WOE 0x40000u
#define PS_CALLINC 0x30000u
#define PS_CALLINC_SHIFT 16
#define MOFEI_SIM_FAKE_APP_STACK_TOP 0x3fd88000u
#define SYSCALL_CAUSE 1u
#define INTEGER_DIVIDE_BY_ZERO_CAUSE 6u
#define XTENSA_OPTION_RELOCATABLE_VECTOR 1
#define CPU_LOG_INT 0
#define qemu_log_mask(...) ((void)0)
typedef struct { unsigned cpu_index; bool halted; int exception_index; } CPUState;
typedef struct { bool ndep,ndepc,relocatable; uint32_t vecbase,exception_vector[EXC_MAX]; } Config;
typedef struct { uint32_t pc,regs[16],sregs[12],phys_regs[64],windowbase_next; const Config* config; CPUState* cpu; int yield_needed; } CPUXtensaState;
static struct { uint32_t murphySimulatorHooksEnabled_addr; } mofei_sim_addrs;
static bool mofei_initialize_main_tls(CPUXtensaState* env) {(void)env;return true;}
static uint32_t mofei_main_stack_top(void) {return MOFEI_SIM_FAKE_APP_STACK_TOP;}
static void cpu_abort(CPUState* cpu,const char* message) {(void)cpu;(void)message;assert(0);}
static uint32_t stack_writes[4],stack_addresses[4];
static unsigned stack_count;
static void cpu_stb_data(CPUXtensaState* env,uint32_t addr,uint8_t value) {(void)env;(void)addr;(void)value;}
static void cpu_stl_data(CPUXtensaState* env,uint32_t addr,uint32_t value) {
 (void)env;assert(stack_count<4);stack_addresses[stack_count]=addr;stack_writes[stack_count++]=value;
}
static void xtensa_sync_phys_from_window(CPUXtensaState* env) {for(unsigned i=0;i<16;i++)env->phys_regs[i]=env->regs[i];}
static const char* mofei_murphy_or_app_main_entry_name(void) {return "murphySimulatorMain";}
static unsigned exits;
static CPUState* env_cpu(CPUXtensaState* env) {return env->cpu;}
static void cpu_loop_exit(CPUState* cpu) {(void)cpu;++exits;}
static bool xtensa_option_enabled(const Config* config,unsigned option) {(void)option;return config->relocatable;}
static bool mofei_handle_zero_exception_vector(CPUXtensaState* e,uint32_t p,uint32_t c) {(void)e;(void)p;(void)c;return false;}
static bool mofei_handle_fetch_exception_artifact(CPUXtensaState* e,uint32_t p,uint32_t c) {(void)e;(void)p;(void)c;return false;}
static bool mofei_handle_illegal_instruction_artifact(CPUXtensaState* e,uint32_t p,uint32_t c) {(void)e;(void)p;(void)c;return false;}
${delivered}
${cause}
${relocated}
${bootFrame}
static void actualConfiguredVectorBranch(CPUXtensaState* env,CPUState* cs) {${route}}
int main(void) {
 Config config={.ndepc=true,.relocatable=true,.vecbase=0x40000000};
 config.exception_vector[EXC_USER]=0x40000340;config.exception_vector[EXC_KERNEL]=0x40000300;
 config.exception_vector[EXC_DOUBLE]=0x400003c0;
 for(unsigned i=0;i<4;i++) {
   config.ndepc=i!=3;CPUState cpu={0};CPUXtensaState env={.config=&config,.cpu=&cpu};
   env.sregs[VECBASE]=0x40374000;env.sregs[EPC1]=0x42111111;env.sregs[DEPC]=0x42222222;
   env.sregs[PS]=0x60000|(i==0?PS_UM:0)|(i>=2?PS_EXCM:0);
   env.regs[0]=0x820058df;env.regs[1]=0x3fd87f00;unsigned before=exits;
   helper_exception_cause(&env,0x4002e243,SYSCALL_CAUSE);
   assert(exits==before+1 && env.pc==0x4002e243);
   assert(cpu.exception_index==(i==0?EXC_USER:i==1?EXC_KERNEL:EXC_DOUBLE));
   assert(env.sregs[EXCCAUSE]==SYSCALL_CAUSE && (env.sregs[PS]&PS_EXCM));
   if(i<2||i==3)assert(env.sregs[EPC1]==0x4002e243);else assert(env.sregs[EPC1]==0x42111111);
   if(i==2)assert(env.sregs[DEPC]==0x4002e243);else assert(env.sregs[DEPC]==0x42222222);
   assert(env.regs[0]==0x820058df && env.regs[1]==0x3fd87f00);
   actualConfiguredVectorBranch(&env,&cpu);
   assert(env.pc==(i==0?0x40374340:i==1?0x40374300:0x403743c0));
 }
 CPUState task_cpu={0};CPUXtensaState task={.config=&config,.cpu=&task_cpu};
 task.sregs[PS]=0x60000|PS_EXCM;
 mofei_prepare_boot_firmware_frame(&task);
 assert((task.sregs[PS]&(PS_UM|PS_WOE))==(PS_UM|PS_WOE));
 assert(!(task.sregs[PS]&PS_EXCM) && task.sregs[WINDOW_BASE]==0 && task.sregs[WINDOW_START]==1);
 assert(task.regs[1]==MOFEI_SIM_FAKE_APP_STACK_TOP && task.regs[8]==0x8037d000);
 assert(stack_count==4 && stack_addresses[0]==MOFEI_SIM_FAKE_APP_STACK_TOP-16);
 assert(stack_writes[0]==task.regs[8] && stack_writes[1]==task.regs[1]);
 task.sregs[VECBASE]=0x40374000;
 helper_exception_cause(&task,0x4002e243,SYSCALL_CAUSE);
 assert(task_cpu.exception_index==EXC_USER);
 actualConfiguredVectorBranch(&task,&task_cpu);
 assert(task.pc==0x40374340);
 puts("canonical syscall cause delivery + actual configured vector branch PASS");
}
`);
    const compiled=spawnSync("cc",["-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",join(dir,"route.c"),"-o",join(dir,"route")],{encoding:"utf8"});
    assert.equal(compiled.status,0,compiled.stderr);
    const run=spawnSync(join(dir,"route"),[],{encoding:"utf8",timeout:5000});
    assert.equal(run.status,0,run.stderr);
    assert.match(run.stdout,/configured vector branch PASS/);
  } finally {rmSync(dir,{recursive:true,force:true});}
});
