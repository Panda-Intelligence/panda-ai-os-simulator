// Compatibility entry for an explicitly supplied, source-labelled runtime set.
// No downloads, guest execution, public deployment or account operations occur.
import { createHash } from "node:crypto";
import { lstatSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { gzipSync, gunzipSync } from "node:zlib";
import { readVerifiedArtifact } from "./pack-safe-io.mjs";
import { assembleWebArtifacts } from "./pack-web-manifest.mjs";
import { patchQemuScriptForBrowserWorker } from "./patch-browser-qemu.mjs";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const MAX_BYTES = 256 * 1024 * 1024;
const MAX_JSON_BYTES = 4 * 1024 * 1024;
const sha = data => createHash("sha256").update(data).digest("hex");
function fail(message) { throw new Error(`existing_runtime_invalid:${message}`); }
function readBounded(path, limit = MAX_BYTES) {
  const stat = lstatSync(path);
  if (!stat.isFile() || stat.isSymbolicLink() || stat.size < 1 || stat.size > limit) fail("unsafe_or_oversized_input");
  const bytes = readFileSync(path);
  if (bytes.length !== stat.size) fail("input_changed");
  return bytes;
}
function validArtifact(info) {
  if (!info || typeof info !== "object" || typeof info.fileName !== "string" ||
      !/^[A-Za-z0-9][A-Za-z0-9_.-]{0,159}$/.test(info.fileName) || info.relativePath !== info.fileName ||
      !Number.isSafeInteger(info.bytes) || info.bytes < 1 || info.bytes > MAX_BYTES || !/^[a-f0-9]{64}$/.test(info.sha256)) fail("artifact_shape");
  return { path: info.fileName, bytes: info.bytes, sha256: info.sha256 };
}
function stageBytes(root, name, data) {
  writeFileSync(join(root, name), data, {flag:"wx"});
  return {path:name, bytes:data.length, sha256:sha(data)};
}
export function packageExistingRuntime(options) {
  for (const key of ["qemuDir","uiDir","outputDir"]) if (!options[key]) fail(`missing_${key}`);
  const qemuDir = resolve(options.qemuDir);
  const source = JSON.parse(readBounded(join(qemuDir,"qemu-wasm-artifacts.json"),MAX_JSON_BYTES));
  if (![2,3].includes(source.schemaVersion) || source.status !== "ready" || source.target !== "xtensa-softmmu" ||
      !/^[a-f0-9]{40}$/.test(source.sourceRevision) || !Array.isArray(source.artifacts) || source.artifacts.length > 64 ||
      !source.boards || typeof source.boards !== "object" || Object.keys(source.boards).length > 16) fail("source_manifest_not_ready");
  const artifacts = new Map();
  for (const info of source.artifacts) {
    const entry = validArtifact(info);
    if (artifacts.has(entry.path)) fail("duplicate_artifact");
    artifacts.set(entry.path,entry);
  }
  const registry = JSON.parse(readFileSync(join(ROOT,"boards.json"),"utf8"));
  const boardMap = options.boardMap ? JSON.parse(readBounded(resolve(options.boardMap),MAX_JSON_BYTES)) : null;
  if (boardMap && (boardMap.schemaVersion !== 1 || !Array.isArray(boardMap.boards))) fail("product_map_invalid");
  const mapped = new Map((boardMap?.boards ?? []).map(b=>[b.id,b]));
  if (boardMap && mapped.size !== boardMap.boards.length) fail("duplicate_product_board");
  const work = mkdtempSync(join(tmpdir(),"panda-runtime-pack-"));
  const runtimeDir = join(work,"runtime"), guestDir = join(work,"guest");
  mkdirSync(runtimeDir);mkdirSync(guestDir);
  try {
    const qemu = {};
    const roles = {script:"qemu-system-xtensa.js",wasm:"qemu-system-xtensa.wasm",worker:"qemu-system-xtensa.worker.js",
      data:"qemu-system-xtensa.data",bootloader:"bootloader.bin",partitionTable:"partition-table.bin",otaData:"ota_data_initial.bin",rom:"esp32s3_rev0_rom.bin"};
    for (const [role,name] of Object.entries(roles)) {
      const entry = artifacts.get(name);
      if (!entry && role === "data") continue;
      if (!entry) fail(`missing_${name}`);
      const original = readVerifiedArtifact(qemuDir,entry,role).data;
      const bytes = role === "script" ? Buffer.from(patchQemuScriptForBrowserWorker(original.toString("utf8"))) : original;
      qemu[role]=stageBytes(runtimeDir,name,bytes);
    }
    const workerScript=stageBytes(runtimeDir,"browser-simulator-worker.js",readBounded(join(ROOT,"public/simulator-runtime/browser-simulator-worker.js")));
    const sdStoreScript=stageBytes(runtimeDir,"sd-card-store.js",readBounded(join(ROOT,"public/simulator-runtime/sd-card-store.js")));
    if (options.sdImage) {
      const packed = readBounded(resolve(options.sdImage));
      const maxRaw = options.sdRawBytes ?? 128*1024*1024;
      if (!Number.isSafeInteger(maxRaw) || maxRaw<512 || maxRaw>MAX_BYTES) fail("sd_raw_limit");
      const raw = packed[0]===0x1f && packed[1]===0x8b ? gunzipSync(packed,{maxOutputLength:maxRaw}) : packed;
      if (raw.length!==maxRaw) fail("sd_raw_size_mismatch");
      const compressed = packed[0]===0x1f && packed[1]===0x8b ? packed : gzipSync(packed);
      const partBytes=24*1024*1024;
      if (compressed.length>25*1024*1024) {
        qemu.sdImage=[];
        for (let offset=0,index=0;offset<compressed.length;offset+=partBytes,index++) {
          qemu.sdImage.push(stageBytes(runtimeDir,`sdcard.img.gz.part${String(index).padStart(2,"0")}`,compressed.subarray(offset,offset+partBytes)));
        }
      } else qemu.sdImage=stageBytes(runtimeDir,"sdcard.img.gz",compressed);
      qemu.sdRawBytes=maxRaw;
    }
    const guests=[];
    for (const profile of registry.boards) {
      const requested = mapped.get(profile.id);
      const alias = requested?.murphyBoard === "mofei" ? "default" : requested?.murphyBoard;
      const board = source.boards[profile.id] || (alias==="default" ? source.boards[registry.defaultBoard] : null);
      if (!board || board.status !== "ready") continue;
      if (requested && alias !== board.buildBoard) fail(`board_mapping_${profile.id}`);
      if (board.framebuffer?.width!==profile.framebufferWidth || board.framebuffer?.height!==profile.framebufferHeight || board.framebuffer?.format!==profile.framebufferFormat) fail(`framebuffer_${profile.id}`);
      const guest={id:profile.id};
      if (board.bootArtifacts) {
        for (const role of ["bootloader","partitionTable","otaData"]) {
          const entry=validArtifact(board.bootArtifacts[role]);
          guest[role]=stageBytes(guestDir,`${profile.id}-${role}.bin`,readVerifiedArtifact(qemuDir,entry,role).data);
        }
      }
      const bytesByRole={};
      for (const [role,field] of Object.entries({firmware:"bin",kernel:"kernel",symbols:"symbols"})) {
        const entry=validArtifact(board.firmware?.[field]);
        const bytes=readVerifiedArtifact(qemuDir,entry,role).data;
        bytesByRole[role]=bytes;
        guest[role]=stageBytes(guestDir,`${profile.id}-${role}`,bytes);
      }
      const prov=board.provenance;
      if (!prov || prov.sourceRevision!==source.sourceRevision || prov.board!==board.buildBoard ||
          prov.publishedBinSha256!==sha(bytesByRole.firmware) || prov.sourceBinSha256!==sha(bytesByRole.firmware) ||
          prov.publishedKernelSha256!==sha(bytesByRole.kernel) || prov.publishedSymbolsSha256!==sha(bytesByRole.symbols) ||
          !/^[a-f0-9]{64}$/.test(prov.sourceElfSha256)) fail(`guest_provenance_${profile.id}`);
      guest.provenance=stageBytes(guestDir,`${profile.id}-provenance.json`,Buffer.from(JSON.stringify(prov)));
      guests.push(guest);
    }
    if (!guests.length) fail("no_ready_guest");
    if (options.requireBoards) {
      const expected = options.requireBoards === "all" ? registry.boards.map(b => b.id) : options.requireBoards.split(",");
      if (!expected.length || new Set(expected).size !== expected.length || expected.some(id => !registry.boards.some(b => b.id === id))) fail("invalid_required_boards");
      const missing = expected.filter(id => !guests.some(guest => guest.id === id));
      if (missing.length) fail(`required_boards_missing:${missing.join(",")}`);
    }
    const input={schemaVersion:1,...(options.basePath ? {basePath:options.basePath}:{}),source:{revision:source.sourceRevision,label:"verified-existing-runtime"},
      publication:{visibility:"private"},runtime:{workerScript,sdStoreScript,qemu},guests};
    const manifestPath=join(work,"input-manifest.json");
    writeFileSync(manifestPath,JSON.stringify(input));
    return assembleWebArtifacts({manifestPath,runtimeDir,guestDir,uiDir:options.uiDir,outputDir:options.outputDir});
  } finally { rmSync(work,{recursive:true,force:true}); }
}
function cli(argv) {
  const opts={};
  const keys={"--qemu-dir":"qemuDir","--ui-dir":"uiDir","--output-dir":"outputDir","--board-map":"boardMap","--sd-image":"sdImage","--sd-raw-bytes":"sdRawBytes","--base-path":"basePath","--require-boards":"requireBoards"};
  if (argv.length===1 && argv[0]==="--help") {
    console.log("pack-existing-runtime --qemu-dir DIR --ui-dir DIST --output-dir NEW_OR_OWNED_DIR [--board-map JSON] [--require-boards all|id,id] [--sd-image FILE --sd-raw-bytes N]");
    return;
  }
  for (let i=0;i<argv.length;i+=2) {
    const key=keys[argv[i]],value=argv[i+1];
    if (!key || !value || value.startsWith("--") || key in opts) fail("cli_arguments");
    opts[key]=key==="sdRawBytes" ? Number(value) : value;
  }
  const result=packageExistingRuntime(opts);
  console.log(JSON.stringify({status:"private-package-written",...result,guestBootQualified:false}));
}
if (process.argv[1] && resolve(process.argv[1])===fileURLToPath(import.meta.url)) {
  try {cli(process.argv.slice(2));}
  catch (error) {console.error(String(error.message ?? error));process.exitCode=1;}
}
