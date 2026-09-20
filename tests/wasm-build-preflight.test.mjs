import test from "node:test";
import assert from "node:assert/strict";
import {copyFileSync,mkdtempSync,mkdirSync,rmSync,existsSync} from "node:fs";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {spawnSync} from "node:child_process";

test("missing host post-link dependencies fail before source checkout or Docker",()=>{
 const root=mkdtempSync(join(tmpdir(),"panda-build-preflight-"));
 try{
  mkdirSync(join(root,"scripts"));copyFileSync(new URL("../scripts/build-qemu-wasm.sh",import.meta.url),join(root,"scripts/build-qemu-wasm.sh"));
  // The isolated project cannot resolve a parent TypeScript installation.
  const result=spawnSync("bash",[join(root,"scripts/build-qemu-wasm.sh"),"--runtime-only"],{
   cwd:root,env:{PATH:process.env.PATH,HOME:root,QEMU_WASM_CACHE_DIR:join(root,"must-not-clone")},encoding:"utf8",timeout:10000,
  });
  assert.notEqual(result.status,0);assert.match(result.stdout+result.stderr,/bun install --frozen-lockfile/);
  assert.equal(existsSync(join(root,"must-not-clone")),false);
 }finally{rmSync(root,{recursive:true,force:true});}
});
