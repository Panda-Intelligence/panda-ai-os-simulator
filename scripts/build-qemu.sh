#!/usr/bin/env bash
# build-qemu.sh — Build the Espressif QEMU fork for ESP32-S3 simulation.
#
# Idempotent: validates the pinned checkout and skips the build if a recent
# qemu-system-xtensa already exists in the cache.
#
# Required dependencies:
#   macOS:  brew install ninja glib pixman libgcrypt pkg-config gnutls
#   Linux:  apt install ninja-build libglib2.0-dev libpixman-1-dev libgcrypt20-dev pkg-config libgnutls28-dev
#
# Usage:
#   apps/simulator/scripts/build-qemu.sh           # build if missing
#   apps/simulator/scripts/build-qemu.sh --force   # always rebuild
#   apps/simulator/scripts/build-qemu.sh --check   # validate without writes
#
# Output:
#   apps/simulator/.qemu-cache/qemu/build/qemu-system-xtensa

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CACHE_DIR="${QEMU_CACHE_DIR:-${SIM_ROOT}/.qemu-cache}"
QEMU_SRC_DIR="${CACHE_DIR}/qemu"
QEMU_BUILD_DIR="${QEMU_SRC_DIR}/build"
QEMU_BIN="${QEMU_BUILD_DIR}/qemu-system-xtensa"
QEMU_UNSIGNED_BIN="${QEMU_BUILD_DIR}/qemu-system-xtensa-unsigned"
QEMU_BUILD_LOCK_DIR="${CACHE_DIR}/build-qemu.lock"
QEMU_RUNTIME_MANIFEST="${QEMU_RUNTIME_MANIFEST:-${SIM_ROOT}/qemu-runtime.env}"
[[ -f "${QEMU_RUNTIME_MANIFEST}" ]] || {
  echo "[build-qemu] ERROR: runtime manifest missing: ${QEMU_RUNTIME_MANIFEST}" >&2
  exit 1
}
# shellcheck source=../qemu-runtime.env
source "${QEMU_RUNTIME_MANIFEST}"
QEMU_REPO_URL="${QEMU_REPO_URL:-${PANDA_QEMU_REPO_URL}}"
QEMU_REF="${QEMU_REF:-${PANDA_QEMU_REF}}"
QEMU_FETCH_REF="${QEMU_FETCH_REF:-${PANDA_QEMU_FETCH_REF}}"
QEMU_MIN_FREE_KIB="${QEMU_MIN_FREE_KIB:-${PANDA_QEMU_MIN_FREE_KIB}}"
INTEGRATE_SCRIPT="${QEMU_INTEGRATE_SCRIPT:-${SIM_ROOT}/scripts/integrate-peripherals.sh}"
QEMU_PYTHON="${QEMU_PYTHON:-}"
QEMU_BUILD_LOCK_HELD=0
MODE=build
FORCE=0

usage() {
  echo "Usage: $0 [--check|--force]" >&2
}

die() {
  echo "[build-qemu] ERROR: $*" >&2
  exit 1
}

repair_hint() {
  echo "[build-qemu] Repair: remove '${QEMU_SRC_DIR}' and rerun '${SCRIPT_DIR}/build-qemu.sh'." >&2
}

normalize_repo_url() {
  local url="$1"
  url="${url%/}"
  url="${url%.git}"
  printf '%s\n' "${url}"
}

check_host() {
  local os arch
  os="$(uname -s)"
  arch="$(uname -m)"
  case "${os}:${arch}" in
    Darwin:arm64|Darwin:x86_64|Linux:aarch64|Linux:x86_64) ;;
    *) die "unsupported host ${os}/${arch}; supported hosts are macOS or Linux on arm64/x86_64" ;;
  esac
  echo "[build-qemu] host: ${os}/${arch}"
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "missing prerequisite '$1' ($2)"
}

check_build_prerequisites() {
  require_command git "install Git"
  require_command python3 "install Python 3"
  require_command ninja "macOS: brew install ninja; Linux: install ninja-build"
  require_command pkg-config "macOS: brew install pkg-config; Linux: install pkg-config"
  require_command patch "install patch"
  require_command tar "install tar"
  require_command awk "install awk"
  require_command df "install core system utilities"
  require_command file "install file"
  if [[ "$(uname -s)" == "Darwin" ]]; then
    require_command codesign "install the Xcode command-line tools"
  fi
}

existing_disk_anchor() {
  local path="${CACHE_DIR}"
  while [[ ! -e "${path}" && "${path}" != "/" ]]; do
    path="$(dirname "${path}")"
  done
  printf '%s\n' "${path}"
}

check_free_disk() {
  local available_kib anchor
  [[ "${QEMU_MIN_FREE_KIB}" =~ ^[0-9]+$ ]] || die "QEMU_MIN_FREE_KIB must be an integer, got '${QEMU_MIN_FREE_KIB}'"
  anchor="$(existing_disk_anchor)"
  available_kib="$(df -Pk "${anchor}" | awk 'END {print $4}')"
  [[ "${available_kib}" =~ ^[0-9]+$ ]] || die "could not determine free disk space for ${anchor}"
  if ((available_kib < QEMU_MIN_FREE_KIB)); then
    die "insufficient free disk space: $((available_kib / 1024)) MiB available, $((QEMU_MIN_FREE_KIB / 1024)) MiB required before the QEMU build"
  fi
  echo "[build-qemu] disk: $((available_kib / 1024)) MiB available"
}

validate_checkout() {
  if [[ ! -d "${QEMU_SRC_DIR}/.git" ]]; then
    echo "[build-qemu] ERROR: QEMU checkout missing at ${QEMU_SRC_DIR}" >&2
    repair_hint
    return 1
  fi

  local actual_ref actual_remote
  actual_ref="$(git -C "${QEMU_SRC_DIR}" rev-parse HEAD 2>/dev/null || true)"
  if [[ "${actual_ref}" != "${QEMU_REF}" ]]; then
    echo "[build-qemu] ERROR: QEMU revision mismatch: expected ${QEMU_REF}, found ${actual_ref:-unknown}" >&2
    repair_hint
    return 1
  fi

  actual_remote="$(git -C "${QEMU_SRC_DIR}" remote get-url origin 2>/dev/null || true)"
  if [[ "$(normalize_repo_url "${actual_remote}")" != "$(normalize_repo_url "${QEMU_REPO_URL}")" ]]; then
    echo "[build-qemu] ERROR: QEMU remote mismatch: expected ${QEMU_REPO_URL}, found ${actual_remote:-unknown}" >&2
    repair_hint
    return 1
  fi
  echo "[build-qemu] checkout: ${QEMU_REF}"
}

validate_binary() {
  if [[ ! -x "${QEMU_BIN}" ]]; then
    echo "[build-qemu] ERROR: QEMU binary missing at ${QEMU_BIN}" >&2
    repair_hint
    return 1
  fi
  "${QEMU_BIN}" --version 2>&1 | grep -qi "QEMU" || die "${QEMU_BIN} does not report a QEMU version"
  "${QEMU_BIN}" -machine help 2>&1 | grep -qi "esp32s3" || die "${QEMU_BIN} does not advertise the esp32s3 machine"

  local binary_kind host_arch
  binary_kind="$(file -b "${QEMU_BIN}" 2>/dev/null || true)"
  host_arch="$(uname -m)"
  if [[ "${binary_kind}" == *"Mach-O"* || "${binary_kind}" == *"ELF"* ]]; then
    case "${host_arch}" in
      arm64|aarch64) [[ "${binary_kind}" == *"arm64"* || "${binary_kind}" == *"aarch64"* ]] || die "QEMU binary architecture mismatch: ${binary_kind}" ;;
      x86_64) [[ "${binary_kind}" == *"x86_64"* || "${binary_kind}" == *"x86-64"* ]] || die "QEMU binary architecture mismatch: ${binary_kind}" ;;
    esac
  fi
  echo "[build-qemu] binary: ${QEMU_BIN}"
}

validate_runtime() {
  check_host
  require_command git "install Git"
  require_command file "install file"
  require_command patch "install patch"
  require_command tar "install tar"
  require_command python3 "install Python 3"
  validate_checkout
  validate_binary
  [[ -x "${INTEGRATE_SCRIPT}" ]] || die "integration script missing or not executable: ${INTEGRATE_SCRIPT}"
  QEMU_CACHE_DIR="${CACHE_DIR}" "${INTEGRATE_SCRIPT}" --check
  echo "[build-qemu] runtime check passed"
}

release_build_lock() {
  if [[ "${QEMU_BUILD_LOCK_HELD}" -eq 1 ]]; then
    rm -f "${QEMU_BUILD_LOCK_DIR}/pid" 2>/dev/null || true
    rmdir "${QEMU_BUILD_LOCK_DIR}" 2>/dev/null || true
    QEMU_BUILD_LOCK_HELD=0
  fi
}

acquire_build_lock() {
  local lock_pid
  mkdir -p "${CACHE_DIR}"
  while ! mkdir "${QEMU_BUILD_LOCK_DIR}" 2>/dev/null; do
    lock_pid="$(cat "${QEMU_BUILD_LOCK_DIR}/pid" 2>/dev/null || true)"
    if [[ -n "${lock_pid}" ]] && ! kill -0 "${lock_pid}" 2>/dev/null; then
      rm -f "${QEMU_BUILD_LOCK_DIR}/pid" 2>/dev/null || true
      rmdir "${QEMU_BUILD_LOCK_DIR}" 2>/dev/null || true
      continue
    fi
    echo "[build-qemu] waiting for build lock: ${QEMU_BUILD_LOCK_DIR}" >&2
    sleep 2
  done
  printf '%s\n' "$$" > "${QEMU_BUILD_LOCK_DIR}/pid"
  QEMU_BUILD_LOCK_HELD=1
}

trap release_build_lock EXIT INT TERM

for arg in "$@"; do
  case "${arg}" in
    --check) MODE=check ;;
    --force) FORCE=1 ;;
    -h|--help) usage; exit 0 ;;
    *) usage; die "unknown flag: ${arg}" ;;
  esac
done
if [[ "${MODE}" == "check" && "${FORCE}" -eq 1 ]]; then
  die "--check and --force cannot be combined"
fi

if [[ "${MODE}" == "check" ]]; then
  validate_runtime
  exit 0
fi

check_host
check_build_prerequisites

if [[ -z "${QEMU_PYTHON}" ]]; then
  if [[ "$(uname -s)" == "Darwin" && -x "/usr/bin/python3" ]]; then
    QEMU_PYTHON="/usr/bin/python3"
  else
    QEMU_PYTHON="$(command -v python3)"
  fi
fi

qemu_sources_newer_than_binary() {
  if [[ ! -x "${QEMU_BIN}" ]]; then
    return 0
  fi
  local newer
  newer="$(find \
    "${SIM_ROOT}/qemu-peripherals" \
    "${INTEGRATE_SCRIPT}" \
    "${SCRIPT_DIR}/build-qemu.sh" \
    -type f -newer "${QEMU_BIN}" -print -quit)"
  [[ -n "${newer}" ]]
}

if [[ ${FORCE} -eq 0 && -x "${QEMU_BIN}" ]]; then
  validate_checkout
  if qemu_sources_newer_than_binary; then
    echo "[build-qemu] cache stale: reviewed QEMU sources are newer than ${QEMU_BIN}"
  else
    validate_runtime
    echo "[build-qemu] cache hit: ${QEMU_BIN}"
    echo "[build-qemu] (use --force to rebuild)"
    exit 0
  fi
fi

check_free_disk
mkdir -p "${CACHE_DIR}"
acquire_build_lock

if [[ ! -d "${QEMU_SRC_DIR}/.git" ]]; then
  echo "[build-qemu] acquiring ${QEMU_REPO_URL}@${QEMU_FETCH_REF} into ${QEMU_SRC_DIR}"
  mkdir -p "${QEMU_SRC_DIR}"
  git -C "${QEMU_SRC_DIR}" init -q
  git -C "${QEMU_SRC_DIR}" remote add origin "${QEMU_REPO_URL}"
  git -C "${QEMU_SRC_DIR}" fetch --depth 1 origin "${QEMU_FETCH_REF}"
  git -C "${QEMU_SRC_DIR}" checkout --detach "${QEMU_REF}"
else
  echo "[build-qemu] reusing existing checkout at ${QEMU_SRC_DIR}"
fi

validate_checkout

cd "${QEMU_SRC_DIR}"

if [[ -x "${INTEGRATE_SCRIPT}" ]]; then
  echo "[build-qemu] syncing reviewed QEMU source mirrors"
  "${INTEGRATE_SCRIPT}" --sync-reviewed-sources
fi
QEMU_CACHE_DIR="${CACHE_DIR}" "${INTEGRATE_SCRIPT}" --check

if [[ ! -f "${QEMU_BUILD_DIR}/build.ninja" || ${FORCE} -eq 1 ]]; then
  echo "[build-qemu] configuring (target: xtensa-softmmu)"
  rm -rf "${QEMU_BUILD_DIR}"
  mkdir -p "${QEMU_BUILD_DIR}"
  cd "${QEMU_BUILD_DIR}"
  ../configure \
    --python="${QEMU_PYTHON}" \
    --target-list=xtensa-softmmu \
    --disable-containers \
    --enable-gcrypt \
    --enable-slirp \
    --disable-gnutls \
    --disable-libcbor \
    --disable-u2f \
    --disable-docs \
    --disable-werror
else
  cd "${QEMU_BUILD_DIR}"
fi

JOBS="${QEMU_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
echo "[build-qemu] ninja -j${JOBS}"
ninja "-j${JOBS}"
if [[ "$(uname -s)" == "Darwin" ]]; then
  echo "[build-qemu] ninja -j${JOBS} qemu-system-xtensa-unsigned"
  ninja "-j${JOBS}" qemu-system-xtensa-unsigned
fi

# macOS post-build: the espressif fork emits qemu-system-xtensa-unsigned and
# expects a downstream codesign step. Apply an ad-hoc signature with JIT
# entitlements so QEMU's TCG can map executable pages on hardened runtime.
if [[ "$(uname -s)" == "Darwin" && -f "${QEMU_UNSIGNED_BIN}" ]]; then
  ENTITLEMENTS="${QEMU_BUILD_DIR}/qemu-jit.entitlements"
  cat > "${ENTITLEMENTS}" <<'PLIST_EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.cs.allow-jit</key>
    <true/>
    <key>com.apple.security.cs.allow-unsigned-executable-memory</key>
    <true/>
    <key>com.apple.security.cs.disable-executable-page-protection</key>
    <true/>
  </dict>
</plist>
PLIST_EOF
  echo "[build-qemu] codesigning with JIT entitlements"
  cp -f "${QEMU_UNSIGNED_BIN}" "${QEMU_BIN}"
  codesign --entitlements "${ENTITLEMENTS}" --force -s - "${QEMU_BIN}"
fi

if [[ ! -x "${QEMU_BIN}" ]]; then
  echo "[build-qemu] ERROR: build completed but ${QEMU_BIN} not found" >&2
  echo "[build-qemu] check the actual output binary name in ${QEMU_BUILD_DIR}" >&2
  exit 1
fi

echo "[build-qemu] success: ${QEMU_BIN}"
validate_runtime
"${QEMU_BIN}" --version
echo "[build-qemu] supported ESP32-class machines:"
"${QEMU_BIN}" -machine help 2>/dev/null | grep -iE "esp32" || true
