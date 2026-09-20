import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
const root=resolve(dirname(fileURLToPath(import.meta.url)),"..");
const source=readFileSync(join(root,"qemu-peripherals/ssd1677_gdeq0426t82.c"),"utf8");
const start=source.indexOf("static void ssd1677_inject_touch(");
const end=source.indexOf("static void ssd1677_inject_button(",start);
assert.ok(start>=0 && end>start);
const route=source.slice(start,end);
const program=`
#include <assert.h>
#include <stdio.h>
#include "mofei-sim-addrs.h"
#include "lilygo_display_input.h"
MofeiSimBoardKind mofei_sim_active_board;
static unsigned legacy_calls, gt_calls, log_errors;
static LilygoGt911Model model;
#define LOG_GUEST_ERROR 1
#define qemu_log_mask(...) (++log_errors)
static void ft6336u_inject_touch(const uint8_t* bytes, uint32_t n) {
  assert(bytes != NULL && n > 0); ++legacy_calls;
}
static bool lilygo_gt911_inject_contact(uint8_t a,uint16_t x,uint16_t y,uint8_t id) {
  ++gt_calls; return lilygo_gt911_model_inject(&model,a,x,y,id)==LILYGO_MODEL_OK;
}
${route}
int main(void) {
  const uint8_t touch[]={1,0x0e,1,0xc0,0,0, 3,0x0e,1,0xc0,0,0};
  for(unsigned board=0;board<4;board++) {
    mofei_sim_active_board=(MofeiSimBoardKind)board;
    legacy_calls=gt_calls=log_errors=0; lilygo_gt911_model_reset(&model);
    ssd1677_inject_touch(touch,sizeof(touch));
    if(board==MOFEI_SIM_BOARD_LILYGO_T5S3_PRO || board==MOFEI_SIM_BOARD_M5PAPERS3) {
      assert(mofei_sim_board_uses_gt911()); assert(gt_calls==2 && legacy_calls==0);
      assert(lilygo_gt911_model_read(&model,0x814e)==0x81);
      assert(lilygo_gt911_model_read(&model,0x8150)==0x0e);
      assert(lilygo_gt911_model_read(&model,0x8151)==1);
      assert(lilygo_gt911_model_write(&model,0x814e,0)==LILYGO_MODEL_OK);
      assert(lilygo_gt911_model_read(&model,0x814e)==0x80);
      assert(lilygo_gt911_model_write(&model,0x814e,0)==LILYGO_MODEL_OK);
      assert(lilygo_gt911_model_read(&model,0x814e)==0);
      ssd1677_inject_touch(touch,5);assert(log_errors==1 && gt_calls==2);
    } else { assert(!mofei_sim_board_uses_gt911()); assert(legacy_calls==1 && gt_calls==0); }
  }
  puts("4 board routes, real GT911 contact+release+ack, malformed payload: pass");
  return 0;
}
`;
test("actual input router targets the modeled controller on all board kinds",()=>{
  const temp=mkdtempSync(join(tmpdir(),"panda-gt911-routing-"));
  try {
    const c=join(temp,"routing.c"),bin=join(temp,"routing-test");writeFileSync(c,program);
    const built=spawnSync(process.env.CC || "cc",["-std=c11","-Wall","-Wextra","-Werror","-DLILYGO_DISPLAY_INPUT_MODEL_STANDALONE","-I",join(root,"qemu-peripherals"),c,join(root,"qemu-peripherals/lilygo_display_input.c"),"-o",bin],{encoding:"utf8",timeout:60000});
    assert.equal(built.status,0,built.stdout+built.stderr);
    const run=spawnSync(bin,[],{encoding:"utf8",timeout:10000});
    assert.equal(run.status,0,run.stdout+run.stderr);
  } finally {rmSync(temp,{recursive:true,force:true});}
});
test("native patch and WASM source share the GT911 I2C transfer guard",()=>{
  for(const name of ["target_xtensa_translate.c","xtensa-reset-vector.patch"]){
    const text=readFileSync(join(root,"qemu-peripherals",name),"utf8");
    assert.match(text,/if \(mofei_sim_board_uses_gt911\(\)\) \{\n\+?    uint32_t transfer_kind = 0;/);
  }
});
