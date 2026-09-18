import { createHash } from "node:crypto";
import { lstatSync, readdirSync, readFileSync } from "node:fs";
import { basename, extname, join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { createGunzip } from "node:zlib";
import { Readable, Writable } from "node:stream";
import { pipeline } from "node:stream/promises";

export const MAX_STATIC_ASSET_BYTES = 25 * 1024 * 1024;
export const MAX_MANIFEST_BYTES = 4 * 1024 * 1024;
export const BROWSER_SD_IMAGE_BYTES = 128 * 1024 * 1024;
export const MAX_SD_PARTS = 16;

const REQUIRED_RUNTIME_PATHS = [
  "workerScript",
  "firmwareArtifact",
  "qemuScript",
  "qemuWasm",
  "qemuKernel",
  "qemuSymbols",
  "qemuBootloader",
  "qemuPartitionTable",
  "qemuOtaData",
  "qemuRom",
  "qemuWorkerScript",
  "artifactManifest",
];

function fail(message) {
  throw new Error(`web_package_invalid:${message}`);
}

function assertObject(value, label) {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    fail(`${label}_must_be_object`);
  }
}

function assertString(value, label) {
  if (typeof value !== "string" || value.length === 0) {
    fail(`${label}_must_be_non_empty_string`);
  }
}

function assertSha256(value, label) {
  if (typeof value !== "string" || !/^[a-f0-9]{64}$/.test(value)) {
    fail(`${label}_sha256_invalid`);
  }
}

function assertBasePath(value) {
  if (typeof value !== "string" || value.length === 0 || value === "//" || !value.startsWith("/")) {
    fail("base_path_invalid");
  }
  if (value !== "/" && !/^\/(?:[A-Za-z0-9_-]+\/)+$/.test(value)) {
    fail("base_path_invalid");
  }
  return value;
}

function assertSafeRelativePath(value, label) {
  assertString(value, label);
  if (value.length > 512 || value.includes("\0") || value.includes("\\")) {
    fail(`${label}_path_invalid`);
  }
  if (value.startsWith("/") || /^[A-Za-z]:/.test(value) || /^[A-Za-z][A-Za-z0-9+.-]*:/.test(value)) {
    fail(`${label}_path_not_relative`);
  }
  if (value.includes("%") || value.startsWith("//")) {
    fail(`${label}_encoded_or_network_path`);
  }
  const parts = value.split("/");
  if (parts.some((part) => part.length === 0 || part === "." || part === "..")) {
    fail(`${label}_path_traversal`);
  }
  return value;
}

function advertisedPathToRelative(value, basePath, label) {
  assertString(value, label);
  if (value.includes("\0") || value.includes("\\") || value.includes("?") || value.includes("#")) {
    fail(`${label}_url_invalid`);
  }
  if (!value.startsWith("/") || value.startsWith("//") || /^[A-Za-z][A-Za-z0-9+.-]*:/.test(value)) {
    fail(`${label}_url_not_static`);
  }
  if (basePath !== "/" && !value.startsWith(basePath)) {
    fail(`${label}_base_path_mismatch`);
  }
  const path = basePath === "/" ? value.slice(1) : value.slice(basePath.length);
  return assertSafeRelativePath(path, label);
}

function assertPackageRoot(packageRoot) {
  assertString(packageRoot, "package_root");
  const root = resolve(packageRoot);
  let stats;
  try {
    stats = lstatSync(root);
  } catch (error) {
    fail(`package_root_missing:${error instanceof Error ? error.message : String(error)}`);
  }
  if (!stats.isDirectory() || stats.isSymbolicLink()) {
    fail("package_root_not_directory");
  }
  return root;
}

function assertPathBelowRoot(root, candidate, label) {
  const resolvedCandidate = resolve(candidate);
  const candidateRelative = relative(root, resolvedCandidate);
  if (
    candidateRelative === "" ||
    candidateRelative === ".." ||
    candidateRelative.startsWith(`..${sep}`) ||
    candidateRelative.startsWith(sep)
  ) {
    fail(`${label}_escapes_package_root`);
  }
  return candidateRelative.split(sep).join("/");
}

function lstatSafe(path, label) {
  try {
    return lstatSync(path);
  } catch (error) {
    fail(`${label}_missing:${error instanceof Error ? error.message : String(error)}`);
  }
}

function assertNoSymlinkPath(root, relativePath, label) {
  const safePath = assertSafeRelativePath(relativePath, label);
  let current = root;
  for (const part of safePath.split("/")) {
    current = join(current, part);
    const stats = lstatSafe(current, label);
    if (stats.isSymbolicLink()) {
      fail(`${label}_symlink_not_allowed`);
    }
  }
  return current;
}

function readBoundedFile(path, label, limit = MAX_STATIC_ASSET_BYTES) {
  const stats = lstatSafe(path, label);
  if (stats.isSymbolicLink() || !stats.isFile()) {
    fail(`${label}_must_be_regular_file`);
  }
  if (stats.size < 1 || stats.size > limit) {
    fail(`${label}_size_invalid:${stats.size}`);
  }
  const bytes = readFileSync(path);
  if (bytes.length !== stats.size) {
    fail(`${label}_changed_during_read`);
  }
  return bytes;
}

function readJsonFile(path, label) {
  const bytes = readBoundedFile(path, label, MAX_MANIFEST_BYTES);
  try {
    return JSON.parse(bytes.toString("utf8"));
  } catch (error) {
    fail(`${label}_invalid_json:${error instanceof Error ? error.message : String(error)}`);
  }
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function validateStaticPackage(packageRoot) {
  const files = new Set();
  const pending = [packageRoot];
  let totalBytes = 0;
  while (pending.length > 0) {
    const current = pending.pop();
    for (const entry of readdirSync(current, { withFileTypes: true })) {
      const path = join(current, entry.name);
      const relativePath = relative(packageRoot, path).split(sep).join("/");
      const stats = lstatSafe(path, `static_${relativePath}`);
      if (stats.isSymbolicLink()) {
        fail(`static_${relativePath}_symlink_not_allowed`);
      }
      if (stats.isDirectory()) {
        pending.push(path);
        continue;
      }
      if (!stats.isFile()) {
        fail(`static_${relativePath}_not_regular_file`);
      }
      if (stats.size > MAX_STATIC_ASSET_BYTES) {
        fail(`static_${relativePath}_exceeds_25_mib:${stats.size}`);
      }
      if ([".elf", ".map"].includes(extname(entry.name).toLowerCase())) {
        fail(`static_${relativePath}_debug_or_source_map_not_deployable`);
      }
      files.add(relativePath);
      totalBytes += stats.size;
      if (totalBytes > 512 * 1024 * 1024) {
        fail("static_package_total_exceeds_512_mib");
      }
    }
  }
  return { files, totalBytes };
}

function validateRuntimeManifest(manifest, basePath) {
  assertObject(manifest, "runtime_manifest");
  if (manifest.schemaVersion !== 1) fail("runtime_manifest_schema_unsupported");
  if (manifest.publication?.visibility !== "private") fail("runtime_manifest_publication_not_private");
  assertObject(manifest.runtime, "runtime_manifest_runtime");
  if (manifest.runtime.kind !== "wasm-worker" || manifest.runtime.status !== "ready") {
    fail("public_runtime_not_ready");
  }
  for (const key of REQUIRED_RUNTIME_PATHS) {
    assertString(manifest.runtime[key], `runtime_${key}`);
  }
  if (
    !(
      typeof manifest.runtime.qemuSdImage === "string" ||
      (Array.isArray(manifest.runtime.qemuSdImage) && manifest.runtime.qemuSdImage.every((value) => typeof value === "string"))
    )
  ) {
    fail("runtime_qemuSdImage_must_be_static_path_or_parts");
  }
  if (!Number.isSafeInteger(manifest.runtime.qemuSdRawBytes) || manifest.runtime.qemuSdRawBytes !== BROWSER_SD_IMAGE_BYTES) {
    fail("runtime_qemu_sd_raw_bytes_invalid");
  }
  assertObject(manifest.runtime.firmwareArtifacts, "runtime_firmware_artifacts");
  if (Object.keys(manifest.runtime.firmwareArtifacts).length === 0) {
    fail("runtime_firmware_artifacts_missing");
  }
  assertObject(manifest.runtime.source, "runtime_source");
  assertSha256(manifest.runtime.source.artifactManifestSha256, "runtime_source_artifact_manifest");
  assertObject(manifest.runtime.digests, "runtime_digests");

  const advertised = new Map();
  const addPath = (label, value) => {
    if (value == null) return;
    const relativePath = advertisedPathToRelative(value, basePath, label);
    if (!advertised.has(relativePath)) advertised.set(relativePath, label);
  };
  for (const key of REQUIRED_RUNTIME_PATHS) addPath(`runtime_${key}`, manifest.runtime[key]);
  if (Array.isArray(manifest.runtime.qemuSdImage)) {
    manifest.runtime.qemuSdImage.forEach((value, index) => addPath(`runtime_qemu_sd_part_${index}`, value));
  } else {
    addPath("runtime_qemuSdImage", manifest.runtime.qemuSdImage);
  }
  for (const [board, artifacts] of Object.entries(manifest.runtime.firmwareArtifacts)) {
    assertObject(artifacts, `runtime_firmware_artifacts_${board}`);
    for (const [role, value] of Object.entries(artifacts)) {
      if (value != null) addPath(`runtime_firmware_${board}_${role}`, value);
    }
  }
  if (Array.isArray(manifest.runtime.qemuSdImage) &&
      (manifest.runtime.qemuSdImage.length < 2 || manifest.runtime.qemuSdImage.length > MAX_SD_PARTS)) {
    fail("runtime_qemu_sd_parts_invalid");
  }
  return { advertised, sdImage: manifest.runtime.qemuSdImage };
}

function validateArtifactManifest(packageRoot, manifest, artifactManifestRelative, advertised) {
  const artifactManifestPath = assertNoSymlinkPath(packageRoot, artifactManifestRelative, "artifact_manifest");
  const artifactManifestBytes = readBoundedFile(artifactManifestPath, "artifact_manifest", MAX_MANIFEST_BYTES);
  if (sha256(artifactManifestBytes) !== manifest.runtime.source.artifactManifestSha256) {
    fail("artifact_manifest_digest_mismatch");
  }
  let artifactManifest;
  try {
    artifactManifest = JSON.parse(artifactManifestBytes.toString("utf8"));
  } catch (error) {
    fail(`artifact_manifest_invalid_json:${error instanceof Error ? error.message : String(error)}`);
  }
  assertObject(artifactManifest, "artifact_manifest");
  if (artifactManifest.schemaVersion !== 1 || !Array.isArray(artifactManifest.artifacts) || artifactManifest.artifacts.length === 0) {
    fail("artifact_manifest_shape_invalid");
  }

  const byLabel = new Map();
  const byPath = new Map();
  for (const [index, entry] of artifactManifest.artifacts.entries()) {
    assertObject(entry, `artifact_${index}`);
    assertString(entry.label, `artifact_${index}_label`);
    assertSafeRelativePath(entry.path, `artifact_${entry.label}`);
    if (!Number.isSafeInteger(entry.bytes) || entry.bytes < 1 || entry.bytes > MAX_STATIC_ASSET_BYTES) {
      fail(`artifact_${entry.label}_bytes_invalid`);
    }
    assertSha256(entry.sha256, `artifact_${entry.label}`);
    if (byLabel.has(entry.label) || byPath.has(entry.path)) fail(`artifact_duplicate:${entry.label}`);
    byLabel.set(entry.label, entry);
    byPath.set(entry.path, entry);

    const artifactPath = assertNoSymlinkPath(packageRoot, entry.path, `artifact_${entry.label}`);
    const bytes = readBoundedFile(artifactPath, `artifact_${entry.label}`);
    if (bytes.length !== entry.bytes) fail(`artifact_${entry.label}_byte_length_mismatch`);
    if (sha256(bytes) !== entry.sha256) fail(`artifact_${entry.label}_digest_mismatch`);
  }

  for (const [relativePath, label] of advertised) {
    if (relativePath === artifactManifestRelative) continue;
    if (!byPath.has(relativePath)) fail(`${label}_not_declared_in_artifact_manifest`);
  }
  const digestLabels = Object.keys(manifest.runtime.digests);
  if (digestLabels.length !== byLabel.size || digestLabels.some((label) => !byLabel.has(label))) {
    fail("runtime_digest_labels_mismatch");
  }
  for (const [label, entry] of byLabel) {
    if (manifest.runtime.digests[label] !== entry.sha256) fail(`runtime_digest_mismatch:${label}`);
  }
  return { artifactManifest, byLabel, byPath };
}

function validateSdNames(sdImage, basePath) {
  const values = Array.isArray(sdImage) ? sdImage : [sdImage];
  const paths = values.map((value, index) => advertisedPathToRelative(value, basePath, `runtime_qemu_sd_${index}`));
  if (paths.length === 1) {
    if (basename(paths[0]) !== "sdcard.img.gz") fail("runtime_qemu_sd_filename_invalid");
    return paths;
  }
  paths.forEach((path, index) => {
    const match = basename(path).match(/^sdcard\.img\.gz\.part(\d{2})$/);
    if (!match || Number(match[1]) !== index) fail("runtime_qemu_sd_parts_not_contiguous");
  });
  return paths;
}

async function validateGzipDelivery(packageRoot, sdPaths) {
  let decodedBytes = 0;
  async function* source() {
    for (const [index, path] of sdPaths.entries()) {
      yield readBoundedFile(assertNoSymlinkPath(packageRoot, path, `sd_part_${index}`), `sd_part_${index}`);
    }
  }
  const sink = new Writable({
    write(chunk, _encoding, callback) {
      decodedBytes += chunk.length;
      callback(decodedBytes > BROWSER_SD_IMAGE_BYTES ? new Error("decoded SD image exceeds 128 MiB") : undefined);
    },
  });
  try {
    await pipeline(Readable.from(source()), createGunzip(), sink);
  } catch (error) {
    fail(`runtime_qemu_sd_gzip_invalid:${error instanceof Error ? error.message : String(error)}`);
  }
  if (decodedBytes !== BROWSER_SD_IMAGE_BYTES) {
    fail(`runtime_qemu_sd_decoded_size_invalid:${decodedBytes}`);
  }
}

export async function validateWebPackage({ packageRoot, manifestPath, basePath }) {
  const root = assertPackageRoot(packageRoot);
  const resolvedBasePath = assertBasePath(basePath);
  const manifestAbsolute = resolve(manifestPath || join(root, "manifest.json"));
  const manifestRelative = assertPathBelowRoot(root, manifestAbsolute, "runtime_manifest");
  const manifestFile = assertNoSymlinkPath(root, manifestRelative, "runtime_manifest");
  const manifest = readJsonFile(manifestFile, "runtime_manifest");
  const { advertised, sdImage } = validateRuntimeManifest(manifest, resolvedBasePath);
  const artifactManifestRelative = advertisedPathToRelative(
    manifest.runtime.artifactManifest,
    resolvedBasePath,
    "runtime_artifact_manifest",
  );
  const artifactManifestResult = validateArtifactManifest(root, manifest, artifactManifestRelative, advertised);
  const staticResult = validateStaticPackage(root);
  for (const [path, label] of advertised) {
    if (!staticResult.files.has(path)) fail(`${label}_not_in_static_package`);
  }
  const sdPaths = validateSdNames(sdImage, resolvedBasePath);
  await validateGzipDelivery(root, sdPaths);
  return {
    packageRoot: root,
    manifestPath: manifestAbsolute,
    artifactCount: artifactManifestResult.byLabel.size,
    scannedFiles: staticResult.files.size,
    totalBytes: staticResult.totalBytes,
    sdParts: sdPaths.length,
    sdDecodedBytes: BROWSER_SD_IMAGE_BYTES,
  };
}

function usage() {
  return "Usage: node scripts/validate-web-package.mjs --package-root DIR --manifest FILE --base-path / or /simulator/app/";
}

function parseArgs(argv) {
  const values = {};
  for (let index = 0; index < argv.length; index += 1) {
    const flag = argv[index];
    if (flag === "--help") {
      console.log(usage());
      return null;
    }
    if (!flag.startsWith("--") || !argv[index + 1] || argv[index + 1].startsWith("--")) {
      throw new Error(usage());
    }
    const key = flag.slice(2);
    if (!["package-root", "manifest", "base-path"].includes(key) || values[key]) {
      throw new Error(`Unknown or duplicate option: ${flag}\n${usage()}`);
    }
    values[key] = argv[index + 1];
    index += 1;
  }
  if (!values["package-root"] || !values.manifest || !values["base-path"]) throw new Error(usage());
  return values;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args) return;
  const result = await validateWebPackage({
    packageRoot: args["package-root"],
    manifestPath: args.manifest,
    basePath: args["base-path"],
  });
  console.log(`[validate-web-package] valid static package: ${result.artifactCount} artifacts, ${result.scannedFiles} files`);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch((error) => {
    console.error(`[validate-web-package] ${error instanceof Error ? error.message : String(error)}`);
    process.exitCode = 1;
  });
}
