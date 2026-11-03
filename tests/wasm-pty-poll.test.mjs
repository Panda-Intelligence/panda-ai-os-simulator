import test from "node:test";
import assert from "node:assert/strict";
import vm from "node:vm";
import { readFileSync } from "node:fs";
import ts from "typescript";
import { patchWasmPtyPoll } from "../scripts/patch-wasm-continuations.mjs";

// Emscripten 3.1.50 与 xterm-pty 0.10.1 的实际生成函数；可直接注入构建产物复核同一边界。
const generatedPoll = `function xterm_pty_old_poll(fds,nfds,timeout){if(ENVIRONMENT_IS_PTHREAD)return proxyToMainThread(26,1,fds,nfds,timeout);try{var nonzero=0;for(var i=0;i<nfds;i++){var pollfd=fds+8*i;var fd=HEAP32[pollfd>>2];var events=HEAP16[pollfd+4>>1];var mask=32;var stream=FS.getStream(fd);if(stream){mask=SYSCALLS.DEFAULT_POLLMASK;if(stream.stream_ops.poll){mask=stream.stream_ops.poll(stream,-1)}}mask&=events|8|16;if(mask)nonzero++;HEAP16[pollfd+6>>1]=mask}return nonzero}catch(e){if(typeof FS=="undefined"||!(e.name==="ErrnoError"))throw e;return-e.errno}}`;
function pollFunction(source) {
  const file = ts.createSourceFile("runtime.js", source, ts.ScriptTarget.ESNext, true, ts.ScriptKind.JS);
  let result;
  function visit(node) {
    if (ts.isFunctionDeclaration(node) && node.name?.text === "xterm_pty_old_poll") result = node.getText(file);
    ts.forEachChild(node, visit);
  }
  visit(file);
  assert.ok(result);
  return result;
}
const source = process.env.PANDA_QEMU_WASM_GLUE_SOURCE
  ? pollFunction(readFileSync(process.env.PANDA_QEMU_WASM_GLUE_SOURCE, "utf8")) : generatedPoll;
const pty = `
var PTY_pollTimeout=0;
var PTY_askToWaitAgain=timeout=>{PTY_pollTimeout=timeout;throw new FS.ErrnoError(1006)};
var ttyPoll=(stream,timeout)=>{if(!PTY.readable&&timeout){PTY_askToWaitAgain(timeout)}return(PTY.readable?1:0)|(PTY.writable?4:0)};
var PTY_waitForReadableWithCallback=callback=>{if(PTY_pollTimeout===0){return callback(PTY.readable?0:2)}let handlerReadable,handlerSignal,timeoutId;new Promise(resolve=>{handlerReadable=PTY.onReadable(()=>resolve(0));handlerSignal=PTY.onSignal(()=>resolve(1));if(PTY_pollTimeout>=0){timeoutId=setTimeout(resolve,PTY_pollTimeout,2)}}).then(type=>{handlerReadable.dispose();handlerSignal.dispose();clearTimeout(timeoutId);callback(type)})};
`;
function runtime(patched) {
  const heap = new ArrayBuffer(128), input = [], readable = new Set(), signal = new Set(), timers = [];
  const state = { HEAP32: new Int32Array(heap), HEAP16: new Int16Array(heap), ENVIRONMENT_IS_PTHREAD: false,
    SYSCALLS: { DEFAULT_POLLMASK: 5 }, setTimeout: (...args) => { timers.push(args[1]); return setTimeout(...args); }, clearTimeout,
    FS: { ErrnoError: class extends Error { constructor(errno) { super(); this.name = "ErrnoError"; this.errno = errno; } } },
    PTY: { get readable() { return input.length > 0; }, writable: true,
      onReadable(fn) { readable.add(fn); return { dispose: () => readable.delete(fn) }; },
      onSignal(fn) { signal.add(fn); return { dispose: () => signal.delete(fn) }; } } };
  vm.createContext(state);
  vm.runInContext(pty + (patched ? patchWasmPtyPoll(source) : source), state);
  const otherTimeouts = [];
  state.FS.getStream = fd => fd === 0 ? { tty: {}, stream_ops: { poll: state.ttyPoll } }
    : fd === 7 ? { stream_ops: { poll: (_stream, timeout) => { otherTimeouts.push(timeout); return 1; } } } : undefined;
  return { state, timers, otherTimeouts, input,
    poll(timeout, fd = 0) { state.HEAP32[0] = fd; state.HEAP16[2] = 1; return state.xterm_pty_old_poll(0, 1, timeout); },
    wait() { return new Promise(resolve => state.PTY_waitForReadableWithCallback(resolve)); },
    push(bytes) { input.push(...bytes); for (const fn of readable) fn(); },
    interrupt() { for (const fn of signal) fn(); },
    listenerCount() { return readable.size + signal.size; } };
}

test("baseline loses finite poll deadline; patched PTY expires without fabricating bytes", async () => {
  const old = runtime(false);
  assert.equal(old.poll(10), -1006);
  assert.equal(old.state.PTY_pollTimeout, -1);
  let oldDone = false;
  const pending = old.wait().then(value => { oldDone = true; return value; });
  await new Promise(resolve => setTimeout(resolve, 25));
  assert.equal(oldDone, false);
  assert.deepEqual(old.timers, []);
  old.push([65]);
  assert.equal(await pending, 0);
  const fixed = runtime(true);
  assert.equal(fixed.poll(10), -1006);
  assert.equal(fixed.state.PTY_pollTimeout, 10);
  assert.equal(await fixed.wait(), 2);
  assert.deepEqual(fixed.timers, [10]);
  assert.deepEqual(fixed.input, []);
  assert.equal(fixed.state.HEAP16[3], 0);
  assert.equal(fixed.listenerCount(), 0);
});

test("zero poll is nonblocking and non-TTY/invalid descriptors keep their behavior", () => {
  const r = runtime(true);
  assert.equal(r.poll(0), 0);
  assert.deepEqual(r.timers, []);
  assert.equal(r.poll(20, 7), 1);
  assert.deepEqual(r.otherTimeouts, [-1]);
  assert.equal(r.state.HEAP16[3], 1);
  const old = runtime(false);
  assert.equal(r.poll(20, 99), old.poll(20, 99));
  assert.equal(r.state.HEAP16[3], old.state.HEAP16[3]);
});

test("positive and legitimate indefinite waits wake on real unread bytes without consuming them", async () => {
  for (const timeout of [1000, -1]) {
    const r = runtime(true);
    assert.equal(r.poll(timeout), -1006);
    const pending = r.wait();
    assert.deepEqual(r.timers, timeout < 0 ? [] : [timeout]);
    r.push([67, 77, 68, 10]);
    assert.equal(await pending, 0);
    assert.equal(r.poll(timeout), 1);
    assert.deepEqual(r.input, [67, 77, 68, 10]);
    assert.equal(r.state.HEAP16[3], 1);
    assert.equal(r.listenerCount(), 0);
  }
});

test("signals and synchronous pthread proxy preserve original result/arguments", async () => {
  const r = runtime(true);
  assert.equal(r.poll(-1), -1006);
  const pending = r.wait();
  r.interrupt();
  assert.equal(await pending, 1);
  assert.equal(r.listenerCount(), 0);
  r.state.ENVIRONMENT_IS_PTHREAD = true;
  r.state.proxyToMainThread = (...args) => { assert.deepEqual(args, [26, 1, 0, 1, 17]); return 23; };
  assert.equal(r.poll(17), 23);
});

test("poll patch is idempotent and rejects compiler shape drift", () => {
  const patched = patchWasmPtyPoll(source);
  assert.equal(patchWasmPtyPoll(patched), patched);
  assert.throws(() => patchWasmPtyPoll(source.replace("nfds,timeout", "nfds,deadline")), /pty_poll_function/);
  assert.throws(() => patchWasmPtyPoll(source.replace("poll(stream,-1)", "poll(stream,99)")), /pty_poll_timeout/);
});
