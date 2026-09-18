import { copyBuiltUi } from "./pack-built-ui.mjs";
import { readVerifiedArtifact } from "./pack-safe-io.mjs";
import { createHash } from "node:crypto";
import { closeSync, openSync, lstatSync, mkdirSync, readFileSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { basename, dirname, join, normalize, relative, resolve, sep } from "node:path";

export const MAX_ARTIFACT_BYTES = 256 * 1024 * 1024;
export const MAX_MANIFEST_BYTES = 4 * 1024 * 1024;

const PRIVATE_PATH_PARTS = new Set([
  ".git",
  ".qemu-cache",
  ".qemu-wasm-cache",
  "node_modules",
  "murphy",
  "panda-cloud",
  "shared",
  "secrets",
  "users",
  "home",
  "private",
]);

const ALLOWED_ARTIFACT_KEYS = new Set(["path", "bytes", "sha256"]);

function fail(message) {
  throw new Error(`web_artifact_manifest_invalid:${message}`);
}

function assertObject(value, label) {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    fail(`${label}_must_be_object`);
  }
}

function assertKeys(value, allowed, label) {
  for (const key of Object.keys(value)) {
    if (!allowed.has(key)) {
      fail(`${label}_unknown_key:${key}`);
    }
  }
}

function assertSafeRelativePath(value, label) {
  if (typeof value !== "string" || value.length === 0 || value.length > 240) {
    fail(`${label}_path_invalid`);
  }
  if (value.includes("\0") || value.includes("\\") || value.startsWith("/") || /^[A-Za-z]:/.test(value)) {
    fail(`${label}_path_not_relative`);
  }
  if (/^[A-Za-z][A-Za-z0-9+.-]*:/.test(value) || value.startsWith("//")) {
    fail(`${label}_path_url_not_allowed`);
  }
  const parts = value.split("/");
  if (parts.some((part) => part === "" || part === "." || part === "..")) {
    fail(`${label}_path_traversal`);
  }
  if (parts.some((part) => PRIVATE_PATH_PARTS.has(part.toLowerCase()))) {
    fail(`${label}_private_path`);
  }
  if (normalize(value).split(sep).join("/") !== value) {
    fail(`${label}_path_not_normalized`);
  }
  return value;
}

function assertArtifactShape(value, label) {
  assertObject(value, label);
  assertKeys(value, ALLOWED_ARTIFACT_KEYS, label);
  assertSafeRelativePath(value.path, label);
  if (!Number.isSafeInteger(value.bytes) || value.bytes < 1 || value.bytes > MAX_ARTIFACT_BYTES) {
    fail(`${label}_byte_length_invalid`);
  }
  if (typeof value.sha256 !== "string" || !/^[0-9a-f]{64}$/.test(value.sha256)) {
    fail(`${label}_sha256_invalid`);
  }
}

function assertSource(source) {
  if (source === undefined) return;
  assertObject(source, "source");
  assertKeys(source, new Set(["revision", "label"]), "source");
  if (source.revision !== undefined && (typeof source.revision !== "string" || !/^[0-9a-fA-F]{7,64}$/.test(source.revision))) {
    fail("source_revision_invalid");
  }
  if (source.label !== undefined && (typeof source.label !== "string" || source.label.length < 1 || source.label.length > 100 || source.label.includes("://"))) {
    fail("source_label_invalid");
  }
}

export function parseWebArtifactManifest(payload) {
  assertObject(payload, "manifest");
  assertKeys(payload, new Set(["schemaVersion", "basePath", "source", "publication", "runtime", "guests"]), "manifest");
  if (payload.schemaVersion !== 1) fail("schema_version_unsupported");
  assertSource(payload.source);
  if (payload.basePath !== undefined && (typeof payload.basePath !== "string" || !/^\/(?:[a-zA-Z0-9_-]+\/)*$/.test(payload.basePath))) fail("base_path_invalid");
  if (payload.publication !== undefined) {
    assertObject(payload.publication, "publication");
    assertKeys(payload.publication, new Set(["visibility"]), "publication");
    if (payload.publication.visibility !== "private") fail("public_publish_gate_requires_private_pack");
  }
  assertObject(payload.runtime, "runtime");
  assertKeys(payload.runtime, new Set(["workerScript", "sdStoreScript", "qemu"]), "runtime");
  assertArtifactShape(payload.runtime.workerScript, "runtime_workerScript");
  if(payload.runtime.sdStoreScript)assertArtifactShape(payload.runtime.sdStoreScript,"runtime_sdStoreScript");
  assertObject(payload.runtime.qemu, "runtime_qemu");
  assertKeys(payload.runtime.qemu, new Set(["script", "wasm", "worker", "data", "bootloader", "partitionTable", "otaData", "rom", "sdImage", "sdRawBytes"]), "runtime_qemu");
  for (const role of ["script", "wasm", "worker", "bootloader", "partitionTable", "otaData", "rom"]) {
    assertArtifactShape(payload.runtime.qemu[role], `runtime_qemu_${role}`);
  }
  if (payload.runtime.qemu.data !== undefined) assertArtifactShape(payload.runtime.qemu.data, "runtime_qemu_data");
  const sd = payload.runtime.qemu.sdImage;
  if (sd !== undefined) {
    if (Array.isArray(sd)) {
      if (sd.length < 2 || sd.length > 16) fail("sd_parts_invalid");
      sd.forEach((entry,index) => assertArtifactShape(entry, `runtime_qemu_sdImage_${index}`));
    } else assertArtifactShape(sd,"runtime_qemu_sdImage");
  }
  const sdRawBytes = payload.runtime.qemu.sdRawBytes;
  if (sdRawBytes !== undefined && (!Number.isSafeInteger(sdRawBytes) || sdRawBytes < 512 || sdRawBytes > MAX_ARTIFACT_BYTES || sd === undefined)) fail("sd_raw_size_invalid");
  if (!Array.isArray(payload.guests) || payload.guests.length === 0 || payload.guests.length > 16) fail("guests_missing");
  const boardIds = new Set();
  for (const [index, guest] of payload.guests.entries()) {
    assertObject(guest, `guest_${index}`);
    assertKeys(guest, new Set(["id", "firmware", "kernel", "symbols", "provenance", "bootloader", "partitionTable", "otaData"]), `guest_${index}`);
    if (typeof guest.id !== "string" || !/^[a-z0-9][a-z0-9-]{0,31}$/.test(guest.id) || boardIds.has(guest.id)) {
      fail(`guest_${index}_id_invalid_or_duplicate`);
    }
    boardIds.add(guest.id);
    for (const role of ["firmware", "kernel", "symbols", "provenance"]) {
      assertArtifactShape(guest[role], `guest_${guest.id}_${role}`);
    }
    for (const role of ["bootloader", "partitionTable", "otaData"]) {
      if (guest[role] !== undefined) assertArtifactShape(guest[role], `guest_${guest.id}_${role}`);
    }
  }
  return payload;
}

const resolveArtifact = readVerifiedArtifact;

function collectEntries(manifest) {
  const entries = [];
  const add = (artifact, root, output, label) => {
    entries.push({ artifact, root, output, label });
  };
  add(manifest.runtime.workerScript, "runtime", "simulator-runtime/browser-simulator-worker.js", "runtime_workerScript");
  if(manifest.runtime.sdStoreScript)add(manifest.runtime.sdStoreScript,"runtime","simulator-runtime/sd-card-store.js","runtime_sdStoreScript");
  const qemu = manifest.runtime.qemu;
  const qemuOutputs = {
    script: "simulator-runtime/qemu/qemu-system-xtensa.js",
    wasm: "simulator-runtime/qemu/qemu-system-xtensa.wasm",
    worker: "simulator-runtime/qemu/qemu-system-xtensa.worker.js",
    data: "simulator-runtime/qemu/qemu-system-xtensa.data",
    bootloader: "simulator-runtime/qemu/bootloader.bin",
    partitionTable: "simulator-runtime/qemu/partition-table.bin",
    otaData: "simulator-runtime/qemu/ota_data_initial.bin",
    rom: "simulator-runtime/qemu/esp32s3_rev0_rom.bin",
  };
  for (const role of Object.keys(qemuOutputs)) {
    if (qemu[role] !== undefined) add(qemu[role], "runtime", qemuOutputs[role], `runtime_qemu_${role}`);
  }
  if (Array.isArray(qemu.sdImage)) {
    qemu.sdImage.forEach((entry,index) => add(entry,"runtime",`simulator-runtime/qemu/sdcard.img.gz.part${String(index).padStart(2,"0")}`,`runtime_qemu_sdImage_${index}`));
  } else if (qemu.sdImage) {
    add(qemu.sdImage,"runtime",`simulator-runtime/qemu/sdcard.img${qemu.sdImage.path.endsWith(".gz") ? ".gz" : ""}`,"runtime_qemu_sdImage");
  }
  for (const guest of manifest.guests) {
    for (const role of ["firmware", "kernel", "symbols", "provenance", "bootloader", "partitionTable", "otaData"]) {
      if (!guest[role]) continue;
      add(guest[role], "guest", `simulator-runtime/qemu/${role === "firmware" ? `firmware-${guest.id}.bin` : `${role}-${guest.id}.${role === "kernel" ? "img" : role === "symbols" ? "txt" : role === "provenance" ? "json" : "bin"}`}`, `guest_${guest.id}_${role}`);
    }
  }
  const outputs = new Set();
  const sources = new Set();
  for (const entry of entries) {
    if (outputs.has(entry.output)) fail(`duplicate_output:${entry.output}`);
    outputs.add(entry.output);
    const sourceKey = `${entry.root}:${entry.artifact.path}`;
    if (sources.has(sourceKey)) fail(`duplicate_source:${sourceKey}`);
    sources.add(sourceKey);
  }
  if (entries.reduce((sum, e) => sum + e.artifact.bytes, 0) > 512 * 1024 * 1024) fail("total_bytes_exceeded");
  return entries;
}

function copyEntry(entry, roots, outputDir) {
  const resolved = resolveArtifact(roots[entry.root], entry.artifact, entry.label);
  const destination = resolve(outputDir, entry.output);
  const destinationRelative = relative(resolve(outputDir), destination);
  if (destinationRelative.startsWith(`..${sep}`) || destinationRelative === ".." || destinationRelative.startsWith(sep)) {
    fail(`${entry.label}_output_escapes_root`);
  }
  mkdirSync(dirname(destination), { recursive: true });
  writeFileSync(destination, resolved.data, { flag: "wx" });
  return { ...entry, sourcePath: resolved.sourcePath, bytes: resolved.bytes, sha256: resolved.sha256, output: entry.output };
}

function toRuntimeManifest(manifest, copiedEntries, artifactManifestSha256) {
  const byLabel = new Map(copiedEntries.map((entry) => [entry.label, entry]));
  const pathFor = (label) => byLabel.has(label) ? `${manifest.basePath ?? ""}${byLabel.get(label).output}` : null;
  const firmwareArtifacts = Object.fromEntries(manifest.guests.map((guest) => [guest.id, {
    bin: pathFor(`guest_${guest.id}_firmware`),
    kernel: pathFor(`guest_${guest.id}_kernel`),
    symbols: pathFor(`guest_${guest.id}_symbols`),
    bootloader: pathFor(`guest_${guest.id}_bootloader`),
    partitionTable: pathFor(`guest_${guest.id}_partitionTable`),
    otaData: pathFor(`guest_${guest.id}_otaData`),
  }]));
  const defaultGuest = manifest.guests[0];
  const source = {
    kind: "standalone-web-assembly",
    revision: manifest.source?.revision ?? null,
    label: manifest.source?.label ?? "private-local-pack",
    artifactManifestSha256,
  };
  const digests = Object.fromEntries(copiedEntries.map((entry) => [entry.label, entry.sha256]));
  return {
    schemaVersion: 1,
    runtime: {
      kind: "wasm-worker",
      status: "ready",
      workerScript: pathFor("runtime_workerScript"),
      firmwareArtifact: pathFor(`guest_${defaultGuest.id}_firmware`),
      firmwareArtifacts,
      qemuScript: pathFor("runtime_qemu_script"),
      qemuWasm: pathFor("runtime_qemu_wasm"),
      qemuKernel: pathFor(`guest_${defaultGuest.id}_kernel`),
      qemuSymbols: pathFor(`guest_${defaultGuest.id}_symbols`),
      qemuBootloader: pathFor("runtime_qemu_bootloader"),
      qemuPartitionTable: pathFor("runtime_qemu_partitionTable"),
      qemuOtaData: pathFor("runtime_qemu_otaData"),
      qemuRom: pathFor("runtime_qemu_rom"),
      qemuSdImage: Array.isArray(manifest.runtime.qemu.sdImage) ? manifest.runtime.qemu.sdImage.map((_,i)=>pathFor(`runtime_qemu_sdImage_${i}`)) : pathFor("runtime_qemu_sdImage"),
      qemuSdRawBytes: manifest.runtime.qemu.sdRawBytes ?? null,
      qemuWorkerScript: pathFor("runtime_qemu_worker"),
      artifactManifest: `${manifest.basePath ?? ""}simulator-runtime/artifact-manifest.json`,
      source,
      digests,
    },
    publication: { visibility: "private", localOnly: true, approvalRequired: true },
  };
}

export function assembleWebArtifacts({ manifestPath, runtimeDir, guestDir, outputDir, uiDir }) {
  if (!manifestPath || !runtimeDir || !guestDir || !outputDir) fail("explicit_paths_required");
  const manifestStats = lstatSync(resolve(manifestPath));
  if (!manifestStats.isFile() || manifestStats.isSymbolicLink() || manifestStats.size > MAX_MANIFEST_BYTES) {
    fail("manifest_file_invalid");
  }
  const manifest = parseWebArtifactManifest(JSON.parse(readFileSync(resolve(manifestPath), "utf8")));
  const entries = collectEntries(manifest);
  const resolvedOutput = resolve(outputDir);
  const stage = join(dirname(resolvedOutput), `.${basename(resolvedOutput)}.staging-${process.pid}-${Math.random().toString(16).slice(2)}`);
  const roots = { runtime: resolve(runtimeDir), guest: resolve(guestDir) };
  for (const root of [...Object.values(roots), ...(uiDir ? [resolve(uiDir)] : [])]) {
    if (root === resolvedOutput || root.startsWith(resolvedOutput + sep) || resolvedOutput.startsWith(root + sep)) fail("output_overlaps_input");
  }
  const lock = resolvedOutput + ".panda-pack-lock";
  closeSync(openSync(lock, "wx"));
  let committed = false;
  try {
    mkdirSync(stage, { recursive: true });
    const copiedEntries = entries.map((entry) => copyEntry(entry, roots, stage));
    if (uiDir) copyBuiltUi(uiDir, stage);
    writeFileSync(join(stage, ".panda-web-package.json"), JSON.stringify({owner:"panda-simulator-pack",schemaVersion:1}));
    const artifactManifest = {
      schemaVersion: 1,
      generatedAt: new Date().toISOString(),
      source: { ...(manifest.source ?? {}), kind: "standalone-web-assembly" },
      publication: { visibility: "private", localOnly: true },
      artifacts: copiedEntries.map((entry) => ({ label: entry.label, path: entry.output, bytes: entry.bytes, sha256: entry.sha256 })),
    };
    const artifactManifestText = `${JSON.stringify(artifactManifest, null, 2)}\n`;
    const artifactManifestSha256 = createHash("sha256").update(artifactManifestText).digest("hex");
    writeFileSync(join(stage, "simulator-runtime", "artifact-manifest.json"), artifactManifestText);
    writeFileSync(join(stage, "manifest.json"), `${JSON.stringify(toRuntimeManifest(manifest, copiedEntries, artifactManifestSha256), null, 2)}\n`);
    if (lstatSafe(resolvedOutput)) {
      const oldStats = lstatSync(resolvedOutput);
      if (oldStats.isSymbolicLink() || !oldStats.isDirectory()) fail("output_must_be_directory");
      const ownerPath = join(resolvedOutput, ".panda-web-package.json");
      if (!lstatSafe(ownerPath) || lstatSync(ownerPath).isSymbolicLink()) fail("refuse_unowned_output");
      const owner = JSON.parse(readFileSync(ownerPath, "utf8"));
      if (owner.owner !== "panda-simulator-pack" || owner.schemaVersion !== 1) fail("refuse_unowned_output");
      const backup = join(dirname(resolvedOutput), `.${basename(resolvedOutput)}.previous-${process.pid}-${Math.random().toString(16).slice(2)}`);
      renamePath(resolvedOutput, backup);
      try {
        renamePath(stage, resolvedOutput);
        committed = true;
      } finally {
        if (committed) rmSync(backup, { recursive: true, force: true });
        else renamePath(backup, resolvedOutput);
      }
    } else {
      renamePath(stage, resolvedOutput);
      committed = true;
    }
    return { outputDir: resolvedOutput, manifestPath: join(resolvedOutput, "manifest.json"), artifactCount: copiedEntries.length };
  } finally {
    try { if (!committed && lstatSafe(stage)) rmSync(stage, { recursive: true, force: true }); }
    finally { rmSync(lock, { force: true }); }
  }
}

function lstatSafe(path) {
  try {
    lstatSync(path);
    return true;
  } catch {
    return false;
  }
}

function renamePath(from, to) {
  renameSync(from, to);
}
