import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const [outputDirArg, manifestPathArg, ...flags] = process.argv.slice(2);
if (!outputDirArg || !manifestPathArg) {
  throw new Error(
    "Usage: node write-qemu-wasm-artifact-manifest.mjs <output-dir> <manifest-path> " +
      "[--allow-missing] [--require-boards <board> ...]",
  );
}

const outputDir = resolve(outputDirArg);
const manifestPath = resolve(manifestPathArg);
const allowMissing = flags.includes("--allow-missing");
const requireIndex = flags.indexOf("--require-boards");
const hasPreviousManifest = existsSync(manifestPath);
let previousManifest = null;
if (hasPreviousManifest) {
  try {
    previousManifest = JSON.parse(readFileSync(manifestPath, "utf8"));
  } catch {
    previousManifest = null;
  }
}
let requiredBuildBoards = requireIndex >= 0 ? flags.slice(requireIndex + 1).filter((flag) => !flag.startsWith("--")) : [];
if (requiredBuildBoards.length === 0 && previousManifest) {
  requiredBuildBoards = Array.isArray(previousManifest.requiredBuildBoards) ? previousManifest.requiredBuildBoards : [];
}
if (requiredBuildBoards.length === 0) requiredBuildBoards = ["default"];
requiredBuildBoards = [...new Set(requiredBuildBoards)];

const scriptDir = dirname(fileURLToPath(import.meta.url));
const simulatorDir = resolve(scriptDir, "..");
const repoRoot = resolve(simulatorDir, "../..");
const registry = JSON.parse(readFileSync(join(simulatorDir, "boards.json"), "utf8"));
const currentSourceRevision = execFileSync("git", ["-C", repoRoot, "rev-parse", "HEAD"], {
  encoding: "utf8",
}).trim();
const requestedProfile = process.env.QEMU_WASM_PROFILE || process.env.PANDA_PRODUCT_PROFILE || "default";
const previousAllocator = typeof previousManifest?.wasmAllocator === "string" ? previousManifest.wasmAllocator : "";
const requestedAllocator = process.env.QEMU_WASM_MALLOC || previousAllocator || (hasPreviousManifest ? "" : "emmalloc");
if (requestedAllocator && !["emmalloc", "dlmalloc", "mimalloc"].includes(requestedAllocator)) {
  throw new Error(`invalid QEMU-WASM allocator: ${requestedAllocator}`);
}
const allocatorIdentityError = hasPreviousManifest &&
  ((!previousAllocator && requestedAllocator) ||
    (previousAllocator && requestedAllocator && previousAllocator !== requestedAllocator));
const profilePlanPath = process.env.QEMU_WASM_PROFILE_PLAN || "";
let profileIdentity;
if (profilePlanPath) {
  const plan = JSON.parse(readFileSync(resolve(profilePlanPath), "utf8"));
  const planProfile = plan.profile || {};
  profileIdentity = {
    id: planProfile.id,
    revision: planProfile.revision,
    digest: plan.profileContractSha256,
    namespace: planProfile.otaNamespace,
    sourceRevision: plan.sourceRevision,
  };
} else if (requestedProfile !== "default") {
  profileIdentity = {
    id: requestedProfile,
    revision: Number(process.env.QEMU_WASM_PROFILE_REVISION || 0),
    digest: process.env.QEMU_WASM_PROFILE_DIGEST || "",
    namespace: process.env.QEMU_WASM_OTA_NAMESPACE || requestedProfile,
    sourceRevision: process.env.QEMU_WASM_SOURCE_REVISION || currentSourceRevision,
  };
}
if (requestedProfile !== "default" && (!profileIdentity || profileIdentity.id !== requestedProfile)) {
  throw new Error(`QEMU-WASM profile identity is missing or inconsistent: ${requestedProfile}`);
}
if (profileIdentity && profileIdentity.id !== "default" &&
  (!Number.isSafeInteger(profileIdentity.revision) || profileIdentity.revision < 1 ||
    !/^[0-9a-fA-F]{64}$/.test(profileIdentity.digest) || profileIdentity.namespace !== profileIdentity.id ||
    typeof profileIdentity.sourceRevision !== "string" || !profileIdentity.sourceRevision)) {
  throw new Error(`invalid QEMU-WASM profile identity for ${profileIdentity.id}`);
}
const sourceInputCompatibility = new Map();

function sourceInputsMatchCurrentRevision(sourceRevision) {
  if (sourceRevision === currentSourceRevision) return true;
  if (!/^[0-9a-f]{40}$/.test(sourceRevision)) return false;
  if (sourceInputCompatibility.has(sourceRevision)) return sourceInputCompatibility.get(sourceRevision);

  let matches = false;
  try {
    // 发布提交可能只更新云端元数据；固件来源提交仍然有效时允许复用产物。
    execFileSync(
      "git",
      [
        "-C",
        repoRoot,
        "diff",
        "--quiet",
        `${sourceRevision}..${currentSourceRevision}`,
        "--",
        "apps/panda-os",
        "apps/simulator",
        "apps/shared",
        ":(exclude)apps/simulator/scripts/write-qemu-wasm-artifact-manifest.mjs",
      ],
      { stdio: "ignore" },
    );
    matches = true;
  } catch {
    matches = false;
  }
  sourceInputCompatibility.set(sourceRevision, matches);
  return matches;
}

const sharedRequired = [
  "qemu-system-xtensa.js",
  "qemu-system-xtensa.wasm",
  "bootloader.bin",
  "partition-table.bin",
  "ota_data_initial.bin",
  "esp32s3_rev0_rom.bin",
];
const optionalArtifacts = ["qemu-system-xtensa.worker.js", "qemu-system-xtensa.data", "sdcard.img", "load.js"];

function buildBoardForProfile(profile) {
  return profile.murphyBoard === "mofei" ? "default" : profile.murphyBoard;
}

function profileForBuildBoard(buildBoard) {
  if (buildBoard === "default") {
    return registry.boards.find((profile) => profile.id === "mofei");
  }
  return registry.boards.find((profile) => buildBoardForProfile(profile) === buildBoard || profile.id === buildBoard);
}

function firmwareNames(buildBoard) {
  const suffix = buildBoard === "default" ? "" : `-${buildBoard}`;
  return {
    bin: `firmware${suffix}.bin`,
    kernel: `firmware${suffix}-kernel.img`,
    symbols: `firmware${suffix}-symbols.txt`,
    provenance: `firmware${suffix}-provenance.json`,
  };
}

function sha256(path) {
  return createHash("sha256").update(readFileSync(path)).digest("hex");
}

function artifactInfo(fileName) {
  const path = join(outputDir, fileName);
  if (!existsSync(path)) return null;
  const stats = statSync(path);
  if (!stats.isFile() || stats.size === 0) return null;
  return {
    fileName,
    relativePath: relative(outputDir, path),
    bytes: stats.size,
    sha256: sha256(path),
  };
}

const sharedArtifacts = Object.fromEntries(
  [...sharedRequired, ...optionalArtifacts]
    .map((fileName) => [fileName, artifactInfo(fileName)])
    .filter(([, info]) => info !== null),
);
const missingShared = sharedRequired.filter((fileName) => !sharedArtifacts[fileName]);
const boards = {};
const boardErrors = [];
const sourceRevisions = new Set();
if (!requestedAllocator) boardErrors.push("wasm allocator provenance is missing");
if (allocatorIdentityError) {
  boardErrors.push(
    previousAllocator
      ? `existing qemu-wasm allocator ${previousAllocator} does not match requested ${requestedAllocator}`
      : "existing qemu-wasm manifest has no allocator provenance",
  );
}

for (const buildBoard of requiredBuildBoards) {
  const errorsBeforeBoard = boardErrors.length;
  const profile = profileForBuildBoard(buildBoard);
  if (!profile) {
    boardErrors.push(`${buildBoard}: board registry profile is missing`);
    continue;
  }
  const names = firmwareNames(buildBoard);
  const firmware = {
    bin: artifactInfo(names.bin),
    kernel: artifactInfo(names.kernel),
    symbols: artifactInfo(names.symbols),
  };
  const missing = Object.entries(firmware)
    .filter(([, info]) => info === null)
    .map(([kind]) => kind);
  let provenance = null;
  const provenancePath = join(outputDir, names.provenance);
  if (existsSync(provenancePath)) {
    const parsedProvenance = JSON.parse(readFileSync(provenancePath, "utf8"));
    if (parsedProvenance && typeof parsedProvenance === "object" && !Array.isArray(parsedProvenance)) {
      provenance = parsedProvenance;
    } else {
      missing.push("provenance");
    }
  } else {
    missing.push("provenance");
  }
  if (provenance) {
    if (provenance.board !== buildBoard) {
      boardErrors.push(`${buildBoard}: provenance board does not match build board`);
    }
    if (typeof provenance.sourceElfSha256 !== "string" || !provenance.sourceElfSha256) {
      boardErrors.push(`${buildBoard}: source ELF hash is missing from provenance`);
    }
    const expected = {
      sourceBinSha256: firmware.bin?.sha256,
      publishedBinSha256: firmware.bin?.sha256,
      publishedKernelSha256: firmware.kernel?.sha256,
      publishedSymbolsSha256: firmware.symbols?.sha256,
    };
    for (const [field, digest] of Object.entries(expected)) {
      if (digest && (typeof provenance[field] !== "string" || provenance[field] !== digest)) {
        const label = field === "sourceBinSha256" ? "source BIN hash" : `published ${field.slice(9, -6).toLowerCase()} hash`;
        boardErrors.push(`${buildBoard}: ${label} does not match published artifact`);
      }
    }
    if (profileIdentity) {
      const provenanceProfile = provenance.profile;
      if (!provenanceProfile || provenanceProfile.id !== profileIdentity.id ||
        provenanceProfile.revision !== profileIdentity.revision ||
        provenanceProfile.digest !== profileIdentity.digest ||
        provenanceProfile.namespace !== profileIdentity.namespace ||
        (provenanceProfile.sourceRevision && provenanceProfile.sourceRevision !== profileIdentity.sourceRevision)) {
        boardErrors.push(`${buildBoard}: profile identity does not match resolved release profile`);
      }
    } else if (provenance.profile && provenance.profile.id !== "default") {
      boardErrors.push(`${buildBoard}: non-default provenance profile requires a resolved profile plan`);
    }
    if (typeof provenance.sourceRevision !== "string" || !provenance.sourceRevision) {
      boardErrors.push(`${buildBoard}: source revision is missing`);
    } else {
      sourceRevisions.add(provenance.sourceRevision);
      if (!sourceInputsMatchCurrentRevision(provenance.sourceRevision)) {
        boardErrors.push(
          `${buildBoard}: source revision ${provenance.sourceRevision} does not match current HEAD ${currentSourceRevision}`,
        );
      }
    }
    provenance = { ...provenance, status: "ready" };
  }
  if (missing.length > 0) boardErrors.push(`${buildBoard}: missing ${missing.join(", ")}`);
  const boardId = buildBoard === "default" ? "mofei" : profile.id;
  boards[boardId] = {
    status: missing.length === 0 && provenance !== null && boardErrors.length === errorsBeforeBoard ? "ready" : "missing",
    buildBoard,
    framebuffer: {
      width: profile.framebufferWidth,
      height: profile.framebufferHeight,
      format: profile.framebufferFormat,
    },
    firmware,
    provenance,
  };
}

if (sourceRevisions.size > 1) {
  boardErrors.push(`required board source revisions do not match: ${[...sourceRevisions].join(", ")}`);
}
if (profileIdentity && sourceRevisions.size > 0 &&
  [...sourceRevisions].some((revision) => revision !== profileIdentity.sourceRevision)) {
  boardErrors.push("required board source revisions do not match profile sourceRevision");
}

const status = missingShared.length === 0 && boardErrors.length === 0 ? "ready" : "missing";
if (status !== "ready" && !allowMissing) {
  const details = [];
  if (missingShared.length) details.push(`shared artifacts: ${missingShared.join(", ")}`);
  if (boardErrors.length) details.push(`board artifacts: ${boardErrors.join("; ")}`);
  throw new Error(`Missing required qemu-wasm artifacts: ${details.join(" | ")}`);
}

const manifest = {
  schemaVersion: profileIdentity?.id === "latin" ? 3 : 2,
  generatedAt: new Date().toISOString(),
  source: "ktock/qemu-wasm + Panda AI OS simulator overlay",
  sourceRevision: sourceRevisions.size === 1 ? [...sourceRevisions][0] : currentSourceRevision,
  ...(profileIdentity ? { profile: profileIdentity } : {}),
  target: "xtensa-softmmu",
  status,
  ...(requestedAllocator ? { wasmAllocator: requestedAllocator } : {}),
  requiredBuildBoards,
  missingRequired: [...missingShared, ...boardErrors],
  artifacts: Object.values(sharedArtifacts),
  boards,
};

writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
