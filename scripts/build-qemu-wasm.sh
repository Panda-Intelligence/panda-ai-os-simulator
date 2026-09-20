#!/usr/bin/env bash
# build-qemu-wasm.sh — Build the browser QEMU/WASM runtime for Panda AI Cloud.
#
# This is intentionally separate from scripts/build-qemu.sh. The native Tauri
# simulator uses espressif/qemu plus a Unix-socket chardev. The browser runtime
# starts from ktock/qemu-wasm and overlays the same reviewed ESP32-S3/Mofei
# source mirrors before compiling xtensa-softmmu with Emscripten.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

QEMU_WASM_REPO_URL="${QEMU_WASM_REPO_URL:-https://github.com/ktock/qemu-wasm}"
QEMU_WASM_REF="${QEMU_WASM_REF:-dev-wasm-j}"
QEMU_WASM_CACHE_DIR="${QEMU_WASM_CACHE_DIR:-${SIM_ROOT}/.qemu-wasm-cache}"
QEMU_WASM_SRC_DIR="${QEMU_WASM_SRC_DIR:-${QEMU_WASM_CACHE_DIR}/qemu-wasm}"
QEMU_WASM_OFFLINE="${QEMU_WASM_OFFLINE:-0}"
QEMU_WASM_BUILD_DIR="${QEMU_WASM_BUILD_DIR:-/build}"
QEMU_WASM_SCRIPT_MOUNT="${QEMU_WASM_SCRIPT_MOUNT:-/murphy-scripts}"
QEMU_WASM_CONTAINER="${QEMU_WASM_CONTAINER:-murphy-qemu-wasm-build}"
QEMU_WASM_IMAGE="${QEMU_WASM_IMAGE:-murphy-qemu-wasm:${QEMU_WASM_REF}}"
QEMU_WASM_BASE_IMAGE="${QEMU_WASM_BASE_IMAGE:-${QEMU_WASM_IMAGE}-base}"
NATIVE_QEMU_DIR="${NATIVE_QEMU_DIR:-${SIM_ROOT}/.qemu-cache/qemu}"
OUTPUT_DIR="${QEMU_WASM_OUTPUT_DIR:-${SIM_ROOT}/public/simulator-runtime/qemu}"
BOARD="${BOARD:-default}"
QEMU_WASM_PROFILE="${QEMU_WASM_PROFILE:-${PANDA_PRODUCT_PROFILE:-default}}"
QEMU_WASM_PROFILE_PLAN="${QEMU_WASM_PROFILE_PLAN:-}"
QEMU_WASM_PROFILE_DIGEST="${QEMU_WASM_PROFILE_DIGEST:-}"
QEMU_WASM_PROFILE_REVISION="${QEMU_WASM_PROFILE_REVISION:-}"
QEMU_WASM_OTA_NAMESPACE="${QEMU_WASM_OTA_NAMESPACE:-${QEMU_WASM_PROFILE}}"
QEMU_WASM_MALLOC="${QEMU_WASM_MALLOC:-emmalloc}"
if [ "${BOARD}" = "default" ]; then
  FIRMWARE_SUFFIX=""
else
  FIRMWARE_SUFFIX="-${BOARD}"
fi
FIRMWARE_INPUT_DIR="${QEMU_WASM_FIRMWARE_DIR:-}"
ARTIFACT_MANIFEST="${OUTPUT_DIR}/qemu-wasm-artifacts.json"
JOBS="${QEMU_WASM_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
QEMU_WASM_RUNTIME_REVISION="asyncify-stack-v4-papers3-sdspi"
export QEMU_WASM_RUNTIME_REVISION
CONTAINER_STARTED=0
CONTAINER_ID=""

RUNTIME_JS="${OUTPUT_DIR}/qemu-system-xtensa.js"
RUNTIME_WASM="${OUTPUT_DIR}/qemu-system-xtensa.wasm"
RUNTIME_WORKER="${OUTPUT_DIR}/qemu-system-xtensa.worker.js"
RUNTIME_DATA="${OUTPUT_DIR}/qemu-system-xtensa.data"
RUNTIME_LOAD_JS="${OUTPUT_DIR}/load.js"
RUNTIME_FIRMWARE_BIN="${OUTPUT_DIR}/firmware${FIRMWARE_SUFFIX}.bin"
RUNTIME_FIRMWARE_KERNEL="${OUTPUT_DIR}/firmware${FIRMWARE_SUFFIX}-kernel.img"
RUNTIME_SYMBOLS_TXT="${OUTPUT_DIR}/firmware${FIRMWARE_SUFFIX}-symbols.txt"
RUNTIME_PROVENANCE_JSON="${OUTPUT_DIR}/firmware${FIRMWARE_SUFFIX}-provenance.json"
RUNTIME_ROM_BIN="${OUTPUT_DIR}/esp32s3_rev0_rom.bin"
RUNTIME_BOOTLOADER_BIN="${OUTPUT_DIR}/bootloader${FIRMWARE_SUFFIX}.bin"
RUNTIME_PARTITION_TABLE_BIN="${OUTPUT_DIR}/partition-table${FIRMWARE_SUFFIX}.bin"
RUNTIME_OTA_DATA_BIN="${OUTPUT_DIR}/ota_data_initial${FIRMWARE_SUFFIX}.bin"

usage() {
  cat <<EOF
Usage:
  scripts/build-qemu-wasm.sh [--force] [--probe-only] [--no-build]

Environment:
  QEMU_WASM_REF          ktock/qemu-wasm ref (default: dev-wasm-j)
  QEMU_WASM_OFFLINE      reuse the existing qemu-wasm checkout without fetching (default: 0)
  QEMU_WASM_IMAGE        Docker image name (default: murphy-qemu-wasm:<ref>)
  QEMU_WASM_CONTAINER    Docker container name (default: murphy-qemu-wasm-build)
  QEMU_WASM_OUTPUT_DIR   Artifact output dir (default: public/simulator-runtime/qemu)
  BOARD                  Panda AI OS board value for firmware naming (default: default)
                         Non-default boards write firmware-<board>.bin; default writes firmware.bin
  QEMU_WASM_FIRMWARE_BUILD_DIR
                         Panda AI OS simulator build dir used for firmware refresh
                         (default: standalone firmware build directory)
  QEMU_WASM_FIRMWARE_DIR
                         Coherent firmware artifact directory. The directory must contain
                         panda_os.bin, panda_os.elf, bootloader/bootloader.bin,
                         partition_table/partition-table.bin, and ota_data_initial.bin.
  QEMU_WASM_FIRMWARE_BIN Panda AI OS simulator app binary override
  QEMU_WASM_FIRMWARE_ELF Panda AI OS simulator ELF override used for kernel/symbol sidecars
  QEMU_WASM_REQUIRED_BOARDS
                         Space-separated build-board list retained in the artifact manifest
  QEMU_WASM_PROFILE     Product profile (default or latin; default: default)
  QEMU_WASM_PROFILE_PLAN
                         Resolved release profile JSON used for provenance identity
  QEMU_WASM_MALLOC      Emscripten allocator (emmalloc, dlmalloc, or mimalloc; default: emmalloc)
  NATIVE_QEMU_DIR        Native espressif/qemu checkout used as ESP32-S3 source overlay
  --force                Rebuild qemu-wasm Docker images and runtime artifacts even when cached
  --firmware-only        Refresh firmware[.<board>].bin, firmware[.<board>]-kernel.img,
                         firmware[.<board>]-symbols.txt, and qemu-wasm-artifacts.json without
                         rebuilding qemu-wasm
  --firmware-dir=DIR     Use one explicit coherent firmware artifact directory with --firmware-only
  --runtime-only        Build runtime independently; output must be a new directory
  QEMU_WASM_LINK_DEBUG   1 emits names and source map for diagnosis (default0)

Output:
  ${OUTPUT_DIR}/qemu-system-xtensa.js
  ${OUTPUT_DIR}/qemu-system-xtensa.wasm
  ${OUTPUT_DIR}/qemu-system-xtensa.worker.js
  ${OUTPUT_DIR}/firmware${FIRMWARE_SUFFIX}.bin
  ${OUTPUT_DIR}/firmware${FIRMWARE_SUFFIX}-kernel.img
  ${OUTPUT_DIR}/firmware${FIRMWARE_SUFFIX}-symbols.txt
  ${OUTPUT_DIR}/firmware${FIRMWARE_SUFFIX}-provenance.json
  ${OUTPUT_DIR}/bootloader${FIRMWARE_SUFFIX}.bin
  ${OUTPUT_DIR}/partition-table${FIRMWARE_SUFFIX}.bin
  ${OUTPUT_DIR}/ota_data_initial${FIRMWARE_SUFFIX}.bin
  ${OUTPUT_DIR}/esp32s3_rev0_rom.bin
  ${OUTPUT_DIR}/qemu-wasm-artifacts.json
EOF
}

FORCE=0
PROBE_ONLY=0
NO_BUILD=0
FIRMWARE_ONLY=0
RUNTIME_ONLY=0
for arg in "$@"; do
  case "${arg}" in
    --force) FORCE=1 ;;
    --firmware-only) FIRMWARE_ONLY=1 ;;
    --runtime-only) RUNTIME_ONLY=1 ;;
    --probe-only) PROBE_ONLY=1 ;;
    --no-build) NO_BUILD=1 ;;
    --firmware-dir=*) FIRMWARE_INPUT_DIR="${arg#*=}" ;;
    -h|--help) usage; exit 0 ;;
    *) echo "[build-qemu-wasm] unknown flag: ${arg}" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "${RUNTIME_ONLY}" -eq 0 || "${FIRMWARE_ONLY}" -eq 0 ]] || { echo "--runtime-only and --firmware-only are mutually exclusive" >&2; exit 2; }

DEFAULT_FIRMWARE_BUILD_DIR="${QEMU_WASM_FIRMWARE_BUILD_DIR:-${FIRMWARE_INPUT_DIR:-${SIM_ROOT}/firmware}}"
DEFAULT_FIRMWARE_BIN="${QEMU_WASM_FIRMWARE_BIN:-${DEFAULT_FIRMWARE_BUILD_DIR}/panda_os.bin}"
DEFAULT_FIRMWARE_ELF="${QEMU_WASM_FIRMWARE_ELF:-${DEFAULT_FIRMWARE_BUILD_DIR}/panda_os.elf}"
DEFAULT_FIRMWARE_BOOTLOADER="${QEMU_WASM_FIRMWARE_BOOTLOADER:-${DEFAULT_FIRMWARE_BUILD_DIR}/bootloader/bootloader.bin}"
DEFAULT_FIRMWARE_PARTITION_TABLE="${QEMU_WASM_FIRMWARE_PARTITION_TABLE:-${DEFAULT_FIRMWARE_BUILD_DIR}/partition_table/partition-table.bin}"
DEFAULT_FIRMWARE_OTA_DATA="${QEMU_WASM_FIRMWARE_OTA_DATA:-${DEFAULT_FIRMWARE_BUILD_DIR}/ota_data_initial.bin}"

step() { printf '[build-qemu-wasm] %s\n' "$*"; }
die() { printf '[build-qemu-wasm] ERROR: %s\n' "$*" >&2; exit 1; }

validate_wasm_allocator() {
  case "${QEMU_WASM_MALLOC}" in
    emmalloc|dlmalloc|mimalloc) ;;
    *) die "QEMU_WASM_MALLOC must be emmalloc, dlmalloc, or mimalloc (got ${QEMU_WASM_MALLOC})" ;;
  esac
}

validate_wasm_allocator

require_tool() {
  command -v "$1" >/dev/null 2>&1 || die "$1 is required"
}

copy_if_present() {
  local source="$1"
  local target="$2"
  [[ -f "${source}" ]] || return 0
  mkdir -p "$(dirname "${target}")"
  cp -f "${source}" "${target}"
}

find_firmware_nm_tool() {
  local tool
  for tool in xtensa-esp32s3-elf-nm xtensa-esp32-elf-nm; do
    if command -v "${tool}" >/dev/null 2>&1; then
      command -v "${tool}"
      return 0
    fi
  done

  if [[ -d "${HOME}/.espressif/tools" ]]; then
    local esp_candidate
    esp_candidate="$(find "${HOME}/.espressif/tools" -type f \( -name xtensa-esp32s3-elf-nm -o -name xtensa-esp32-elf-nm \) 2>/dev/null | sort -V | tail -n 1)"
    if [[ -n "${esp_candidate}" ]]; then
      printf '%s\n' "${esp_candidate}"
      return 0
    fi
  fi

  return 1
}

find_firmware_strip_tool() {
  local tool
  for tool in xtensa-esp32s3-elf-strip xtensa-esp32-elf-strip; do
    if command -v "${tool}" >/dev/null 2>&1; then
      command -v "${tool}"
      return 0
    fi
  done

  if [[ -d "${HOME}/.espressif/tools" ]]; then
    local esp_candidate
    esp_candidate="$(find "${HOME}/.espressif/tools" -type f \( -name xtensa-esp32s3-elf-strip -o -name xtensa-esp32-elf-strip \) 2>/dev/null | sort -V | tail -n 1)"
    if [[ -n "${esp_candidate}" ]]; then
      printf '%s\n' "${esp_candidate}"
      return 0
    fi
  fi

  return 1
}

generate_firmware_kernel() {
  local firmware_elf="$1"
  local target="$2"
  [[ -f "${firmware_elf}" ]] || {
    step "default firmware ELF missing: ${firmware_elf}"
    return 0
  }

  local strip_tool
  strip_tool="$(find_firmware_strip_tool)" || die "xtensa firmware strip tool missing; cannot generate firmware-kernel.img"

  step "generating browser qemu kernel image from ${firmware_elf}"
  mkdir -p "$(dirname "${target}")"
  # 生成物不应携带源码/构建目录的 macOS provenance 扩展属性；避免 fcopyfile 在临界空间时复制元数据失败。
  cp -fX "${firmware_elf}" "${target}"
  "${strip_tool}" --strip-all "${target}"
}

generate_firmware_symbols() {
  local firmware_elf="$1"
  local target="$2"
  [[ -f "${firmware_elf}" ]] || {
    step "default firmware ELF missing: ${firmware_elf}"
    return 0
  }

  local nm_tool
  nm_tool="$(find_firmware_nm_tool)" || {
    step "xtensa firmware nm tool missing; cannot generate firmware-symbols.txt"
    return 0
  }

  step "generating compact firmware symbols from ${firmware_elf}"
  mkdir -p "$(dirname "${target}")"
  python3 - "${target}" "${firmware_elf}" "${nm_tool}" <<'PY'
import pathlib
import re
import subprocess
import sys

target = pathlib.Path(sys.argv[1])
firmware_elf = pathlib.Path(sys.argv[2])
nm_tool = sys.argv[3]
symbol_re = re.compile(r"^([0-9a-fA-F]+)(?:\s+([0-9a-fA-F]+))?\s+([A-Za-z?])\s+(.+)$")
lines = [
    "# murphy-reader qemu-wasm firmware symbols",
    "# format: <hex-address> <hex-size> <type> <name>",
]
result = subprocess.run(
    [nm_tool, "-S", "--defined-only", str(firmware_elf)],
    check=True,
    text=True,
    stdout=subprocess.PIPE,
)
for raw_line in result.stdout.splitlines():
    line = raw_line.strip()
    match = symbol_re.match(line)
    if not match:
        continue
    address, size, symbol_type, name = match.groups()
    if not name or name.startswith("."):
        continue
    lines.append(f"{int(address, 16):08x} {int(size or '0', 16):08x} {symbol_type} {name}")

target.write_text("\n".join(lines) + "\n", encoding="utf-8")
PY
}

write_firmware_provenance() {
  local source_revision
  source_revision="${QEMU_WASM_SOURCE_REVISION:-$(git -C "${SIM_ROOT}" rev-parse HEAD 2>/dev/null || printf 'standalone-unversioned')}"
  python3 - \
    "${RUNTIME_PROVENANCE_JSON}" \
    "${BOARD}" \
    "${source_revision}" \
    "${DEFAULT_FIRMWARE_BIN}" \
    "${DEFAULT_FIRMWARE_ELF}" \
    "${RUNTIME_FIRMWARE_BIN}" \
    "${RUNTIME_FIRMWARE_KERNEL}" \
    "${RUNTIME_SYMBOLS_TXT}" \
    "${QEMU_WASM_PROFILE}" \
    "${QEMU_WASM_PROFILE_PLAN}" \
    "${QEMU_WASM_PROFILE_DIGEST}" \
    "${QEMU_WASM_PROFILE_REVISION}" \
    "${QEMU_WASM_OTA_NAMESPACE}" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

target = Path(sys.argv[1])
board = sys.argv[2]
revision = sys.argv[3]
source_bin = Path(sys.argv[4])
source_elf = Path(sys.argv[5])
published_bin = Path(sys.argv[6])
published_kernel = Path(sys.argv[7])
published_symbols = Path(sys.argv[8])
profile_id, profile_plan_path, profile_digest, profile_revision, ota_namespace = sys.argv[9:14]
profile = {
    "id": profile_id,
    "revision": int(profile_revision) if profile_revision else 0,
    "digest": profile_digest,
    "namespace": ota_namespace,
    "sourceRevision": revision,
}
if profile_plan_path:
    plan = json.loads(Path(profile_plan_path).read_text(encoding="utf-8"))
    plan_profile = plan.get("profile", {})
    profile = {
        "id": plan_profile.get("id", profile_id),
        "revision": plan_profile.get("revision", profile["revision"]),
        "digest": plan.get("profileContractSha256", profile_digest),
        "namespace": plan_profile.get("otaNamespace", ota_namespace),
        "sourceRevision": plan.get("sourceRevision", revision),
    }

def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

payload = {
    "schemaVersion": 2 if profile["id"] != "default" else 1,
    "board": str(board),
    "sourceRevision": str(profile["sourceRevision"]),
    "profile": profile,
    "sourceBinSha256": sha256(source_bin),
    "sourceElfSha256": sha256(source_elf),
    "publishedBinSha256": sha256(published_bin),
    "publishedKernelSha256": sha256(published_kernel),
    "publishedSymbolsSha256": sha256(published_symbols),
}
target.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
PY
}

write_firmware_manifest() {
  local required_boards_text="${QEMU_WASM_REQUIRED_BOARDS:-${BOARD}}"
  local -a required_boards
  read -r -a required_boards <<<"${required_boards_text}"
  [[ "${#required_boards[@]}" -gt 0 ]] || die "QEMU_WASM_REQUIRED_BOARDS must name at least one board"

  local manifest_allocator="${QEMU_WASM_MALLOC}"
  local source_revision="${QEMU_WASM_SOURCE_REVISION:-$(git -C "${SIM_ROOT}" rev-parse HEAD 2>/dev/null || printf 'standalone-unversioned')}"
  local allow_missing=0
  if [[ "${FIRMWARE_ONLY}" -eq 1 && -s "${ARTIFACT_MANIFEST}" ]]; then
    # Firmware-only refresh keeps the existing runtime allocator identity.
    manifest_allocator="$(node -e '
      const fs = require("node:fs");
      try {
        const manifest = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
        process.stdout.write(typeof manifest.wasmAllocator === "string" ? manifest.wasmAllocator : "");
      } catch {
        process.stdout.write("");
      }
    ' "${ARTIFACT_MANIFEST}")"
  fi

  if [[ -n "${QEMU_WASM_REQUIRED_BOARDS:-}" || "${BOARD}" != "default" ]]; then
    allow_missing=1
  fi
  node --input-type=module - \
    "${OUTPUT_DIR}" "${ARTIFACT_MANIFEST}" "${manifest_allocator}" "${source_revision}" \
    "${SIM_ROOT}/boards.json" "${allow_missing}" "${required_boards[@]}" <<'NODE'
import { createHash } from "node:crypto";
import { existsSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { join, relative, resolve } from "node:path";

const [outputDirArg, manifestPathArg, allocator, sourceRevision, registryPath, allowMissing, ...requiredBuildBoards] = process.argv.slice(2);
const outputDir = resolve(outputDirArg);
const manifestPath = resolve(manifestPathArg);
const registry = JSON.parse(readFileSync(registryPath, "utf8"));
const requiredBoards = [...new Set(requiredBuildBoards.length ? requiredBuildBoards : ["default"])]
  .filter((value) => value && !value.startsWith("--"));
const artifactInfo = (fileName) => {
  const path = join(outputDir, fileName);
  if (!existsSync(path) || !statSync(path).isFile() || statSync(path).size === 0) return null;
  return { fileName, relativePath: relative(outputDir, path), bytes: statSync(path).size,
    sha256: createHash("sha256").update(readFileSync(path)).digest("hex") };
};
const sharedNames = ["qemu-system-xtensa.js", "qemu-system-xtensa.wasm", "bootloader.bin",
  "partition-table.bin", "ota_data_initial.bin", "esp32s3_rev0_rom.bin"];
const sharedArtifacts = Object.fromEntries(sharedNames.map((name) => [name, artifactInfo(name)]).filter(([, value]) => value));
const missingRequired = sharedNames.filter((name) => !sharedArtifacts[name]);
const boards = {};
for (const buildBoard of requiredBoards) {
  const profile = buildBoard === "default"
    ? registry.boards.find((board) => board.id === "mofei")
    : registry.boards.find((board) => board.id === buildBoard);
  if (!profile) {
    missingRequired.push(`${buildBoard}: board registry profile is missing`);
    continue;
  }
  const suffix = buildBoard === "default" ? "" : `-${buildBoard}`;
  const names = { bin: `firmware${suffix}.bin`, kernel: `firmware${suffix}-kernel.img`, symbols: `firmware${suffix}-symbols.txt`, provenance: `firmware${suffix}-provenance.json` };
  const firmware = Object.fromEntries(Object.entries(names).slice(0, 3).map(([kind, name]) => [kind, artifactInfo(name)]));
  const missing = Object.entries(firmware).filter(([, value]) => !value).map(([kind]) => kind);
  const provenancePath = join(outputDir, names.provenance);
  const provenance = existsSync(provenancePath) ? JSON.parse(readFileSync(provenancePath, "utf8")) : null;
  if (!provenance) missing.push("provenance");
  if (missing.length) missingRequired.push(`${buildBoard}: missing ${missing.join(", ")}`);
  const boardId = buildBoard === "default" ? "mofei" : profile.id;
  boards[boardId] = { status: missing.length === 0 ? "ready" : "missing", buildBoard,
    framebuffer: { width: profile.framebufferWidth, height: profile.framebufferHeight, format: profile.framebufferFormat },
    firmware, provenance };
}
const status = missingRequired.length === 0 ? "ready" : "missing";
if (status !== "ready" && allowMissing !== "1") {
  throw new Error(`Missing required qemu-wasm artifacts: ${missingRequired.join(" | ")}`);
}
writeFileSync(manifestPath, `${JSON.stringify({ schemaVersion: 2, generatedAt: new Date().toISOString(),
  source: "ktock/qemu-wasm + standalone simulator overlay", sourceRevision, target: "xtensa-softmmu", status,
  ...(allocator ? { wasmAllocator: allocator } : {}), requiredBuildBoards: requiredBoards,
  missingRequired, artifacts: Object.values(sharedArtifacts), boards }, null, 2)}\n`);
NODE
}

copy_tree_if_present() {
  local source="$1"
  local target="$2"
  [[ -d "${source}" ]] || return 0
  mkdir -p "${target}"
  cp -R "${source}/." "${target}/"
}

copy_matching_if_present() {
  local source_dir="$1"
  local target_dir="$2"
  local pattern="$3"
  [[ -d "${source_dir}" ]] || return 0
  mkdir -p "${target_dir}"
  find "${source_dir}" -maxdepth 1 -type f -name "${pattern}" -exec cp -f {} "${target_dir}/" \;
}

patch_file_if_present() {
  local file="$1"
  local search="$2"
  local replacement="$3"
  [[ -f "${file}" ]] || return 0
  python3 - "$file" "$search" "$replacement" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
search = sys.argv[2]
replacement = sys.argv[3]
text = path.read_text()
if search not in text:
    sys.exit(0)
path.write_text(text.replace(search, replacement))
PY
}

patch_python_if_present() {
  local file="$1"
  local script="$2"
  [[ -f "${file}" ]] || return 0
  python3 - "$file" "$script" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
script = sys.argv[2]
text = path.read_text()
updated = text
namespace = {"text": text}
exec(script, namespace)
updated = namespace.get("text", updated)
if updated != text:
    path.write_text(updated)
PY
}

append_if_missing() {
  local file="$1"
  local sentinel="$2"
  local fragment="$3"
  [[ -f "${file}" ]] || return 0
  if grep -qF "${sentinel}" "${file}"; then
    return 0
  fi
  printf '\n' >> "${file}"
  cat "${fragment}" >> "${file}"
}

append_line_if_missing() {
  local file="$1"
  local sentinel="$2"
  local line="$3"
  [[ -f "${file}" ]] || return 0
  if grep -qF "${sentinel}" "${file}"; then
    return 0
  fi
  printf '\n%s\n' "${line}" >> "${file}"
}

append_text_if_missing() {
  local file="$1"
  local sentinel="$2"
  local text="$3"
  [[ -f "${file}" ]] || return 0
  if grep -qF "${sentinel}" "${file}"; then
    return 0
  fi
  printf '\n%s\n' "${text}" >> "${file}"
}

append_trace_events_block_if_missing() {
  local source="$1"
  local target="$2"
  local header="$3"
  [[ -f "${source}" && -f "${target}" ]] || return 0
  if grep -qF "${header}" "${target}"; then
    return 0
  fi
  python3 - "$source" "$target" "$header" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
target = pathlib.Path(sys.argv[2])
header = sys.argv[3]
lines = source.read_text().splitlines()
block = []
capturing = False
for line in lines:
    if line.strip() == header:
        capturing = True
    elif capturing and line.startswith("# ") and block:
        break
    if capturing:
        block.append(line)

if not block:
    raise SystemExit(f"trace-events block not found: {header}")

text = target.read_text()
separator = "" if text.endswith("\n") else "\n"
target.write_text(f"{text}{separator}\n{chr(10).join(block).rstrip()}\n")
PY
}

ensure_checkout() {
  require_tool git
  mkdir -p "${QEMU_WASM_CACHE_DIR}"
  if [[ ! -d "${QEMU_WASM_SRC_DIR}/.git" ]]; then
    [[ "${QEMU_WASM_OFFLINE}" != "1" ]] || die "QEMU_WASM_OFFLINE=1 but qemu-wasm checkout is missing at ${QEMU_WASM_SRC_DIR}"
    step "cloning ${QEMU_WASM_REPO_URL} (${QEMU_WASM_REF})"
    git clone --depth 1 --branch "${QEMU_WASM_REF}" "${QEMU_WASM_REPO_URL}" "${QEMU_WASM_SRC_DIR}"
    return
  fi

  step "reusing ${QEMU_WASM_SRC_DIR}"
  if [[ "${QEMU_WASM_OFFLINE}" == "1" ]]; then
    step "offline mode enabled; resetting cached qemu-wasm checkout without fetching"
    git -C "${QEMU_WASM_SRC_DIR}" reset --hard HEAD
    git -C "${QEMU_WASM_SRC_DIR}" clean -fdx
    return
  fi
  if ! git -C "${QEMU_WASM_SRC_DIR}" remote get-url origin >/dev/null 2>&1; then
    step "cached qemu-wasm checkout has no origin remote; resetting existing HEAD without fetching"
    git -C "${QEMU_WASM_SRC_DIR}" reset --hard HEAD
    git -C "${QEMU_WASM_SRC_DIR}" clean -fdx
    return
  fi
  git -C "${QEMU_WASM_SRC_DIR}" fetch --depth 1 origin "${QEMU_WASM_REF}"
  git -C "${QEMU_WASM_SRC_DIR}" checkout --detach FETCH_HEAD
  git -C "${QEMU_WASM_SRC_DIR}" reset --hard FETCH_HEAD
  git -C "${QEMU_WASM_SRC_DIR}" clean -fdx
}

assert_qemu_wasm_base() {
  [[ -f "${QEMU_WASM_SRC_DIR}/tcg/wasm32.c" ]] || die "qemu-wasm TCG backend missing: tcg/wasm32.c"
  [[ -f "${QEMU_WASM_SRC_DIR}/tcg/wasm32/tcg-target.c.inc" ]] || die "qemu-wasm TCG backend missing: tcg/wasm32/tcg-target.c.inc"
  [[ -f "${QEMU_WASM_SRC_DIR}/configs/targets/xtensa-softmmu.mak" ]] || die "qemu-wasm xtensa-softmmu target missing"
  grep -q "TARGET_ARCH=xtensa" "${QEMU_WASM_SRC_DIR}/configs/targets/xtensa-softmmu.mak" || \
    die "qemu-wasm xtensa-softmmu target is malformed"
}

patch_qemu_wasm_build_inputs() {
  local dockerfile="${QEMU_WASM_SRC_DIR}/tests/docker/dockerfiles/emsdk-wasm32-cross.docker"
  local meson_build="${QEMU_WASM_SRC_DIR}/meson.build"

  step "patching qemu-wasm build inputs for stable source archives"
  patch_file_if_present "${dockerfile}" \
    'https://zlib.net/zlib-$ZLIB_VERSION.tar.xz' \
    'https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.xz'

  QEMU_WASM_SCRIPT_MOUNT="${QEMU_WASM_SCRIPT_MOUNT}" patch_python_if_present "${meson_build}" '
import os

script_mount = os.environ.get("QEMU_WASM_SCRIPT_MOUNT", "/murphy-scripts").rstrip("/")
bridge_arg = f"--js-library={script_mount}/qemu-wasm-mofei-bridge.js"
needle = """elif host_os == '\''emscripten'\''
  if get_option('\''optimization'\'') != '\''plain'\''
    emulator_link_args += ['\''-O'\'' + get_option('\''optimization'\'')]
  endif
endif
"""
replacement = """elif host_os == '\''emscripten'\''
  if get_option('\''optimization'\'') != '\''plain'\''
    emulator_link_args += ['\''-O'\'' + get_option('\''optimization'\'')]
  endif
  emulator_link_args += [
    '\''--js-library=/build/node_modules/xterm-pty/emscripten-pty.js'\'',
    '\''--js-library=/murphy-scripts/qemu-wasm-mofei-bridge.js'\'',
    '\''-sMODULARIZE=1'\'',
    '\''-sEXPORT_ES6=1'\'',
    '\''-sMALLOC=__PANDA_WASM_MALLOC__'\'',
    '\''-sEXPORTED_RUNTIME_METHODS=getTempRet0,setTempRet0,addFunction,removeFunction,TTY,FS,callMain'\'',
    '\''-sEXPORTED_FUNCTIONS=_main,_malloc,_free,_mofei_wasm_inject_touch,_mofei_wasm_inject_button,_mofei_wasm_release_buttons,_mofei_wasm_i2c_control'\'',
  ]
endif
"""
replacement = replacement.replace("--js-library=/murphy-scripts/qemu-wasm-mofei-bridge.js", bridge_arg)
if bridge_arg not in text:
    text = text.replace(needle, replacement)
'
  QEMU_WASM_MALLOC="${QEMU_WASM_MALLOC}" patch_python_if_present "${meson_build}" '
import os
import re

allocator = os.environ.get("QEMU_WASM_MALLOC", "")
if not allocator:
    raise SystemExit("QEMU_WASM_MALLOC is not set")
text = re.sub(r"-sMALLOC=(?:mimalloc|dlmalloc|emmalloc|__PANDA_WASM_MALLOC__)", f"-sMALLOC={allocator}", text)
'
}

overlay_espressif_sources() {
  [[ -d "${NATIVE_QEMU_DIR}" ]] || die "native espressif/qemu checkout missing at ${NATIVE_QEMU_DIR}; build the standalone native runtime first"

  step "overlaying ESP32-S3 machine sources from ${NATIVE_QEMU_DIR}"
  local rel
  local overlays=(
    "configs/devices/xtensa-softmmu/default.mak"
    "pc-bios/esp32s3_rev0_rom.bin"
  )

  for rel in "${overlays[@]}"; do
    copy_if_present "${NATIVE_QEMU_DIR}/${rel}" "${QEMU_WASM_SRC_DIR}/${rel}"
  done

  for rel in hw/char hw/display hw/dma hw/gpio hw/i2c hw/misc hw/net/can hw/nvram hw/sd hw/ssi hw/timer hw/xtensa hw/sensor; do
    copy_if_present "${NATIVE_QEMU_DIR}/${rel}/meson.build" "${QEMU_WASM_SRC_DIR}/${rel}/meson.build"
    copy_if_present "${NATIVE_QEMU_DIR}/${rel}/Kconfig" "${QEMU_WASM_SRC_DIR}/${rel}/Kconfig"
  done

  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/char" "${QEMU_WASM_SRC_DIR}/hw/char" "esp32*.c"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/display" "${QEMU_WASM_SRC_DIR}/hw/display" "esp_rgb.c"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/dma" "${QEMU_WASM_SRC_DIR}/hw/dma" "esp*.c"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/gpio" "${QEMU_WASM_SRC_DIR}/hw/gpio" "esp32*.c"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/i2c" "${QEMU_WASM_SRC_DIR}/hw/i2c" "esp32*.c"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/misc" "${QEMU_WASM_SRC_DIR}/hw/misc" "esp*.c"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/misc" "${QEMU_WASM_SRC_DIR}/hw/misc" "ssi_psram.c"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/net/can" "${QEMU_WASM_SRC_DIR}/hw/net/can" "esp32*.c"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/net/can" "${QEMU_WASM_SRC_DIR}/hw/net/can" "esp32*.h"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/nvram" "${QEMU_WASM_SRC_DIR}/hw/nvram" "esp*.c"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/sd" "${QEMU_WASM_SRC_DIR}/hw/sd" "dwc_sdmmc.c"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/ssi" "${QEMU_WASM_SRC_DIR}/hw/ssi" "esp32*.c"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/timer" "${QEMU_WASM_SRC_DIR}/hw/timer" "esp*.c"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/hw/xtensa" "${QEMU_WASM_SRC_DIR}/hw/xtensa" "esp32*.c"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/include/hw/char" "${QEMU_WASM_SRC_DIR}/include/hw/char" "esp32*.h"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/include/hw/display" "${QEMU_WASM_SRC_DIR}/include/hw/display" "esp_rgb.h"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/include/hw/dma" "${QEMU_WASM_SRC_DIR}/include/hw/dma" "esp*.h"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/include/hw/gpio" "${QEMU_WASM_SRC_DIR}/include/hw/gpio" "esp32*.h"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/include/hw/i2c" "${QEMU_WASM_SRC_DIR}/include/hw/i2c" "esp32*.h"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/include/hw/misc" "${QEMU_WASM_SRC_DIR}/include/hw/misc" "esp*.h"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/include/hw/misc" "${QEMU_WASM_SRC_DIR}/include/hw/misc" "ssi_psram.h"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/include/hw/nvram" "${QEMU_WASM_SRC_DIR}/include/hw/nvram" "esp*.h"
  copy_if_present "${NATIVE_QEMU_DIR}/include/hw/sd/sd.h" "${QEMU_WASM_SRC_DIR}/include/hw/sd/sd.h"
  copy_if_present "${NATIVE_QEMU_DIR}/include/hw/sd/sdcard_legacy.h" "${QEMU_WASM_SRC_DIR}/include/hw/sd/sdcard_legacy.h"
  copy_if_present "${NATIVE_QEMU_DIR}/include/hw/sd/dwc_sdmmc.h" "${QEMU_WASM_SRC_DIR}/include/hw/sd/dwc_sdmmc.h"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/include/hw/ssi" "${QEMU_WASM_SRC_DIR}/include/hw/ssi" "esp32*.h"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/include/hw/timer" "${QEMU_WASM_SRC_DIR}/include/hw/timer" "esp*.h"
  copy_matching_if_present "${NATIVE_QEMU_DIR}/include/hw/xtensa" "${QEMU_WASM_SRC_DIR}/include/hw/xtensa" "esp32*.h"
  copy_tree_if_present "${NATIVE_QEMU_DIR}/target/xtensa/core-esp32" "${QEMU_WASM_SRC_DIR}/target/xtensa/core-esp32"
  copy_tree_if_present "${NATIVE_QEMU_DIR}/target/xtensa/core-esp32s3" "${QEMU_WASM_SRC_DIR}/target/xtensa/core-esp32s3"
  copy_if_present "${NATIVE_QEMU_DIR}/target/xtensa/cores.list" "${QEMU_WASM_SRC_DIR}/target/xtensa/cores.list"
  copy_if_present "${NATIVE_QEMU_DIR}/target/xtensa/core-esp32.c" "${QEMU_WASM_SRC_DIR}/target/xtensa/core-esp32.c"
  copy_if_present "${NATIVE_QEMU_DIR}/target/xtensa/core-esp32s3.c" "${QEMU_WASM_SRC_DIR}/target/xtensa/core-esp32s3.c"
  copy_if_present "${NATIVE_QEMU_DIR}/target/xtensa/translate.h" "${QEMU_WASM_SRC_DIR}/target/xtensa/translate.h"
  copy_if_present "${NATIVE_QEMU_DIR}/target/xtensa/translate_tie_esp32s3.c" \
    "${QEMU_WASM_SRC_DIR}/target/xtensa/translate_tie_esp32s3.c"
  copy_if_present "${NATIVE_QEMU_DIR}/target/xtensa/cpu_esp32s3.h" "${QEMU_WASM_SRC_DIR}/target/xtensa/cpu_esp32s3.h"
  copy_if_present "${NATIVE_QEMU_DIR}/target/xtensa/meson.build" "${QEMU_WASM_SRC_DIR}/target/xtensa/meson.build"

  append_line_if_missing "${QEMU_WASM_SRC_DIR}/hw/xtensa/meson.build" "CONFIG_XTENSA_ESP32" \
    "xtensa_ss.add(when: 'CONFIG_XTENSA_ESP32', if_true: files('esp32.c', 'esp32_intc.c'))"
  if ! grep -qF "CONFIG_XTENSA_ESP32S3" "${QEMU_WASM_SRC_DIR}/hw/xtensa/meson.build"; then
    cat >> "${QEMU_WASM_SRC_DIR}/hw/xtensa/meson.build" <<'EOF'
xtensa_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files(
  'esp32s3_clk.c',
  'esp32s3_intc.c',
  'esp32s3.c',
))
EOF
  fi

  if ! grep -qF "config XTENSA_ESP32S3" "${QEMU_WASM_SRC_DIR}/hw/xtensa/Kconfig"; then
    cat >> "${QEMU_WASM_SRC_DIR}/hw/xtensa/Kconfig" <<'EOF'

config XTENSA_ESP32S3
    bool
    default y
    depends on XTENSA
    select SSI
    select SSI_M25P80
    select UNIMP
    select OPENCORES_ETH
    select DWC_SDMMC
    select TMP105
    select ESP_RGB
EOF
  fi

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/hw/xtensa/Kconfig" '
import re

for config_name in ("XTENSA_ESP32", "XTENSA_ESP32S3"):
    pattern = rf"(config {config_name}\n(?:(?!\nconfig ).*\n)*)"

    def add_sja1000(match):
        block = match.group(1)
        if "select CAN_SJA1000" in block:
            return block
        return block + "    select CAN_SJA1000\n"

    text = re.sub(pattern, add_sja1000, text, count=1)
'
}

restrict_qemu_wasm_xtensa_boards() {
  local default_mak="${QEMU_WASM_SRC_DIR}/configs/devices/xtensa-softmmu/default.mak"
  [[ -f "${default_mak}" ]] || return 0

  step "restricting qemu-wasm xtensa target to ESP32-S3"
  cat > "${default_mak}" <<'EOF'
# Browser simulator only builds the Mofei ESP32-S3 machine.
CONFIG_XTENSA_SIM=n
CONFIG_XTENSA_VIRT=n
CONFIG_XTENSA_XTFPGA=n
CONFIG_XTENSA_ESP32=n
CONFIG_XTENSA_ESP32S3=y
EOF
}

overlay_mofei_peripherals() {
  local src="${SIM_ROOT}/qemu-peripherals"
  step "overlaying reviewed Mofei QEMU peripherals"

  copy_if_present "${src}/hw_xtensa_esp32s3.c" "${QEMU_WASM_SRC_DIR}/hw/xtensa/esp32s3.c"
  copy_if_present "${src}/target_xtensa_translate.c" "${QEMU_WASM_SRC_DIR}/target/xtensa/translate.c"
  copy_if_present "${src}/target_xtensa_exc_helper.c" "${QEMU_WASM_SRC_DIR}/target/xtensa/exc_helper.c"
  copy_if_present "${src}/target_xtensa_helper.h" "${QEMU_WASM_SRC_DIR}/target/xtensa/helper.h"
  python3 "${SCRIPT_DIR}/select-xtensa-helper.py" "${QEMU_WASM_SRC_DIR}"
  copy_if_present "${src}/mofei-sim-addrs.h" "${QEMU_WASM_SRC_DIR}/include/hw/xtensa/mofei-sim-addrs.h"
  copy_if_present "${src}/mofei-sim-addrs.h" "${QEMU_WASM_SRC_DIR}/hw/xtensa/mofei-sim-addrs.h"
  copy_if_present "${src}/mofei-sim-addrs.h" "${QEMU_WASM_SRC_DIR}/target/xtensa/mofei-sim-addrs.h"
  copy_if_present "${src}/esp32s3_gpspi2.c" "${QEMU_WASM_SRC_DIR}/hw/ssi/esp32s3_gpspi2.c"
  copy_if_present "${src}/ssd1677_gdeq0426t82.c" "${QEMU_WASM_SRC_DIR}/hw/display/ssd1677_gdeq0426t82.c"
  copy_if_present "${src}/uc8253c.c" "${QEMU_WASM_SRC_DIR}/hw/display/uc8253c.c"
  mkdir -p "${QEMU_WASM_SRC_DIR}/hw/sensor"
  copy_if_present "${src}/ft6336u.c" "${QEMU_WASM_SRC_DIR}/hw/sensor/ft6336u.c"
  copy_if_present "${src}/chsc6440.c" "${QEMU_WASM_SRC_DIR}/hw/sensor/chsc6440.c"
  copy_if_present "${src}/esp32_gpio.c" "${QEMU_WASM_SRC_DIR}/hw/gpio/esp32_gpio.c"
  copy_if_present "${src}/esp32s3_gpio.c" "${QEMU_WASM_SRC_DIR}/hw/gpio/esp32s3_gpio.c"
  copy_if_present "${src}/esp32_gpio.h" "${QEMU_WASM_SRC_DIR}/include/hw/gpio/esp32_gpio.h"
  copy_if_present "${src}/esp32_i2c.c" "${QEMU_WASM_SRC_DIR}/hw/i2c/esp32_i2c.c"
  copy_if_present "${src}/esp32_i2c.h" "${QEMU_WASM_SRC_DIR}/include/hw/i2c/esp32_i2c.h"
  copy_if_present "${src}/lilygo_i2c_probe.c" "${QEMU_WASM_SRC_DIR}/hw/i2c/lilygo_i2c_probe.c"
  copy_if_present "${src}/lilygo_i2c_probe.h" "${QEMU_WASM_SRC_DIR}/include/hw/i2c/lilygo_i2c_probe.h"
  copy_if_present "${src}/lilygo_display_input.c" "${QEMU_WASM_SRC_DIR}/hw/i2c/lilygo_display_input.c"
  copy_if_present "${src}/lilygo_display_input.h" "${QEMU_WASM_SRC_DIR}/include/hw/i2c/lilygo_display_input.h"
  copy_if_present "${src}/lilygo_bq27220.c" "${QEMU_WASM_SRC_DIR}/hw/i2c/lilygo_bq27220.c"
  copy_if_present "${src}/lilygo_bq27220.h" "${QEMU_WASM_SRC_DIR}/include/hw/i2c/lilygo_bq27220.h"
  copy_if_present "${src}/lilygo_power_rtc.c" "${QEMU_WASM_SRC_DIR}/hw/i2c/lilygo_power_rtc.c"
  copy_if_present "${src}/lilygo_power_rtc.h" "${QEMU_WASM_SRC_DIR}/include/hw/i2c/lilygo_power_rtc.h"
  copy_if_present "${src}/lilygo_gnss_uart.c" "${QEMU_WASM_SRC_DIR}/hw/char/lilygo_gnss_uart.c"
  copy_if_present "${src}/lilygo_gnss_uart.h" "${QEMU_WASM_SRC_DIR}/include/hw/char/lilygo_gnss_uart.h"
  copy_if_present "${src}/lilygo_gnss_chardev.c" "${QEMU_WASM_SRC_DIR}/hw/char/lilygo_gnss_chardev.c"
  copy_if_present "${src}/lilygo_gnss_chardev.h" "${QEMU_WASM_SRC_DIR}/include/hw/char/lilygo_gnss_chardev.h"
  copy_if_present "${src}/lilygo_peripheral_control.c" "${QEMU_WASM_SRC_DIR}/hw/misc/lilygo_peripheral_control.c"
  copy_if_present "${src}/lilygo_peripheral_control.h" \
    "${QEMU_WASM_SRC_DIR}/include/hw/misc/lilygo_peripheral_control.h"
  copy_if_present "${src}/lilygo_sx1262.c" "${QEMU_WASM_SRC_DIR}/hw/ssi/lilygo_sx1262.c"
  copy_if_present "${src}/lilygo_sx1262.h" "${QEMU_WASM_SRC_DIR}/include/hw/ssi/lilygo_sx1262.h"
  copy_if_present "${src}/sd.c" "${QEMU_WASM_SRC_DIR}/hw/sd/sd.c"

  append_line_if_missing "${QEMU_WASM_SRC_DIR}/hw/i2c/meson.build" "lilygo_i2c_probe.c" \
    "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('lilygo_i2c_probe.c'))"
  append_line_if_missing "${QEMU_WASM_SRC_DIR}/hw/i2c/meson.build" "lilygo_display_input.c" \
    "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('lilygo_display_input.c'))"
  append_line_if_missing "${QEMU_WASM_SRC_DIR}/hw/i2c/meson.build" "lilygo_bq27220.c" \
    "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('lilygo_bq27220.c'))"
  append_line_if_missing "${QEMU_WASM_SRC_DIR}/hw/ssi/meson.build" "lilygo_sx1262.c" \
    "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('lilygo_sx1262.c'))"
  append_line_if_missing "${QEMU_WASM_SRC_DIR}/hw/i2c/meson.build" "lilygo_power_rtc.c" \
    "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('lilygo_power_rtc.c'))"
  append_line_if_missing "${QEMU_WASM_SRC_DIR}/hw/char/meson.build" "lilygo_gnss_uart.c" \
    "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('lilygo_gnss_uart.c', 'lilygo_gnss_chardev.c'))"
  append_line_if_missing "${QEMU_WASM_SRC_DIR}/hw/misc/meson.build" "lilygo_peripheral_control.c" \
    "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('lilygo_peripheral_control.c'))"

  append_if_missing "${QEMU_WASM_SRC_DIR}/hw/ssi/meson.build" "CONFIG_ESP32S3_GPSPI2" "${src}/gpspi2-meson.fragment"
  append_if_missing "${QEMU_WASM_SRC_DIR}/hw/ssi/Kconfig" "config ESP32S3_GPSPI2" "${src}/gpspi2-Kconfig.fragment"
  append_if_missing "${QEMU_WASM_SRC_DIR}/hw/display/meson.build" "CONFIG_SSD1677_GDEQ0426" "${src}/display-meson.fragment"
  append_if_missing "${QEMU_WASM_SRC_DIR}/hw/display/Kconfig" "config SSD1677_GDEQ0426" "${src}/display-Kconfig.fragment"

  if [[ ! -f "${QEMU_WASM_SRC_DIR}/hw/sensor/meson.build" ]]; then
    printf "sensor_ss = ss.source_set()\n" > "${QEMU_WASM_SRC_DIR}/hw/sensor/meson.build"
  fi
  if [[ ! -f "${QEMU_WASM_SRC_DIR}/hw/sensor/Kconfig" ]]; then
    : > "${QEMU_WASM_SRC_DIR}/hw/sensor/Kconfig"
  fi
  append_if_missing "${QEMU_WASM_SRC_DIR}/hw/sensor/meson.build" "CONFIG_FT6336U_MOFEI" "${src}/sensor-meson.fragment"
  append_if_missing "${QEMU_WASM_SRC_DIR}/hw/sensor/Kconfig" "config FT6336U_MOFEI" "${src}/sensor-Kconfig.fragment"
  append_if_missing "${QEMU_WASM_SRC_DIR}/hw/xtensa/Kconfig" "select SSD1677_GDEQ0426" "${src}/xtensa-Kconfig.fragment"
  append_line_if_missing "${QEMU_WASM_SRC_DIR}/hw/xtensa/Kconfig" "select SSI_SD" "    select SSI_SD"

  patch_file_if_present "${QEMU_WASM_SRC_DIR}/target/xtensa/exc_helper.c" \
    '#include <sys/prctl.h>' \
    '#ifndef __EMSCRIPTEN__\n#include <sys/prctl.h>\n#endif'
  patch_file_if_present "${QEMU_WASM_SRC_DIR}/target/xtensa/exc_helper.c" \
    'prctl(PR_SET_NAME, "mofei-qemu");' \
    '#ifndef __EMSCRIPTEN__\n  prctl(PR_SET_NAME, "mofei-qemu");\n#endif'
}

overlay_espressif_crypto_helpers() {
  step "overlaying ESP SHA/HMAC crypto helpers from native QEMU"
  local rel
  local helpers=(
    "crypto/sha1-internal.c"
    "crypto/sha1_i.h"
    "crypto/sha224-internal.c"
    "crypto/sha224_i.h"
    "crypto/sha256-internal.c"
    "crypto/sha256_i.h"
    "crypto/sha384-internal.c"
    "crypto/sha384_i.h"
    "crypto/sha512-internal.c"
    "crypto/sha512_i.h"
    "crypto/sha512_224-internal.c"
    "crypto/sha512_224_i.h"
    "crypto/sha512_256-internal.c"
    "crypto/sha512_256_i.h"
    "crypto/sha512_t-internal.c"
    "crypto/sha512_t_i.h"
    "crypto/hmac256-internal.c"
    "crypto/hmac256_i.h"
  )

  for rel in "${helpers[@]}"; do
    copy_if_present "${NATIVE_QEMU_DIR}/${rel}" "${QEMU_WASM_SRC_DIR}/${rel}"
  done

  patch_file_if_present "${QEMU_WASM_SRC_DIR}/hw/misc/esp32_flash_enc.c" \
    '#include <gcrypt.h>
#include "qemu/osdep.h"' \
    '#include "qemu/osdep.h"
#ifdef CONFIG_GCRYPT
#include <gcrypt.h>
#endif'

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/crypto/meson.build" '
extra_helpers = [
    "sha512_t-internal.c",
    "sha512_256-internal.c",
    "sha512_224-internal.c",
    "sha512-internal.c",
    "sha384-internal.c",
    "sha256-internal.c",
    "sha224-internal.c",
    "sha1-internal.c",
    "hmac256-internal.c",
]
missing_helpers = [helper for helper in extra_helpers if f"  '\''{helper}'\''," not in text]
if missing_helpers:
    needle = "  '\''sm4.c'\'',\n"
    replacement = needle + "".join(f"  '\''{helper}'\'',\n" for helper in missing_helpers)
    text = text.replace(needle, replacement)
'
}

patch_qemu_wasm_browser_crypto_compat() {
  step "patching browser qemu-wasm crypto device compatibility"

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/hw/misc/meson.build" '
import re

def ensure_file_in_config_block(source, config, entries):
    pattern = re.compile(
        r"(system_ss\.add\(when: '\''" + re.escape(config) + r"'\'', if_true: files\(\n)(.*?)(\n\)\))",
        re.S,
    )

    def update(match):
        body = match.group(2)
        updated = body
        missing = [entry for entry in entries if f"'\''{entry}'\''" not in body]
        if missing:
            lines = "".join(f"  '\''{entry}'\'',\n" for entry in missing)
            updated = body.rstrip()
            if updated and not updated.endswith(","):
                updated += ","
            updated += "\n" + lines.rstrip("\n")
        return match.group(1) + updated + match.group(3)

    return pattern.sub(update, source, count=1)

text = ensure_file_in_config_block(
    text,
    "CONFIG_XTENSA_ESP32S3",
    ["esp_aes.c", "esp32s3_aes.c", "esp32s3_xts_aes.c"],
)

gcrypt_block = re.compile(
    r"(system_ss\.add\(when: \[gcrypt, '\''CONFIG_XTENSA_ESP32S3'\''\], if_true: files\(\n)(.*?)(\n  \)\))",
    re.S,
)

def prune_gcrypt_only_block(match):
    body_lines = match.group(2).splitlines()
    browser_safe = {"'\''esp_aes.c'\''", "'\''esp32s3_aes.c'\''", "'\''esp32s3_xts_aes.c'\''"}
    kept = [line for line in body_lines if not any(entry in line for entry in browser_safe)]
    return match.group(1) + "\n".join(kept).rstrip() + match.group(3)

text = gcrypt_block.sub(prune_gcrypt_only_block, text, count=1)
'

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/crypto/meson.build" '
needle = "  crypto_ss.add(files('\''hash-glib.c'\'', '\''hmac-glib.c'\'', '\''pbkdf-stub.c'\''))"
replacement = "  crypto_ss.add(files('\''hash-glib.c'\'', '\''hmac-glib.c'\'', '\''pbkdf-stub.c'\'', '\''xts.c'\''))"
if needle in text and replacement not in text:
    text = text.replace(needle, replacement)
'

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/hw/misc/esp_aes.c" '
if "#include <gcrypt.h>" in text and "#ifdef CONFIG_GCRYPT\n#include <gcrypt.h>\n#endif" not in text:
    text = text.replace("#include <gcrypt.h>\n", "#ifdef CONFIG_GCRYPT\n#include <gcrypt.h>\n#endif\n", 1)

start_marker = "static void esp_aes_dma_start(ESPAesState *s)"
end_marker = "\nstatic void aes_block_start"
if "#ifdef CONFIG_GCRYPT\nstatic void esp_aes_dma_start" not in text and start_marker in text and end_marker in text:
    start = text.index(start_marker)
    end = text.index(end_marker, start)
    gcrypt_impl = text[start:end].rstrip()
    fallback = """#ifdef CONFIG_GCRYPT
{gcrypt_impl}

#else
static void esp_aes_dma_start(ESPAesState *s)
{{
    warn_report("[AES] DMA mode is unavailable in browser QEMU without libgcrypt");
    s->state_reg = ESP_AES_DONE;
    if (s->int_ena_reg) {{
        qemu_irq_raise(s->irq);
    }}
}}
#endif
""".format(gcrypt_impl=gcrypt_impl)
    text = text[:start] + fallback + text[end:]
'

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/hw/xtensa/esp32s3.c" '
def wrap_line_once(source, line):
    wrapped = "#ifndef __EMSCRIPTEN__\n" + line + "#endif\n"
    if wrapped in source:
        return source
    return source.replace(line, wrapped, 1)

text = wrap_line_once(text, "  object_initialize_child(OBJECT(ss), \"rsa\", &ss->rsa, TYPE_ESP32S3_RSA);\n")
text = wrap_line_once(text, "  object_initialize_child(OBJECT(ss), \"ds\", &ss->ds, TYPE_ESP32S3_DS);\n")

def wrap_block_before_marker(source, start_marker, end_marker):
    if start_marker not in source or end_marker not in source:
        return source
    start = source.index(start_marker)
    end = source.index(end_marker, start)
    block = source[start:end]
    if "#ifndef __EMSCRIPTEN__" in block:
        return source
    return source[:start] + "#ifndef __EMSCRIPTEN__\n" + block + "#endif\n" + source[end:]

text = wrap_block_before_marker(text, "  /* RSA realization */\n", "  /* PMS realization */\n")
text = wrap_block_before_marker(text, "  /* Digital Signature realization */\n", "  /* XTS-AES realization */\n")
'
}

overlay_espressif_trace_events() {
  step "overlaying ESP trace event declarations from native QEMU"
  append_trace_events_block_if_missing \
    "${NATIVE_QEMU_DIR}/hw/timer/trace-events" \
    "${QEMU_WASM_SRC_DIR}/hw/timer/trace-events" \
    "# esp32_frc_timer.c"
}

patch_qemu_wasm_helper_arity_compat() {
  step "patching qemu-wasm TCG helper arity compatibility"

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/include/exec/helper-head.h.inc" '
needle = """#define DEF_HELPER_7(name, ret, t1, t2, t3, t4, t5, t6, t7) \\
    DEF_HELPER_FLAGS_7(name, 0, ret, t1, t2, t3, t4, t5, t6, t7)
"""
insert = needle + """#define DEF_HELPER_8(name, ret, t1, t2, t3, t4, t5, t6, t7, t8) \\
    DEF_HELPER_FLAGS_8(name, 0, ret, t1, t2, t3, t4, t5, t6, t7, t8)
"""
if "DEF_HELPER_8(name, ret" not in text:
    text = text.replace(needle, insert)
'

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/include/exec/helper-proto.h.inc" '
needle = """#define DEF_HELPER_FLAGS_7(name, flags, ret, t1, t2, t3, t4, t5, t6, t7) \\
dh_ctype(ret) HELPER(name) (dh_ctype(t1), dh_ctype(t2), dh_ctype(t3), \\
                            dh_ctype(t4), dh_ctype(t5), dh_ctype(t6), \\
                            dh_ctype(t7)) DEF_HELPER_ATTR;
"""
insert = needle + """
#define DEF_HELPER_FLAGS_8(name, flags, ret, t1, t2, t3, t4, t5, t6, t7, t8) \\
dh_ctype(ret) HELPER(name) (dh_ctype(t1), dh_ctype(t2), dh_ctype(t3), \\
                            dh_ctype(t4), dh_ctype(t5), dh_ctype(t6), \\
                            dh_ctype(t7), dh_ctype(t8)) DEF_HELPER_ATTR;
"""
if "DEF_HELPER_FLAGS_8(name, flags, ret" not in text:
    text = text.replace(needle, insert)
if "#undef DEF_HELPER_FLAGS_8" not in text:
    text = text.replace("#undef DEF_HELPER_FLAGS_7\n", "#undef DEF_HELPER_FLAGS_7\n#undef DEF_HELPER_FLAGS_8\n")
'

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/include/exec/helper-gen.h.inc" '
needle = """#define DEF_HELPER_FLAGS_7(name, flags, ret, t1, t2, t3, t4, t5, t6, t7)\\
extern TCGHelperInfo glue(helper_info_, name);                          \\
static inline void glue(gen_helper_, name)(dh_retvar_decl(ret)          \\
    dh_arg_decl(t1, 1), dh_arg_decl(t2, 2), dh_arg_decl(t3, 3),         \\
    dh_arg_decl(t4, 4), dh_arg_decl(t5, 5), dh_arg_decl(t6, 6),         \\
    dh_arg_decl(t7, 7))                                                 \\
{                                                                       \\
    tcg_gen_call7(glue(helper_info_,name).func,                         \\
                  &glue(helper_info_,name), dh_retvar(ret),             \\
                  dh_arg(t1, 1), dh_arg(t2, 2), dh_arg(t3, 3),          \\
                  dh_arg(t4, 4), dh_arg(t5, 5), dh_arg(t6, 6),          \\
                  dh_arg(t7, 7));                                       \\
}
"""
insert = needle + """
#define DEF_HELPER_FLAGS_8(name, flags, ret, t1, t2, t3, t4, t5, t6, t7, t8)\\
extern TCGHelperInfo glue(helper_info_, name);                          \\
static inline void glue(gen_helper_, name)(dh_retvar_decl(ret)          \\
    dh_arg_decl(t1, 1), dh_arg_decl(t2, 2), dh_arg_decl(t3, 3),         \\
    dh_arg_decl(t4, 4), dh_arg_decl(t5, 5), dh_arg_decl(t6, 6),         \\
    dh_arg_decl(t7, 7), dh_arg_decl(t8, 8))                             \\
{                                                                       \\
    tcg_gen_call8(glue(helper_info_,name).func,                         \\
                  &glue(helper_info_,name), dh_retvar(ret),             \\
                  dh_arg(t1, 1), dh_arg(t2, 2), dh_arg(t3, 3),          \\
                  dh_arg(t4, 4), dh_arg(t5, 5), dh_arg(t6, 6),          \\
                  dh_arg(t7, 7), dh_arg(t8, 8));                        \\
}
"""
if "DEF_HELPER_FLAGS_8(name, flags, ret" not in text:
    text = text.replace(needle, insert)
if "#undef DEF_HELPER_FLAGS_8" not in text:
    text = text.replace("#undef DEF_HELPER_FLAGS_7\n", "#undef DEF_HELPER_FLAGS_7\n#undef DEF_HELPER_FLAGS_8\n")
'

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/include/exec/helper-info.c.inc" '
needle = """#define DEF_HELPER_FLAGS_7(NAME, FLAGS, RET, T1, T2, T3, T4, T5, T6, T7) \\
    TCGHelperInfo glue(helper_info_, NAME) = {                          \\
        .func = HELPER(NAME), .name = str(NAME),                        \\
        .flags = FLAGS | dh_callflag(RET),                              \\
        .typemask = dh_typemask(RET, 0) | dh_typemask(T1, 1)            \\
                  | dh_typemask(T2, 2) | dh_typemask(T3, 3)             \\
                  | dh_typemask(T4, 4) | dh_typemask(T5, 5)             \\
                  | dh_typemask(T6, 6) | dh_typemask(T7, 7)             \\
    };
"""
insert = needle + """
#define DEF_HELPER_FLAGS_8(NAME, FLAGS, RET, T1, T2, T3, T4, T5, T6, T7, T8) \\
    TCGHelperInfo glue(helper_info_, NAME) = {                          \\
        .func = HELPER(NAME), .name = str(NAME),                        \\
        .flags = FLAGS | dh_callflag(RET),                              \\
        .typemask = dh_typemask(RET, 0) | dh_typemask(T1, 1)            \\
                  | dh_typemask(T2, 2) | dh_typemask(T3, 3)             \\
                  | dh_typemask(T4, 4) | dh_typemask(T5, 5)             \\
                  | dh_typemask(T6, 6) | dh_typemask(T7, 7)             \\
                  | dh_typemask(T8, 8)                                  \\
    };
"""
if "DEF_HELPER_FLAGS_8(NAME, FLAGS, RET" not in text:
    text = text.replace(needle, insert)
if "#undef DEF_HELPER_FLAGS_8" not in text:
    text = text.replace("#undef DEF_HELPER_FLAGS_7\n", "#undef DEF_HELPER_FLAGS_7\n#undef DEF_HELPER_FLAGS_8\n")
'

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/include/tcg/tcg.h" '
needle = """void tcg_gen_call7(void *func, TCGHelperInfo *, TCGTemp *ret,
                   TCGTemp *, TCGTemp *, TCGTemp *, TCGTemp *,
                   TCGTemp *, TCGTemp *, TCGTemp *);
"""
insert = needle + """void tcg_gen_call8(void *func, TCGHelperInfo *, TCGTemp *ret,
                   TCGTemp *, TCGTemp *, TCGTemp *, TCGTemp *,
                   TCGTemp *, TCGTemp *, TCGTemp *, TCGTemp *);
"""
if "void tcg_gen_call8" not in text:
    text = text.replace(needle, insert)
'

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/tcg/tcg.c" '
needle = """void tcg_gen_call7(void *func, TCGHelperInfo *info, TCGTemp *ret, TCGTemp *t1,
                   TCGTemp *t2, TCGTemp *t3, TCGTemp *t4,
                   TCGTemp *t5, TCGTemp *t6, TCGTemp *t7)
{
    TCGTemp *args[7] = { t1, t2, t3, t4, t5, t6, t7 };
    tcg_gen_callN(func, info, ret, args);
}
"""
insert = needle + """
void tcg_gen_call8(void *func, TCGHelperInfo *info, TCGTemp *ret, TCGTemp *t1,
                   TCGTemp *t2, TCGTemp *t3, TCGTemp *t4,
                   TCGTemp *t5, TCGTemp *t6, TCGTemp *t7,
                   TCGTemp *t8)
{
    TCGTemp *args[8] = { t1, t2, t3, t4, t5, t6, t7, t8 };
    tcg_gen_callN(func, info, ret, args);
}
"""
if "void tcg_gen_call8" not in text:
    text = text.replace(needle, insert)
'
}

patch_qemu_wasm_kconfig_compat() {
  step "patching qemu-wasm Kconfig compatibility after ESP32-S3 overlay"
  append_text_if_missing "${QEMU_WASM_SRC_DIR}/hw/misc/Kconfig" "config PVPANIC_MMIO" \
    "config FSL_IMX8MP_ANALOG
    bool

config FSL_IMX8MP_CCM
    bool

config PVPANIC_MMIO
    bool
    select PVPANIC_COMMON

config DIVA_GSP
    bool"
  append_text_if_missing "${QEMU_WASM_SRC_DIR}/hw/display/Kconfig" "config MAC_PVG_MMIO" \
    "config MAC_PVG_MMIO
    bool
    depends on MAC_PVG && AARCH64

config MAC_PVG_PCI
    bool
    depends on MAC_PVG && PCI
    default y if PCI_DEVICES"
  patch_file_if_present "${QEMU_WASM_SRC_DIR}/hw/misc/meson.build" \
    "system_ss.add(when: 'CONFIG_IVSHMEM_DEVICE', if_true: files('ivshmem.c'))" \
    "# ivshmem devices
system_ss.add(when: 'CONFIG_IVSHMEM_DEVICE', if_true: files('ivshmem-pci.c'))
system_ss.add(when: 'CONFIG_IVSHMEM_FLAT_DEVICE', if_true: files('ivshmem-flat.c'))"
  patch_file_if_present "${QEMU_WASM_SRC_DIR}/hw/misc/meson.build" "npcm7xx_clk.c" "npcm_clk.c"
  patch_file_if_present "${QEMU_WASM_SRC_DIR}/hw/misc/meson.build" "npcm7xx_gcr.c" "npcm_gcr.c"
}

patch_qemu_wasm_xtensa_cpu_state_compat() {
  step "patching qemu-wasm Xtensa CPU state compatibility after ESP32-S3 overlay"

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/target/xtensa/cpu.h" '
kernel_needle = """    bool runstall;
    AddressSpace *address_space_er;
"""
kernel_insert = """    bool runstall;
    uint64_t kernel_entry; /* Non-zero to override reset PC (for -kernel ELF) */
    AddressSpace *address_space_er;
"""
if "uint64_t kernel_entry;" not in text:
    text = text.replace(kernel_needle, kernel_insert)

ext_needle = """    /* Breakpoints for IBREAK registers */
    struct CPUBreakpoint *cpu_breakpoint[MAX_NIBREAK];
"""
ext_insert = """    /* Breakpoints for IBREAK registers */
    struct CPUBreakpoint *cpu_breakpoint[MAX_NIBREAK];

    void *ext;
"""
if "void *ext;" not in text:
    text = text.replace(ext_needle, ext_insert)
'
}

patch_qemu_wasm_system_include_compat() {
  local file="$1"
  patch_python_if_present "${file}" '
include_map = {
    "#include \"sysemu/dma.h\"": "#include \"system/dma.h\"",
    "#include \"sysemu/blockdev.h\"": "#include \"system/blockdev.h\"",
    "#include \"sysemu/block-backend.h\"": "#include \"system/block-backend.h\"",
    "#include \"sysemu/block-backend-io.h\"": "#include \"system/block-backend-io.h\"",
    "#include \"sysemu/cpus.h\"": "#include \"system/cpus.h\"",
    "#include \"sysemu/reset.h\"": "#include \"system/reset.h\"",
    "#include \"sysemu/runstate.h\"": "#include \"system/runstate.h\"",
    "#include \"sysemu/sysemu.h\"": "#include \"system/system.h\"",
    "#include \"sysemu/device_tree.h\"": "#include \"system/device_tree.h\"",
    "#include \"sysemu/watchdog.h\"": "#include \"system/watchdog.h\"",
    "#include \"sysemu/qtest.h\"": "#include \"system/qtest.h\"",
    "#include \"sysemu/rtc.h\"": "#include \"system/rtc.h\"",
    "#include \"sysemu/hostmem.h\"": "#include \"system/hostmem.h\"",
    "#include \"sysemu/kvm.h\"": "#include \"system/kvm.h\"",
    "#include \"exec/address-spaces.h\"": "#include \"system/address-spaces.h\"",
    "#include \"exec/memory.h\"": "#include \"system/memory.h\"",
    "#include \"exec/cpu_ldst.h\"": "#include \"accel/tcg/cpu-ldst.h\"",
    "#include \"exec/exec-all.h\"": """#include \"exec/cpu-common.h\"
#include \"exec/cputlb.h\"
#include \"exec/target_page.h\"
#include \"exec/translation-block.h\"
#include \"exec/watchpoint.h\"
#include \"system/address-spaces.h\"
#include \"system/memory.h\"""",
    "#include \"qapi/qmp/qbool.h\"": "#include \"qobject/qbool.h\"",
    "#include \"qapi/qmp/qdict.h\"": "#include \"qobject/qdict.h\"",
    "#include \"qapi/qmp/qjson.h\"": "#include \"qobject/qjson.h\"",
    "#include \"qapi/qmp/qlist.h\"": "#include \"qobject/qlist.h\"",
    "#include \"qapi/qmp/qnull.h\"": "#include \"qobject/qnull.h\"",
    "#include \"qapi/qmp/qnum.h\"": "#include \"qobject/qnum.h\"",
    "#include \"qapi/qmp/qobject.h\"": "#include \"qobject/qobject.h\"",
    "#include \"qapi/qmp/qstring.h\"": "#include \"qobject/qstring.h\"",
}
for old, new in include_map.items():
    text = text.replace(old, new)
'
}

patch_qemu_wasm_system_include_tree_compat() {
  local root="$1"
  [[ -d "${root}" ]] || return 0
  python3 - "${root}" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
include_map = {
    '#include "sysemu/dma.h"': '#include "system/dma.h"',
    '#include "sysemu/blockdev.h"': '#include "system/blockdev.h"',
    '#include "sysemu/block-backend.h"': '#include "system/block-backend.h"',
    '#include "sysemu/block-backend-io.h"': '#include "system/block-backend-io.h"',
    '#include "sysemu/cpus.h"': '#include "system/cpus.h"',
    '#include "sysemu/reset.h"': '#include "system/reset.h"',
    '#include "sysemu/runstate.h"': '#include "system/runstate.h"',
    '#include "sysemu/sysemu.h"': '#include "system/system.h"',
    '#include "sysemu/device_tree.h"': '#include "system/device_tree.h"',
    '#include "sysemu/watchdog.h"': '#include "system/watchdog.h"',
    '#include "sysemu/qtest.h"': '#include "system/qtest.h"',
    '#include "sysemu/rtc.h"': '#include "system/rtc.h"',
    '#include "sysemu/hostmem.h"': '#include "system/hostmem.h"',
    '#include "sysemu/kvm.h"': '#include "system/kvm.h"',
    '#include "exec/address-spaces.h"': '#include "system/address-spaces.h"',
    '#include "exec/memory.h"': '#include "system/memory.h"',
    '#include "exec/cpu_ldst.h"': '#include "accel/tcg/cpu-ldst.h"',
    '#include "exec/exec-all.h"': '''#include "exec/cpu-common.h"
#include "exec/cputlb.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "exec/watchpoint.h"
#include "system/address-spaces.h"
#include "system/memory.h"''',
    '#include "qapi/qmp/qbool.h"': '#include "qobject/qbool.h"',
    '#include "qapi/qmp/qdict.h"': '#include "qobject/qdict.h"',
    '#include "qapi/qmp/qjson.h"': '#include "qobject/qjson.h"',
    '#include "qapi/qmp/qlist.h"': '#include "qobject/qlist.h"',
    '#include "qapi/qmp/qnull.h"': '#include "qobject/qnull.h"',
    '#include "qapi/qmp/qnum.h"': '#include "qobject/qnum.h"',
    '#include "qapi/qmp/qobject.h"': '#include "qobject/qobject.h"',
    '#include "qapi/qmp/qstring.h"': '#include "qobject/qstring.h"',
}
for path in root.rglob("*"):
    if path.suffix not in {".c", ".h", ".m"}:
        continue
    text = path.read_text(errors="ignore")
    updated = text
    for old, new in include_map.items():
        updated = updated.replace(old, new)
    if updated != text:
        path.write_text(updated)
PY
}

patch_qemu_wasm_xtensa_target_include_compat() {
  step "patching qemu-wasm Xtensa target include compatibility after ESP32-S3 overlay"
  [[ -d "${QEMU_WASM_SRC_DIR}/target/xtensa" ]] || return 0
  find "${QEMU_WASM_SRC_DIR}/target/xtensa" -maxdepth 1 -type f \
    \( -name '*.c' -o -name '*.h' -o -name '*.inc' \) -print0 | \
    while IFS= read -r -d '' file; do
      patch_qemu_wasm_system_include_compat "${file}"
    done
}

patch_qemu_wasm_xtensa_target_api_compat() {
  step "patching qemu-wasm Xtensa target API compatibility after ESP32-S3 overlay"

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/target/xtensa/exc_helper.c" '
text = text.replace(
    "static void xtensa_core_class_init(ObjectClass* oc, void* data)",
    "static void xtensa_core_class_init(ObjectClass* oc, const void* data)",
)
text = text.replace(
    "  XtensaConfig* config = data;\n",
    "  XtensaConfig* config = (XtensaConfig*)data;\n",
)
text = text.replace(
    "  type_register(&type);\n",
    "  type_register_static(&type);\n",
)

window_helper = """static inline unsigned mofei_windowbase_bound(unsigned a, const CPUXtensaState* env) {
  return a & (env->config->nareg / 4 - 1);
}

"""
if "mofei_windowbase_bound" not in text:
    text = text.replace("static bool mofei_guest_addr_in_sim_ram(uint32_t guest_addr);\n\n", "static bool mofei_guest_addr_in_sim_ram(uint32_t guest_addr);\n\n" + window_helper)
text = text.replace("mofei_mofei_windowbase_bound", "mofei_windowbase_bound")
text = text.replace(
    "window_bit = 1u << windowbase_bound(env->sregs[WINDOW_BASE], env);",
    "window_bit = 1u << mofei_windowbase_bound(env->sregs[WINDOW_BASE], env);",
)
'

  patch_python_if_present "${QEMU_WASM_SRC_DIR}/target/xtensa/translate.c" '
text = text.replace(
    "void gen_intermediate_code(CPUState* cpu, TranslationBlock* tb, int* max_insns, vaddr pc, void* host_pc)",
    "void xtensa_translate_code(CPUState* cpu, TranslationBlock* tb, int* max_insns, vaddr pc, void* host_pc)",
)
text = text.replace(
    "void gen_intermediate_code(CPUState *cpu, TranslationBlock *tb, int *max_insns, vaddr pc, void *host_pc)",
    "void xtensa_translate_code(CPUState *cpu, TranslationBlock *tb, int *max_insns, vaddr pc, void *host_pc)",
)
'
}

patch_qemu_wasm_qom_api_compat() {
  step "patching qemu-wasm QOM API compatibility after ESP32-S3 overlay"
  local source_dir
  for source_dir in hw/char hw/display hw/dma hw/gpio hw/i2c hw/misc hw/net/can hw/nvram hw/sd hw/sensor hw/ssi hw/timer hw/xtensa; do
    [[ -d "${QEMU_WASM_SRC_DIR}/${source_dir}" ]] || continue
    find "${QEMU_WASM_SRC_DIR}/${source_dir}" -maxdepth 1 -type f \( -name 'esp32*.c' -o -name 'esp_*.c' \
      -o -name 'dwc_sdmmc.c' -o -name 'sd.c' -o -name 'ssd1677*.c' -o -name 'uc8253c.c' \
      -o -name 'ft6336u.c' -o -name 'chsc6440.c' -o -name 'lilygo*.c' -o -name 'mofei*.c' \
      -o -name 'ssi_psram.c' \) -print0 | \
      while IFS= read -r -d '' file; do
        patch_file_if_present "${file}" "ObjectClass* klass, void* data" "ObjectClass* klass, const void* data"
        patch_file_if_present "${file}" "ObjectClass * klass, void * data" "ObjectClass * klass, const void * data"
        patch_file_if_present "${file}" "ObjectClass *klass, void *data" "ObjectClass *klass, const void *data"
        patch_file_if_present "${file}" "ObjectClass *klass, void * data" "ObjectClass *klass, const void * data"
        patch_file_if_present "${file}" "ObjectClass * klass, void *data" "ObjectClass * klass, const void *data"
        patch_file_if_present "${file}" "ObjectClass* oc, void* data" "ObjectClass* oc, const void* data"
        patch_file_if_present "${file}" "ObjectClass *oc, void *data" "ObjectClass *oc, const void *data"
        patch_file_if_present "${file}" "ObjectClass *oc, void * data" "ObjectClass *oc, const void * data"
        patch_qemu_wasm_system_include_compat "${file}"
        patch_python_if_present "${file}" '
import re

text = re.sub(r"(?m)^[ \t]*DEFINE_PROP_END_OF_LIST\(\),?\n", "", text)
text = re.sub(r",\s*DEFINE_PROP_END_OF_LIST\(\)", "", text)
text = re.sub(r"DEFINE_PROP_END_OF_LIST\(\),?\s*", "", text)
text = re.sub(r"\bstatic\s+Property\s+(\w+\[\]\s*=\s*\{)", r"static const Property \1", text)

empty_property_arrays = set()

def normalize_property_array(match):
    qualifier = match.group(1) or ""
    name = match.group(2)
    body = match.group(3).strip()
    if body:
        return match.group(0)
    empty_property_arrays.add(name)
    return f"static {qualifier}Property {name}[] = {{}};"

text = re.sub(
    r"(?s)static\s+(const\s+)?Property\s+(\w+)\[\]\s*=\s*\{(.*?)\};",
    normalize_property_array,
    text,
)
for name in empty_property_arrays:
    text = re.sub(
        r"(?m)^[ \t]*device_class_set_props\([^;]*,\s*" + re.escape(name) + r"\);\n",
        "",
        text,
    )
'
      done
  done
  patch_qemu_wasm_system_include_tree_compat "${QEMU_WASM_SRC_DIR}/include/hw"
}

build_image() {
  require_tool docker
  local dockerfile="${QEMU_WASM_SRC_DIR}/tests/docker/dockerfiles/emsdk-wasm32-cross.docker"
  local root_dockerfile="${QEMU_WASM_SRC_DIR}/Dockerfile"

  if [[ "${FORCE}" -eq 0 ]] && docker image inspect "${QEMU_WASM_IMAGE}" >/dev/null 2>&1; then
    step "reusing Docker image ${QEMU_WASM_IMAGE} (use --force to rebuild)"
    return
  fi

  step "building Docker image ${QEMU_WASM_IMAGE}"

  if [[ "${FORCE}" -eq 0 ]] && docker image inspect "${QEMU_WASM_BASE_IMAGE}" >/dev/null 2>&1; then
    step "reusing qemu-wasm Emscripten base image ${QEMU_WASM_BASE_IMAGE}"
  else
    if [[ -f "${dockerfile}" ]]; then
      step "building qemu-wasm Emscripten base image ${QEMU_WASM_BASE_IMAGE}"
      docker build -t "${QEMU_WASM_BASE_IMAGE}" - < "${dockerfile}"
    elif [[ -f "${root_dockerfile}" ]]; then
      step "building qemu-wasm root Dockerfile base image ${QEMU_WASM_BASE_IMAGE}"
      docker build -t "${QEMU_WASM_BASE_IMAGE}" -f "${root_dockerfile}" "${QEMU_WASM_SRC_DIR}"
    else
      die "qemu-wasm Emscripten Dockerfile missing: ${dockerfile} or ${root_dockerfile}"
    fi
  fi

  cat <<EOF | docker build -t "${QEMU_WASM_IMAGE}" -
FROM ${QEMU_WASM_BASE_IMAGE}
WORKDIR /build
RUN npm i xterm-pty@v0.10.1
ENV EMCC_CFLAGS="--js-library=/build/node_modules/xterm-pty/emscripten-pty.js"
CMD ["sleep", "infinity"]
EOF
}

start_container() {
  require_tool docker
  if docker ps -a --format '{{.Names}}' | grep -qx "${QEMU_WASM_CONTAINER}"; then
    docker rm -f "${QEMU_WASM_CONTAINER}" >/dev/null
  fi
  step "starting Docker container ${QEMU_WASM_CONTAINER}"
  docker run --rm -d --name "${QEMU_WASM_CONTAINER}" \
    -v "${QEMU_WASM_SRC_DIR}:/qemu" \
    -v "${SCRIPT_DIR}:${QEMU_WASM_SCRIPT_MOUNT}:ro" \
    "${QEMU_WASM_IMAGE}" >/dev/null
  CONTAINER_ID="$(docker inspect --format '{{.Id}}' "${QEMU_WASM_CONTAINER}")"
  CONTAINER_STARTED=1
}

stop_container() {
  [[ "${CONTAINER_STARTED}" -eq 1 ]] || return 0
  if command -v docker >/dev/null 2>&1 && docker ps --format '{{.Names}}' | grep -qx "${QEMU_WASM_CONTAINER}"; then
    docker rm -f "${QEMU_WASM_CONTAINER}" >/dev/null || true
  fi
}

configure_and_make() {
  local extra_cflags
  case "${QEMU_WASM_MALLOC}" in
    emmalloc|dlmalloc|mimalloc) ;;
    *) die "QEMU_WASM_MALLOC must be emmalloc, dlmalloc, or mimalloc (got ${QEMU_WASM_MALLOC})" ;;
  esac
  extra_cflags="-O3 -g -Wno-error=unused-command-line-argument -matomics -mbulk-memory -DNDEBUG -DG_DISABLE_ASSERT -D_GNU_SOURCE -sASYNCIFY=1 -pthread -sPROXY_TO_PTHREAD=1 -sFORCE_FILESYSTEM -sALLOW_TABLE_GROWTH -sTOTAL_MEMORY=2300MB -sWASM_BIGINT -sMALLOC=${QEMU_WASM_MALLOC} -sSTACK_OVERFLOW_CHECK=2 --js-library=/build/node_modules/xterm-pty/emscripten-pty.js --js-library=${QEMU_WASM_SCRIPT_MOUNT}/qemu-wasm-mofei-bridge.js -sMODULARIZE=1 -sEXPORT_ES6=1 -sASYNCIFY_IMPORTS=ffi_call_js"
  step "configuring qemu-system-xtensa for wasm32"
  docker exec "${QEMU_WASM_CONTAINER}" /bin/sh -lc "rm -rf ${QEMU_WASM_BUILD_DIR}/qemu-system-xtensa* ${QEMU_WASM_BUILD_DIR}/config-host.mak ${QEMU_WASM_BUILD_DIR}/build.ninja"
  docker exec \
    -e "EXTRA_CFLAGS=${extra_cflags}" \
    "${QEMU_WASM_CONTAINER}" \
    emconfigure /qemu/configure \
      --static \
      --target-list=xtensa-softmmu \
      --cpu=wasm32 \
      --cross-prefix= \
      --without-default-features \
      --enable-system \
      --disable-tools \
      --disable-docs \
      --with-coroutine=wasm \
      --extra-cflags="${extra_cflags}" \
      --extra-cxxflags="${extra_cflags}" \
      --extra-ldflags="${extra_cflags} -sEXPORTED_RUNTIME_METHODS=getTempRet0,setTempRet0,addFunction,removeFunction,TTY,FS,callMain -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_mofei_wasm_inject_touch,_mofei_wasm_inject_button,_mofei_wasm_release_buttons,_mofei_wasm_i2c_control"

  step "building qemu-system-xtensa"
  docker exec "${QEMU_WASM_CONTAINER}" emmake make -j "${JOBS}"
}

patch_qemu_wasm_runtime_env_bridge() {
  local runtime_js="$1"
  [[ -f "${runtime_js}" ]] || return 0

  step "patching qemu-wasm runtime ENV bridge"
  patch_python_if_present "${runtime_js}" '
needle = "var ENV={TERM:\"xterm-256color\"};"
replacement = "var ENV=Object.assign({TERM:\"xterm-256color\"},Module[\"ENV\"]||{});"
if needle not in text and replacement not in text:
    raise SystemExit("qemu wasm generated ENV declaration changed")
text = text.replace(needle, replacement)
'
}

patch_qemu_wasm_runtime_worker_bridge() {
  local runtime_js="$1"
  [[ -f "${runtime_js}" ]] || return 0

  step "patching qemu-wasm pthread IPC bridge"
  patch_python_if_present "${runtime_js}" '
import re

# qemu-wasm 的生成脚本可能把字符串边界拆成多个 JavaScript token；先恢复成可匹配的形状。
text = text.replace("payloadLen\"+\" tSelf=", "payloadLen+\" tSelf=")

legacy_ipc_hook = """function _mofei_wasm_ipc_send(channel,flags,payloadPtr,payloadLen){if(typeof Module.print==="function")Module.print("[BRIDGE] ch="+channel+" ptr="+payloadPtr+" len="+payloadLen+" tSelf="+typeof self._mofeiWasmIpcSend);if(typeof self._mofeiWasmIpcSend==="function"){self._mofeiWasmIpcSend(channel>>>0,flags>>>0,payloadPtr>>>0,payloadLen>>>0)}else{Module.print("[BRIDGE-SKIP] self._mofeiWasmIpcSend missing")}}"""
module_dot_ipc_hook = """function _mofei_wasm_ipc_send(channel,flags,payloadPtr,payloadLen){if(typeof Module._mofeiWasmIpcSend==="function"){Module._mofeiWasmIpcSend(channel>>>0,flags>>>0,payloadPtr>>>0,payloadLen>>>0)}}"""
module_bracket_ipc_hook = """function _mofei_wasm_ipc_send(channel,flags,payloadPtr,payloadLen){if(typeof Module["_mofeiWasmIpcSend"]==="function"){Module["_mofeiWasmIpcSend"](channel>>>0,flags>>>0,payloadPtr>>>0,payloadLen>>>0)}}"""

if legacy_ipc_hook in text:
    text = text.replace(legacy_ipc_hook, module_bracket_ipc_hook)
elif module_dot_ipc_hook in text:
    text = text.replace(module_dot_ipc_hook, module_bracket_ipc_hook)
elif module_bracket_ipc_hook not in text:
    raise SystemExit("qemu-wasm runtime IPC hook shape changed; update build-qemu-wasm.sh patch")

known_handlers_pattern = re.compile(r"var knownHandlers=\[([^\]]*)\];")
known_handlers_match = known_handlers_pattern.search(text)
if not known_handlers_match:
    raise SystemExit("qemu-wasm runtime knownHandlers shape changed; update build-qemu-wasm.sh patch")

known_handlers = [handler.strip() for handler in known_handlers_match.group(1).split(",") if handler.strip()]
if "\"_mofeiWasmIpcSend\"" not in known_handlers:
    known_handlers.append("\"_mofeiWasmIpcSend\"")
text = text[:known_handlers_match.start()] + "var knownHandlers=[" + ",".join(known_handlers) + "];" + text[known_handlers_match.end():]
'
}

copy_artifacts() {
  if [[ "${RUNTIME_ONLY}" -eq 1 ]]; then
    [[ ! -e "${OUTPUT_DIR}" ]] || die "runtime-only output must be new: ${OUTPUT_DIR}"
  else
    rm -rf "${OUTPUT_DIR}"
  fi
  mkdir -p "${OUTPUT_DIR}"

  docker cp "${QEMU_WASM_CONTAINER}:${QEMU_WASM_BUILD_DIR}/qemu-system-xtensa.js" "${RUNTIME_JS}" 2>/dev/null || \
    docker cp "${QEMU_WASM_CONTAINER}:${QEMU_WASM_BUILD_DIR}/qemu-system-xtensa" "${RUNTIME_JS}"
  patch_qemu_wasm_runtime_env_bridge "${RUNTIME_JS}"
  patch_qemu_wasm_runtime_worker_bridge "${RUNTIME_JS}"
  node "${SCRIPT_DIR}/patch-wasm-continuations.mjs" "${RUNTIME_JS}"
  docker cp "${QEMU_WASM_CONTAINER}:${QEMU_WASM_BUILD_DIR}/qemu-system-xtensa.wasm" "${RUNTIME_WASM}"
  docker cp "${QEMU_WASM_CONTAINER}:${QEMU_WASM_BUILD_DIR}/qemu-system-xtensa.worker.js" "${RUNTIME_WORKER}" 2>/dev/null || true
  docker cp "${QEMU_WASM_CONTAINER}:${QEMU_WASM_BUILD_DIR}/qemu-system-xtensa.data" "${RUNTIME_DATA}" 2>/dev/null || true
  docker cp "${QEMU_WASM_CONTAINER}:${QEMU_WASM_BUILD_DIR}/load.js" "${RUNTIME_LOAD_JS}" 2>/dev/null || true
  copy_if_present "${QEMU_WASM_SRC_DIR}/pc-bios/esp32s3_rev0_rom.bin" "${RUNTIME_ROM_BIN}"

  if [[ "${RUNTIME_ONLY}" -eq 1 ]]; then
    python3 "${SCRIPT_DIR}/runtime-source-manifest.py" "${OUTPUT_DIR}" "${QEMU_WASM_SRC_DIR}" "${NATIVE_QEMU_DIR}" "${SIM_ROOT}" "${QEMU_WASM_IMAGE}"
  else
    refresh_firmware_artifacts
  fi
  step "artifacts ready: ${OUTPUT_DIR}"
}

refresh_firmware_artifacts() {
  mkdir -p "${OUTPUT_DIR}"
  rm -f \
    "${RUNTIME_FIRMWARE_BIN}" \
    "${RUNTIME_FIRMWARE_KERNEL}" \
    "${RUNTIME_SYMBOLS_TXT}" \
    "${RUNTIME_PROVENANCE_JSON}" \
    "${RUNTIME_BOOTLOADER_BIN}" \
    "${RUNTIME_PARTITION_TABLE_BIN}" \
    "${RUNTIME_OTA_DATA_BIN}"

  [[ -f "${DEFAULT_FIRMWARE_BIN}" ]] || die "Panda AI OS simulator firmware BIN missing: ${DEFAULT_FIRMWARE_BIN}"
  [[ -f "${DEFAULT_FIRMWARE_ELF}" ]] || die "Panda AI OS simulator firmware ELF missing: ${DEFAULT_FIRMWARE_ELF}"
  [[ -f "${DEFAULT_FIRMWARE_BOOTLOADER}" ]] || die "Panda AI OS simulator bootloader missing: ${DEFAULT_FIRMWARE_BOOTLOADER}"
  [[ -f "${DEFAULT_FIRMWARE_PARTITION_TABLE}" ]] || die "Panda AI OS simulator partition table missing: ${DEFAULT_FIRMWARE_PARTITION_TABLE}"
  [[ -f "${DEFAULT_FIRMWARE_OTA_DATA}" ]] || die "Panda AI OS simulator OTA data image missing: ${DEFAULT_FIRMWARE_OTA_DATA}"

  step "copying browser qemu firmware from ${DEFAULT_FIRMWARE_BIN}"
  cp -f "${DEFAULT_FIRMWARE_BIN}" "${RUNTIME_FIRMWARE_BIN}"
  cp -f "${DEFAULT_FIRMWARE_BOOTLOADER}" "${RUNTIME_BOOTLOADER_BIN}"
  cp -f "${DEFAULT_FIRMWARE_PARTITION_TABLE}" "${RUNTIME_PARTITION_TABLE_BIN}"
  cp -f "${DEFAULT_FIRMWARE_OTA_DATA}" "${RUNTIME_OTA_DATA_BIN}"
  generate_firmware_kernel "${DEFAULT_FIRMWARE_ELF}" "${RUNTIME_FIRMWARE_KERNEL}"
  generate_firmware_symbols "${DEFAULT_FIRMWARE_ELF}" "${RUNTIME_SYMBOLS_TXT}"

  [[ -s "${RUNTIME_FIRMWARE_BIN}" ]] || die "failed to write ${RUNTIME_FIRMWARE_BIN}"
  [[ -s "${RUNTIME_FIRMWARE_KERNEL}" ]] || die "failed to write ${RUNTIME_FIRMWARE_KERNEL}"
  [[ -s "${RUNTIME_SYMBOLS_TXT}" ]] || die "failed to write ${RUNTIME_SYMBOLS_TXT}"
  [[ -s "${RUNTIME_BOOTLOADER_BIN}" ]] || die "failed to write ${RUNTIME_BOOTLOADER_BIN}"
  [[ -s "${RUNTIME_PARTITION_TABLE_BIN}" ]] || die "failed to write ${RUNTIME_PARTITION_TABLE_BIN}"
  [[ -s "${RUNTIME_OTA_DATA_BIN}" ]] || die "failed to write ${RUNTIME_OTA_DATA_BIN}"
  write_firmware_provenance
  [[ -s "${RUNTIME_PROVENANCE_JSON}" ]] || die "failed to write ${RUNTIME_PROVENANCE_JSON}"

  write_firmware_manifest
  step "firmware artifacts refreshed: ${RUNTIME_FIRMWARE_BIN}"
}

write_probe_manifest() {
  mkdir -p "${OUTPUT_DIR}"
  local required_boards_text="${QEMU_WASM_REQUIRED_BOARDS:-${BOARD}}"
  local -a required_boards
  read -r -a required_boards <<<"${required_boards_text}"
  [[ "${#required_boards[@]}" -gt 0 ]] || die "QEMU_WASM_REQUIRED_BOARDS must name at least one board"
  QEMU_WASM_REQUIRED_BOARDS="${required_boards[*]}" FIRMWARE_ONLY=0 write_firmware_manifest
  step "probe manifest written: ${ARTIFACT_MANIFEST}"
}

runtime_supports_required_boards() {
  [[ -s "${RUNTIME_WASM}" ]] || return 1

  if [[ ! -s "${ARTIFACT_MANIFEST}" ]] ||
    ! node -e '
      const fs = require("node:fs");
      const manifest = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
      process.exit(manifest.status === "ready" && manifest.wasmAllocator === process.argv[2] && manifest.runtimeBuildRevision === process.argv[3] ? 0 : 1);
    ' "${ARTIFACT_MANIFEST}" "${QEMU_WASM_MALLOC}" "${QEMU_WASM_RUNTIME_REVISION}"; then
    return 1
  fi

  local required_boards_text="${QEMU_WASM_REQUIRED_BOARDS:-${BOARD}}"
  local board
  for board in ${required_boards_text}; do
    case "${board}" in
      default|mofei) ;;
      *)
        strings "${RUNTIME_WASM}" | grep -F -- "${board}" >/dev/null || return 1
        ;;
    esac
  done
}

if [[ "${FIRMWARE_ONLY}" -eq 1 ]]; then
  refresh_firmware_artifacts
  exit 0
fi

# The post-link ownership patch is a host-side TypeScript consumer. Fail before
# cloning/building instead of discovering a missing npm install after 1,269 steps.
if [[ "${PROBE_ONLY}" -eq 0 && "${NO_BUILD}" -eq 0 ]]; then
  require_tool node
  (cd "${SIM_ROOT}" && node --input-type=module -e 'await import("typescript")') >/dev/null 2>&1 ||
    die "Simulator build dependencies missing; run npm ci --ignore-scripts in ${SIM_ROOT} before building WASM"
fi

trap stop_container EXIT INT TERM

ensure_checkout
patch_qemu_wasm_build_inputs
assert_qemu_wasm_base
overlay_espressif_sources
restrict_qemu_wasm_xtensa_boards
overlay_mofei_peripherals
python3 "${SCRIPT_DIR}/patch-qemu-wasm-stack.py" "${QEMU_WASM_SRC_DIR}"
overlay_espressif_crypto_helpers
patch_qemu_wasm_browser_crypto_compat
overlay_espressif_trace_events
patch_qemu_wasm_helper_arity_compat
patch_qemu_wasm_kconfig_compat
patch_qemu_wasm_xtensa_cpu_state_compat
patch_qemu_wasm_xtensa_target_include_compat
patch_qemu_wasm_xtensa_target_api_compat
patch_qemu_wasm_qom_api_compat

if [[ "${PROBE_ONLY}" -eq 1 || "${NO_BUILD}" -eq 1 ]]; then
  write_probe_manifest
  exit 0
fi

patch_qemu_wasm_runtime_env_bridge "${RUNTIME_JS}"
patch_qemu_wasm_runtime_worker_bridge "${RUNTIME_JS}"
if [[ -f "${RUNTIME_JS}" ]]; then
  node "${SCRIPT_DIR}/patch-wasm-continuations.mjs" "${RUNTIME_JS}"
fi

if [[ "${FORCE}" -eq 0 && -f "${RUNTIME_JS}" && -f "${RUNTIME_WASM}" && -f "${RUNTIME_ROM_BIN}" ]] &&
  runtime_supports_required_boards; then
  step "cache hit: ${OUTPUT_DIR} (use --force to rebuild qemu-wasm); refreshing firmware artifacts"
  refresh_firmware_artifacts
  exit 0
fi

if [[ "${FORCE}" -eq 0 && -f "${RUNTIME_WASM}" ]]; then
  step "cached qemu-wasm runtime does not support all required boards; rebuilding"
fi

build_image
start_container
configure_and_make
copy_artifacts
