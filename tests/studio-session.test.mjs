import test from "node:test";
import assert from "node:assert/strict";
import { createHash, webcrypto } from "node:crypto";
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
import ts from "typescript";

// 这里只验证真实桥接模块的协议与生命周期；模拟回包不是固件执行验收。
function load(path, imports = {}) {
  const source = readFileSync(new URL(`../src/${path}`, import.meta.url), "utf8");
  const code = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } }).outputText;
  const exports = {};
  runInNewContext(code, { exports, require: name => imports[name], crypto: webcrypto, Uint8Array, AbortController, TextEncoder, setTimeout, clearTimeout, Date, Map, Promise });
  return exports;
}
const contract = load("studio/contract.ts");
const bridge = load("posDevBridge.ts", { "./browserSimulatorRuntime": {}, "./simulatorLocation": { createSimulatedLocation: () => ({ name: "London" }) } });
const { StudioSimulatorSession } = load("studio/session.ts", { "../posDevBridge": bridge, "./contract": contract });
const bytes = new TextEncoder().encode("signed archive boundary fixture");
const digest = "sha256:" + createHash("sha256").update(bytes).digest("hex");
const request = { channel: contract.STUDIO_SIMULATOR_CHANNEL, type: "run", runId: "one", boardId: "mofei-480x800", appId: "app.example.studio", version: "1.0.0", packageDigest: digest, bytes };

function fixture(onCommand) {
  const listeners = new Set(), errors = new Set(), calls = [], snapshots = [];
  const emit = result => { for (const handler of listeners) handler(`POS_APP:${result.requestId}:${JSON.stringify({ schemaVersion: 1, ...result })}\n`); };
  const host = {
    async subscribeSerialLog(handler) { listeners.add(handler); return () => listeners.delete(handler); },
    async subscribeSimulatorError(handler) { errors.add(handler); return () => errors.delete(handler); },
    async chooseFirmware(board) { calls.push(["firmware", board]); return "firmware.bin"; },
    async stopSim() { calls.push(["stop"]); return "stopped"; },
    async createSdDirectory(board, path) { calls.push(["mkdir", board, path]); },
    async writeSdFile(board, path, actual) { calls.push(["write", board, path]); assert.deepEqual(actual, bytes); },
    async startSim() { calls.push(["start"]); return "started"; },
    async sendSerialLine(line) {
      calls.push(["serial", line]);
      const [, , requestId, operation] = line.split(":");
      onCommand?.(operation, requestId, emit);
      emit({ requestId, operation, state: "completed", terminal: true, ok: true, appId: request.appId, version: request.version, packageDigest: digest, capabilities: { install: true, open: true } });
      if (operation === "open" && !onCommand) emit({ requestId, operation: "runtime", state: "running", terminal: false, ok: true, appId: request.appId, version: request.version, packageDigest: digest });
    },
  };
  const session = new StudioSimulatorSession(host, (runId, snapshot) => snapshots.push({ runId, ...snapshot }));
  return { session, snapshots, calls, emit, listeners, errors };
}

test("Studio stages exact archive then waits for guest capabilities/install/open", async () => {
  const f = fixture();
  await f.session.run(request);
  assert.equal(f.snapshots.at(-1).status, "running");
  assert.deepEqual(f.calls.map(call => call[0]), ["firmware", "stop", "mkdir", "write", "start", "serial", "serial", "serial"]);
  assert.equal(f.calls.find(call => call[0] === "write")[2], `/PandaDev/${digest.slice(7)}.pap`);
  assert.equal(f.snapshots.at(-1).packageSha256, digest);
  await f.session.dispose();
  assert.equal(f.listeners.size, 0);
  assert.equal(f.errors.size, 0);
});

test("callback failure arriving before open resolves remains failed and retains actual error", async () => {
  const f = fixture((operation, requestId, emit) => {
    if (operation === "open") emit({ requestId, operation: "runtime", state: "failed", terminal: true, ok: false, appId: request.appId, version: request.version, packageDigest: digest, error: "src/main.lua:17: callback failure" });
  });
  await f.session.run(request);
  assert.equal(f.snapshots.at(-1).status, "failed");
  assert.match(f.snapshots.at(-1).diagnostics[0].message, /main.lua:17/);
  await f.session.dispose();
});

test("open acknowledgement alone stays preparing until the same Runtime owner reports running", async () => {
  const f = fixture(() => {});
  await f.session.run(request);
  assert.equal(f.snapshots.at(-1).status, "preparing");
  const command = f.calls.filter(call => call[0] === "serial").at(-1)[1];
  const requestId = command.split(":")[2];
  f.emit({ requestId, operation: "runtime", state: "running", ok: true, appId: request.appId, version: request.version, packageDigest: digest });
  assert.equal(f.snapshots.at(-1).status, "running");
  await f.session.dispose();
});

test("unrelated runtime identity cannot change active package status", async () => {
  const f = fixture();
  await f.session.run(request);
  const command = f.calls.filter(call => call[0] === "serial").at(-1)[1];
  const requestId = command.split(":")[2];
  f.emit({ requestId, operation: "runtime", state: "failed", ok: false, appId: request.appId, version: "2.0.0", packageDigest: digest, error: "wrong app version" });
  assert.equal(f.snapshots.at(-1).status, "running");
  f.emit({ requestId, operation: "runtime", state: "stopped", ok: true, appId: request.appId, version: request.version, packageDigest: digest });
  assert.equal(f.snapshots.at(-1).status, "stopped");
  await f.session.dispose();
});

test("digest mismatch fails before any firmware or SD mutation", async () => {
  const f = fixture();
  await f.session.run({ ...request, packageDigest: "sha256:" + "0".repeat(64) });
  assert.equal(f.snapshots.at(-1).status, "failed");
  assert.match(f.snapshots.at(-1).diagnostics[0].message, /digest_mismatch/);
  assert.deepEqual(f.calls, []);
  await f.session.dispose();
});

test("a terminal install receipt for different bytes cannot qualify running", async () => {
  const f = fixture((operation, requestId, emit) => {
    if (operation === "install") emit({ requestId, operation, state: "completed", terminal: true, ok: true, appId: request.appId, version: request.version, packageDigest: "sha256:" + "0".repeat(64) });
  });
  await f.session.run(request);
  assert.equal(f.snapshots.at(-1).status, "failed");
  assert.match(f.snapshots.at(-1).diagnostics[0].message, /installed_identity_mismatch/);
  assert.equal(f.calls.filter(call => call[0] === "serial").some(call => call[1].includes(":open:")), false);
  await f.session.dispose();
});

test("unsupported X4 is unavailable instead of reusing a Mofei guest", async () => {
  const f = fixture();
  await f.session.run({ ...request, boardId: "x4-pro-480x800" });
  assert.equal(f.snapshots.at(-1).status, "unavailable");
  assert.deepEqual(f.calls, [["stop"]]);
  await f.session.dispose();
});

test("reset releases subscriptions and stops the real host", async () => {
  const f = fixture();
  await f.session.run(request);
  await f.session.stop();
  assert.equal(f.listeners.size, 0);
  assert.equal(f.calls.at(-1)[0], "stop");
  await f.session.dispose();
});

test("frame input rejects non-byte or malformed package identities", () => {
  assert.equal(contract.isStudioRunRequest(request), true);
  assert.equal(contract.isStudioRunRequest({ ...request, bytes: [] }), false);
  assert.equal(contract.isStudioRunRequest({ ...request, appId: "bad:command" }), false);
  assert.equal(contract.isStudioRunRequest({ ...request, channel: "other" }), false);
  assert.equal(contract.isStudioRunRequest({ ...request, bytes: new Uint8Array(2 * 1024 * 1024) }), true);
  assert.equal(contract.isStudioRunRequest({ ...request, bytes: new Uint8Array(2 * 1024 * 1024 + 1) }), false);
  assert.equal(contract.simulatorBoardForStudio("compact-240x416"), "s37uc");
});
