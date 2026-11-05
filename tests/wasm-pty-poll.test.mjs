import test from "node:test";
import assert from "node:assert/strict";
import vm from "node:vm";
import { createHash } from "node:crypto";
import { copyFileSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import ts from "typescript";
import { assertWasmPtySource } from "../scripts/patch-wasm-continuations.mjs";
import { patchWasmPtySources } from "../scripts/patch-wasm-pty-source.mjs";

const fixture = join(dirname(fileURLToPath(import.meta.url)), "fixtures/wasm-pty-source");
const sha = bytes => createHash("sha256").update(bytes).digest("hex");
const emission = JSON.parse(readFileSync(join(fixture, "emission.json"), "utf8"));
const functions = new Map(), sources = new Map();
for (const mode of ["red", "green"]) {
  const source = readFileSync(join(fixture, `emitted-${mode}.js`), "utf8");
  assert.equal(sha(source), emission.emissions.find(entry => entry.mode === mode).jsSha256);
  sources.set(mode, source);
  const file = ts.createSourceFile("actual-emitted.js", source, ts.ScriptTarget.ESNext, true, ts.ScriptKind.JS), nodes = new Map();
  function visit(node) {
    if (ts.isFunctionDeclaration(node) && node.name) nodes.set(node.name.text, node.getText(file));
    if (ts.isVariableDeclaration(node) && node.initializer) nodes.set(node.name.getText(file), node.initializer.getText(file));
    ts.forEachChild(node, visit);
  }
  visit(file);functions.set(mode, nodes);
}
const worker = ts.createSourceFile("actual-worker.js", readFileSync(new URL("../src/browserSimulatorRuntimeWorker.js", import.meta.url), "utf8"), ts.ScriptTarget.ESNext, true, ts.ScriptKind.JS);
const workerFunctions = [];
ts.forEachChild(worker, node => { if (ts.isFunctionDeclaration(node) && ["createBrowserQemuPty", "subscribe"].includes(node.name?.text)) workerFunctions.push(node.getText(worker)); });
assert.equal(workerFunctions.length, 2);

function runtime(mode = "green") {
  const heap = new SharedArrayBuffer(1024), timers = [], calls = [], nodes = functions.get(mode);
  const state = { HEAP32: new Int32Array(heap), HEAPU32: new Uint32Array(heap), HEAP16: new Int16Array(heap), HEAPU8: new Uint8Array(heap),
    Uint8Array, TextDecoder, ENVIRONMENT_IS_PTHREAD: false, SYSCALLS: { DEFAULT_POLLMASK: 5 }, log() {},
    setTimeout: (...args) => { timers.push(args[1]);return setTimeout(...args); }, clearTimeout,
    FS: { ErrnoError: class extends Error { constructor(errno) { super();this.name = "ErrnoError";this.errno = errno; } } } };
  vm.createContext(state);vm.runInContext(workerFunctions.join("\n") + "\nglobalThis.PTY=createBrowserQemuPty();", state);
  let subscriptions = 0;
  for (const method of ["onReadable", "onSignal"]) {
    const original = state.PTY[method];
    state.PTY[method] = listener => { subscriptions++;const disposable = original(listener);return { dispose() { subscriptions--;disposable.dispose(); } }; };
  }
  for (const name of ["TTY", "PANDA_PTY_TIMEVAL_LAYOUT", "PTY_pollTimeout", "PTY_askToWaitAgain", "PTY_waitForReadableWithCallback", "PTY_wrapPoll", "___syscall_poll", "___syscall__newselect", "PTY_waitForReadableWithAtomicImpl", "PTY_waitForReadableWithAtomic", "PTY_handleSleepWithAtomic", "PTY_atomicIndex"]) {
    assert.ok(nodes.has(name), name);vm.runInContext(`globalThis.${name}=${nodes.get(name)};`, state);
  }
  if (nodes.has("PTY_selectTimeout")) vm.runInContext(`globalThis.PTY_selectTimeout=${nodes.get("PTY_selectTimeout")};`, state);
  if (nodes.has("PTY_clearSelectFds")) vm.runInContext(`globalThis.PTY_clearSelectFds=${nodes.get("PTY_clearSelectFds")};`, state);
  for (const name of ["xterm_pty_old_poll", "xterm_pty_old_newselect"]) vm.runInContext(nodes.get(name), state);
  state.PTY_handleSleep = callback => { let value;callback(result => { value = result; });return value; };
  state.PTY_waitForReadable = state.PTY_waitForReadableWithCallback;
  const stream = { tty: {}, flags: 0, stream_ops: state.TTY.stream_ops };
  state.FS.getStream = fd => fd === 0 ? stream : fd === 7 ? { stream_ops: { poll: (_stream, timeout) => { calls.push(timeout);return 1; } } } : undefined;
  state.SYSCALLS.getStreamFromFD = fd => { const found = state.FS.getStream(fd);if (!found) throw new state.FS.ErrnoError(8);return found; };
  return { state, stream, timers, calls, subscriptionCount: () => subscriptions,
    poll(timeout, fds = [0]) { fds.forEach((fd, i) => { state.HEAP32[i * 2] = fd;state.HEAP16[i * 4 + 2] = 1; });return state.xterm_pty_old_poll(0, fds.length, timeout); },
    select(timeout, fds = [0], kind = "read") {
      for (const ptr of [16, 32, 48]) { state.HEAP32[ptr >> 2] = 0;state.HEAP32[(ptr + 4) >> 2] = 0; }
      const ptr = { read: 16, write: 32, except: 48 }[kind];
      for (const fd of fds) state.HEAP32[(ptr + (fd < 32 ? 0 : 4)) >> 2] |= 1 << (fd % 32);
      state.HEAPU8.fill(0, 64, 64 + state.PANDA_PTY_TIMEVAL_LAYOUT.size);
      if (timeout !== null) {
        state.HEAP32[(64 + state.PANDA_PTY_TIMEVAL_LAYOUT.sec) >> 2] = Math.floor(timeout / 1000);
        state.HEAP32[(64 + state.PANDA_PTY_TIMEVAL_LAYOUT.usec) >> 2] = (timeout % 1000) * 1000;
      }
      return state.xterm_pty_old_newselect(Math.max(...fds) + 1, kind === "read" ? ptr : 0, kind === "write" ? ptr : 0, kind === "except" ? ptr : 0, timeout === null ? 0 : 64);
    },
    wait(timeout) { return new Promise(resolve => state.PTY_waitForReadableWithCallback(resolve, timeout)); }, push(bytes) { state.PTY.pushInput(bytes); } };
}

test("actual source libraries and compiler emissions have pinned provenance", () => {
  assert.equal(emission.emscriptenVersion, "3.1.50");assert.equal(emission.xtermPtyVersion, "0.10.1");
  assert.equal(sha(readFileSync(new URL("../scripts/wasm-pty-source.patch", import.meta.url))), emission.sourcePatchSha256);
  for (const entry of emission.sourceLibraries) assert.equal(sha(readFileSync(join(fixture, entry.key === "pty" ? "emscripten-pty.js" : "library_syscall.js"))), entry.beforeSha256);
});

test("source patch is exact, idempotent, and rejects unreviewed source", () => {
  const dir = mkdtempSync(join(tmpdir(), "panda-pty-source-test-"));
  try {
    const ptySource = join(dir, "emscripten-pty.js"), syscallSource = join(dir, "library_syscall.js");
    copyFileSync(join(fixture, "emscripten-pty.js"), ptySource);copyFileSync(join(fixture, "library_syscall.js"), syscallSource);
    const options = { ptySource, syscallSource, outputDir: join(dir, "output") }, first = patchWasmPtySources(options);
    assert.deepEqual(patchWasmPtySources(options), first);
    for (const entry of first.files) assert.equal(entry.patchedSha256, emission.sourceLibraries.find(source => source.key === entry.key).afterSha256);
    assert.throws(() => patchWasmPtySources({ ...options, syscallSource: join(fixture, "probe.c") }), /source_version_mismatch/);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test("baseline blocks an empty nonblocking TTY; source-patched read returns EAGAIN", () => {
  const old = runtime("red"), fixed = runtime();old.stream.flags = fixed.stream.flags = 2048;
  assert.throws(() => old.state.TTY.stream_ops.read(old.stream, new Uint8Array(4), 0, 1), e => e.errno === 1006);
  assert.equal(old.state.PTY_pollTimeout, -1);
  assert.throws(() => fixed.state.TTY.stream_ops.read(fixed.stream, new Uint8Array(4), 0, 1), e => e.errno === 6);
  assert.equal(fixed.state.PTY_pollTimeout, 0);assert.deepEqual(fixed.timers, []);
});

test("blocking, readable and zero-length reads preserve their data semantics", () => {
  const r = runtime(), buffer = new Uint8Array(4);
  assert.throws(() => r.state.TTY.stream_ops.read(r.stream, buffer, 0, 1), e => e.errno === 1006);
  r.stream.flags = 2048;r.push([65, 66]);assert.equal(r.state.TTY.stream_ops.read(r.stream, buffer, 0, 1), 1);assert.equal(buffer[0], 65);
  assert.deepEqual(Array.from(r.state.PTY.read(4)), [66]);assert.equal(r.state.TTY.stream_ops.read(r.stream, buffer, 0, 0), 0);
});

test("already unread input wakes positive/negative waits without consuming bytes", async () => {
  for (const timeout of [1000, -1]) {
    const old = runtime("red");old.push([67]);old.state.PTY_pollTimeout = timeout;
    let completed = false;const pending = old.wait(timeout).then(value => { completed = true;return value; });
    await new Promise(resolve => setTimeout(resolve, 8));assert.equal(completed, false);old.state.PTY.signal("SIGINT");assert.equal(await pending, 1);
    const fixed = runtime();fixed.push([67]);assert.equal(await fixed.wait(timeout), 0);assert.deepEqual(fixed.timers, []);
    assert.equal(fixed.subscriptionCount(), 0);assert.deepEqual(Array.from(fixed.state.PTY.read(4)), [67]);
  }
});

test("later real input/signals dispose subscriptions and finite/zero waits preserve deadlines", async () => {
  const r = runtime();assert.equal(await r.wait(0), 2);assert.deepEqual(r.timers, []);
  assert.equal(await r.wait(5), 2);assert.deepEqual(r.timers, [5]);assert.equal(r.subscriptionCount(), 0);
  const pending = r.wait(-1);r.push([68]);assert.equal(await pending, 0);assert.equal(r.subscriptionCount(), 0);assert.deepEqual(Array.from(r.state.PTY.read(4)), [68]);
  const interrupted = r.wait(-1);r.state.PTY.signal("SIGINT");assert.equal(await interrupted, 1);assert.equal(r.subscriptionCount(), 0);
});

test("mixed-fd readiness is returned before a TTY wait in either descriptor order", () => {
  for (const fds of [[0, 7], [7, 0]]) {
    assert.equal(runtime("red").poll(10, fds), -1006);
    const fixed = runtime();assert.equal(fixed.poll(10, fds), 1);assert.deepEqual(fixed.timers, []);assert.deepEqual(fixed.calls, [-1]);
    assert.equal(fixed.state.HEAP16[fds.indexOf(7) * 4 + 3], 1);
  }
});

test("TTY-only and non-TTY/invalid descriptors preserve poll behavior", () => {
  const r = runtime();assert.equal(r.poll(0), 0);assert.equal(r.poll(20), -1006);assert.equal(r.state.PTY_pollTimeout, 20);
  assert.equal(r.poll(-1), -1006);assert.equal(r.state.PTY_pollTimeout, -1);r.push([69]);assert.equal(r.poll(10, [0, 7]), 2);
  assert.deepEqual(Array.from(r.state.PTY.read(4)), [69]);assert.equal(r.poll(20, [7]), 1);assert.equal(r.poll(20, [99]), runtime("red").poll(20, [99]));
});

test("actual pthread proxies retain poll and five-argument select routing", () => {
  const r = runtime();r.state.ENVIRONMENT_IS_PTHREAD = true;const calls = [];
  r.state.proxyToMainThread = (...args) => { calls.push(args);return 23; };
  assert.equal(r.state.xterm_pty_old_poll(12, 2, 17), 23);assert.deepEqual(calls.at(-1).slice(1), [1, 12, 2, 17]);
  assert.equal(r.state.xterm_pty_old_newselect(3, 16, 24, 32, 40), 23);assert.deepEqual(calls.at(-1).slice(1), [1, 3, 16, 24, 32, 40]);
  assert.equal(r.state.___syscall__newselect(3, 16, 24, 32, 40), 23);assert.deepEqual(calls.at(-1).slice(1), [1, 3, 16, 24, 32, 40]);
});

test("select scans every fd before waiting and preserves fd sets across a wait", () => {
  const old = runtime("red");assert.equal(old.select(10, [0, 7]), -1006);
  const r = runtime();assert.equal(r.select(10, [0, 7]), 1);assert.equal(r.state.HEAP32[16 >> 2], 1 << 7);
  const original = r.state.FS.getStream;
  r.state.FS.getStream = fd => fd === 0 ? original(7) : fd === 7 ? r.stream : undefined;
  assert.equal(r.select(10, [0, 7]), 1);assert.equal(r.state.HEAP32[16 >> 2], 1);
  r.state.FS.getStream = original;
  assert.equal(r.select(0), 0);assert.equal(r.state.HEAP32[16 >> 2], 0);
  assert.equal(r.select(17), -1006);assert.equal(r.state.PTY_pollTimeout, 17);assert.equal(r.state.HEAP32[16 >> 2], 1);
  assert.equal(r.select(null), -1006);assert.equal(r.state.PTY_pollTimeout, -1);assert.equal(r.state.HEAP32[16 >> 2], 1);
});

test("select owns its timeval even without readfds and across the pthread proxy", () => {
  const r = runtime();
  assert.equal(r.select(19, [0], "except"), -1006);assert.equal(r.state.PTY_pollTimeout, 19);assert.equal(r.state.HEAP32[48 >> 2], 1);
  assert.equal(r.state.PTY_selectTimeout(64), 19);assert.equal(r.state.PTY_selectTimeout(0), -1);
  r.state.ENVIRONMENT_IS_PTHREAD = true;
  r.state.proxyToMainThread = () => { r.state.PTY_pollTimeout = 12345;return -1006; };
  let captured;
  r.state.PTY_waitForReadable = (callback, timeout) => { captured = timeout;callback(2); };
  assert.equal(r.state.___syscall__newselect(1, 0, 0, 48, 64), 0);assert.equal(captured, 19);
  assert.equal(r.state.___syscall__newselect(1, 0, 0, 48, 0), 0);assert.equal(captured, -1);
});

test("complete finite select timeout clears every supplied fd set", async () => {
  for (const kind of ["read", "except"]) {
    const r = runtime();assert.equal(r.select(5, [0], kind), -1006);
    const original = r.state.FS.getStream;
    r.state.FS.getStream = fd => fd === 7 ? { stream_ops: { poll: () => 0 } } : original(fd);
    r.state.HEAP32[16 >> 2] = 1;r.state.HEAP32[32 >> 2] = 1 << 7;r.state.HEAP32[48 >> 2] = 1;
    r.state.PTY_handleSleep = startAsync => new Promise(resolve => startAsync(resolve));
    const result = await r.state.___syscall__newselect(8, 16, 32, 48, 64);
    assert.equal(result, 0);
    for (const ptr of [16, 32, 48]) { assert.equal(r.state.HEAP32[ptr >> 2], 0);assert.equal(r.state.HEAP32[(ptr + 4) >> 2], 0); }
    assert.equal(r.subscriptionCount(), 0);assert.deepEqual(r.timers, [5]);
  }
});

test("complete select preserves ready results and interruption semantics", async () => {
  const ready = runtime();assert.equal(ready.select(5, [0, 7]), 1);
  ready.state.HEAP32[16 >> 2] = (1 << 7) | 1;
  ready.state.PTY_handleSleep = startAsync => new Promise(resolve => startAsync(resolve));
  assert.equal(await ready.state.___syscall__newselect(8, 16, 0, 0, 64), 1);assert.equal(ready.state.HEAP32[16 >> 2], 1 << 7);assert.deepEqual(ready.timers, []);
  const interrupted = runtime();assert.equal(interrupted.select(1000), -1006);
  interrupted.state.PTY_handleSleep = startAsync => new Promise(resolve => startAsync(resolve));
  const pending = interrupted.state.___syscall__newselect(1, 16, 0, 0, 64);
  interrupted.state.PTY.signal("SIGINT");assert.equal(await pending, -27);assert.equal(interrupted.state.HEAP32[16 >> 2], 1);assert.equal(interrupted.subscriptionCount(), 0);
});

test("wait proxy carries each caller timeout instead of the shared root timeout", async () => {
  for (const timeout of [0, 6, -1]) {
    const r = runtime();r.state.PTY_pollTimeout = 12345;r.state.HEAP32[8] = -1;r.state.PTY_waitForReadableWithAtomicImpl(8, timeout);
    if (timeout < 0) { assert.equal(r.state.HEAP32[8], -1);r.push([70]);await new Promise(resolve => setTimeout(resolve, 0)); }
    else await new Promise(resolve => setTimeout(resolve, timeout + 2));
    assert.equal(r.state.HEAP32[8], timeout < 0 ? 0 : 2);assert.deepEqual(r.timers, timeout > 0 ? [timeout] : []);
    r.state.ENVIRONMENT_IS_PTHREAD = true;let args;r.state.proxyToMainThread = (...value) => { args = value; };
    r.state.PTY_waitForReadableWithAtomicImpl(8, timeout);assert.deepEqual(args.slice(1), [0, 8, timeout]);
  }
});

test("generated PTY validation is read-only and rejects old emissions", () => {
  assert.throws(() => assertWasmPtySource(sources.get("red")), /pty_source_poll_scan/);
  assert.equal(assertWasmPtySource(sources.get("green")), sources.get("green"));
});
