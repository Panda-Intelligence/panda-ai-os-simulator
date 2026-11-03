import assert from "node:assert/strict";
import test from "node:test";
import vm from "node:vm";
import { MessageChannel } from "node:worker_threads";
import { readFileSync } from "node:fs";
import ts from "typescript";
import { patchWasmRootMailbox } from "../scripts/patch-wasm-continuations.mjs";

// Emscripten 3.1.50 的生成函数；可对真实 6437 glue 提取同一函数执行回归。
const fixture = `
var unrelated=typeof Atomics.waitAsync==="function";
var __emscripten_thread_mailbox_await=pthread_ptr=>{if(typeof Atomics.waitAsync==="function"){var wait=Atomics.waitAsync(HEAP32,pthread_ptr>>2,pthread_ptr);wait.value.then(checkMailbox);var waitingAsync=pthread_ptr+128;Atomics.store(HEAP32,waitingAsync>>2,1)}};
var checkMailbox=()=>{var pthread_ptr=_pthread_self();if(pthread_ptr){__emscripten_thread_mailbox_await(pthread_ptr);callUserCallback(__emscripten_check_mailbox)}};
var __emscripten_notify_mailbox_postmessage=(targetThreadId,currThreadId,mainThreadId)=>{if(targetThreadId==currThreadId){setTimeout(()=>checkMailbox())}else if(ENVIRONMENT_IS_PTHREAD){postMessage({"targetThread":targetThreadId,"cmd":"checkMailbox"})}else{var worker=PThread.pthreads[targetThreadId];if(!worker){return}worker.postMessage({"cmd":"checkMailbox"})}};
`;
const source = process.env.PANDA_QEMU_WASM_GLUE_SOURCE ? readFileSync(process.env.PANDA_QEMU_WASM_GLUE_SOURCE, "utf8") : fixture;
function bindings(text) {
  const file = ts.createSourceFile("runtime.js", text, ts.ScriptTarget.ESNext, true, ts.ScriptKind.JS);
  const wanted = ["__emscripten_thread_mailbox_await", "checkMailbox", "__emscripten_notify_mailbox_postmessage"], found = [];
  function visit(node) {
    if (ts.isVariableDeclaration(node) && wanted.includes(node.name.getText(file))) found.push(node.parent.parent.getText(file));
    ts.forEachChild(node, visit);
  }
  visit(file);
  assert.equal(found.length, 3);
  return found.join("\n");
}
function runtime(pthread, patched = true, nativeWait = true) {
  const waits = [], wakeups = [], messages = [], callbacks = [];
  const heap = new Int32Array(new SharedArrayBuffer(1024));
  const self = pthread ? 256 : 64;
  heap[self >> 2] = self;
  const state = { ENVIRONMENT_IS_PTHREAD: pthread, HEAP32: heap, _pthread_self: () => self,
    Atomics: { store: Atomics.store, ...(nativeWait ? { waitAsync: (...args) => {
      waits.push(args); return { async: true, value: { then: callback => wakeups.push(callback) } };
    } } : {}) },
    callUserCallback: callback => callback(), __emscripten_check_mailbox: () => callbacks.push("consumed"),
    postMessage: message => messages.push(message), PThread: { pthreads: {} }, setTimeout,
  };
  vm.createContext(state);
  vm.runInContext(bindings(patched ? patchWasmRootMailbox(source) : source), state);
  return { state, waits, wakeups, messages, callbacks, self,
    awaitMailbox() { state.__emscripten_thread_mailbox_await(self); },
    waitingAsync() { return Atomics.load(heap, (self + 128) >> 2); } };
}

test("baseline root advertises native waiting; candidate leaves SDK postMessage fallback selected", () => {
  const old = runtime(false, false); old.awaitMailbox();
  assert.equal(old.waits.length, 1); assert.equal(old.waitingAsync(), 1);
  const root = runtime(false); root.awaitMailbox();
  assert.equal(root.waits.length, 0); assert.equal(root.waitingAsync(), 0);
  for (let i = 0; i < 100; i++) root.state.checkMailbox();
  assert.equal(root.waits.length, 0);
  assert.equal(root.callbacks.length, 100);
});

test("PThread retains the original native wait registration and rearm behavior", () => {
  const worker = runtime(true); worker.awaitMailbox();
  assert.equal(worker.waits.length, 1); assert.equal(worker.waitingAsync(), 1);
  assert.equal(worker.waits[0][1], worker.self >> 2);
  assert.equal(worker.waits[0][2], worker.self);
  worker.wakeups[0]();
  assert.equal(worker.waits.length, 2);
  assert.equal(worker.callbacks.length, 1);
});

test("SDK root notification uses real postMessage delivery and consumes the queued callback", async () => {
  const root = runtime(false), worker = runtime(true), channel = new MessageChannel();
  root.awaitMailbox();
  const payload = new Uint8Array([17, 23, 42]), before = payload.slice();
  let consumed = false;
  root.state.__emscripten_check_mailbox = () => { consumed = true; assert.deepEqual(payload, before); };
  const arrived = new Promise((resolve, reject) => {
    channel.port1.once("message", message => {
      try {
        assert.deepEqual(message, { targetThread: root.self, cmd: "checkMailbox" });
        root.state.checkMailbox(); resolve();
      } catch (error) { reject(error); }
    });
  });
  worker.state.postMessage = message => channel.port2.postMessage(message);
  try {
    assert.equal(root.waitingAsync(), 0);
    worker.state.__emscripten_notify_mailbox_postmessage(root.self, worker.self, root.self);
    await arrived;
    assert.equal(consumed, true);
    assert.equal(root.waits.length, 0);
    assert.equal(root.waitingAsync(), 0);
  } finally { channel.port1.close(); channel.port2.close(); }
});

test("without waitAsync both owners retain the SDK fallback and no new scheduling is introduced", () => {
  for (const pthread of [false, true]) {
    const r = runtime(pthread, true, false); r.awaitMailbox();
    assert.equal(r.waits.length, 0); assert.equal(r.waitingAsync(), 0);
    r.state.checkMailbox(); assert.equal(r.callbacks.length, 1);
  }
});

test("AST changes only the mailbox condition, is idempotent, and rejects compiler drift", () => {
  const patched = patchWasmRootMailbox(source);
  assert.equal(patchWasmRootMailbox(patched), patched);
  const file = ts.createSourceFile("original.js", source, ts.ScriptTarget.ESNext, true, ts.ScriptKind.JS);
  let condition;
  function visit(node) {
    if (ts.isVariableDeclaration(node) && node.name.getText(file) === "__emscripten_thread_mailbox_await") condition = node.initializer.body.statements[0].expression.getText(file);
    ts.forEachChild(node, visit);
  }
  visit(file);
  assert.equal(patched.replace('ENVIRONMENT_IS_PTHREAD && typeof Atomics.waitAsync === "function"', condition), source);
  assert.throws(() => patchWasmRootMailbox(fixture.replace('if(typeof Atomics.waitAsync==="function")', 'if(typeof Atomics.waitAsync!=="function")')), /root_mailbox_condition/);
  assert.throws(() => patchWasmRootMailbox(fixture.replaceAll("__emscripten_thread_mailbox_await", "other")), /root_mailbox_function/);
});
