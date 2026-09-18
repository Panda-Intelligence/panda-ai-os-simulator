import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, unlinkSync, writeFileSync, symlinkSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { spawnSync } from "node:child_process";
import { gzipSync } from "node:zlib";

const ROOT = new URL("..", import.meta.url).pathname.replace(/\/$/, "");
const VALIDATOR = join(ROOT, "scripts", "validate-web-package.mjs");
const BASE_PATH = "/simulator/app/";
const SD_BYTES = 128 * 1024 * 1024;

function digest(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function writeFile(root, relativePath, bytes) {
  const path = join(root, relativePath);
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, bytes);
  return path;
}

function url(relativePath) {
  return `${BASE_PATH}${relativePath}`;
}

function runValidator(fixture, basePath = BASE_PATH, ...extraArgs) {
  return spawnSync(
    process.execPath,
    [VALIDATOR, "--package-root", fixture.packageRoot, "--manifest", fixture.manifestPath, "--base-path", basePath, ...extraArgs],
    { encoding: "utf8", maxBuffer: 1024 * 1024 },
  );
}

function syncManifests(fixture) {
  for (const entry of fixture.entries) {
    const bytes = readFileSync(join(fixture.packageRoot, entry.path));
    entry.bytes = bytes.length;
    entry.sha256 = digest(bytes);
  }
  const artifactText = `${JSON.stringify({
    schemaVersion: 1,
    source: { revision: "fixture", kind: "standalone-web-assembly" },
    publication: { visibility: "private", localOnly: true },
    artifacts: fixture.entries,
  }, null, 2)}\n`;
  writeFile(fixture.packageRoot, "simulator-runtime/artifact-manifest.json", artifactText);
  fixture.runtime.source.artifactManifestSha256 = digest(Buffer.from(artifactText));
  fixture.runtime.digests = Object.fromEntries(fixture.entries.map((entry) => [entry.label, entry.sha256]));
  writeFile(fixture.packageRoot, "manifest.json", `${JSON.stringify(fixture.manifest, null, 2)}\n`);
}

function createFixture() {
  const root = mkdtempSync(join(tmpdir(), "panda-web-validation-"));
  const packageRoot = join(root, "package");
  mkdirSync(packageRoot, { recursive: true });
  const entries = [];
  const addArtifact = (label, path, bytes) => {
    writeFile(packageRoot, path, bytes);
    entries.push({ label, path, bytes: bytes.length, sha256: digest(bytes) });
  };
  addArtifact("runtime_workerScript", "simulator-runtime/browser-simulator-worker.js", Buffer.from("worker fixture"));
  addArtifact("runtime_qemu_script", "simulator-runtime/qemu/qemu-system-xtensa.js", Buffer.from("qemu script fixture"));
  addArtifact("runtime_qemu_wasm", "simulator-runtime/qemu/qemu-system-xtensa.wasm", Buffer.from("wasm fixture"));
  addArtifact("runtime_qemu_worker", "simulator-runtime/qemu/qemu-system-xtensa.worker.js", Buffer.from("worker fixture"));
  addArtifact("runtime_qemu_bootloader", "simulator-runtime/qemu/bootloader.bin", Buffer.from("bootloader fixture"));
  addArtifact("runtime_qemu_partitionTable", "simulator-runtime/qemu/partition-table.bin", Buffer.from("partition fixture"));
  addArtifact("runtime_qemu_otaData", "simulator-runtime/qemu/ota_data_initial.bin", Buffer.from("ota fixture"));
  addArtifact("runtime_qemu_rom", "simulator-runtime/qemu/esp32s3_rev0_rom.bin", Buffer.from("rom fixture"));
  addArtifact("guest_mofei_firmware", "simulator-runtime/qemu/firmware-mofei.bin", Buffer.from("firmware fixture"));
  addArtifact("guest_mofei_kernel", "simulator-runtime/qemu/kernel-mofei.img", Buffer.from("kernel fixture"));
  addArtifact("guest_mofei_symbols", "simulator-runtime/qemu/symbols-mofei.txt", Buffer.from("symbols fixture"));

  const compressed = gzipSync(Buffer.alloc(SD_BYTES, 0x5a), { level: 9, mtime: 0 });
  const partSize = Math.ceil(compressed.length / 3);
  const sdPaths = [];
  for (let index = 0; index < 3; index += 1) {
    const path = `simulator-runtime/qemu/sdcard.img.gz.part${String(index).padStart(2, "0")}`;
    sdPaths.push(path);
    addArtifact(`runtime_qemu_sdImage_${index}`, path, compressed.subarray(index * partSize, (index + 1) * partSize));
  }

  const runtime = {
    kind: "wasm-worker",
    status: "ready",
    workerScript: url("simulator-runtime/browser-simulator-worker.js"),
    firmwareArtifact: url("simulator-runtime/qemu/firmware-mofei.bin"),
    firmwareArtifacts: {
      mofei: {
        bin: url("simulator-runtime/qemu/firmware-mofei.bin"),
        kernel: url("simulator-runtime/qemu/kernel-mofei.img"),
        symbols: url("simulator-runtime/qemu/symbols-mofei.txt"),
      },
    },
    qemuScript: url("simulator-runtime/qemu/qemu-system-xtensa.js"),
    qemuWasm: url("simulator-runtime/qemu/qemu-system-xtensa.wasm"),
    qemuKernel: url("simulator-runtime/qemu/kernel-mofei.img"),
    qemuSymbols: url("simulator-runtime/qemu/symbols-mofei.txt"),
    qemuBootloader: url("simulator-runtime/qemu/bootloader.bin"),
    qemuPartitionTable: url("simulator-runtime/qemu/partition-table.bin"),
    qemuOtaData: url("simulator-runtime/qemu/ota_data_initial.bin"),
    qemuRom: url("simulator-runtime/qemu/esp32s3_rev0_rom.bin"),
    qemuSdImage: sdPaths.map(url),
    qemuSdRawBytes: SD_BYTES,
    qemuWorkerScript: url("simulator-runtime/qemu/qemu-system-xtensa.worker.js"),
    artifactManifest: url("simulator-runtime/artifact-manifest.json"),
    source: { kind: "standalone-web-assembly", revision: "fixture", label: "fixture" },
    digests: {},
  };
  const fixture = {
    root,
    packageRoot,
    manifestPath: join(packageRoot, "manifest.json"),
    entries,
    runtime,
    manifest: { schemaVersion: 1, runtime, publication: { visibility: "private", localOnly: true } },
  };
  writeFile(packageRoot, "index.html", "<!doctype html><title>fixture package</title>");
  syncManifests(fixture);
  return fixture;
}

function withFixture(callback) {
  const fixture = createFixture();
  try {
    callback(fixture);
  } finally {
    rmSync(fixture.root, { recursive: true, force: true });
  }
}

test("accepts a valid packaged output with contiguous split SD delivery", () => {
  withFixture((fixture) => {
    const result = runValidator(fixture);
    assert.equal(result.status, 0, result.stderr);
    assert.match(result.stdout, /valid static package/);
  });
});

test("accepts the same static package at the root base path", () => {
  withFixture((fixture) => {
    const rewrite = (value) => {
      if (typeof value === "string") return value.startsWith(BASE_PATH) ? `/${value.slice(BASE_PATH.length)}` : value;
      if (Array.isArray(value)) return value.map(rewrite);
      if (value && typeof value === "object") return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, rewrite(item)]));
      return value;
    };
    fixture.manifest.runtime = rewrite(fixture.manifest.runtime);
    writeFile(fixture.packageRoot, "manifest.json", `${JSON.stringify(fixture.manifest, null, 2)}\n`);
    const result = runValidator(fixture, "/");
    assert.equal(result.status, 0, result.stderr);
  });
});

test("rejects a missing public runtime manifest", () => {
  withFixture((fixture) => {
    unlinkSync(fixture.manifestPath);
    const result = runValidator(fixture);
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /runtime_manifest_missing|runtime_manifest/);
  });
});

test("rejects a declared artifact with a bad digest", () => {
  withFixture((fixture) => {
    fixture.entries[0].sha256 = "0".repeat(64);
    const artifactText = `${JSON.stringify({ schemaVersion: 1, artifacts: fixture.entries }, null, 2)}\n`;
    writeFile(fixture.packageRoot, "simulator-runtime/artifact-manifest.json", artifactText);
    fixture.runtime.source.artifactManifestSha256 = digest(Buffer.from(artifactText));
    fixture.runtime.digests[fixture.entries[0].label] = fixture.entries[0].sha256;
    writeFile(fixture.packageRoot, "manifest.json", `${JSON.stringify(fixture.manifest, null, 2)}\n`);
    const result = runValidator(fixture);
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /digest_mismatch/);
  });
});

for (const extension of [".map", ".elf"]) {
  test(`rejects an unadvertised ${extension} debug artifact`, () => {
    withFixture((fixture) => {
      writeFile(fixture.packageRoot, `debug${extension}`, Buffer.from("debug fixture"));
      const result = runValidator(fixture);
      assert.notEqual(result.status, 0);
      assert.match(result.stderr, /debug_or_source_map_not_deployable/);
    });
  });
}

test("rejects a symlink anywhere in the static package", () => {
  withFixture((fixture) => {
    const outside = writeFile(fixture.root, "outside.txt", Buffer.from("outside"));
    symlinkSync(outside, join(fixture.packageRoot, "linked.txt"));
    const result = runValidator(fixture);
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /symlink_not_allowed/);
  });
});

test("rejects corrupted gzip bytes after refreshing declared hashes", () => {
  withFixture((fixture) => {
    const path = join(fixture.packageRoot, fixture.entries.at(-1).path);
    const bytes = readFileSync(path);
    bytes[bytes.length - 1] ^= 0xff;
    writeFileSync(path, bytes);
    syncManifests(fixture);
    const result = runValidator(fixture);
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /sd_gzip_invalid|decoded_size_invalid/);
  });
});

test("rejects a non-contiguous split sequence", () => {
  withFixture((fixture) => {
    const removed = fixture.entries.find((entry) => entry.label === "runtime_qemu_sdImage_1");
    fixture.entries.splice(fixture.entries.indexOf(removed), 1);
    fixture.runtime.qemuSdImage = [url(removed.path.replace("part01", "part00")), url(removed.path.replace("part01", "part02"))];
    unlinkSync(join(fixture.packageRoot, removed.path));
    syncManifests(fixture);
    const result = runValidator(fixture);
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /parts_not_contiguous|not_declared/);
  });
});

test("rejects an advertised path outside the explicit base path", () => {
  withFixture((fixture) => {
    fixture.runtime.workerScript = "/other/app/simulator-runtime/browser-simulator-worker.js";
    writeFile(fixture.packageRoot, "manifest.json", `${JSON.stringify(fixture.manifest, null, 2)}\n`);
    const result = runValidator(fixture);
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /base_path_mismatch/);
  });
});
