import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { gzipSync } from "node:zlib";
import { packageExistingRuntime } from "../scripts/pack-existing-runtime.mjs";
const digest=data=>createHash("sha256").update(data).digest("hex");
const revision="a".repeat(40);
function fixture(){
  const root=mkdtempSync(join(tmpdir(),"panda-existing-test-"));
  const qemuDir=join(root,"qemu"),uiDir=join(root,"dist"),outputDir=join(root,"output");
  mkdirSync(qemuDir);mkdirSync(uiDir);writeFileSync(join(uiDir,"index.html"),"<!doctype html><title>fixture</title>");
  const artifact=(name,bytes)=>{
    writeFileSync(join(qemuDir,name),bytes);
    return {fileName:name,relativePath:name,bytes:Buffer.byteLength(bytes),sha256:digest(bytes)};
  };
  const wrappers=`function ffi_call_js(cif,fn,rvalue,avalue){var orig_stack_ptr=stackSave();var cur_stack_ptr=orig_stack_ptr;var args=[],ret_by_arg=false,rtype_id=1;stackRestore(cur_stack_ptr);var result=getWasmTableEntry(fn).apply(null,args);stackRestore(orig_stack_ptr);return result;}function invoke_iii(index,a1,a2){var sp=stackSave();try{return dynCall_iii(index,a1,a2);}catch(e){stackRestore(sp);if(e!==e+0)throw e;_setThrew(1,0);}}`;
  const js=wrappers+'function _mofei_wasm_ipc_send(channel,flags,payloadPtr,payloadLen){if(typeof Module["_mofeiWasmIpcSend"]==="function"){Module["_mofeiWasmIpcSend"](channel>>>0,flags>>>0,payloadPtr>>>0,payloadLen>>>0)}};var knownHandlers=[];export default Module;';
  const artifacts=[artifact("qemu-system-xtensa.js",js),...["qemu-system-xtensa.wasm","qemu-system-xtensa.worker.js","bootloader.bin","partition-table.bin","ota_data_initial.bin","esp32s3_rev0_rom.bin"].map(name=>artifact(name,"fixture"))];
  const firmware={bin:artifact("firmware.bin","bin"),kernel:artifact("firmware-kernel.img","kernel"),symbols:artifact("firmware-symbols.txt","symbols")};
  const provenance={board:"default",sourceRevision:revision,sourceElfSha256:"b".repeat(64),sourceBinSha256:firmware.bin.sha256,publishedBinSha256:firmware.bin.sha256,publishedKernelSha256:firmware.kernel.sha256,publishedSymbolsSha256:firmware.symbols.sha256};
  const manifest={schemaVersion:2,status:"ready",target:"xtensa-softmmu",sourceRevision:revision,artifacts,boards:{mofei:{status:"ready",buildBoard:"default",framebuffer:{width:800,height:480,format:"mono1"},firmware,provenance}}};
  const save=()=>writeFileSync(join(qemuDir,"qemu-wasm-artifacts.json"),JSON.stringify(manifest));save();
  return {root,qemuDir,uiDir,outputDir,manifest,save};
}
test("existing runtime preserves guest identity and legacy base URLs with bounded gzip seed",()=>{
  const f=fixture();try{
    f.sdImage=join(f.root,"seed.gz");f.sdRawBytes=512;
    writeFileSync(f.sdImage,gzipSync(Buffer.alloc(512)));
    packageExistingRuntime({...f,basePath:"/simulator/app/"});
    const result=JSON.parse(readFileSync(join(f.outputDir,"manifest.json")));
    assert.equal(result.runtime.source.revision,revision);
    assert.equal(result.runtime.qemuSdRawBytes,512);
    assert.equal(result.runtime.qemuSdImage,"/simulator/app/simulator-runtime/qemu/sdcard.img.gz");
    assert.equal(result.publication.localOnly,true);
    assert.match(readFileSync(join(f.outputDir,"simulator-runtime/qemu/qemu-system-xtensa.js"),"utf8"),/knownHandlers=\["_mofeiWasmIpcSend"\]/);
  }finally{rmSync(f.root,{recursive:true,force:true});}
});
for(const kind of ["bad-digest","bad-provenance","bad-geometry","bad-gzip-size","duplicate-artifact"]){
  test(`existing runtime rejects ${kind} and preserves owned prior output`,()=>{
    const f=fixture();try{
      packageExistingRuntime(f);const before=readFileSync(join(f.outputDir,"manifest.json"));
      if(kind==="bad-digest")f.manifest.artifacts[1].sha256="0".repeat(64);
      if(kind==="bad-provenance")f.manifest.boards.mofei.provenance.sourceRevision="c".repeat(40);
      if(kind==="bad-geometry")f.manifest.boards.mofei.framebuffer.width=1;
      if(kind==="duplicate-artifact")f.manifest.artifacts.push(f.manifest.artifacts[0]);
      if(kind==="bad-gzip-size"){f.sdImage=join(f.root,"seed.gz");f.sdRawBytes=512;writeFileSync(f.sdImage,gzipSync(Buffer.alloc(1024)));}
      f.save();assert.throws(()=>packageExistingRuntime(f));
      assert.deepEqual(readFileSync(join(f.outputDir,"manifest.json")),before);
    }finally{rmSync(f.root,{recursive:true,force:true});}
  });
}

for (const required of ["all", "mofei,m5papers3", "unknown", "mofei,mofei"]) {
  test(`required board gate refuses ${required} before replacing a package`, () => {
    const f = fixture();
    try {
      packageExistingRuntime(f);
      const before = readFileSync(join(f.outputDir, "manifest.json"));
      assert.throws(() => packageExistingRuntime({...f, requireBoards: required}), /required_boards|invalid_required/);
      assert.deepEqual(readFileSync(join(f.outputDir, "manifest.json")), before);
    } finally { rmSync(f.root, {recursive:true, force:true}); }
  });
}
test("an explicit available board subset passes without fabricating other guests", () => {
  const f = fixture();
  try {
    packageExistingRuntime({...f, requireBoards:"mofei"});
    const manifest = JSON.parse(readFileSync(join(f.outputDir,"manifest.json")));
    assert.deepEqual(Object.keys(manifest.runtime.firmwareArtifacts), ["mofei"]);
  } finally { rmSync(f.root,{recursive:true,force:true}); }
});

test("required runtime revision refuses stale or unlabelled builds before replacement", () => {
  const f=fixture();
  try {
    packageExistingRuntime(f);
    const before=readFileSync(join(f.outputDir,"manifest.json"));
    for(const revision of [undefined,"asyncify-stack-v2"]){
      f.manifest.runtimeBuildRevision=revision;f.save();
      assert.throws(()=>packageExistingRuntime({...f,requireRuntimeRevision:"asyncify-stack-v4-papers3-sdspi"}),/runtime_revision_mismatch/);
      assert.deepEqual(readFileSync(join(f.outputDir,"manifest.json")),before);
    }
    f.manifest.runtimeBuildRevision="asyncify-stack-v4-papers3-sdspi";f.save();
    packageExistingRuntime({...f,requireRuntimeRevision:"asyncify-stack-v4-papers3-sdspi"});
  } finally {rmSync(f.root,{recursive:true,force:true});}
});
