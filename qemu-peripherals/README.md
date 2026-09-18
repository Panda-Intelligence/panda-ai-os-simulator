# QEMU peripherals - Murphy simulator

C source for the custom QEMU virtual peripherals that present Murphy-supported
e-paper boards, SPI controller, and capacitive touch controllers to the guest
firmware.

## Devices

| Device | QOM Type | Parent | Bus | Channel |
|--------|----------|--------|-----|---------|
| GPSPI2 controller | `esp32s3-gpspi2` | `TYPE_SYS_BUS_DEVICE` | SSI (master) | N/A |
| SSD1677 e-ink panel | `ssd1677-gdeq0426` | `TYPE_SSI_PERIPHERAL` | SSI (slave) | 0x01 framebuffer |
| FT6336U touch | `ft6336u-mofei` | `TYPE_I2C_SLAVE` | I2C (slave) | 0x04 touch-event |
| UC8253C e-ink panel | `uc8253c` | `TYPE_SSI_PERIPHERAL` | SSI (slave) | 0x01 framebuffer |
| CHSC6440 touch | `chsc6440` | `TYPE_I2C_SLAVE` | I2C (slave) | 0x04 touch-event |

## Files

| File | Purpose |
|------|---------|
| `esp32s3_gpspi2.c` | GPSPI2 minimal controller (FIFO + DMA shortcut) |
| `ssd1677_gdeq0426t82.c` | SSD1677 e-ink panel hybrid FSM |
| `ft6336u.c` | FT6336U touch minimal register machine |
| `uc8253c.c` | UC8253C 416x240 calibration e-ink panel FSM for `s37uc` |
| `chsc6440.c` | CHSC6440 touch register machine for `s37uc` |
| `esp32s3-soc-machine.patch` | Conceptual diff for `hw/xtensa/esp32s3.c` |
| `*-meson.fragment` | Meson build fragments |
| `*-Kconfig.fragment` | Kconfig entries |
| `xtensa-Kconfig.fragment` | SoC-level selects |

## Integration

Run `scripts/integrate-peripherals.sh [--soc]` to copy sources and apply
build fragments. See [`INTEGRATION.md`](INTEGRATION.md) for full details.

## Manual test checklist (post-integration)

- [ ] HW reset (RES# low pulse) clears framebuffer to all-white (0xFF).
- [ ] SW reset (cmd 0x12) does the same and briefly drives BUSY high.
- [ ] Cmd 0x11 → 0x44/0x45 → 0x4E/0x4F → 0x24 + N data bytes populates `fb[]`.
- [ ] Cmd 0x47 (auto-write BW) with arg 0x55 produces a striped framebuffer.
- [ ] Cmd 0x22 + 0xF7 followed by cmd 0x20 emits framebuffer to chardev.
- [ ] `EpdBusEsp` command/data calls reach the same SSD1677 FSM; `0xD7`/`0xCF`
      lock-transition profiles report one modeled activation and the reduced
      command-level BUSY interval.
- [ ] Checkerboard test firmware renders on the Tauri canvas.
- [ ] FT6336U: mouse-down on canvas → touch event → firmware serial log shows touch.

The SSD1677 model covers the portable Panda AI OS `EpdBusEsp` transport as well
as the legacy Mofei display wrapper. This lets simulator E2E runs verify the
selected register profile, RAM-plane writes, activation count, and modeled BUSY
duration. It deliberately does not claim to reproduce the panel's
electrophoretic optical waveform or perceptible flash phases; those remain a
physical Mofei qualification gate.

## Direct framebuffer injection contract

The full firmware path does not rely only on the SSD1677 SPI byte stream. QEMU can sample the firmware-owned
framebuffer directly and publish it to the host UI.

- Wire the direct-read framebuffer source once from `HELPER(mofei_inject_framebuffer)`.
- Start the fallback inject timer once. Repeated `starting periodic push timer` logs mean the helper is reinitializing instead of publishing.
- Publish synchronously on firmware publish hooks so touch/UI transitions appear at host speed.
- Keep the fallback timer on `QEMU_CLOCK_REALTIME` with a short cadence, currently 33ms.
- Suppress timer-only transient all-white samples after content has been observed, but allow synchronous publish hooks to send intentional all-white frames.

This split prevents touch-triggered blank screens where a timer samples between `clearScreen()` and the following draw,
while still letting firmware deliberately clear the display.

## Activity lifecycle fidelity

The simulator should behave like a running Mofei device, not like a one-shot screenshot capture.

- QEMU and simulator firmware hooks must let the guest continue through `setup()`, `loop()`, `ActivityManager`, Dashboard, and normal activity transitions.
- Touch events should enter the firmware input path and remain usable after boot, not just during the first frame capture.
- Simulator-only policy bypasses are allowed when real hardware state would block automated smoke runs, for example bypassing passcode wake-guard routing under `MOFEI_SIMULATOR`, but these bypasses must not skip Dashboard/activity code.
- Smoke tests should look for continued framebuffer updates plus touch queue/read evidence. If the simulator renders one lockscreen or boot frame and then stops progressing, treat that as a fidelity regression.

## Roadmap: browser/WASM frontend

Future work should allow the simulator to run in a browser/WASM environment for lightweight demos and contributor QA.
That frontend is out of scope for the current native QEMU/headless fidelity gate, and it must consume real firmware
framebuffer/touch semantics rather than replacing Dashboard or activity rendering with mocked frames.
