import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

function functionSource(source, signature) {
  const start = source.indexOf(signature);
  assert.ok(start >= 0);
  let end = source.indexOf("{", start) + 1;
  for (let depth = 1; depth; end += 1) {
    if (source[end] === "{") depth += 1;
    if (source[end] === "}") depth -= 1;
  }
  return source.slice(start, end);
}

test("direct kernel entry supplies the caller frame needed by real window overflow", () => {
  const machine = readFileSync(new URL("../qemu-peripherals/hw_xtensa_esp32s3.c", import.meta.url), "utf8");
  const helper = readFileSync(new URL("../qemu-peripherals/target_xtensa_exc_helper.c", import.meta.url), "utf8");
  const prepare = functionSource(machine, "static void mofei_prepare_kernel_call_frame(");
  const spill = functionSource(helper, "static void mofei_store_window_overflow(");
  const dir = mkdtempSync(join(tmpdir(), "panda-kernel-entry-"));
  try {
    writeFileSync(join(dir, "kernel.c"), `
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum {WINDOW_BASE,WINDOW_START,PS};
#define PS_WOE 0x40000u
#define PS_CALLINC_SHIFT 16
#define MOFEI_SIM_FAKE_APP_STACK_TOP 0x3fd88000u
#define MEMTXATTRS_UNSPECIFIED 0
typedef struct { uint32_t regs[16],phys_regs[64],sregs[3],windowbase_next,kernel_entry; } CPUXtensaState;
static uint32_t memory[256],address_space_memory;
static uint32_t* at(uint32_t addr) {
 assert(addr>=MOFEI_SIM_FAKE_APP_STACK_TOP-sizeof(memory) && addr<MOFEI_SIM_FAKE_APP_STACK_TOP);
 assert(!(addr&3));return &memory[(addr-(MOFEI_SIM_FAKE_APP_STACK_TOP-sizeof(memory)))/4];
}
static void address_space_write(void* space,uint32_t addr,int attrs,const void* src,unsigned size) {
 (void)space;(void)attrs;assert(size==16);memcpy(at(addr),src,size);
}
static uint32_t cpu_ldl_data(CPUXtensaState* env,uint32_t addr) {(void)env;return *at(addr);}
static void cpu_stl_data(CPUXtensaState* env,uint32_t addr,uint32_t value) {(void)env;*at(addr)=value;}
static void xtensa_sync_phys_from_window(CPUXtensaState* env) {memcpy(env->phys_regs,env->regs,sizeof(env->regs));}
${prepare}
${spill}
int main(void) {
 for(unsigned i=0;i<256;i++)memory[i]=0x73657236;
 CPUXtensaState env={.kernel_entry=0x40375970};
 mofei_prepare_kernel_call_frame(&env);
 assert(env.sregs[WINDOW_BASE]==0 && env.sregs[WINDOW_START]==1 && env.windowbase_next==0);
 assert(env.sregs[PS]==0x60000 && env.regs[1]==MOFEI_SIM_FAKE_APP_STACK_TOP);
 assert(env.regs[8]==0x80375970 && !memcmp(env.regs,env.phys_regs,sizeof(env.regs)));
 assert(*at(env.regs[1]-12)==MOFEI_SIM_FAKE_APP_STACK_TOP);
 // 按 ENTRY a1,64 后的寄存器重叠构造第一次 CALL8 窗口溢出。
 const uint32_t root_sp=MOFEI_SIM_FAKE_APP_STACK_TOP-64;
 env.regs[9]=root_sp;env.regs[2]=0x1234;env.regs[3]=0x5678;
 mofei_store_window_overflow(&env,8);
 assert(*at(root_sp-12)==MOFEI_SIM_FAKE_APP_STACK_TOP);
 assert(*at(root_sp-16)==0 && *at(root_sp-8)==0x1234 && *at(root_sp-4)==0x5678);
 // 根 C 帧随后溢出时，其 ABI 回链已经由上一次保存建立。
 env.regs[0]=0x80375970;env.regs[1]=root_sp;env.regs[9]=root_sp-48;
 mofei_store_window_overflow(&env,8);
 assert(*at(root_sp-48-12)==root_sp);
 assert(*at(root_sp-48-16)==0x80375970);
 puts("kernel caller frame + actual source window spill PASS");
}
`);
    const compiled = spawnSync("cc", ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", join(dir, "kernel.c"), "-o", join(dir, "kernel")], { encoding: "utf8" });
    assert.equal(compiled.status, 0, compiled.stderr);
    const run = spawnSync(join(dir, "kernel"), [], { encoding: "utf8", timeout: 5000 });
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /actual source window spill PASS/);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test("existing skipped IDF init callbacks return ESP_OK and preserve entry and other stub classes", () => {
  const machine = readFileSync(new URL("../qemu-peripherals/hw_xtensa_esp32s3.c", import.meta.url), "utf8");
  const namesStart = machine.indexOf("static const char* retw_names[]");
  const names = machine.slice(namesStart, machine.indexOf("};", namesStart));
  const initNames = [...new Set([...names.matchAll(/^\s*"(__esp_system_init_fn_[^"]+)",/gm)].map(match => match[1]))];
  assert.ok(initNames.includes("__esp_system_init_fn_init_efuse_check"));
  const start = machine.lastIndexOf("{", machine.indexOf("uint16_t phnum = *(uint16_t*)(elf_data + 44)", namesStart));
  assert.ok(start > namesStart);
  let end = start + 1;
  for (let depth = 1; depth; end += 1) {
    if (machine[end] === "{") depth += 1;
    if (machine[end] === "}") depth -= 1;
  }
  const patcher = machine.slice(start, end);
  const dir = mkdtempSync(join(tmpdir(), "panda-startup-return-"));
  try {
    writeFileSync(join(dir, "startup.c"), `
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static struct { unsigned count; uint32_t pcs[128]; } mofei_sim_addrs;
static void mofei_add_retw_patch(void* state, uint32_t pc, const char* name) {
  (void)state; assert(name); mofei_sim_addrs.pcs[mofei_sim_addrs.count++]=pc;
}
int main(void) {
  const char* init_names[] = {${initNames.map(name => JSON.stringify(name)).join(",")}};
  const int init_count=sizeof(init_names)/sizeof(init_names[0]);
  const int n_retw=init_count+2;
  struct { const char* name; uint32_t addr; int return_success; } retw_dyn[128];
  union { uint32_t align[1024]; uint8_t bytes[4096]; } elf={0};
  uint8_t* elf_data=elf.bytes;
  const unsigned elf_len=sizeof(elf.bytes), base=0x42001000, data_offset=256;
  *(uint16_t*)(elf_data+44)=1; *(uint32_t*)(elf_data+28)=64;
  uint32_t* ph=(uint32_t*)(elf_data+64);
  ph[0]=1; ph[1]=data_offset; ph[3]=base; ph[4]=n_retw*16;
  for(int i=0;i<n_retw;++i) {
    retw_dyn[i].name=i<init_count?init_names[i]:(i==init_count?"vPortYield":"xQueueReceive");
    retw_dyn[i].addr=base+i*16; retw_dyn[i].return_success=i==init_count+1;
    uint8_t* fn=elf_data+data_offset+i*16;
    memset(fn,0xa5,16); memcpy(fn,"\\x36\\x21\\x00",3);
  }
  ${patcher}
  assert(mofei_sim_addrs.count==(unsigned)n_retw);
  for(int i=0;i<n_retw;++i) {
    const uint8_t* fn=elf_data+data_offset+i*16;
    assert(!memcmp(fn,"\\x36\\x21\\x00",3));
    if(i<init_count) {
      assert(!memcmp(fn+3,"\\x0c\\x02\\x1d\\xf0",4));
      assert(mofei_sim_addrs.pcs[i]==retw_dyn[i].addr+5);
      assert(fn[7]==0xa5);
    } else if(i==init_count) {
      assert(!memcmp(fn+3,"\\x90\\x00\\x00",3));
      assert(mofei_sim_addrs.pcs[i]==retw_dyn[i].addr+3);
      assert(fn[6]==0xa5);
    } else {
      assert(!memcmp(fn+3,"\\x3d\\xf0\\x1d\\xf0",4));
      assert(mofei_sim_addrs.pcs[i]==retw_dyn[i].addr+5);
      assert(fn[7]==0xa5);
    }
  }
  puts("source-owned ELF init callback return patches PASS");
}
`);
    const compiled = spawnSync("cc", ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", join(dir, "startup.c"), "-o", join(dir, "startup")], { encoding: "utf8" });
    assert.equal(compiled.status, 0, compiled.stderr);
    const run = spawnSync(join(dir, "startup"), [], { encoding: "utf8", timeout: 5000 });
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /ELF init callback return patches PASS/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
