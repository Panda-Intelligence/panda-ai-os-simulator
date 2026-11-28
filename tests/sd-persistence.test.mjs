import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import vm from "node:vm";
import ts from "typescript";

const source = readFileSync(new URL("../src/browserSimulatorRuntimeWorker.js", import.meta.url), "utf8");
const file = ts.createSourceFile("worker.js", source, ts.ScriptTarget.ESNext, true, ts.ScriptKind.JS);
const names = new Set([
  "startSdCardPersistence", "stopSdCardPersistence", "persistActiveSdCard", "flushActiveSdCard",
  "stopRuntime", "sameBytes",
]);
const functions = file.statements.filter(node => ts.isFunctionDeclaration(node) && names.has(node.name?.text));
assert.equal(functions.length, names.size);
const stateNames = new Set(["sdCardPersistTimer", "sdCardPersistPromise", "sdCardPersistRequested", "sdCardPersistReason"]);
const states = file.statements.filter(node => ts.isVariableStatement(node)
  && node.declarationList.declarations.some(declaration => stateNames.has(declaration.name.getText(file))));
assert.equal(states.length, stateNames.size);
const workerCode = [...states, ...functions].map(node => node.getText(file)).join("\n");

function harness(write = async () => {}) {
  const reads = [];
  const writes = [];
  const saved = [];
  const errors = [];
  const results = [];
  const timers = new Map();
  let image = Uint8Array.of(1, 2, 3, 4);
  let nextTimer = 0;
  const runtime = { FS: { readFile() {
    const bytes = image.slice();
    reads.push(bytes);
    return bytes;
  } } };
  const context = vm.createContext({
    Uint8Array, Uint32Array, Promise,
    SD_CARD_PERSIST_INTERVAL_MS: 15000,
    activeSdCard: { path: "/sd.img", storageKey: "board", templateFingerprint: "fixture" },
    editableSdCard: null,
    qemuInstance: runtime,
    qemuIpcOwner: { instance: runtime },
    qemuModulePromise: Promise.resolve(runtime),
    running: true,
    setInterval(callback) {
      const id = ++nextTimer;
      timers.set(id, callback);
      return id;
    },
    clearInterval(id) { timers.delete(id); },
    async writeStoredSdCard(key, value) {
      assert.equal(key, "board");
      writes.push(value.bytes);
      await write(value, writes.length);
      saved.push(value.bytes.slice());
    },
    describeStorageError: error => error.message,
    emitError: error => errors.push(error),
    log() {},
    result: (...value) => results.push(value),
  });
  vm.runInContext(workerCode, context);
  return {
    context, reads, writes, saved, errors, results, timers, runtime,
    change(value) { image = Uint8Array.of(value, 2, 3, 4); },
  };
}

test("slow persistence coalesces repeated interval requests before copying another SD image", async () => {
  const entered = Promise.withResolvers();
  const release = Promise.withResolvers();
  const h = harness(async (_value, index) => {
    if (index === 1) {
      entered.resolve();
      await release.promise;
    }
  });
  const first = h.context.persistActiveSdCard("interval");
  await entered.promise;
  h.change(9);
  for (let index = 0; index < 1000; index++) {
    assert.equal(h.context.persistActiveSdCard("interval"), first);
  }
  assert.equal(h.reads.length, 1);
  assert.equal(h.writes.length, 1);
  release.resolve();
  await first;
  assert.equal(h.reads.length, 2);
  assert.equal(h.writes.length, 2);
  assert.equal(h.saved.at(-1)[0], 9);
  assert.equal(h.context.editableSdCard.bytes[0], 9);
});

test("changes arriving during the coalesced write receive one fresh final snapshot", async () => {
  const entered = [Promise.withResolvers(), Promise.withResolvers()];
  const release = [Promise.withResolvers(), Promise.withResolvers()];
  const h = harness(async (_value, index) => {
    if (index <= 2) {
      entered[index - 1].resolve();
      await release[index - 1].promise;
    }
  });
  const pending = h.context.persistActiveSdCard("interval");
  await entered[0].promise;
  h.change(5);
  h.context.persistActiveSdCard("interval");
  release[0].resolve();
  await entered[1].promise;
  h.change(8);
  h.context.persistActiveSdCard("interval");
  h.context.persistActiveSdCard("interval");
  assert.equal(h.reads.length, 2);
  release[1].resolve();
  await pending;
  assert.deepEqual(h.saved.map(bytes => bytes[0]), [1, 5, 8]);
  assert.equal(h.reads.length, 3);
});

test("an unchanged coalesced snapshot does not rewrite durable SD bytes", async () => {
  const entered = Promise.withResolvers();
  const release = Promise.withResolvers();
  const h = harness(async () => {
    entered.resolve();
    await release.promise;
  });
  const pending = h.context.persistActiveSdCard("interval");
  await entered.promise;
  h.context.persistActiveSdCard("interval");
  release.resolve();
  await pending;
  assert.equal(h.reads.length, 2);
  assert.equal(h.writes.length, 1);
  await h.context.persistActiveSdCard("interval");
  assert.equal(h.writes.length, 1);
});

test("failed persistence rejects its waiters without publishing stale bytes and remains retryable", async () => {
  const entered = Promise.withResolvers();
  const release = Promise.withResolvers();
  const h = harness(async (_value, index) => {
    if (index === 1) {
      entered.resolve();
      await release.promise;
      throw new Error("storage unavailable");
    }
  });
  const first = h.context.persistActiveSdCard("interval");
  const rejected = assert.rejects(first, /storage unavailable/);
  await entered.promise;
  h.change(7);
  assert.equal(h.context.persistActiveSdCard("interval"), first);
  release.resolve();
  await rejected;
  assert.equal(h.context.editableSdCard, null);
  assert.equal(h.saved.length, 0);
  assert.equal(h.errors.length, 1);
  const retry = h.context.persistActiveSdCard("interval");
  assert.notEqual(retry, first);
  await retry;
  assert.equal(h.saved.at(-1)[0], 7);
});

test("stop disables periodic capture and waits for the latest queued SD snapshot before releasing QEMU", async () => {
  const entered = [Promise.withResolvers(), Promise.withResolvers()];
  const release = [Promise.withResolvers(), Promise.withResolvers()];
  const h = harness(async (_value, index) => {
    entered[index - 1].resolve();
    await release[index - 1].promise;
  });
  h.context.startSdCardPersistence();
  const interval = h.context.persistActiveSdCard("interval");
  await entered[0].promise;
  h.change(6);
  const stopped = h.context.stopRuntime(42, true);
  assert.equal(h.timers.size, 0);
  assert.equal(h.context.qemuInstance, h.runtime);
  assert.equal(h.results.length, 0);
  assert.equal(h.reads.length, 1);
  release[0].resolve();
  await entered[1].promise;
  assert.equal(h.context.qemuInstance, h.runtime);
  assert.equal(h.results.length, 0);
  release[1].resolve();
  await Promise.all([interval, stopped]);
  assert.equal(h.saved.at(-1)[0], 6);
  assert.equal(h.context.qemuInstance, null);
  assert.equal(h.context.running, false);
  assert.deepEqual(h.results, [[42, true, "stopped"]]);
});

test("a failed stop preserves the live runtime and resumes periodic persistence for retry", async () => {
  let fail = true;
  const h = harness(async () => {
    if (fail) throw new Error("disk full");
  });
  h.context.startSdCardPersistence();
  await assert.rejects(h.context.stopRuntime(43, true), /disk full/);
  assert.equal(h.context.qemuInstance, h.runtime);
  assert.equal(h.context.qemuIpcOwner.instance, h.runtime);
  assert.equal(h.context.running, true);
  assert.equal(h.timers.size, 1);
  assert.equal(h.results.length, 0);
  assert.equal(h.context.editableSdCard, null);
  fail = false;
  h.change(4);
  await h.context.stopRuntime(44, true);
  assert.equal(h.saved.at(-1)[0], 4);
  assert.equal(h.timers.size, 0);
  assert.equal(h.context.qemuInstance, null);
  assert.deepEqual(h.results, [[44, true, "stopped"]]);
});
