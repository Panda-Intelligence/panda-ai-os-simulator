import test from "node:test";
import assert from "node:assert/strict";
import {readFileSync,mkdtempSync,writeFileSync,rmSync} from "node:fs";
import {tmpdir} from "node:os";
import {join,dirname} from "node:path";
import {fileURLToPath} from "node:url";
import {spawnSync} from "node:child_process";
const root=dirname(dirname(fileURLToPath(import.meta.url)));
const helper=readFileSync(join(root,"qemu-peripherals/target_xtensa_exc_helper.c"),"utf8");
const body=helper.slice(helper.indexOf("uint32_t HELPER(mofei_gpio_set_level)"),helper.indexOf("uint32_t HELPER(mofei_gpio_get_level)"));
const program=`
#include <assert.h>
#include "mofei-sim-addrs.h"
#define HELPER(name) name
#define BIT(n) (1u<<(n))
#define MOFEI_ESP32S3_GPIO_OUT1_W1TS 101u
#define MOFEI_ESP32S3_GPIO_OUT_W1TS 102u
#define MOFEI_ESP32S3_GPIO_OUT1_W1TC 103u
#define MOFEI_ESP32S3_GPIO_OUT_W1TC 104u
MofeiSimBoardKind mofei_sim_active_board;
typedef struct {uint32_t regs[32];} CPUXtensaState;
static uint32_t seen_addr,seen_bit,calls;
static uint32_t mofei_skip_fn_common(CPUXtensaState* env,uint32_t* raw,unsigned* inc){(void)env;*raw=0;*inc=1;return 123;}
static void mofei_write_mmio_u32(uint32_t addr,uint32_t bit){seen_addr=addr;seen_bit=bit;++calls;}
${body}
int main(void){
 const unsigned pins[]={12,39,40,46,47};
 for(unsigned board=0;board<4;board++)for(unsigned p=0;p<5;p++)for(unsigned level=0;level<2;level++){
  CPUXtensaState env={0};mofei_sim_active_board=(MofeiSimBoardKind)board;calls=0;
  unsigned pin=pins[p];env.regs[6]=pin;env.regs[7]=level;
  assert(mofei_gpio_set_level(&env)==123);assert(env.regs[6]==0);
  unsigned expected=(board==MOFEI_SIM_BOARD_LILYGO_T5S3_PRO&&pin!=47)||(board==MOFEI_SIM_BOARD_M5PAPERS3&&pin==47);
  assert(calls==expected);
  if(expected){assert(seen_bit==BIT(pin&31));assert(seen_addr==(pin>=32?(level?101:103):(level?102:104)));}
 }
 return 0;
}`;
test("actual GPIO helper routes PaperS3 CS47 without changing LilyGo or other boards",()=>{
 const dir=mkdtempSync(join(tmpdir(),"panda-papers3-spi-"));
 try{
  const file=join(dir,"test.c"),bin=join(dir,"test");writeFileSync(file,program);
  const built=spawnSync(process.env.CC||"cc",["-std=c11","-Wall","-Wextra","-Werror","-I",join(root,"qemu-peripherals"),file,"-o",bin],{encoding:"utf8",timeout:30000});
  assert.equal(built.status,0,built.stdout+built.stderr);
  const run=spawnSync(bin,[],{encoding:"utf8",timeout:10000});assert.equal(run.status,0,run.stdout+run.stderr);
 }finally{rmSync(dir,{recursive:true,force:true});}
});
test("native and WASM machine graphs attach SPI SD exactly once and wire CS47",()=>{
 for(const name of ["hw_xtensa_esp32s3.c","esp32s3-soc-machine.patch"]){
  const s=readFileSync(join(root,"qemu-peripherals",name),"utf8").replace(/^\+/gm,"");
  assert.match(s,/#define M5PAPERS3_SD_CS_GPIO 47/);
  assert.match(s,/esp32s3_machine_attach_lilygo_spi_sd\(ss->mofei_spi2, sd_blk, 0\)/);
  assert.match(s,/gpio-out", M5PAPERS3_SD_CS_GPIO/);
  assert.match(s,/!mofei_sim_board_is_lilygo_t5s3_pro\(\) && !mofei_sim_board_is_m5papers3\(\)/);
 }
});
