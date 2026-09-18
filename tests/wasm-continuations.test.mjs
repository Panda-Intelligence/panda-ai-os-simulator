import test from "node:test";
import assert from "node:assert/strict";
import vm from "node:vm";
import { patchWasmContinuations } from "../scripts/patch-wasm-continuations.mjs";

// Executable wrapper-shaped fixture authored for this regression. Tests execute
// transformed functions, not a second implementation of the continuation helper.
const fixture = `
function ffi_call_js(cif, fn, rvalue, avalue) {
 var orig_stack_ptr = stackSave();
 var cur_stack_ptr = orig_stack_ptr;
 var args = [], ret_by_arg = false, rtype_id = 1;
 if (cif === 16) cur_stack_ptr -= 16;
 stackRestore(cur_stack_ptr);
 var result = getWasmTableEntry(fn).apply(null, args);
 stackRestore(orig_stack_ptr);
 if (ret_by_arg) return;
 results[rvalue] = result;
}
function invoke_iii(index, a1, a2) {
 var sp = stackSave();
 try { return dynCall_iii(index, a1, a2); }
 catch (e) { stackRestore(sp); if (e !== e + 0) throw e; _setThrew(1, 0); }
}
`;
function runtime(patched = true) {
  const state = { sp: 100000, Asyncify: {state: 0, currData: 0}, results: {}, called: 0, calls: {} };
  state.stackSave = () => state.sp;
  state.stackRestore = value => { state.sp = value; };
  state.getWasmTableEntry = index => state.calls[index];
  state.dynCall_iii = (index, a, b) => state.calls[index](a,b);
  state._setThrew = () => { state.called++; };
  vm.createContext(state);
  vm.runInContext(patched ? patchWasmContinuations(fixture) : fixture, state);
  return state;
}

test("normal scalar/stack-marshalled results and numeric exception semantics remain", () => {
  const r=runtime(); r.calls[1]=()=>{assert.equal(r.sp,99984); return 47;};
  r.ffi_call_js(16,1,3,0); assert.equal(r.results[3],47); assert.equal(r.sp,100000);
  r.calls[2]=()=>{r.sp-=900; throw 17;}; r.invoke_iii(2,0,0);
  assert.equal(r.sp,100000); assert.equal(r.called,1);
  r.calls[3]=()=>{throw new Error("script failure");};
  assert.throws(()=>r.invoke_iii(3,0,0),/script failure/);
});

test("baseline reproduces rewind stack drift; corrected invoke retains the pre-unwind stack", () => {
  for(const patched of [false,true]){
    const r=runtime(patched); r.Asyncify.currData=128;
    r.calls[1]=()=>{r.sp=98000;r.Asyncify.state=1;};
    r.invoke_iii(1,0,0); r.Asyncify.state=2;
    r.calls[1]=()=>{r.Asyncify.state=0;r.sp=97000;throw 1;};
    r.invoke_iii(1,0,0);
    assert.equal(r.sp,patched?100000:98000);
  }
});
test("FFI unwinding neither publishes a result nor re-marshals during repeated rewind", () => {
  const r=runtime(); r.results[2]="previous";
  for(let round=0;round<500;round++){
    r.Asyncify.currData=256; r.Asyncify.state=0;
    r.calls[1]=()=>{assert.equal(r.sp,99984);r.sp=95000;r.Asyncify.state=1;};
    r.ffi_call_js(16,1,2,0); assert.equal(r.results[2],round?7:"previous");
    r.Asyncify.state=2;
    r.calls[1]=()=>{assert.equal(r.sp,95000);r.sp=94000;r.Asyncify.state=1;};
    r.ffi_call_js(16,1,2,0); r.Asyncify.state=2;
    r.calls[1]=()=>{assert.equal(r.sp,94000);r.Asyncify.state=0;r.sp=99984;return 7;};
    r.ffi_call_js(16,1,2,0);
    assert.equal(r.results[2],7); assert.equal(r.sp,100000);
    assert.equal(r.pandaAsyncifyFramesV1.ffi.size,0);
  }
});

test("separate fiber keys restore their own invoke frames", () => {
  const r=runtime();
  for(const key of [128,256]){
    r.sp=100000+key;r.Asyncify={state:0,currData:key};
    r.calls[key]=()=>{r.sp-=2000;r.Asyncify.state=1;};r.invoke_iii(key,0,0);
  }
  for(const key of [128,256]){
    r.sp=98000+key;r.Asyncify={state:2,currData:key};
    r.calls[key]=()=>{r.Asyncify.state=0;throw 1;};r.invoke_iii(key,0,0);
    assert.equal(r.sp,100000+key);
  }
  assert.equal(r.pandaAsyncifyFramesV1.invoke.size,0);
});
test("nested invokes unwind inner-first and rewind outer-first without leaking frames", () => {
  const r=runtime();r.Asyncify.currData=256;
  r.calls[1]=()=>{r.sp-=200;r.invoke_iii(2,0,0);};
  r.calls[2]=()=>{r.sp-=500;r.Asyncify.state=1;};
  r.invoke_iii(1,0,0);r.Asyncify.state=2;
  r.calls[1]=()=>{r.invoke_iii(2,0,0);throw 2;};
  r.calls[2]=()=>{r.Asyncify.state=0;throw 1;};
  r.invoke_iii(1,0,0);
  assert.equal(r.sp,100000);assert.equal(r.called,2);
  assert.equal(r.pandaAsyncifyFramesV1.invoke.size,0);
});

test("missing or mismatched rewind identity fails instead of restoring an unrelated stack", () => {
  const r=runtime();r.Asyncify={state:2,currData:256};
  assert.throws(()=>r.invoke_iii(1,0,0),/missing/);
  r.Asyncify.state=0;r.calls[1]=()=>{r.Asyncify.state=1;};r.invoke_iii(1,0,0);
  r.Asyncify.state=2;assert.throws(()=>r.invoke_iii(2,0,0),/mismatch/);
});

test("patching is idempotent and rejects changed compiler wrappers", () => {
  const patched=patchWasmContinuations(fixture);
  assert.equal(patchWasmContinuations(patched),patched);
  assert.throws(()=>patchWasmContinuations(fixture.replace("var sp = stackSave();","let sp = stackSave();")),/shape_changed/);
  assert.throws(()=>patchWasmContinuations("/* panda-asyncify-stack-ownership-v1 */"),/invalid_existing_marker/);
});
