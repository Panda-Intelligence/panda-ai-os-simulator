import test from "node:test";
import assert from "node:assert/strict";
import {chmodSync,existsSync,mkdirSync,mkdtempSync,readFileSync,rmSync,writeFileSync} from "node:fs";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {spawnSync} from "node:child_process";

const script=new URL("../scripts/build-qemu-wasm.sh",import.meta.url).pathname;
const text=readFileSync(script,"utf8");
const git=(cwd,...args)=>{const r=spawnSync("git",args,{cwd,encoding:"utf8",env:{...process.env,GIT_AUTHOR_NAME:"t",GIT_AUTHOR_EMAIL:"t@t",GIT_COMMITTER_NAME:"t",GIT_COMMITTER_EMAIL:"t@t"}});assert.equal(r.status,0,r.stderr);return r.stdout.trim();};

// Run the named functions of the real script in a minimal harness: no Docker, no network.
function harness(dir,body,env={}){
  const lib=join(dir,"lib.sh");
  const start=text.indexOf("is_full_sha()"),end=text.indexOf("assert_qemu_wasm_base()");
  const mountStart=text.indexOf("assert_source_mounted()"),mountEnd=text.indexOf("stop_container()");
  writeFileSync(lib,`step(){ printf '[t] %s\\n' "$*"; }\ndie(){ printf 'ERROR: %s\\n' "$*" >&2; exit 1; }\nrequire_tool(){ command -v "$1" >/dev/null; }\n${text.slice(start,end)}\n${text.slice(mountStart,mountEnd)}\n`);
  return spawnSync("bash",["-c",`set -euo pipefail; source "${lib}"; ${body}`],{encoding:"utf8",env:{PATH:process.env.PATH,HOME:process.env.HOME,...env}});
}
function upstream(root){
  const repo=join(root,"upstream");mkdirSync(repo);git(repo,"init","-q");
  writeFileSync(join(repo,"a"),"1");git(repo,"add","a");git(repo,"commit","-qm","one");const first=git(repo,"rev-parse","HEAD");
  writeFileSync(join(repo,"a"),"2");git(repo,"commit","-qam","two");
  git(repo,"config","uploadpack.allowAnySHA1InWant","true");
  return {repo,first};
}

test("the default ref is a full commit SHA and the Docker image tag does not change with it",()=>{
  assert.match(text,/QEMU_WASM_REF="\$\{QEMU_WASM_REF:-[0-9a-f]{40}\}"/);
  assert.match(text,/QEMU_WASM_IMAGE="\$\{QEMU_WASM_IMAGE:-murphy-qemu-wasm:\$\{QEMU_WASM_IMAGE_TAG\}\}"/);
  assert.match(text,/QEMU_WASM_IMAGE_TAG="\$\{QEMU_WASM_IMAGE_TAG:-dev-wasm-j\}"/);
});

test("a pinned SHA is fetched exactly, not the branch tip",()=>{
  const root=mkdtempSync(join(tmpdir(),"wasm-pin-"));
  try{
    const {repo,first}=upstream(root);
    const r=harness(root,"ensure_checkout",{QEMU_WASM_CACHE_DIR:join(root,"c"),QEMU_WASM_SRC_DIR:join(root,"c/src"),QEMU_WASM_REPO_URL:`file://${repo}`,QEMU_WASM_REF:first,QEMU_WASM_OFFLINE:"0",QEMU_WASM_KEEP_BUILD:"0"});
    assert.equal(r.status,0,r.stderr+r.stdout);
    assert.equal(git(join(root,"c/src"),"rev-parse","HEAD"),first);
  }finally{rmSync(root,{recursive:true,force:true});}
});

test("an interrupted clone's leftovers and a checkout without the pinned commit are not trusted",()=>{
  const root=mkdtempSync(join(tmpdir(),"wasm-partial-"));
  try{
    const {repo,first}=upstream(root);
    const src=join(root,"c/src");mkdirSync(src,{recursive:true});git(src,"init","-q");  // looks like a checkout, holds nothing
    mkdirSync(`${src}.partial.123`);writeFileSync(join(`${src}.partial.123`,"junk"),"x");
    const r=harness(root,"ensure_checkout",{QEMU_WASM_CACHE_DIR:join(root,"c"),QEMU_WASM_SRC_DIR:src,QEMU_WASM_REPO_URL:`file://${repo}`,QEMU_WASM_REF:first,QEMU_WASM_OFFLINE:"0",QEMU_WASM_KEEP_BUILD:"0"});
    assert.equal(r.status,0,r.stderr+r.stdout);
    assert.match(r.stdout,/discarding it/);
    assert.equal(git(src,"rev-parse","HEAD"),first);
    assert.equal(existsSync(`${src}.partial.123`),false);
  }finally{rmSync(root,{recursive:true,force:true});}
});

test("an unshared cache directory is reported before configure runs",()=>{
  const root=mkdtempSync(join(tmpdir(),"wasm-mount-"));
  try{
    const bin=join(root,"bin");mkdirSync(bin);
    writeFileSync(join(bin,"docker"),"#!/bin/sh\nexit 1\n");chmodSync(join(bin,"docker"),0o755);
    const r=harness(root,"assert_source_mounted",{PATH:`${bin}:${process.env.PATH}`,QEMU_WASM_CONTAINER:"x",QEMU_WASM_SRC_DIR:"/private/tmp/x/qemu-wasm"});
    assert.notEqual(r.status,0);
    assert.match(r.stderr,/not shared with the Docker VM/);
    assert.match(r.stderr,/QEMU_WASM_CACHE_DIR under your home directory/);
    writeFileSync(join(bin,"docker"),"#!/bin/sh\nexit 0\n");
    assert.equal(harness(root,"assert_source_mounted",{PATH:`${bin}:${process.env.PATH}`,QEMU_WASM_CONTAINER:"x",QEMU_WASM_SRC_DIR:"/q"}).status,0);
  }finally{rmSync(root,{recursive:true,force:true});}
});

test("INT and TERM end the script and a failed configure wipes its partial build directory",()=>{
  assert.match(text,/trap stop_container EXIT\ntrap 'exit 130' INT\ntrap 'exit 143' TERM/);
  assert.doesNotMatch(text,/trap stop_container EXIT INT TERM/);
  assert.match(text,/wipe_incomplete_configure; die "qemu-wasm configure failed/);
});

test("one-shot builds use a per-run container name so concurrent runs cannot remove each other",()=>{
  assert.match(text,/QEMU_WASM_CONTAINER:-murphy-qemu-wasm-build-\$\$\}/);
  assert.match(text,/KEEP_BUILD.*== "1" \]\]; then\n  QEMU_WASM_CONTAINER="\$\{QEMU_WASM_CONTAINER:-murphy-qemu-wasm-build\}"/);
  assert.ok(text.indexOf("QEMU_WASM_KEEP_BUILD=\"${QEMU_WASM_KEEP_BUILD:-0}\"")<text.indexOf("murphy-qemu-wasm-build-$$"));
});

test("offline and no-origin cached checkouts select the requested commit instead of an unrelated HEAD",()=>{
  for(const offline of ["0","1"]){
    const root=mkdtempSync(join(tmpdir(),"wasm-cached-pin-"));
    try{
      const {repo,first}=upstream(root);
      const src=join(root,"cache/src");mkdirSync(join(root,"cache"));
      const cloned=spawnSync("git",["clone","-q",repo,src],{encoding:"utf8"});
      assert.equal(cloned.status,0,cloned.stderr);
      if(offline==="0")git(src,"remote","remove","origin");
      assert.notEqual(git(src,"rev-parse","HEAD"),first);
      const r=harness(root,"ensure_checkout",{
        QEMU_WASM_CACHE_DIR:join(root,"cache"),QEMU_WASM_SRC_DIR:src,
        QEMU_WASM_REPO_URL:"file:///must-not-fetch",QEMU_WASM_REF:first,
        QEMU_WASM_OFFLINE:offline,QEMU_WASM_KEEP_BUILD:"0",
      });
      assert.equal(r.status,0,r.stderr+r.stdout);
      assert.equal(git(src,"rev-parse","HEAD"),first);
    }finally{rmSync(root,{recursive:true,force:true});}
  }
});
