# Murphy Simulator

iPhone-Simulator-style development environment for Murphy-supported ESP32-S3
e-paper boards.

Runs simulator firmware targets under QEMU full emulation, with virtual e-ink
and touch peripherals bridged to a Tauri webview UI for display,
mouse/keyboard input, and SD-card-as-local-directory.

> **Status (2026-06-19): Panda AI OS simulator firmware boot and release-gate E2E are active.**
> The firmware build uses Panda AI OS device artifacts from `apps/panda-os`.
> The release flow builds Panda AI OS and runs the full simulator E2E suite as a
> blocking gate before building release firmware.
> The simulator can represent framebuffer output, touch/buttons, host-backed SD,
> deterministic network fixtures, coarse battery/charging state, frontlight and
> buzzer traces, and deterministic sensor readings. Physical e-ink waveform
> quality, ghosting, real USB enumeration, BLE radio behavior, ADC noise, charger
> electrical behavior, and SD signal integrity remain hardware-only validation.
> For the Mofei SSD1677 path, native QEMU also models the firmware command-level
> panel sequence, including lock-transition `0xD7`/`0xCF` profiles, RAM writes,
> activation count, and BUSY timing. This validates routing and timing intent,
> not the panel's optical flash phases.

## Board registry

`boards.json` is the single source of truth for simulator board metadata. The
Python runners, React/Tauri app, Tauri backend, and headless harness all load
that file directly.

| Board | Panda AI OS board | Firmware artifact | Raw FB | UI output | QEMU display/touch |
|-------|-----------------|-------------------|--------|-----------|--------------------|
| `mofei` (default) | `default` | `apps/panda-os/device/build/panda_os.bin` | 800x480 | 480x800 | `ssd1677-gdeq0426` / `ft6336u` |
| `s3r8` | `mofei` | `apps/panda-os/device/build/panda_os.bin` | 800x480 | 480x800 | `ssd1677-gdeq0426` / `ft6336u` |
| `s37uc` | `s37uc` | `apps/panda-os/device/build-s37uc/panda_os.bin` | 416x240 | 240x416 | `uc8253c` / `chsc6440` |

The Tauri UI exposes the same list through the device picker. Stop the
simulator before switching boards so the backend can restart with the matching
firmware default, framebuffer geometry, and key map.

## Headless usage

```bash
scripts/run_e2e.py --target simulator --board mofei --case test/simulator_e2e/dashboard/navigation/dashboard_dashboard_grid_navigation_smoke.json
scripts/run_e2e.py --target simulator --board s3r8 --case test/simulator_e2e/dashboard/navigation/dashboard_dashboard_grid_navigation_smoke.json
scripts/run_e2e.py --target simulator --board s37uc --case test/simulator_e2e/dashboard/navigation/dashboard_dashboard_grid_navigation_smoke.json
scripts/run_simulator_e2e_parallel.py --board mofei
scripts/run_simulator_e2e_parallel.py --board s3r8 --groups dashboard
scripts/run_simulator_e2e_parallel.py --board s37uc --groups dashboard
```

### LilyGo SDSPI owner matrix

Build a simulator-console LilyGo firmware and run the storage-owned cases with an explicit ELF:

```bash
PANDA_SIMULATOR=1 BOARD=lilygo-t5s3-pro \
  PANDA_BUILD_DIR=apps/panda-os/device/build-sim-lilygo-storage-owner \
  apps/panda-os/tools/build-device.sh
cargo build --manifest-path apps/simulator/headless/Cargo.toml
python3 scripts/run_simulator_e2e_parallel.py \
  --board lilygo-t5s3-pro \
  --firmware apps/panda-os/device/build-sim-lilygo-storage-owner/murphy_os.elf \
  --case-root test/simulator_board_e2e/lilygo-t5s3-pro \
  --groups storage \
  --artifacts-root .codex/lilygo-storage-fault-matrix \
  --jobs 1 \
  --step-timeout 45
```

These cases require `storage.sdspi` and cover seeded read/write persistence, initial absence, removal/reinsertion, read
and write faults, and invalid FAT recovery. They prove firmware-visible behavior only, not electrical timing, signal
integrity, throughput, wear, or physical power-loss durability.

## One-time setup
