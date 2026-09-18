import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtempSync,mkdirSync,writeFileSync,readFileSync,rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {spawnSync} from 'node:child_process';
const script=new URL('../scripts/select-xtensa-helper.py',import.meta.url);
const functions=['xtensa_register_core','xtensa_breakpoint_handler','xtensa_cpu_do_unaligned_access','xtensa_cpu_tlb_fill','xtensa_cpu_do_transaction_failed','xtensa_runstall'];
for (const scenario of ['merged','missing-definition','duplicate-registration','already-selected']) {
 test(`Xtensa helper source selection: ${scenario}`,()=>{
  const root=mkdtempSync(join(tmpdir(),'panda-xtensa-selector-'));
  try {
   const dir=join(root,'target/xtensa');mkdirSync(dir,{recursive:true});
   const names=scenario==='missing-definition'?functions.slice(1):functions;
   writeFileSync(join(dir,'exc_helper.c'),names.map(name=>`void ${name}(void) {}`).join('\n'));
   const registration=scenario==='already-selected'?"  'exc_helper.c',\n":scenario==='duplicate-registration'?"  'exc_helper.c',\n  'helper.c',\n  'helper.c',\n":"  'exc_helper.c',\n  'helper.c',\n";
   const original=`xtensa_ss.add(files(\n${registration}  'cpu.c',\n))\n`;
   writeFileSync(join(dir,'meson.build'),original);
   const run=()=>spawnSync('python3',[script.pathname,root],{encoding:'utf8'});
   const result=run();
   if (scenario==='merged'||scenario==='already-selected') {
    assert.equal(result.status,0,result.stderr);
    assert.equal((readFileSync(join(dir,'meson.build'),'utf8').match(/'exc_helper.c'/g)||[]).length,1);
    assert.ok(!readFileSync(join(dir,'meson.build'),'utf8').includes("'helper.c'"));
    assert.equal(run().status,0);
   } else {assert.notEqual(result.status,0);assert.equal(readFileSync(join(dir,'meson.build'),'utf8'),original);}
  } finally {rmSync(root,{recursive:true,force:true});}
 });
}
