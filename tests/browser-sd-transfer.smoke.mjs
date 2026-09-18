// 使用真实 Chromium worker 和 IndexedDB 验证传输契约，不启动 QEMU。
import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { pathToFileURL } from "node:url";

for (const name of ["PLAYWRIGHT_MODULE", "SIMULATOR_TEST_URL", "SIMULATOR_TEST_OUT", "CHROME_EXECUTABLE"]) {
  if (!process.env[name]) {
    throw new Error(`Explicit ${name} is required`);
  }
}

const playwrightModulePath = /\.m?js$/.test(process.env.PLAYWRIGHT_MODULE)
  ? process.env.PLAYWRIGHT_MODULE
  : join(process.env.PLAYWRIGHT_MODULE, "index.js");
const playwright = await import(pathToFileURL(playwrightModulePath).href);
const chromium = playwright.chromium ?? playwright.default?.chromium;
if (!chromium) throw new Error("PLAYWRIGHT_MODULE does not expose chromium");
const origin = new URL(process.env.SIMULATOR_TEST_URL);
assert.ok(["127.0.0.1", "localhost"].includes(origin.hostname));

function makeFat12Image(totalSectors = 300, marker = 0) {
  const bytes = new Uint8Array(totalSectors * 512);
  const view = new DataView(bytes.buffer);
  bytes.set([0xeb, 0x3c, 0x90], 0);
  view.setUint16(11, 512, true);
  bytes[13] = 1;
  view.setUint16(14, 1, true);
  bytes[16] = 1;
  view.setUint16(17, 16, true);
  view.setUint16(19, totalSectors, true);
  bytes[21] = 0xf8;
  view.setUint16(22, 1, true);
  bytes[510] = 0x55;
  bytes[511] = 0xaa;
  bytes.set([0xf8, 0xff, 0xff], 512);
  bytes[64] = marker;
  return bytes;
}

const seed = makeFat12Image(300, 17);
const imported = makeFat12Image(300, 101);
const seedUrl = `data:application/octet-stream;base64,${Buffer.from(seed).toString("base64")}`;
const importedUrl = `data:application/octet-stream;base64,${Buffer.from(imported).toString("base64")}`;
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROME_EXECUTABLE });
const context = await browser.newContext({ serviceWorkers: "block" });
const blocked = [];
await context.route("**/*", async (route) => {
  const url = new URL(route.request().url());
  if (["http:", "https:"].includes(url.protocol) && url.origin !== origin.origin) {
    blocked.push(url.origin);
    await route.abort();
    return;
  }
  await route.continue();
});

try {
const page = await context.newPage();
await page.goto(origin.href, { waitUntil: "domcontentloaded" });
const report = await page.evaluate(async ({ workerUrl, firstSeedUrl, secondSeedUrl, byteLength }) => {
  const ensure = (condition, message) => {
    if (!condition) throw new Error(message);
  };
  const boardId = `browser-transfer-test-${crypto.randomUUID()}`;
  const manifest = (qemuSdImage) => ({
    kind: "wasm-worker",
    qemuSdImage,
    qemuSdRawBytes: byteLength,
  });
  const worker = new Worker(workerUrl, { type: "module" });
  const pending = new Map();
  const ready = new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error("worker_ready_timeout")), 10000);
    worker.addEventListener("message", (event) => {
      if (event.data?.type === "ready") {
        clearTimeout(timeout);
        resolve();
      }
    }, { once: true });
  });
  worker.addEventListener("message", (event) => {
    const result = event.data;
    if (result?.type !== "result") return;
    const resolve = pending.get(result.requestId);
    if (!resolve) return;
    pending.delete(result.requestId);
    resolve(result);
  });
  let requestId = 1;
  const command = (type, extra, transfer = []) => new Promise((resolve) => {
    const id = requestId++;
    pending.set(id, resolve);
    worker.postMessage({ type, requestId: id, boardId, manifest: manifest(firstSeedUrl), ...extra }, transfer);
  });
  const exportImage = async (qemuSdImage = firstSeedUrl) => {
    const id = requestId++;
    const result = await new Promise((resolve) => {
      pending.set(id, resolve);
      worker.postMessage({ type: "exportSdImage", requestId: id, boardId, manifest: manifest(qemuSdImage) });
    });
    ensure(result.ok, result.error);
    return result;
  };

  await ready;
  const first = await exportImage();
  const firstBytes = new Uint8Array(first.data.bytes);
  const invalid = new Uint8Array(byteLength);
  const invalidResult = await command("importSdImage", { bytes: invalid }, [invalid.buffer]);
  ensure(!invalidResult.ok, "invalid import should fail");
  const afterInvalid = await exportImage();
  ensure(new Uint8Array(afterInvalid.data.bytes)[64] === firstBytes[64], "invalid import replaced image");
  const wrongSize = new Uint8Array(byteLength - 512);
  const wrongSizeResult = await command("importSdImage", { bytes: wrongSize }, [wrongSize.buffer]);
  ensure(!wrongSizeResult.ok, "wrong-size import should fail");
  const afterWrongSize = await exportImage();
  ensure(new Uint8Array(afterWrongSize.data.bytes)[64] === firstBytes[64], "wrong-size import replaced image");

  const importedBytes = new Uint8Array(byteLength);
  importedBytes.set(firstBytes);
  importedBytes[64] = 93;
  const importedResult = await command("importSdImage", { bytes: importedBytes }, [importedBytes.buffer]);
  ensure(importedResult.ok, importedResult.error || "valid import should succeed");
  const afterImport = await exportImage();
  ensure(new Uint8Array(afterImport.data.bytes)[64] === 93, "valid import did not replace image");

  const concurrentA = new Uint8Array(afterImport.data.bytes).slice();
  concurrentA[64] = 41;
  const concurrentB = new Uint8Array(afterImport.data.bytes).slice();
  concurrentB[64] = 42;
  const concurrentResults = await Promise.all([
    command("importSdImage", { bytes: concurrentA }, [concurrentA.buffer]),
    command("importSdImage", { bytes: concurrentB }, [concurrentB.buffer]),
  ]);
  ensure(concurrentResults.every((entry) => entry.ok), "concurrent imports were not serialized");
  const afterConcurrent = await exportImage();
  ensure([41, 42].includes(new Uint8Array(afterConcurrent.data.bytes)[64]), "concurrent imports produced a mixed image");

  const conflict = await exportImage(secondSeedUrl);
  ensure(conflict.data.templateConflict === true, "template conflict was hidden");
  ensure(new Uint8Array(conflict.data.bytes)[64] === new Uint8Array(afterConcurrent.data.bytes)[64], "template conflict returned seed bytes");
  worker.terminate();
  return {
    passed: true,
    rawBytes: byteLength,
    invalidImportRetained: true,
    wrongSizeImportRetained: true,
    manualImportChangedImage: true,
    concurrentImportsSerialized: true,
    templateConflictExportable: true,
  };
}, {
  workerUrl: new URL("simulator-runtime/browser-simulator-worker.js", origin).href,
  firstSeedUrl: seedUrl,
  secondSeedUrl: importedUrl,
  byteLength: seed.byteLength,
});

report.blockedExternalOrigins = [...new Set(blocked)];
assert.deepEqual(report.blockedExternalOrigins, []);
await mkdir(dirname(process.env.SIMULATOR_TEST_OUT), { recursive: true });
await writeFile(process.env.SIMULATOR_TEST_OUT, `${JSON.stringify(report, null, 2)}\n`);
console.log(JSON.stringify(report));
} finally {
  await context.close();
  await browser.close();
}
