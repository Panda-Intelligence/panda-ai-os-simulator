import test from "node:test";
import assert from "node:assert/strict";
import {readFileSync, mkdtempSync, writeFileSync, rmSync} from "node:fs";
import {homedir, tmpdir} from "node:os";
import {join} from "node:path";
import {spawnSync} from "node:child_process";
import {idfPath, romPath, missingInputs} from "./helpers/sdkInputs.mjs";

const source=name=>readFileSync(new URL(`../qemu-peripherals/${name}`,import.meta.url),"utf8");
test("protected Lua calls use the actual ROM setjmp and SDK longjmp bodies",()=>{
  const translate=source("target_xtensa_translate.c");
  assert.doesNotMatch(translate,/setjmp_addr|not_setjmp_stub|initial setjmp return/);
  const machine=source("hw_xtensa_esp32s3.c");
  assert.match(machine,/\{"setjmp", &a->setjmp_addr\}/);
  for(const name of ["setjmp","longjmp","__wrap_longjmp"]) {
    assert.ok(!machine.includes(`            "${name}",`),`${name} body must not be RETW-patched`);
  }
  const helper=source("target_xtensa_exc_helper.c");
  assert.doesNotMatch(helper,/HELPER\(.*(?:setjmp|longjmp).*\)/);
  const shim=readFileSync(new URL("../src/browserQemuPlatform.js",import.meta.url),"utf8");
  assert.match(shim,/3fcef758/); // Existing ROM clock-data loader is separate from code bytes.
  assert.doesNotMatch(shim,/4000144c|4002e23c|40001440|4002e2a0|setjmp|longjmp/);
});

test("real SDK machine header compiles the 17-word windowed jmp_buf expected by wrapped longjmp",{skip:missingInputs("ESP-IDF",[join(idfPath(),"components/esp_rom/patches/esp_rom_longjmp.S")])},()=>{
  const header=process.env.PANDA_MACHINE_SETJMP_HEADER || join(homedir(), ".espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/xtensa-esp-elf/include/machine/setjmp.h");
  const idf=idfPath();
  const assembly=readFileSync(join(idf,"components/esp_rom/patches/esp_rom_longjmp.S"),"utf8");
  assert.match(assembly,/l32i\s+a0, a2, 64/);
  assert.match(assembly,/l32i\s+a8, a2, 52/);
  assert.match(assembly,/movnez\s+a2, a3, a3/);
  assert.match(assembly,/retw/);
  const dir=mkdtempSync(join(tmpdir(),"panda-real-jmp-layout-"));
  try {
    writeFileSync(join(dir,"layout.c"),`
#include <stddef.h>
#undef __aarch64__
#undef __arm__
#undef __x86_64__
#undef __i386__
#define __XTENSA__ 1
#define __XTENSA_WINDOWED_ABI__ 1
#define _BEGIN_STD_C
#define _END_STD_C
#include ${JSON.stringify(header)}
_Static_assert(_JBLEN==17,"real windowed ABI word count");
_Static_assert(sizeof(jmp_buf)==68,"real SDK jmp_buf must match ROM/SDK wrapper");
int main(void) {return 0;}
`);
    const build=spawnSync("cc",["-std=c11","-Wall","-Wextra","-Werror",join(dir,"layout.c"),"-o",join(dir,"layout")],{encoding:"utf8"});
    assert.equal(build.status,0,build.stderr);
    const run=spawnSync(join(dir,"layout"),[],{encoding:"utf8"});
    assert.equal(run.status,0,run.stderr);
  } finally {rmSync(dir,{recursive:true,force:true});}
});

test("actual selected ROM setjmp bytes save raw A0 at byte64 and retain the syscall spill path",{skip:missingInputs("ROM image",[romPath()])},()=>{
  const rom=readFileSync(romPath());
  assert.equal(rom.length,0x60000);
  // Actual revision-0 alias literal targets and instructions, not a fabricated jump context.
  assert.equal(rom.readUInt32LE(0x1448),0x4002e23c);
  assert.equal(rom.readUInt32LE(0x143c),0x4002e2a0);
  assert.equal(rom.subarray(0x2e243,0x2e246).toString("hex"),"005000");
  assert.equal(rom.subarray(0x2e299,0x2e29c).toString("hex"),"026210");
  assert.equal(rom.subarray(0x2e29c,0x2e2a0).toString("hex"),"0c021df0");
});

test("selected ROM lock and UART setters match the kernel bootstrap data layout",{skip:missingInputs("ROM image",[romPath()])},()=>{
  const rom=readFileSync(romPath());
  assert.equal(rom.readUInt32LE(0x614),0x400441fc);
  assert.equal(rom.readUInt32LE(0x441f4),0x3fcef74c);
  assert.equal(rom.readUInt32LE(0x441f8),0x3fcef748);
  assert.equal(rom.subarray(0x441fc,0x4420b).toString("hex"),"36410081fdff290881fdff39081df0");
  assert.equal(rom.readUInt32LE(0x5e4),0x4004421c);
  assert.equal(rom.readUInt32LE(0x43cbc),0x3fcef754);
  assert.equal(rom.readUInt32LE(0x44218),0x40043ce8);
  assert.equal(rom.subarray(0x4426a,0x44276).toString("hex"),"21e2ff822200162800e00800");
  assert.equal(rom.readUInt32LE(0x6bc),0x400488b4);
  assert.equal(rom.readUInt32LE(0x1a54),0x400442a4);
  assert.equal(rom.readUInt32LE(0x44294),0x3fcef744);
  assert.equal(rom.subarray(0x44298,0x442a3).toString("hex"),"36410081feff2248001df0");
  assert.equal(rom.subarray(0x442a4,0x442af).toString("hex"),"36410081fbff2208001df0");
});
