import test from "node:test";
import {fileURLToPath} from "node:url";
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

// 2026-11-05，Codex：执行原 observer 函数，不把主机单测当作真实来宾资格。
test("shipping Arcade and wallpaper observer mappings preserve vptr address points and unknown refusal",()=>{
  const source=readFileSync(new URL("../qemu-peripherals/target_xtensa_exc_helper.c",import.meta.url),"utf8");
  const machine=readFileSync(new URL("../qemu-peripherals/hw_xtensa_esp32s3.c",import.meta.url),"utf8");
  const expected=[
  {
    "field": "murphyVtableGame2048Scene_addr",
    "activity": "Game2048",
    "vtableSymbol": "_ZTVN5panda3app13Game2048SceneE"
  },
  {
    "field": "murphyVtableSokobanScene_addr",
    "activity": "Sokoban",
    "vtableSymbol": "_ZTVN5panda3app12SokobanSceneE"
  },
  {
    "field": "murphyVtableSudokuScene_addr",
    "activity": "Sudoku",
    "vtableSymbol": "_ZTVN5panda3app11SudokuSceneE"
  },
  {
    "field": "murphyVtableDailyMazeScene_addr",
    "activity": "DailyMaze",
    "vtableSymbol": "_ZTVN5panda3app14DailyMazeSceneE"
  },
  {
    "field": "murphyVtableArcadeChallengesScene_addr",
    "activity": "ArcadeChallenges",
    "vtableSymbol": "_ZTVN5panda3app21ArcadeChallengesSceneE"
  },
  {
    "field": "murphyVtableWordPuzzleScene_addr",
    "activity": "WordPuzzle",
    "vtableSymbol": "_ZTVN5panda3app15WordPuzzleSceneE"
  },
  {
    "field": "murphyVtableVirtualPetScene_addr",
    "activity": "VirtualPet",
    "vtableSymbol": "_ZTVN5panda3app15VirtualPetSceneE"
  },
  {
    "field": "murphyVtablePaperBoyMinesweeperScene_addr",
    "activity": "PaperBoyMinesweeper",
    "vtableSymbol": "_ZTVN5panda3app24PaperBoyMinesweeperSceneE"
  },
  {
    "field": "murphyVtablePaperboyGbScene_addr",
    "activity": "PaperboyGbScene",
    "vtableSymbol": "_ZTVN5panda3app15PaperboyGbSceneE"
  },
  {
    "field": "murphyVtablePaperboyNesScene_addr",
    "activity": "PaperboyNesScene",
    "vtableSymbol": "_ZTVN5panda3app16PaperboyNesSceneE"
  },
  {
    "field": "murphyVtableElectronicSandScene_addr",
    "activity": "ElectronicSand",
    "vtableSymbol": "_ZTVN5panda3app19ElectronicSandSceneE"
  },
  {
    "field": "murphyVtableWallpaperPickerScene_addr",
    "activity": "SleepWallpaper",
    "vtableSymbol": "_ZTVN5panda3app20WallpaperPickerSceneE"
  },
  {
    "field": "murphyVtableSleepAppearancePreviewScene_addr",
    "activity": "SleepAppearancePreview",
    "vtableSymbol": "_ZTVN5panda3app27SleepAppearancePreviewSceneE"
  }
];
  const body=(signature)=>{
    const start=source.indexOf(signature);assert.ok(start>=0,signature);
    let end=source.indexOf("{",start)+1;
    for(let depth=1;depth;++end){if(source[end]==="{")++depth;if(source[end]==="}")--depth;}
    return source.slice(start,end);
  };
  for(const item of expected){
    assert.match(machine,new RegExp('"'+item.vtableSymbol+'",\\s*&a->'+item.field));
  }
  const assign=expected.map((item,i)=>`mofei_sim_addrs.${item.field}=0x${(0x1000+i*0x100).toString(16)};`).join("\n");
  const checks=expected.map((item,i)=>`assert(strcmp(mofei_murphy_activity_for_vptr(0x${(0x1000+i*0x100).toString(16)}),${JSON.stringify(item.activity)})==0);assert(strcmp(mofei_murphy_activity_for_vptr(0x${(0x1008+i*0x100).toString(16)}),${JSON.stringify(item.activity)})==0);`).join("\n");
  const header=fileURLToPath(new URL("../qemu-peripherals/mofei-sim-addrs.h",import.meta.url));
  const dir=mkdtempSync(join(tmpdir(),"panda-observer-map-"));
  try {
    writeFileSync(join(dir,"map.c"),`
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include ${JSON.stringify(header)}
#define HELPER(name) helper_##name
typedef struct { uint32_t sentinel; } CPUXtensaState;
MofeiSimAddrs mofei_sim_addrs;
static uint32_t mofei_read_guest_data_u32(CPUXtensaState* env,uint32_t addr){(void)env;return addr;}
${body("static bool mofei_vptr_matches(")}
${body("static const char* mofei_murphy_activity_for_vptr(")}
${body("void HELPER(mofei_trace_murphy_activity)(")}
int main(void){
 CPUXtensaState env={0x12345678};
 assert(mofei_murphy_activity_for_vptr(0)==NULL);
 assert(mofei_murphy_activity_for_vptr(8)==NULL);
 ${assign}
 MofeiSimAddrs before=mofei_sim_addrs;
 ${checks}
 assert(mofei_murphy_activity_for_vptr(0xfeed0000)==NULL);
 helper_mofei_trace_murphy_activity(&env,0,0x1008,1,0,0,0);
 helper_mofei_trace_murphy_activity(&env,0,0xfeed0000,1,0,0,0);
 assert(env.sentinel==0x12345678 && memcmp(&before,&mofei_sim_addrs,sizeof(before))==0);
 puts("real observer map and vptr address points PASS");
}
`);
    const compiled=spawnSync("cc",["-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",join(dir,"map.c"),"-o",join(dir,"map")],{encoding:"utf8"});
    assert.equal(compiled.status,0,compiled.stderr);
    const run=spawnSync(join(dir,"map"),[],{encoding:"utf8",timeout:5000});
    assert.equal(run.status,0,run.stderr);
    assert.deepEqual(run.stderr.split("\n").filter(line=>line.startsWith("UIREFRESH ")),["UIREFRESH activity=Game2048 mode=fast"]);
    assert.match(run.stderr,/unknown activity/);
    assert.match(run.stdout,/real observer map and vptr address points PASS/);
  } finally {rmSync(dir,{recursive:true,force:true});}
});

// 发布资格检查显式接收真实新 ELF；当前 F4 缺失的七项必须失败，不能伪造地址。
if(process.env.PANDA_OBSERVER_ELF){
  test("actual shipping ELF defines all eleven registry and both wallpaper observer classes",()=>{
    const expected=[
  {
    "field": "murphyVtableGame2048Scene_addr",
    "activity": "Game2048",
    "vtableSymbol": "_ZTVN5panda3app13Game2048SceneE"
  },
  {
    "field": "murphyVtableSokobanScene_addr",
    "activity": "Sokoban",
    "vtableSymbol": "_ZTVN5panda3app12SokobanSceneE"
  },
  {
    "field": "murphyVtableSudokuScene_addr",
    "activity": "Sudoku",
    "vtableSymbol": "_ZTVN5panda3app11SudokuSceneE"
  },
  {
    "field": "murphyVtableDailyMazeScene_addr",
    "activity": "DailyMaze",
    "vtableSymbol": "_ZTVN5panda3app14DailyMazeSceneE"
  },
  {
    "field": "murphyVtableArcadeChallengesScene_addr",
    "activity": "ArcadeChallenges",
    "vtableSymbol": "_ZTVN5panda3app21ArcadeChallengesSceneE"
  },
  {
    "field": "murphyVtableWordPuzzleScene_addr",
    "activity": "WordPuzzle",
    "vtableSymbol": "_ZTVN5panda3app15WordPuzzleSceneE"
  },
  {
    "field": "murphyVtableVirtualPetScene_addr",
    "activity": "VirtualPet",
    "vtableSymbol": "_ZTVN5panda3app15VirtualPetSceneE"
  },
  {
    "field": "murphyVtablePaperBoyMinesweeperScene_addr",
    "activity": "PaperBoyMinesweeper",
    "vtableSymbol": "_ZTVN5panda3app24PaperBoyMinesweeperSceneE"
  },
  {
    "field": "murphyVtablePaperboyGbScene_addr",
    "activity": "PaperboyGbScene",
    "vtableSymbol": "_ZTVN5panda3app15PaperboyGbSceneE"
  },
  {
    "field": "murphyVtablePaperboyNesScene_addr",
    "activity": "PaperboyNesScene",
    "vtableSymbol": "_ZTVN5panda3app16PaperboyNesSceneE"
  },
  {
    "field": "murphyVtableElectronicSandScene_addr",
    "activity": "ElectronicSand",
    "vtableSymbol": "_ZTVN5panda3app19ElectronicSandSceneE"
  },
  {
    "field": "murphyVtableWallpaperPickerScene_addr",
    "activity": "SleepWallpaper",
    "vtableSymbol": "_ZTVN5panda3app20WallpaperPickerSceneE"
  },
  {
    "field": "murphyVtableSleepAppearancePreviewScene_addr",
    "activity": "SleepAppearancePreview",
    "vtableSymbol": "_ZTVN5panda3app27SleepAppearancePreviewSceneE"
  }
];
    const result=spawnSync(process.env.PANDA_OBSERVER_NM||"xtensa-esp32s3-elf-nm",["--defined-only",process.env.PANDA_OBSERVER_ELF],{encoding:"utf8",maxBuffer:8*1024*1024});
    assert.equal(result.error,undefined,result.error?.message);
    assert.equal(result.status,0,result.stderr);
    const defined=new Set(result.stdout.split("\n").map(line=>line.trim().split(/\s+/)).filter(row=>row.length===3).map(row=>row[2]));
    assert.deepEqual(expected.filter(item=>!defined.has(item.vtableSymbol)).map(item=>item.vtableSymbol),[]);
    assert.ok(![...defined].some(symbol=>symbol.startsWith("_ZTV")&&symbol.includes("FortuneScene")));
  });
}
