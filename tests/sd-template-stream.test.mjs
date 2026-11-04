import assert from "node:assert/strict";
import test from "node:test";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { gzipSync } from "node:zlib";
import ts from "typescript";

const source = readFileSync(new URL("../src/browserSimulatorRuntimeWorker.js", import.meta.url), "utf8");
const file = ts.createSourceFile("worker.js", source, ts.ScriptTarget.ESNext, true, ts.ScriptKind.JS);
const functions = file.statements.filter(node => ts.isFunctionDeclaration(node)
  && ["decodeSdImageArtifact", "isGzipArtifact"].includes(node.name?.text));
assert.equal(functions.length, 2);
const decoder = (rawBytes, overrides = {}) => {
  const context = vm.createContext({ Blob, Response, ReadableStream, DecompressionStream, Uint8Array,
    runtimeManifest: { qemuSdRawBytes: rawBytes }, ...overrides });
  vm.runInContext(functions.map(node => node.getText(file)).join("\n"), context);
  return context.decodeSdImageArtifact;
};
const raw = Uint8Array.from({ length: 2048 }, (_, index) => index % 251);
const packed = gzipSync(raw);

test("real gzip bytes retry one transient network error in the Blob stream", async () => {
  let attempts = 0;
  class RetryBlob {
    constructor(parts) { this.parts = parts; }
    stream() {
      attempts++;
      return new ReadableStream({ start(controller) { controller.error(new TypeError("network error")); } });
    }
  }
  const decoded = await decoder(raw.length, { Blob: RetryBlob })("sdcard.img.gz.part00", packed);
  assert.deepEqual(Buffer.from(decoded), Buffer.from(raw));
  assert.equal(attempts, 1);
});

test("gzip integrity, decoded limit and declared byte count remain enforced", async () => {
  await assert.rejects(decoder(raw.length)("seed.gz", packed.subarray(0, packed.length - 4)));
  await assert.rejects(decoder(raw.length - 1)("seed.gz", packed), /browser_sd_card_decoded_size_exceeded/);
  await assert.rejects(decoder(raw.length + 1)("seed.gz", packed), /browser_sd_card_decoded_size_mismatch/);
  await assert.rejects(decoder(raw.length, { DecompressionStream: undefined })("seed.gz", packed), /browser_sd_card_gzip_unsupported/);
});

test("uncompressed SD input retains its original bytes without a Blob or gzip decoder", async () => {
  const decode = decoder(raw.length, { Blob: undefined, DecompressionStream: undefined });
  assert.equal(await decode("seed.img", raw), raw);
});
