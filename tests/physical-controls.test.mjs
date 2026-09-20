import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import ts from "typescript";
const boards=JSON.parse(await readFile(new URL("../boards.json",import.meta.url),"utf8")).boards;
const source=await readFile(new URL("../src/boardPhysicalControls.ts",import.meta.url),"utf8");
const compiled=ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.ESNext,target:ts.ScriptTarget.ES2022}}).outputText;
const {getPhysicalControls,PHYSICAL_CONTROL_LAYOUTS}=await import("data:text/javascript;base64,"+Buffer.from(compiled).toString("base64"));
const board=id=>boards.find(b=>b.id===id);

test("PaperS3 PWR is near the lower right rather than the old 61% top",()=>{
  assert.deepEqual(getPhysicalControls(board("m5papers3")),[
    {name:"PWR",edge:"right",center:.825,length:.082,projection:.012,buttonId:0}]);
});
test("LILYGO distributes its four controls over both portrait edges",()=>{
  const keys=getPhysicalControls(board("lilygo-t5s3-pro"));
  assert.deepEqual(keys.filter(k=>k.edge==="left").map(k=>[k.name,k.buttonId,k.center]),[["BOOT",0,.575],["IO48",1,.665]]);
  assert.deepEqual(keys.filter(k=>k.edge==="right").map(k=>[k.name,k.center]),[["RST",.590],["PWR",.680]]);
});
test("RST never injects an invented GPIO",()=>{
  const rst=getPhysicalControls(board("lilygo-t5s3-pro")).find(k=>k.name==="RST");
  assert.equal(rst.reset,true);assert.equal(rst.buttonId,undefined);
});
test("keyMap reordering cannot change control anchors or identities",()=>{
  for(const id of Object.keys(PHYSICAL_CONTROL_LAYOUTS)) {
    const b=board(id);assert.deepEqual(getPhysicalControls({...b,keyMap:[...b.keyMap].reverse()}),getPhysicalControls(b));
  }
});
test("missing or duplicate logical mappings are rejected",()=>{
  const b=board("m5papers3");
  assert.throws(()=>getPhysicalControls({...b,keyMap:[]}),/physical_control_mapping_invalid/);
  assert.throws(()=>getPhysicalControls({...b,keyMap:[...b.keyMap,...b.keyMap]}),/physical_control_mapping_invalid/);
});
test("caps stay on the chassis and cannot overlap a same-edge key",()=>{
  for(const controls of Object.values(PHYSICAL_CONTROL_LAYOUTS)) {
    for(const c of controls) {assert.ok(c.center-c.length/2>0);assert.ok(c.center+c.length/2<1);assert.ok(c.projection>0&&c.projection<.05);}
    for(const edge of ["left","right"]) {
      const sorted=controls.filter(c=>c.edge===edge).sort((a,b)=>a.center-b.center);
      for(let i=1;i<sorted.length;i++) assert.ok(sorted[i-1].center+sorted[i-1].length/2<sorted[i].center-sorted[i].length/2);
    }
  }
});
test("returned data cannot mutate source calibration",()=>{
  const b=board("m5papers3");getPhysicalControls(b)[0].center=.1;assert.equal(getPhysicalControls(b)[0].center,.825);
});
test("unrelated boards keep the existing renderer",()=>{
  for(const b of boards.filter(b=>!PHYSICAL_CONTROL_LAYOUTS[b.id])) assert.equal(getPhysicalControls(b),null);
});
