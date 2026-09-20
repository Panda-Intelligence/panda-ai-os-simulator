import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { runInNewContext } from "node:vm";
import ts from "typescript";
import createFatFs, * as FatFs from "js-fatfs";

const root = new URL("../", import.meta.url);
const source = readFileSync(new URL("src/simulatorLocation.ts", root), "utf8");
const javascript = ts.transpileModule(source, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ES2022 },
}).outputText;
const { createSimulatedLocation, SIMULATED_LONDON_LOCATION, prepareSimulatedLocationImage } =
  await import("data:text/javascript;base64," + Buffer.from(javascript).toString("base64"));
const london = { name: "London", timezone: "Europe%2FLondon", latitude: 51.5074, longitude: -0.1278 };

test("fixed London preset is independent of globals and has independent request snapshots", () => {
  assert.deepEqual(createSimulatedLocation(), london);
  assert.equal(Object.isFrozen(SIMULATED_LONDON_LOCATION), true);
  const first = createSimulatedLocation(); first.name = "changed";
  assert.deepEqual(createSimulatedLocation(), london);
  assert.equal(decodeURIComponent(createSimulatedLocation().timezone), "Europe/London");
});

test("all three interactive launch paths use the preset without geolocation or timezone probes", () => {
  const pane = readFileSync(new URL("src/components/SimulatorDevicePane.tsx", root), "utf8");
  assert.equal((pane.match(/const hostLocation = createSimulatedLocation\(\)/g) ?? []).length, 3);
  assert.doesNotMatch(pane, /navigator\.(?:geolocation|permissions)|getCurrentPosition|watchPosition|resolvedOptions\(\)\.timeZone|resolveHostLocation/);
  assert.match(pane, /data-simulated-location="London"/);
});

test("native and headless no-input defaults agree with the London UI preset", () => {
  for (const path of ["src-tauri/src/lib.rs", "headless/src/main.rs"]) {
    const rust = readFileSync(new URL(path, root), "utf8");
    const defaults = rust.slice(rust.indexOf("impl Default for SimulatorLocation"));
    assert.match(defaults.slice(0, 400), /name: "London"\.to_string\(\)/);
    assert.match(defaults.slice(0, 400), /timezone: "Europe%2FLondon"\.to_string\(\)/);
    assert.match(defaults.slice(0, 400), /latitude: 51\.5074/);
    assert.match(defaults.slice(0, 400), /longitude: -0\.1278/);
    assert.doesNotMatch(rust, /"Taipei"|"Asia%2FTaipei"/);
  }
});

test("worker prepares the mock location before mounting the guest SD", () => {
  const worker = readFileSync(new URL("src/browserSimulatorRuntimeWorker.js", root), "utf8");
  const prepare = worker.indexOf("await prepareSimulatedLocationImage(sdImage, withFatFileSystem, writeFatFile)");
  const mount = worker.indexOf("qemuInstance = await loadQemuModule");
  assert.ok(prepare > 0 && prepare < mount);
});

test("no-SD boot does not invent a card or invoke the editor", async () => {
  assert.equal(await prepareSimulatedLocationImage(null, () => assert.fail("no SD"), () => {}), false);
});

test("template conflicts preserve the original image without editing", async () => {
  const card = { bytes: new Uint8Array([1,2,3]), templateConflict:true };
  const before = card.bytes;
  await assert.rejects(prepareSimulatedLocationImage(card, () => assert.fail("conflicted SD"), () => {}), /template_conflict/);
  assert.equal(card.bytes, before);
});

test("failure on the second alias leaves the cached image byte-for-byte unchanged", async () => {
  const card = { bytes:new Uint8Array([1,2,3]) }, before=card.bytes;
  let writes=0;
  await assert.rejects(prepareSimulatedLocationImage(card, async (image, action) => action(image), (image) => {
    image[0]=9;
    if (++writes === 2) throw new Error("simulated disk full");
  }), /disk full/);
  assert.equal(writes, 2);
  assert.equal(card.bytes, before);
  assert.deepEqual([...card.bytes], [1,2,3]);
});

function makeFat12Image() {
  const bytes = new Uint8Array(300 * 512), view = new DataView(bytes.buffer);
  bytes.set([0xeb, 0x3c, 0x90]); view.setUint16(11,512,true); bytes[13]=1;
  view.setUint16(14,1,true); bytes[16]=1; view.setUint16(17,16,true);
  view.setUint16(19,300,true); bytes[21]=0xf8; view.setUint16(22,1,true);
  bytes[510]=0x55; bytes[511]=0xaa; bytes.set([0xf8,0xff,0xff],512);
  return bytes;
}

// Execute the actual worker's FAT editor with the installed WASM library.
// Message dispatch and QEMU are inert; no fake guest boot/framebuffer is used.
const workerSource = readFileSync(new URL("src/browserSimulatorRuntimeWorker.js",root),"utf8").replace(/^import[^\n]+\n/gm, "");
const fat = runInNewContext(workerSource + "\n;({ withFatFileSystem, writeFatFile, readFatFile })", {
  createFatFs, FatFs, fatFsWasmUrl:fileURLToPath(new URL("node_modules/js-fatfs/dist/fatfs.wasm",root)),
  Uint8Array, ArrayBuffer, DataView, TextEncoder, TextDecoder, Date, console,
  postMessage() {}, addEventListener() {}, prepareSimulatedLocationImage,
});

test("actual FAT edit installs London aliases without changing user files or original image", async () => {
  const original = makeFat12Image(), userBytes = new TextEncoder().encode("My original test book\n");
  const paths = ["/.mofei/simulator_location.json", "/mofei/simulator_location.json"];
  await fat.withFatFileSystem(original, ff => {
    fat.writeFatFile(ff,"/Books/keep.txt",userBytes);
    for (const path of paths) fat.writeFatFile(ff,path,new TextEncoder().encode('{"name":"Taipei"}'));
  });
  const originalSnapshot=original.slice(), card={bytes:original};
  assert.equal(await prepareSimulatedLocationImage(card,fat.withFatFileSystem,fat.writeFatFile),true);
  assert.notEqual(card.bytes, original);
  assert.deepEqual(original,originalSnapshot);
  await fat.withFatFileSystem(card.bytes, ff => {
    assert.deepEqual(fat.readFatFile(ff,"/Books/keep.txt"), userBytes);
    for (const path of paths) assert.deepEqual(JSON.parse(new TextDecoder().decode(fat.readFatFile(ff,path))),london);
  });
});
