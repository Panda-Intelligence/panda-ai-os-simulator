import { createHash } from "node:crypto";
import { existsSync, mkdtempSync, mkdirSync, readFileSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import assert from "node:assert/strict";
import { assembleWebArtifacts } from "../scripts/pack-web-manifest.mjs";

function writeArtifact(root, path, content) {
  const target = join(root, path);
  mkdirSync(join(target, ".."), { recursive: true });
  writeFileSync(target, content);
  const bytes = Buffer.byteLength(content);
  const sha256 = createHash("sha256").update(content).digest("hex");
  return { path, bytes, sha256 };
}

function fixtureManifest(runtime, guest) {
  return {
    schemaVersion: 1,
    source: { revision: "972e16c3e", label: "fixture" },
    publication: { visibility: "private" },
    runtime: {
      workerScript: runtime.worker,
      qemu: {
        script: runtime.script,
        wasm: runtime.wasm,
        worker: runtime.workerQemu,
        bootloader: runtime.bootloader,
        partitionTable: runtime.partitionTable,
        otaData: runtime.otaData,
        rom: runtime.rom,
      },
    },
    guests: [{
      id: "mofei",
      firmware: guest.firmware,
      kernel: guest.kernel,
      symbols: guest.symbols,
      provenance: guest.provenance,
    }],
  };
}

function makeFixture() {
  const root = mkdtempSync(join(tmpdir(), "panda-web-pack-"));
  const runtimeDir = join(root, "runtime");
  const guestDir = join(root, "guest");
  const outputDir = join(root, "output");
  const runtime = {
    worker: writeArtifact(runtimeDir, "browser-simulator-worker.js", "worker"),
    script: writeArtifact(runtimeDir, "qemu-system-xtensa.js", "script"),
    wasm: writeArtifact(runtimeDir, "qemu-system-xtensa.wasm", "wasm"),
    workerQemu: writeArtifact(runtimeDir, "qemu-system-xtensa.worker.js", "qemu-worker"),
    bootloader: writeArtifact(runtimeDir, "bootloader.bin", "boot"),
    partitionTable: writeArtifact(runtimeDir, "partition-table.bin", "partition"),
    otaData: writeArtifact(runtimeDir, "ota_data_initial.bin", "ota"),
    rom: writeArtifact(runtimeDir, "esp32s3_rev0_rom.bin", "rom"),
  };
  const guest = {
    firmware: writeArtifact(guestDir, "mofei/firmware.bin", "firmware"),
    kernel: writeArtifact(guestDir, "mofei/kernel.img", "kernel"),
    symbols: writeArtifact(guestDir, "mofei/symbols.txt", "symbols"),
    provenance: writeArtifact(guestDir, "mofei/provenance.json", "provenance"),
  };
  const manifestPath = join(root, "input-manifest.json");
  writeFileSync(manifestPath, `${JSON.stringify(fixtureManifest(runtime, guest), null, 2)}\n`);
  return { root, runtimeDir, guestDir, outputDir, manifestPath, runtime, guest };
}

test("assembles a real filesystem fixture and emits a private v1 runtime manifest", () => {
  const fixture = makeFixture();
  try {
    assembleWebArtifacts(fixture);
    writeFileSync(join(fixture.outputDir, "previous.txt"), "old output");
    const result = assembleWebArtifacts(fixture);
    const manifest = JSON.parse(readFileSync(result.manifestPath, "utf8"));
    assert.equal(result.artifactCount, 12);
    assert.equal(manifest.schemaVersion, 1);
    assert.equal(manifest.runtime.kind, "wasm-worker");
    assert.equal(manifest.runtime.status, "ready");
    assert.equal(manifest.runtime.workerScript, "simulator-runtime/browser-simulator-worker.js");
    assert.equal(manifest.publication.visibility, "private");
    assert.equal(manifest.publication.localOnly, true);
    assert.equal(readFileSync(join(fixture.outputDir, manifest.runtime.firmwareArtifact), "utf8"), "firmware");
    assert.equal(existsSync(join(fixture.outputDir, "previous.txt")), false);
    assert.match(manifest.runtime.source.artifactManifestSha256, /^[0-9a-f]{64}$/);
  } finally {
    rmSync(fixture.root, { recursive: true, force: true });
  }
});

test("rejects a digest mismatch and preserves the previous output", () => {
  const fixture = makeFixture();
  try {
    mkdirSync(fixture.outputDir, { recursive: true });
    writeFileSync(join(fixture.outputDir, "sentinel.txt"), "keep me");
    const manifest = JSON.parse(readFileSync(fixture.manifestPath, "utf8"));
    manifest.runtime.qemu.wasm.sha256 = "0".repeat(64);
    writeFileSync(fixture.manifestPath, JSON.stringify(manifest));
    assert.throws(
      () => assembleWebArtifacts(fixture),
      /runtime_qemu_wasm_sha256_mismatch/,
    );
    assert.equal(readFileSync(join(fixture.outputDir, "sentinel.txt"), "utf8"), "keep me");
    assert.equal(existsSync(join(fixture.outputDir, "manifest.json")), false);
  } finally {
    rmSync(fixture.root, { recursive: true, force: true });
  }
});

test("does not let a JSON public boolean bypass the private publication gate", () => {
  const fixture = makeFixture();
  try {
    const manifest = JSON.parse(readFileSync(fixture.manifestPath, "utf8"));
    manifest.public = true;
    writeFileSync(fixture.manifestPath, JSON.stringify(manifest));
    assert.throws(() => assembleWebArtifacts(fixture), /manifest_unknown_key:public/);
  } finally {
    rmSync(fixture.root, { recursive: true, force: true });
  }
});

for (const situation of ["symlink-parent", "unowned-output", "busy-output", "overlap-input", "oversized-total", "traversal"]) {
  test(`private pack rejects ${situation} without mutating source data`, () => {
    const f = makeFixture();
    try {
      const manifest = JSON.parse(readFileSync(f.manifestPath, "utf8"));
      let pattern;
      if (situation === "symlink-parent") {
        const outside = join(f.root, "outside"); mkdirSync(outside);
        writeFileSync(join(outside,"escaped.wasm"), "wasm");
        symlinkSync(outside,join(f.runtimeDir,"link"),"dir");
        manifest.runtime.qemu.wasm.path="link/escaped.wasm"; pattern=/symlink_not_allowed/;
      } else if (situation === "unowned-output") {
        mkdirSync(f.outputDir);writeFileSync(join(f.outputDir,"keep.txt"),"keep");pattern=/refuse_unowned_output/;
      } else if (situation === "busy-output") {
        writeFileSync(f.outputDir+".panda-pack-lock","other owner");pattern=/EEXIST/;
      } else if (situation === "overlap-input") {
        f.outputDir=f.runtimeDir;pattern=/output_overlaps_input/;
      } else if (situation === "oversized-total") {
        manifest.runtime.qemu.wasm.bytes=256*1024*1024;
        manifest.runtime.qemu.script.bytes=256*1024*1024;pattern=/total_bytes_exceeded/;
      } else {
        manifest.runtime.qemu.wasm.path="../outside.wasm";pattern=/path_traversal/;
      }
      writeFileSync(f.manifestPath,JSON.stringify(manifest));
      assert.throws(()=>assembleWebArtifacts(f),pattern);
      assert.equal(readFileSync(join(f.runtimeDir,"qemu-system-xtensa.wasm"),"utf8"),"wasm");
      if (situation === "unowned-output") assert.equal(readFileSync(join(f.outputDir,"keep.txt"),"utf8"),"keep");
      if (situation === "busy-output") assert.equal(readFileSync(f.outputDir+".panda-pack-lock","utf8"),"other owner");
    } finally { rmSync(f.root,{recursive:true,force:true}); }
  });
}

test("optional built UI is packaged with the verified runtime and rejects source maps", () => {
  const f=makeFixture();
  try {
    f.uiDir=join(f.root,"dist");mkdirSync(f.uiDir);
    writeFileSync(join(f.uiDir,"index.html"),"<!doctype html><title>Private test</title>");
    assembleWebArtifacts(f);
    assert.match(readFileSync(join(f.outputDir,"index.html"),"utf8"),/Private test/);
    writeFileSync(join(f.uiDir,"code.map"),"source metadata");
    assert.throws(()=>assembleWebArtifacts(f),/unapproved_extension/);
    assert.match(readFileSync(join(f.outputDir,"index.html"),"utf8"),/Private test/);
  } finally {rmSync(f.root,{recursive:true,force:true});}
});
