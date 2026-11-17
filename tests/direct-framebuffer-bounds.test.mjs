import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

function body(source, signature) {
  const start = source.indexOf(signature);
  assert.ok(start >= 0, signature);
  let end = source.indexOf("{", start) + 1;
  for (let depth = 1; depth; ++end) {
    if (source[end] === "{") ++depth;
    if (source[end] === "}") --depth;
  }
  return source.slice(start, end);
}

test("real direct capture respects framebuffer bounds and emits the board's wire format", () => {
  const source = readFileSync(new URL("../qemu-peripherals/target_xtensa_exc_helper.c", import.meta.url), "utf8");
  const ssd1677 = readFileSync(new URL("../qemu-peripherals/ssd1677_gdeq0426t82.c", import.meta.url), "utf8");
  const layout = body(source, "typedef struct MofeiDirectFramebufferLayout") + " MofeiDirectFramebufferLayout;";
  const parse = body(source, "static bool mofei_read_direct_framebuffer_layout(");
  const direct = body(source, "if (direct_fb_bytes > 0 && direct_fb_bytes <= MOFEI_DIRECT_FRAMEBUFFER_MAX_BYTES)");
  const gray4 = body(source, "static bool mofei_try_publish_gray4_framebuffer(");
  const simRam = body(source, "static bool mofei_guest_range_in_sim_ram(uint32_t guest_addr, uint32_t len) {");
  const constants = ssd1677.match(/^#define (?:EPD_WIDTH|EPD_HEIGHT|MURPHY_LOGICAL_WIDTH|IPC_CHANNEL_FRAMEBUFFER|IPC_FLAG_FB_(?:FULL|GRAY16)) .+$/gm);
  const sendDirect = body(ssd1677, "void ssd1677_publish_direct_framebuffer(const uint8_t* payload, uint32_t len) {");
  const sendGray4 = body(ssd1677, "void ssd1677_publish_gray4_framebuffer(const uint8_t* payload, uint32_t len, bool portrait) {");
  const header = fileURLToPath(new URL("../qemu-peripherals/mofei-sim-addrs.h", import.meta.url));
  const dir = mkdtempSync(join(tmpdir(), "panda-direct-framebuffer-"));
  try {
    writeFileSync(join(dir, "capture.c"), `
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include ${JSON.stringify(header)}
typedef struct { unsigned unused; } CPUXtensaState;
MofeiSimBoardKind mofei_sim_active_board;
MofeiSimAddrs mofei_sim_addrs;
enum {OBJECT=0x1000,EXPORTED=0x2000,OBJECT_SLOT=0x2004,GRAY4_SLOT=0x3fc80100,PIXELS=0x3c800000};
static uint32_t words[6],exported_bits,object_pointer,failed_metadata,gray4_pointer;
static bool failed_pixel_read;
static uint8_t* pixels;
static uint32_t allocation,reads,last_read,publishes,published_bytes;
static uint8_t published_flags;
static uint8_t published[684u*1216u/2u];
static bool mofei_read_guest_data_u32_checked(CPUXtensaState* env,uint32_t addr,uint32_t* value) {
 (void)env;
 if(addr==failed_metadata)return false;
 if(addr==GRAY4_SLOT){*value=gray4_pointer;return true;}
 if(addr<OBJECT || addr>=OBJECT+sizeof(words) || (addr&3))return false;
 *value=words[(addr-OBJECT)/4];return true;
}
static uint32_t mofei_read_guest_data_u32(CPUXtensaState* env,uint32_t addr) {
 (void)env;
 if(addr==EXPORTED)return exported_bits;
 assert(addr==OBJECT_SLOT);return object_pointer;
}
static bool mofei_read_guest_memory(uint32_t addr,uint8_t* dst,uint32_t bytes) {
 ++reads;last_read=bytes;assert(addr==PIXELS || addr==gray4_pointer);assert(bytes<=allocation);
 if(failed_pixel_read)return false;
 memcpy(dst,pixels,bytes);return true;
}
${constants.join("\n")}
typedef struct { unsigned unused; } SSD1677State;
static SSD1677State display;
static SSD1677State* g_ssd1677=&display;
static void ipc_send_frame(SSD1677State* s,uint8_t channel,uint8_t flags,const uint8_t* data,uint32_t bytes) {
 assert(s==&display && channel==0x01 && bytes<=sizeof(published));
 ++publishes;published_flags=flags;published_bytes=bytes;memcpy(published,data,bytes);
}
${sendDirect}
${sendGray4}
${simRam}
${layout}
${parse}
${gray4}
static void capture(CPUXtensaState* env) {
 static uint8_t direct_framebuffer[MOFEI_DIRECT_FRAMEBUFFER_MAX_BYTES];
 if(mofei_try_publish_gray4_framebuffer(env,direct_framebuffer))return;
 const uint32_t direct_fb_bytes=mofei_sim_board_direct_framebuffer_bytes();
 ${direct}
}
static void setup(MofeiSimBoardKind board,uint32_t width,uint32_t height,uint32_t format) {
 free(pixels);mofei_sim_active_board=board;
 words[0]=width;words[1]=height;words[2]=(width+(format?1:7))/(format?2:8);
 words[3]=format;words[4]=PIXELS;words[5]=allocation=words[2]*height;
 pixels=malloc(allocation);assert(pixels);
 for(unsigned i=0;i<allocation;++i)pixels[i]=(uint8_t)(i*17u+0xa5u);
 exported_bits=PIXELS;object_pointer=OBJECT;failed_metadata=0;gray4_pointer=0;failed_pixel_read=false;
 reads=last_read=publishes=published_bytes=0;
 published_flags=0xFF;
 mofei_sim_addrs.murphySimulatorFramebuffer_addr=EXPORTED;
 mofei_sim_addrs.murphySimulatorFramebufferObject_addr=OBJECT_SLOT;
 mofei_sim_addrs.murphySimulatorGray4Export_addr=0;
}
static void accepted(CPUXtensaState* env,uint32_t input,uint32_t output) {
 capture(env);assert(reads==1 && last_read==input && publishes==1 && published_bytes==output);
}
static void rejected(CPUXtensaState* env) {
 capture(env);assert(reads==0 && publishes==0);
}
static void pico(CPUXtensaState* env,MofeiSimBoardKind board) {
 setup(board,684,1216,0);accepted(env,86u*1216u,684u*1216u/8u);
 for(unsigned y=0;y<1216;y++)for(unsigned x=0;x<684;x++) {
  unsigned src=y*86+x/8,bit=y*684+x;
  assert(((pixels[src]>>(7-(x&7)))&1)==((published[bit/8]>>(7-(bit&7)))&1));
 }
}
static uint8_t gray4_pixel(const uint8_t* packed,unsigned width,unsigned x,unsigned y) {
 unsigned index=y*width+x;
 return (packed[index/2]>>(index%2==0?4:0))&0x0F;
}
static void gray4_setup(MofeiSimBoardKind board,uint32_t width,uint32_t height) {
 setup(board,width,height,1);
 mofei_sim_addrs.murphySimulatorGray4Export_addr=GRAY4_SLOT;
 gray4_pointer=PIXELS;
}
static void gray4_portrait(CPUXtensaState* env,MofeiSimBoardKind board) {
 gray4_setup(board,480,800);
 accepted(env,192000,192000);
 assert(published_flags==0x02);
 for(unsigned y=0;y<800;y++)for(unsigned x=0;x<480;x++) {
  assert(gray4_pixel(pixels,480,x,y)==gray4_pixel(published,800,y,479-x));
 }
}
static void gray4_direct(CPUXtensaState* env,MofeiSimBoardKind board,uint32_t width,uint32_t height) {
 gray4_setup(board,width,height);
 accepted(env,allocation,allocation);
 assert(published_flags==0x02 && !memcmp(pixels,published,allocation));
}
int main(void) {
 CPUXtensaState env={0};
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,540,960,1);accepted(&env,259200,259200);
 assert(published_flags==0 && !memcmp(pixels,published,allocation));
 setup(MOFEI_SIM_BOARD_M5PAPERS3,960,540,1);accepted(&env,259200,259200);
 pico(&env,MOFEI_SIM_BOARD_READ_PICO);
 // Read Pico 固件的 UI Framebuffer 是 684x1216 Gray16（stride 342），比 LilyGo 的 259200 字节上限大。
 setup(MOFEI_SIM_BOARD_READ_PICO,684,1216,1);accepted(&env,342u*1216u,342u*1216u);
 assert(!memcmp(pixels,published,allocation));
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,684,1216,1);accepted(&env,342u*1216u,342u*1216u);
 pico(&env,MOFEI_SIM_BOARD_LILYGO_T5S3_PRO); // 浏览器别名仍按真实 Mono1 对象读取。
 setup(MOFEI_SIM_BOARD_EEGO_A4,552,768,0);accepted(&env,52992,52992);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);words[3]|=0x100;
 accepted(&env,52992,52992);assert(!memcmp(pixels,published,allocation));
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);exported_bits=0;
 accepted(&env,52992,52992); // 元数据 bits 在没有单独导出值时仍可使用。
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);--words[5];rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);words[5]=0;rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);--words[2];rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);words[2]=0;rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);words[1]=0xffffffffu;rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);words[0]=0;rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);words[0]=0xffffffffu;rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);words[3]=2;rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);words[4]=0;rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);++exported_bits;rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);object_pointer=0;rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);object_pointer=0xfffffff0u;rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);failed_metadata=OBJECT+20;rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768,0);exported_bits=words[4]=0xfffffff0u;rejected(&env);
 setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,540,960,1);words[1]=2000;words[5]=0xffffffffu;rejected(&env);
 setup(MOFEI_SIM_BOARD_MOFEI,480,800,0);rejected(&env);
 setup(MOFEI_SIM_BOARD_S37UC,240,416,0);rejected(&env);
 setup(MOFEI_SIM_BOARD_MOLINK,800,480,0);rejected(&env);
 setup(MOFEI_SIM_BOARD_X4PRO,800,480,0);rejected(&env);
 setup(MOFEI_SIM_BOARD_METALIO_EINK4,480,800,0);rejected(&env);
 gray4_portrait(&env,MOFEI_SIM_BOARD_MOFEI);
 gray4_portrait(&env,MOFEI_SIM_BOARD_METALIO_EINK4);
 gray4_direct(&env,MOFEI_SIM_BOARD_MOLINK,800,480);
 gray4_direct(&env,MOFEI_SIM_BOARD_X4PRO,800,480);
 gray4_direct(&env,MOFEI_SIM_BOARD_EEGO_A4,552,768);
 assert(setenv("PANDA_SIM_BOARD","eego-a4",1)==0);
 gray4_direct(&env,MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,552,768);
 assert(unsetenv("PANDA_SIM_BOARD")==0);
 gray4_setup(MOFEI_SIM_BOARD_LILYGO_T5S3_PRO,540,960);
 accepted(&env,259200,259200);assert(published_flags==0);
 gray4_setup(MOFEI_SIM_BOARD_READ_PICO,684,1216);
 accepted(&env,415872,415872);assert(published_flags==0);
 gray4_setup(MOFEI_SIM_BOARD_S37UC,240,416);rejected(&env);
 gray4_setup(MOFEI_SIM_BOARD_MOFEI,480,800);gray4_pointer=0;rejected(&env);
 gray4_setup(MOFEI_SIM_BOARD_MOFEI,480,800);gray4_pointer=0x60000000u;rejected(&env);
 gray4_setup(MOFEI_SIM_BOARD_MOFEI,480,800);gray4_pointer=0xfffffff0u;rejected(&env);
 gray4_setup(MOFEI_SIM_BOARD_MOFEI,480,800);gray4_pointer=MOFEI_SIM_DCACHE_LIMIT-1u;rejected(&env);
 gray4_setup(MOFEI_SIM_BOARD_MOFEI,480,800);failed_metadata=GRAY4_SLOT;rejected(&env);
 gray4_setup(MOFEI_SIM_BOARD_MOFEI,480,800);failed_pixel_read=true;
 capture(&env);assert(reads==1 && publishes==0);
 setup(MOFEI_SIM_BOARD_EEGO_A4,552,768,0);
 mofei_sim_addrs.murphySimulatorGray4Export_addr=GRAY4_SLOT;
 accepted(&env,52992,52992);assert(published_flags==0);
 free(pixels);
 puts("direct framebuffer capacity/format/aliases/pixels/boundaries PASS");
}
`);
    const binary = join(dir, "capture");
    const build = spawnSync("cc", ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", join(dir, "capture.c"), "-o", binary], { encoding: "utf8" });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(binary, [], { encoding: "utf8", timeout: 5000 });
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /aliases\/pixels\/boundaries PASS/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
