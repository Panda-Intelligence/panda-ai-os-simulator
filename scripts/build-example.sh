#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC="${XTENSA_CC:-xtensa-esp32s3-elf-gcc}"
if [[ "${1:-}" == "--help" ]]; then
  echo 'Usage: XTENSA_CC=<cross-compiler> scripts/build-example.sh <new-output-directory>'
  exit 0
fi
[[ $# == 1 ]] || { echo 'Specify a new output directory; use --help.' >&2; exit 2; }
command -v "$CC" >/dev/null || { echo 'Set XTENSA_CC to an installed ESP32-S3 Xtensa cross-compiler.' >&2; exit 2; }
[[ ! -e "$1" ]] || { echo 'Output already exists; no files overwritten.' >&2; exit 2; }
mkdir -p "$1"
OUT="$(cd "$1" && pwd)"
"$CC" -nostdlib -nostartfiles -Wl,--build-id=none -T "$ROOT/examples/uart-demo/esp32s3.ld"   "$ROOT/examples/uart-demo/start.S" -o "$OUT/uart-demo.elf"
python3 "$ROOT/scripts/check-example-elf.py" "$OUT/uart-demo.elf"
echo "Built $OUT/uart-demo.elf (UART PANDA marker; execution qualification is separate)."
