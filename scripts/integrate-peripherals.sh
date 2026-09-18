#!/usr/bin/env bash
# integrate-peripherals.sh — Copy Mofei custom QEMU peripherals into the
# cloned espressif/qemu source tree and apply Meson + Kconfig + machine
# patches. Idempotent. Re-run `scripts/build-qemu.sh` afterwards to compile.
#
# Handles three custom peripherals:
#   1. GPSPI2 controller (hw/ssi/esp32s3_gpspi2.c)
#   2. SSD1677 e-ink panel (hw/display/ssd1677_gdeq0426t82.c)
#   3. FT6336U touch controller (hw/sensor/ft6336u.c)
#
# Plus the ESP32-S3 SoC machine patch that wires them all together.
# Also applies a small Xtensa CPU reset patch that keeps ROM/flash reset
# on the real reset vector while preserving direct -kernel ELF entry.
#
# Usage:
#   apps/simulator/scripts/integrate-peripherals.sh           # copy + meson + kconfig
#   apps/simulator/scripts/integrate-peripherals.sh --check   # verify files + patch, no writes
#   apps/simulator/scripts/integrate-peripherals.sh --soc     # also apply esp32s3 patch
#   apps/simulator/scripts/integrate-peripherals.sh --revert  # undo (best effort)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SRC_DIR="${SIM_ROOT}/qemu-peripherals"
QEMU_CACHE_DIR="${QEMU_CACHE_DIR:-${SIM_ROOT}/.qemu-cache}"
QEMU_DIR="${QEMU_CACHE_DIR}/qemu"

if [[ ! -d "${QEMU_DIR}" ]]; then
  echo "[integrate] ERROR: ${QEMU_DIR} not found. Run scripts/build-qemu.sh first." >&2
  exit 1
fi

APPLY_SOC=0
CHECK=0
REVERT=0
SYNC_REVIEWED_SOURCES=0
for arg in "$@"; do
  case "${arg}" in
    --soc)    APPLY_SOC=1 ;;
    --check)  CHECK=1 ;;
    --revert) REVERT=1 ;;
    --sync-reviewed-sources) SYNC_REVIEWED_SOURCES=1 ;;
    *) echo "[integrate] unknown flag: ${arg}" >&2; exit 2 ;;
  esac
done

GPSPI2_DST="${QEMU_DIR}/hw/ssi/esp32s3_gpspi2.c"
DISPLAY_DST="${QEMU_DIR}/hw/display/ssd1677_gdeq0426t82.c"
SENSOR_DST="${QEMU_DIR}/hw/sensor/ft6336u.c"
LILYGO_I2C_PROBE_DST="${QEMU_DIR}/hw/i2c/lilygo_i2c_probe.c"
LILYGO_I2C_PROBE_HEADER_DST="${QEMU_DIR}/include/hw/i2c/lilygo_i2c_probe.h"
LILYGO_DISPLAY_INPUT_DST="${QEMU_DIR}/hw/i2c/lilygo_display_input.c"
LILYGO_DISPLAY_INPUT_HEADER_DST="${QEMU_DIR}/include/hw/i2c/lilygo_display_input.h"
LILYGO_BQ27220_DST="${QEMU_DIR}/hw/i2c/lilygo_bq27220.c"
LILYGO_BQ27220_HEADER_DST="${QEMU_DIR}/include/hw/i2c/lilygo_bq27220.h"
LILYGO_POWER_RTC_DST="${QEMU_DIR}/hw/i2c/lilygo_power_rtc.c"
LILYGO_POWER_RTC_HEADER_DST="${QEMU_DIR}/include/hw/i2c/lilygo_power_rtc.h"
LILYGO_GNSS_UART_DST="${QEMU_DIR}/hw/char/lilygo_gnss_uart.c"
LILYGO_GNSS_UART_HEADER_DST="${QEMU_DIR}/include/hw/char/lilygo_gnss_uart.h"
LILYGO_GNSS_CHARDEV_DST="${QEMU_DIR}/hw/char/lilygo_gnss_chardev.c"
LILYGO_GNSS_CHARDEV_HEADER_DST="${QEMU_DIR}/include/hw/char/lilygo_gnss_chardev.h"
LILYGO_CONTROL_DST="${QEMU_DIR}/hw/misc/lilygo_peripheral_control.c"
LILYGO_CONTROL_HEADER_DST="${QEMU_DIR}/include/hw/misc/lilygo_peripheral_control.h"
LILYGO_SX1262_DST="${QEMU_DIR}/hw/ssi/lilygo_sx1262.c"
LILYGO_SX1262_HEADER_DST="${QEMU_DIR}/include/hw/ssi/lilygo_sx1262.h"
ESP32_I2C_DST="${QEMU_DIR}/hw/i2c/esp32_i2c.c"
ESP32_I2C_HEADER_DST="${QEMU_DIR}/include/hw/i2c/esp32_i2c.h"
# s37uc board peripheral models (compiled alongside the Mofei ones; the SoC
# machine instantiates only the set matching the selected board).
S37UC_DISPLAY_DST="${QEMU_DIR}/hw/display/uc8253c.c"
S37UC_SENSOR_DST="${QEMU_DIR}/hw/sensor/chsc6440.c"
ADDRS_HEADER_DST="${QEMU_DIR}/include/hw/xtensa/mofei-sim-addrs.h"
HW_ADDRS_HEADER_DST="${QEMU_DIR}/hw/xtensa/mofei-sim-addrs.h"
SOC_MACHINE_DST="${QEMU_DIR}/hw/xtensa/esp32s3.c"
ESP32_GPIO_DST="${QEMU_DIR}/hw/gpio/esp32_gpio.c"
ESP32S3_GPIO_DST="${QEMU_DIR}/hw/gpio/esp32s3_gpio.c"
ESP32_GPIO_HEADER_DST="${QEMU_DIR}/include/hw/gpio/esp32_gpio.h"
SD_DST="${QEMU_DIR}/hw/sd/sd.c"
XTENSA_TRANSLATE_DST="${QEMU_DIR}/target/xtensa/translate.c"
XTENSA_EXC_HELPER_DST="${QEMU_DIR}/target/xtensa/exc_helper.c"
XTENSA_HELPER_DST="${QEMU_DIR}/target/xtensa/helper.h"
XTENSA_CPU_HEADER_DST="${QEMU_DIR}/target/xtensa/cpu.h"
XTENSA_ADDRS_HEADER_DST="${QEMU_DIR}/target/xtensa/mofei-sim-addrs.h"

SSI_MESON="${QEMU_DIR}/hw/ssi/meson.build"
SSI_KCONFIG="${QEMU_DIR}/hw/ssi/Kconfig"
DISPLAY_MESON="${QEMU_DIR}/hw/display/meson.build"
DISPLAY_KCONFIG="${QEMU_DIR}/hw/display/Kconfig"
SENSOR_MESON="${QEMU_DIR}/hw/sensor/meson.build"
SENSOR_KCONFIG="${QEMU_DIR}/hw/sensor/Kconfig"
I2C_MESON="${QEMU_DIR}/hw/i2c/meson.build"
CHAR_MESON="${QEMU_DIR}/hw/char/meson.build"
MISC_MESON="${QEMU_DIR}/hw/misc/meson.build"
XTENSA_KCONFIG="${QEMU_DIR}/hw/xtensa/Kconfig"
XTENSA_MESON="${QEMU_DIR}/target/xtensa/meson.build"

# Sentinel strings for idempotent appending.
GPSPI2_MESON_SENTINEL="CONFIG_ESP32S3_GPSPI2"
GPSPI2_KCONFIG_SENTINEL="config ESP32S3_GPSPI2"
DISPLAY_MESON_SENTINEL="CONFIG_SSD1677_GDEQ0426"
SENSOR_MESON_SENTINEL="CONFIG_FT6336U_MOFEI"
DISPLAY_KCONFIG_SENTINEL="config SSD1677_GDEQ0426"
SENSOR_KCONFIG_SENTINEL="config FT6336U_MOFEI"
XTENSA_KCONFIG_SENTINEL="select SSD1677_GDEQ0426"
S37UC_DISPLAY_MESON_SENTINEL="CONFIG_UC8253C_S37UC"
S37UC_SENSOR_MESON_SENTINEL="CONFIG_CHSC6440_S37UC"
S37UC_DISPLAY_KCONFIG_SENTINEL="config UC8253C_S37UC"
S37UC_SENSOR_KCONFIG_SENTINEL="config CHSC6440_S37UC"
S37UC_DISPLAY_SELECT_SENTINEL="select UC8253C_S37UC"
S37UC_SENSOR_SELECT_SENTINEL="select CHSC6440_S37UC"
SSI_SD_SELECT_SENTINEL="select SSI_SD"
LILYGO_I2C_PROBE_MESON_SENTINEL="lilygo_i2c_probe.c"
LILYGO_DISPLAY_INPUT_MESON_SENTINEL="lilygo_display_input.c"
LILYGO_BQ27220_MESON_SENTINEL="lilygo_bq27220.c"
LILYGO_POWER_RTC_MESON_SENTINEL="lilygo_power_rtc.c"
LILYGO_GNSS_UART_MESON_SENTINEL="lilygo_gnss_uart.c"
LILYGO_CONTROL_MESON_SENTINEL="lilygo_peripheral_control.c"
LILYGO_SX1262_MESON_SENTINEL="lilygo_sx1262.c"

REQUIRED_SOURCE_FILES=(
  "${SRC_DIR}/esp32s3_gpspi2.c"
  "${SRC_DIR}/ssd1677_gdeq0426t82.c"
  "${SRC_DIR}/ft6336u.c"
  "${SRC_DIR}/uc8253c.c"
  "${SRC_DIR}/chsc6440.c"
  "${SRC_DIR}/lilygo_i2c_probe.c"
  "${SRC_DIR}/lilygo_i2c_probe.h"
  "${SRC_DIR}/lilygo_display_input.c"
  "${SRC_DIR}/lilygo_display_input.h"
  "${SRC_DIR}/lilygo_bq27220.c"
  "${SRC_DIR}/lilygo_bq27220.h"
  "${SRC_DIR}/lilygo_power_rtc.c"
  "${SRC_DIR}/lilygo_power_rtc.h"
  "${SRC_DIR}/lilygo_gnss_uart.c"
  "${SRC_DIR}/lilygo_gnss_uart.h"
  "${SRC_DIR}/lilygo_gnss_chardev.c"
  "${SRC_DIR}/lilygo_gnss_chardev.h"
  "${SRC_DIR}/lilygo_peripheral_control.c"
  "${SRC_DIR}/lilygo_peripheral_control.h"
  "${SRC_DIR}/lilygo_sx1262.c"
  "${SRC_DIR}/lilygo_sx1262.h"
  "${SRC_DIR}/esp32_i2c.c"
  "${SRC_DIR}/esp32_i2c.h"
  "${SRC_DIR}/hw_xtensa_esp32s3.c"
  "${SRC_DIR}/esp32_gpio.c"
  "${SRC_DIR}/esp32s3_gpio.c"
  "${SRC_DIR}/esp32_gpio.h"
  "${SRC_DIR}/mofei-sim-addrs.h"
  "${SRC_DIR}/sd.c"
  "${SRC_DIR}/target_xtensa_translate.c"
  "${SRC_DIR}/target_xtensa_exc_helper.c"
  "${SRC_DIR}/target_xtensa_helper.h"
  "${SRC_DIR}/gpspi2-meson.fragment"
  "${SRC_DIR}/gpspi2-Kconfig.fragment"
  "${SRC_DIR}/display-meson.fragment"
  "${SRC_DIR}/display-Kconfig.fragment"
  "${SRC_DIR}/sensor-meson.fragment"
  "${SRC_DIR}/sensor-Kconfig.fragment"
  "${SRC_DIR}/xtensa-Kconfig.fragment"
  "${SRC_DIR}/esp32s3-soc-machine.patch"
  "${SRC_DIR}/xtensa-reset-vector.patch"
)

check_required_files() {
  local missing=0
  for file in "${REQUIRED_SOURCE_FILES[@]}"; do
    if [[ ! -f "${file}" ]]; then
      echo "[integrate] ERROR: missing ${file}" >&2
      missing=1
    fi
  done
  return "${missing}"
}

check_qemu_targets() {
  local missing=0
  for file in \
    "${SSI_MESON}" \
    "${SSI_KCONFIG}" \
    "${DISPLAY_MESON}" \
    "${DISPLAY_KCONFIG}" \
    "${SENSOR_MESON}" \
    "${SENSOR_KCONFIG}" \
    "${I2C_MESON}" \
    "${CHAR_MESON}" \
    "${MISC_MESON}" \
    "${XTENSA_KCONFIG}" \
    "${XTENSA_MESON}" \
    "${QEMU_DIR}/hw/xtensa/esp32s3.c" \
    "${QEMU_DIR}/hw/gpio/esp32_gpio.c" \
    "${QEMU_DIR}/hw/gpio/esp32s3_gpio.c" \
    "${QEMU_DIR}/include/hw/gpio/esp32_gpio.h" \
    "${QEMU_DIR}/include/hw/gpio/esp32s3_gpio.h" \
    "${QEMU_DIR}/target/xtensa/cpu.c" \
    "${QEMU_DIR}/target/xtensa/cpu.h" \
    "${QEMU_DIR}/target/xtensa/exc_helper.c" \
    "${QEMU_DIR}/target/xtensa/helper.h" \
    "${QEMU_DIR}/target/xtensa/translate.c"; do
    if [[ ! -f "${file}" ]]; then
      echo "[integrate] ERROR: missing QEMU target ${file}" >&2
      missing=1
    fi
  done
  return "${missing}"
}

check_patch_against_clean_head() {
  local patch="$1"
  shift

  if [[ ! -f "${patch}" ]]; then
    echo "[integrate] ERROR: ${patch} not found" >&2
    return 1
  fi

  local tmp
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/mofei-qemu-patch-check.XXXXXX")"

  ( cd "${QEMU_DIR}" && \
    git archive HEAD "$@" | tar -x -C "${tmp}" )

  if ( cd "${tmp}" && patch --dry-run -l -p1 --forward < "${patch}" ); then
    echo "[integrate] $(basename "${patch}") applies to clean QEMU HEAD"
    rm -rf "${tmp}"
    return 0
  fi

  echo "[integrate] ERROR: $(basename "${patch}") does not apply to clean QEMU HEAD" >&2
  echo "[integrate]   Patch file: ${patch}" >&2
  rm -rf "${tmp}"
  return 1
}

check_soc_patch() {
  check_patch_against_clean_head \
    "${SRC_DIR}/esp32s3-soc-machine.patch" \
    hw/xtensa/esp32s3.c \
    hw/gpio/esp32_gpio.c \
    hw/gpio/esp32s3_gpio.c \
    include/hw/gpio/esp32_gpio.h \
    include/hw/gpio/esp32s3_gpio.h
}

check_xtensa_reset_patch() {
  check_patch_against_clean_head \
    "${SRC_DIR}/xtensa-reset-vector.patch" \
    target/xtensa/cpu.c \
    target/xtensa/cpu.h \
    target/xtensa/exc_helper.c \
    target/xtensa/helper.h \
    target/xtensa/translate.c
}

check_xtensa_window_vectors() {
  local exc_helper="${QEMU_DIR}/target/xtensa/exc_helper.c"
  if [[ ! -f "${exc_helper}" ]]; then
    echo "[integrate] ERROR: missing QEMU target ${exc_helper}" >&2
    return 1
  fi

  if grep -qE "Handle window (over|under)flow directly|handled window (over|under)flow" "${exc_helper}"; then
    echo "[integrate] ERROR: QEMU cache contains an experimental Xtensa window-vector fast path." >&2
    echo "[integrate] ESP32-S3 ROM flash boot must dispatch window overflow/underflow through ROM vectors." >&2
    echo "[integrate] Clean/recreate apps/simulator/.qemu-cache/qemu or remove the local exc_helper.c experiment." >&2
    return 1
  fi
}

sync_qemu_xtensa_cpu_state_compat() {
  if [[ ! -f "${XTENSA_CPU_HEADER_DST}" ]]; then
    echo "[integrate] ERROR: missing QEMU target ${XTENSA_CPU_HEADER_DST}" >&2
    return 1
  fi

  echo "[integrate] patching Xtensa CPU state compatibility"
  python3 - "${XTENSA_CPU_HEADER_DST}" <<'PY'
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text()

if "uint64_t kernel_entry;" not in text:
    needle = "    bool runstall;\n    AddressSpace *address_space_er;\n"
    replacement = (
        "    bool runstall;\n"
        "    uint64_t kernel_entry; /* Non-zero to override reset PC (for -kernel ELF) */\n"
        "    AddressSpace *address_space_er;\n"
    )
    if needle not in text:
        raise SystemExit("target/xtensa/cpu.h missing runstall/address_space_er anchor")
    text = text.replace(needle, replacement)

if not re.search(r"\bvoid\s*\*\s*ext\s*;", text):
    needle = (
        "    /* Breakpoints for IBREAK registers */\n"
        "    struct CPUBreakpoint *cpu_breakpoint[MAX_NIBREAK];\n"
    )
    replacement = needle + "\n    void *ext;\n"
    if needle not in text:
        raise SystemExit("target/xtensa/cpu.h missing cpu_breakpoint anchor")
    text = text.replace(needle, replacement)

obsolete_decl_patterns = (
    r"\nvoid\s+xtensa_rotate_window_abs\s*\(\s*CPUXtensaState\s*\*\s*env,\s*uint32_t\s+position\s*\);",
    r"\nunsigned\s+windowbase_bound\s*\(\s*unsigned\s+a,\s*const\s+CPUXtensaState\s*\*\s*env\s*\);",
)
for pattern in obsolete_decl_patterns:
    text = re.sub(pattern, "", text)

path.write_text(text)
PY
}

sync_simulator_qemu_sources() {
  echo "[integrate] syncing canonical ESP32-S3 machine source → hw/xtensa/"
  cp -f "${SRC_DIR}/hw_xtensa_esp32s3.c" "${SOC_MACHINE_DST}"

  echo "[integrate] syncing canonical ESP32 GPIO sources → hw/gpio/"
  cp -f "${SRC_DIR}/esp32_gpio.c" "${ESP32_GPIO_DST}"
  cp -f "${SRC_DIR}/esp32s3_gpio.c" "${ESP32S3_GPIO_DST}"
  cp -f "${SRC_DIR}/esp32_gpio.h" "${ESP32_GPIO_HEADER_DST}"

  echo "[integrate] syncing SD model → hw/sd/"
  cp -f "${SRC_DIR}/sd.c" "${SD_DST}"

  echo "[integrate] syncing canonical Xtensa translate/helper sources → target/xtensa/"
  cp -f "${SRC_DIR}/target_xtensa_translate.c" "${XTENSA_TRANSLATE_DST}"
  cp -f "${SRC_DIR}/target_xtensa_exc_helper.c" "${XTENSA_EXC_HELPER_DST}"
  cp -f "${SRC_DIR}/target_xtensa_helper.h" "${XTENSA_HELPER_DST}"
  cp -f "${SRC_DIR}/mofei-sim-addrs.h" "${XTENSA_ADDRS_HEADER_DST}"
  cp -f "${SRC_DIR}/mofei-sim-addrs.h" "${HW_ADDRS_HEADER_DST}"

  if grep -q "'helper.c'," "${XTENSA_MESON}"; then
    echo "[integrate] selecting exc_helper.c in target/xtensa/meson.build"
    sed -i.bak "s/'helper.c',/'exc_helper.c',/" "${XTENSA_MESON}"
    rm -f "${XTENSA_MESON}.bak"
  fi

  sync_qemu_xtensa_cpu_state_compat
}

patch_targets_are_clean() {
  local dirty
  dirty="$(git -C "${QEMU_DIR}" status --short -- \
    hw/xtensa/esp32s3.c \
    hw/gpio/esp32_gpio.c \
    hw/gpio/esp32s3_gpio.c \
    include/hw/gpio/esp32_gpio.h \
    include/hw/gpio/esp32s3_gpio.h \
    target/xtensa/cpu.c \
    target/xtensa/cpu.h \
    target/xtensa/exc_helper.c \
    target/xtensa/helper.h \
    target/xtensa/translate.c)"
  if [[ -n "${dirty}" ]]; then
    echo "[integrate] ERROR: refusing --soc because QEMU patch target files are dirty:" >&2
    echo "${dirty}" >&2
    echo "[integrate] Use --check for non-mutating verification, or clean the QEMU checkout first." >&2
    return 1
  fi
  return 0
}

if [[ ${CHECK} -eq 1 ]]; then
  check_required_files
  check_qemu_targets
  check_xtensa_reset_patch
  check_soc_patch
  check_xtensa_window_vectors
  echo "[integrate] check complete (no files were modified)"
  exit 0
fi

revert_one() {
  local file="$1"
  local sentinel="$2"
  if [[ -f "${file}" ]] && grep -qF "${sentinel}" "${file}"; then
    cp -f "${file}" "${file}.revert.bak"
    grep -vF "${sentinel}" "${file}" > "${file}.tmp"
    mv -f "${file}.tmp" "${file}"
    echo "[integrate] reverted: ${file}"
  fi
}

if [[ ${REVERT} -eq 1 ]]; then
  rm -f "${GPSPI2_DST}" "${DISPLAY_DST}" "${SENSOR_DST}" "${S37UC_DISPLAY_DST}" "${S37UC_SENSOR_DST}" \
    "${LILYGO_I2C_PROBE_DST}" "${LILYGO_I2C_PROBE_HEADER_DST}" \
    "${LILYGO_DISPLAY_INPUT_DST}" "${LILYGO_DISPLAY_INPUT_HEADER_DST}" \
    "${LILYGO_BQ27220_DST}" "${LILYGO_BQ27220_HEADER_DST}" \
    "${LILYGO_POWER_RTC_DST}" "${LILYGO_POWER_RTC_HEADER_DST}" \
    "${LILYGO_GNSS_UART_DST}" "${LILYGO_GNSS_UART_HEADER_DST}" \
    "${LILYGO_GNSS_CHARDEV_DST}" "${LILYGO_GNSS_CHARDEV_HEADER_DST}" \
    "${LILYGO_CONTROL_DST}" "${LILYGO_CONTROL_HEADER_DST}" \
    "${LILYGO_SX1262_DST}" "${LILYGO_SX1262_HEADER_DST}"
  revert_one "${SSI_MESON}"     "${GPSPI2_MESON_SENTINEL}"
  revert_one "${SSI_KCONFIG}"   "${GPSPI2_KCONFIG_SENTINEL}"
  revert_one "${DISPLAY_MESON}"  "${DISPLAY_MESON_SENTINEL}"
  revert_one "${DISPLAY_MESON}"  "${S37UC_DISPLAY_MESON_SENTINEL}"
  revert_one "${DISPLAY_KCONFIG}" "${DISPLAY_KCONFIG_SENTINEL}"
  revert_one "${DISPLAY_KCONFIG}" "${S37UC_DISPLAY_KCONFIG_SENTINEL}"
  revert_one "${SENSOR_MESON}"   "${SENSOR_MESON_SENTINEL}"
  revert_one "${SENSOR_MESON}"   "${S37UC_SENSOR_MESON_SENTINEL}"
  revert_one "${SENSOR_KCONFIG}" "${SENSOR_KCONFIG_SENTINEL}"
  revert_one "${SENSOR_KCONFIG}" "${S37UC_SENSOR_KCONFIG_SENTINEL}"
  revert_one "${I2C_MESON}" "${LILYGO_I2C_PROBE_MESON_SENTINEL}"
  revert_one "${I2C_MESON}" "${LILYGO_DISPLAY_INPUT_MESON_SENTINEL}"
  revert_one "${I2C_MESON}" "${LILYGO_BQ27220_MESON_SENTINEL}"
  revert_one "${I2C_MESON}" "${LILYGO_POWER_RTC_MESON_SENTINEL}"
  revert_one "${CHAR_MESON}" "${LILYGO_GNSS_UART_MESON_SENTINEL}"
  revert_one "${MISC_MESON}" "${LILYGO_CONTROL_MESON_SENTINEL}"
  revert_one "${SSI_MESON}" "${LILYGO_SX1262_MESON_SENTINEL}"
  revert_one "${XTENSA_KCONFIG}" "${XTENSA_KCONFIG_SENTINEL}"
  revert_one "${XTENSA_KCONFIG}" "${S37UC_DISPLAY_SELECT_SENTINEL}"
  revert_one "${XTENSA_KCONFIG}" "${S37UC_SENSOR_SELECT_SENTINEL}"
  revert_one "${XTENSA_KCONFIG}" "${SSI_SD_SELECT_SENTINEL}"
  echo "[integrate] revert complete (rerun build-qemu.sh to recompile)"
  exit 0
fi

# ---- Copy peripheral sources ----

echo "[integrate] copying esp32s3_gpspi2.c → hw/ssi/"
cp -f "${SRC_DIR}/esp32s3_gpspi2.c" "${GPSPI2_DST}"

echo "[integrate] copying ssd1677_gdeq0426t82.c → hw/display/"
cp -f "${SRC_DIR}/ssd1677_gdeq0426t82.c" "${DISPLAY_DST}"
echo "[integrate] copying uc8253c.c → hw/display/"
cp -f "${SRC_DIR}/uc8253c.c" "${S37UC_DISPLAY_DST}"

mkdir -p "${QEMU_DIR}/hw/sensor"
echo "[integrate] copying ft6336u.c → hw/sensor/"
cp -f "${SRC_DIR}/ft6336u.c" "${SENSOR_DST}"
echo "[integrate] copying chsc6440.c → hw/sensor/"
cp -f "${SRC_DIR}/chsc6440.c" "${S37UC_SENSOR_DST}"
echo "[integrate] copying LilyGo I2C foundation probe → hw/i2c/"
cp -f "${SRC_DIR}/lilygo_i2c_probe.c" "${LILYGO_I2C_PROBE_DST}"
cp -f "${SRC_DIR}/lilygo_i2c_probe.h" "${LILYGO_I2C_PROBE_HEADER_DST}"
cp -f "${SRC_DIR}/lilygo_display_input.c" "${LILYGO_DISPLAY_INPUT_DST}"
cp -f "${SRC_DIR}/lilygo_display_input.h" "${LILYGO_DISPLAY_INPUT_HEADER_DST}"
cp -f "${SRC_DIR}/lilygo_bq27220.c" "${LILYGO_BQ27220_DST}"
cp -f "${SRC_DIR}/lilygo_bq27220.h" "${LILYGO_BQ27220_HEADER_DST}"
cp -f "${SRC_DIR}/lilygo_power_rtc.c" "${LILYGO_POWER_RTC_DST}"
cp -f "${SRC_DIR}/lilygo_power_rtc.h" "${LILYGO_POWER_RTC_HEADER_DST}"
echo "[integrate] copying LilyGo GNSS UART2 peer → hw/char/"
cp -f "${SRC_DIR}/lilygo_gnss_uart.c" "${LILYGO_GNSS_UART_DST}"
cp -f "${SRC_DIR}/lilygo_gnss_uart.h" "${LILYGO_GNSS_UART_HEADER_DST}"
cp -f "${SRC_DIR}/lilygo_gnss_chardev.c" "${LILYGO_GNSS_CHARDEV_DST}"
cp -f "${SRC_DIR}/lilygo_gnss_chardev.h" "${LILYGO_GNSS_CHARDEV_HEADER_DST}"
echo "[integrate] copying LilyGo peripheral control owner → hw/misc/"
cp -f "${SRC_DIR}/lilygo_peripheral_control.c" "${LILYGO_CONTROL_DST}"
cp -f "${SRC_DIR}/lilygo_peripheral_control.h" "${LILYGO_CONTROL_HEADER_DST}"
echo "[integrate] copying LilyGo SX1262 → hw/ssi/"
cp -f "${SRC_DIR}/lilygo_sx1262.c" "${LILYGO_SX1262_DST}"
cp -f "${SRC_DIR}/lilygo_sx1262.h" "${LILYGO_SX1262_HEADER_DST}"
echo "[integrate] copying ESP32-S3 compatibility onto upstream I2C controller → hw/i2c/"
cp -f "${SRC_DIR}/esp32_i2c.c" "${ESP32_I2C_DST}"
cp -f "${SRC_DIR}/esp32_i2c.h" "${ESP32_I2C_HEADER_DST}"

mkdir -p "$(dirname "${ADDRS_HEADER_DST}")"
echo "[integrate] copying mofei-sim-addrs.h → include/hw/xtensa/"
cp -f "${SRC_DIR}/mofei-sim-addrs.h" "${ADDRS_HEADER_DST}"
mkdir -p "$(dirname "${HW_ADDRS_HEADER_DST}")"
echo "[integrate] copying mofei-sim-addrs.h → hw/xtensa/"
cp -f "${SRC_DIR}/mofei-sim-addrs.h" "${HW_ADDRS_HEADER_DST}"

# ---- Apply Meson / Kconfig fragments (idempotent) ----

append_if_missing() {
  local file="$1"
  local sentinel="$2"
  local fragment="$3"
  if [[ ! -f "${file}" ]]; then
    echo "[integrate] WARN: ${file} not found, skipping" >&2
    return
  fi
  if grep -qF "${sentinel}" "${file}"; then
    echo "[integrate] already present in ${file##*/}: ${sentinel}"
    return
  fi
  echo "" >> "${file}"
  cat "${fragment}" >> "${file}"
  echo "[integrate] appended fragment to ${file##*/}"
}

append_literal_if_missing() {
  local file="$1"
  local sentinel="$2"
  local line="$3"
  if [[ ! -f "${file}" ]]; then
    echo "[integrate] WARN: ${file} not found, skipping" >&2
    return
  fi
  if grep -qF "${sentinel}" "${file}"; then
    echo "[integrate] already present in ${file##*/}: ${sentinel}"
    return
  fi
  echo "" >> "${file}"
  printf '%s\n' "${line}" >> "${file}"
  echo "[integrate] appended line to ${file##*/}: ${sentinel}"
}

append_block_if_missing() {
  local file="$1"
  local sentinel="$2"
  local block="$3"
  if [[ ! -f "${file}" ]]; then
    echo "[integrate] WARN: ${file} not found, skipping" >&2
    return
  fi
  if grep -qF "${sentinel}" "${file}"; then
    echo "[integrate] already present in ${file##*/}: ${sentinel}"
    return
  fi
  echo "" >> "${file}"
  printf '%s\n' "${block}" >> "${file}"
  echo "[integrate] appended block to ${file##*/}: ${sentinel}"
}

# GPSPI2 → hw/ssi/
append_if_missing "${SSI_MESON}"   "${GPSPI2_MESON_SENTINEL}"   "${SRC_DIR}/gpspi2-meson.fragment"
append_if_missing "${SSI_KCONFIG}" "${GPSPI2_KCONFIG_SENTINEL}" "${SRC_DIR}/gpspi2-Kconfig.fragment"

# SSD1677 → hw/display/
append_if_missing "${DISPLAY_MESON}"   "${DISPLAY_MESON_SENTINEL}"   "${SRC_DIR}/display-meson.fragment"
append_if_missing "${DISPLAY_KCONFIG}" "${DISPLAY_KCONFIG_SENTINEL}" "${SRC_DIR}/display-Kconfig.fragment"

# Existing QEMU caches can already contain the original Mofei fragment but not
# the later s37uc entries. Add the s37uc registrations independently so the
# incremental path compiles both board peripheral sets.
append_literal_if_missing "${DISPLAY_MESON}" "${S37UC_DISPLAY_MESON_SENTINEL}" \
  "system_ss.add(when: 'CONFIG_UC8253C_S37UC', if_true: files('uc8253c.c'))"
append_block_if_missing "${DISPLAY_KCONFIG}" "${S37UC_DISPLAY_KCONFIG_SENTINEL}" \
  $'config UC8253C_S37UC\n    bool\n    depends on SSI'

# FT6336U → hw/sensor/
if [[ ! -f "${SENSOR_MESON}" ]]; then
  echo "[integrate] creating ${SENSOR_MESON} (fork did not have one)"
  cat > "${SENSOR_MESON}" <<'EOF'
# Auto-managed by apps/simulator/scripts/integrate-peripherals.sh
sensor_ss = ss.source_set()
EOF
fi
if [[ ! -f "${SENSOR_KCONFIG}" ]]; then
  echo "[integrate] creating ${SENSOR_KCONFIG} (fork did not have one)"
  touch "${SENSOR_KCONFIG}"
fi

append_if_missing "${SENSOR_MESON}"   "${SENSOR_MESON_SENTINEL}"   "${SRC_DIR}/sensor-meson.fragment"
append_if_missing "${SENSOR_KCONFIG}" "${SENSOR_KCONFIG_SENTINEL}" "${SRC_DIR}/sensor-Kconfig.fragment"
append_literal_if_missing "${SENSOR_MESON}" "${S37UC_SENSOR_MESON_SENTINEL}" \
  "system_ss.add(when: 'CONFIG_CHSC6440_S37UC', if_true: files('chsc6440.c'))"
append_block_if_missing "${SENSOR_KCONFIG}" "${S37UC_SENSOR_KCONFIG_SENTINEL}" \
  $'config CHSC6440_S37UC\n    bool\n    select BITBANG_I2C'
append_literal_if_missing "${I2C_MESON}" "${LILYGO_I2C_PROBE_MESON_SENTINEL}" \
  "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('lilygo_i2c_probe.c'))"
append_literal_if_missing "${I2C_MESON}" "${LILYGO_DISPLAY_INPUT_MESON_SENTINEL}" \
  "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('lilygo_display_input.c'))"
append_literal_if_missing "${I2C_MESON}" "${LILYGO_BQ27220_MESON_SENTINEL}" \
  "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('lilygo_bq27220.c'))"
append_literal_if_missing "${I2C_MESON}" "${LILYGO_POWER_RTC_MESON_SENTINEL}" \
  "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('lilygo_power_rtc.c'))"
append_literal_if_missing "${CHAR_MESON}" "${LILYGO_GNSS_UART_MESON_SENTINEL}" \
  "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('lilygo_gnss_uart.c', 'lilygo_gnss_chardev.c'))"
append_literal_if_missing "${MISC_MESON}" "${LILYGO_CONTROL_MESON_SENTINEL}" \
  "system_ss.add(when: 'CONFIG_XTENSA_ESP32S3', if_true: files('lilygo_peripheral_control.c'))"
append_literal_if_missing "${SSI_MESON}" "${LILYGO_SX1262_MESON_SENTINEL}" \
  "system_ss.add(when: 'CONFIG_ESP32S3_GPSPI2', if_true: files('lilygo_sx1262.c'))"

# XTENSA SoC selects all three devices
append_if_missing "${XTENSA_KCONFIG}" "${XTENSA_KCONFIG_SENTINEL}" "${SRC_DIR}/xtensa-Kconfig.fragment"
append_literal_if_missing "${XTENSA_KCONFIG}" "${SSI_SD_SELECT_SENTINEL}" "    select SSI_SD"
append_literal_if_missing "${XTENSA_KCONFIG}" "${S37UC_SENSOR_SELECT_SENTINEL}" "    select CHSC6440_S37UC"
append_literal_if_missing "${XTENSA_KCONFIG}" "${S37UC_DISPLAY_SELECT_SENTINEL}" "    select UC8253C_S37UC"

if [[ ${SYNC_REVIEWED_SOURCES} -eq 1 ]]; then
  sync_simulator_qemu_sources
  echo "[integrate] reviewed QEMU source mirrors synchronized"
  exit 0
fi

# ---- SoC machine patch (opt-in, requires --soc) ----

if [[ ${APPLY_SOC} -eq 1 ]]; then
  XTENSA_PATCH="${SRC_DIR}/xtensa-reset-vector.patch"
  SOC_PATCH="${SRC_DIR}/esp32s3-soc-machine.patch"
  check_xtensa_window_vectors
  patch_targets_are_clean
  echo "[integrate] applying Xtensa reset-vector patch"
  ( cd "${QEMU_DIR}" && patch --forward -l -p1 < "${XTENSA_PATCH}" ) || {
    echo "[integrate] patch did not apply cleanly."
    echo "[integrate]   Patch file: ${XTENSA_PATCH}"
    echo "[integrate]   Run with --check to verify against clean QEMU HEAD,"
    echo "[integrate]   or clean/recreate apps/simulator/.qemu-cache/qemu before applying."
    exit 4
  }
  echo "[integrate] applying SoC machine patch"
  ( cd "${QEMU_DIR}" && patch --forward -l -p1 < "${SOC_PATCH}" ) || {
    echo "[integrate] patch did not apply cleanly."
    echo "[integrate]   Patch file: ${SOC_PATCH}"
    echo "[integrate]   Run with --check to verify against clean QEMU HEAD,"
    echo "[integrate]   or clean/recreate apps/simulator/.qemu-cache/qemu before applying."
    exit 4
  }
  sync_simulator_qemu_sources
  echo "[integrate] QEMU patches applied. Re-run scripts/build-qemu.sh to recompile."
fi

echo ""
echo "[integrate] done. Peripheral sources and build fragments applied."
echo "[integrate] next: apps/simulator/scripts/build-qemu.sh"
[[ ${APPLY_SOC} -eq 0 ]] && echo "[integrate] (re-run with --soc to also apply the ESP32-S3 machine patch)"
