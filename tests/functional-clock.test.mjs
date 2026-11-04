import test from "node:test";
import assert from "node:assert/strict";
import { functionalClockArgs } from "../src/browserQemuPlatform.js";
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
import ts from "typescript";

function loadRuntime(globals = {}) {
  const source = readFileSync(new URL("../src/browserSimulatorRuntime.ts", import.meta.url), "utf8")
    .replaceAll("import.meta.env.BASE_URL", '"/studio-simulator/"')
    .replaceAll("import.meta.url", '"http://localhost/studio-simulator/module.js"');
  const code = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } }).outputText;
  const exports = {};
  runInNewContext(code, { exports, URL, TextEncoder, console, ...globals });
  return exports;
}

function runtimeFixture() {
  const workers = [];
  class Worker {
    messages = [];
    listeners = new Map();
    terminated = false;
    constructor() { workers.push(this); }
    addEventListener(type, listener) {
      if (!this.listeners.has(type)) this.listeners.set(type, new Set());
      this.listeners.get(type).add(listener);
    }
    removeEventListener(type, listener) { this.listeners.get(type)?.delete(listener); }
    postMessage(message) { this.messages.push(message); }
    terminate() { this.terminated = true; }
    emit(type, event) {
      for (const listener of this.listeners.get(type) ?? []) listener(event);
    }
    reply(command, result) {
      this.emit("message", { data: { type: "result", requestId: command.requestId, ...result } });
    }
  }
  const { BrowserSimulatorRuntimeHost } = loadRuntime({
    Worker,
    fetch: async () => ({ ok: true, json: async () => ({ runtime: {
      kind: "wasm-worker", status: "ready", workerScript: "worker.js",
    } }) }),
  });
  const host = new BrowserSimulatorRuntimeHost("http://localhost/studio-simulator/manifest.json");
  return { host, workers, async sent(count, workerIndex = 0) {
    for (let turn = 0; turn < 20; turn++) {
      if (workers[workerIndex]?.messages.length === count) return workers[workerIndex];
      await Promise.resolve();
    }
    assert.fail(`worker ${workerIndex} did not receive ${count} commands`);
  } };
}

async function bounded(promise) {
  let timer;
  try {
    return await Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error("pending runtime command did not settle")), 200);
    })]);
  } finally { clearTimeout(timer); }
}

test("packaged compatibility worker matches the canonical worker including the clock profile", () => {
  assert.equal(
    readFileSync(new URL("../public/simulator-runtime/browser-simulator-worker.js", import.meta.url), "utf8"),
    readFileSync(new URL("../src/browserSimulatorRuntimeWorker.js", import.meta.url), "utf8"),
  );
});

test("Studio declares one fixed instruction-clock profile and keeps ordinary simulator defaults", () => {
  assert.deepEqual(functionalClockArgs(undefined), []);
  assert.deepEqual(functionalClockArgs("studio-functional-icount-v1"), ["-icount", "shift=3,align=off,sleep=on"]);
  assert.throws(() => functionalClockArgs("auto"), /clock_profile_unsupported/);
});

test("runtime manifest parsing and URL normalization preserve the explicit clock profile", () => {
  const exports = loadRuntime();
  const input = { runtime: { kind: "wasm-worker", status: "ready", workerScript: "simulator-runtime/worker.js", clockProfile: "studio-functional-icount-v1" } };
  const parsed = exports.parseBrowserSimulatorRuntimeManifest(input);
  assert.equal(parsed.clockProfile, "studio-functional-icount-v1");
  const resolved = exports.resolveBrowserSimulatorRuntimeManifestUrls(parsed, "http://localhost/studio-simulator/manifest.json");
  assert.equal(resolved.clockProfile, parsed.clockProfile);
  assert.deepEqual(functionalClockArgs(resolved.clockProfile), ["-icount", "shift=3,align=off,sleep=on"]);
  assert.equal(exports.parseBrowserSimulatorRuntimeManifest({ runtime: { ...input.runtime, clockProfile: "auto" } }).kind, "unavailable");
});

test("successful stop rejects every concurrently pending command instead of leaving promises unresolved", async () => {
  const f = runtimeFixture();
  const stopping = f.host.stopSim();
  const mkdir = assert.rejects(f.host.createSdDirectory("mofei", "/PandaDev"), /browser_wasm_worker_stopped/);
  const serial = assert.rejects(f.host.sendSerialLine("CMD:POS_APP:pending:capabilities"), /browser_wasm_worker_stopped/);
  const worker = await f.sent(3);
  assert.deepEqual(worker.messages.map(command => command.type), ["stop", "createSdDirectory", "serialLine"]);
  worker.reply(worker.messages[0], { ok: true, status: "stopped" });
  assert.equal(await stopping, "stopped");
  await bounded(Promise.all([mkdir, serial]));
  assert.equal(worker.terminated, true);
});

test("fatal worker cleanup preserves the actual failure reason for all pending commands", async () => {
  const f = runtimeFixture();
  const reason = /browser_wasm_worker_fatal: actual worker failure/;
  const commands = [
    assert.rejects(f.host.stopSim(), reason),
    assert.rejects(f.host.createSdDirectory("mofei", "/PandaDev"), reason),
    assert.rejects(f.host.sendSerialLine("CMD:POS_APP:pending:capabilities"), reason),
  ];
  const worker = await f.sent(3);
  worker.emit("error", { message: "actual worker failure" });
  await bounded(Promise.all(commands));
  assert.equal(worker.terminated, true);
  assert.equal(worker.listeners.get("message").size, 0);
  assert.equal(worker.listeners.get("error").size, 0);
});

test("awaited stop permits a fresh worker and old worker events cannot complete its commands", async () => {
  const f = runtimeFixture();
  const stopping = f.host.stopSim();
  const old = await f.sent(1);
  old.reply(old.messages[0], { ok: true, status: "stopped" });
  assert.equal(await stopping, "stopped");
  let completed = false;
  const mkdir = f.host.createSdDirectory("s37uc", "/PandaDev").then(() => { completed = true; });
  const worker = await f.sent(1, 1);
  old.reply(worker.messages[0], { ok: true, status: "sd_directory_created" });
  old.emit("error", { message: "late old worker error" });
  await Promise.resolve();
  assert.equal(completed, false);
  assert.equal(worker.terminated, false);
  worker.reply(worker.messages[0], { ok: true, status: "sd_directory_created" });
  await bounded(mkdir);
  assert.equal(completed, true);
});
