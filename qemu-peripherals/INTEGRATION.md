# Mofei simulator — QEMU integration patches

This directory contains C source files and build-system fragments for three
custom QEMU peripherals, plus a conceptual SoC machine patch showing how to
wire them into the Espressif QEMU fork's ESP32-S3 machine model.

> **Note**: The SoC machine patch has approximate line numbers and does NOT
> apply cleanly with `git apply`. It is a reference document — the actual
> changes are applied by hand to `hw/xtensa/esp32s3.c` during integration.
> The `integrate-peripherals.sh` script handles source copying and
> Meson/Kconfig fragments automatically.

## Architecture

```
                         esp32s3.c (SoC machine)
                         ┌─────────────────────────────────────┐
  Firmware SPI writes    │                                     │
  0x60024000 ──────────► │ GPSPI2 ──SSI bus──► SSD1677 ──chardev──► Tauri UI
  (MMIO)                 │                         ▲               (framebuffer)
                         │          GPIO5 (CS) ────┘
                         │          GPIO6 (DC)  ────┘
                         │          GPIO7 (RST) ────┘
                         │          GPIO8 (BUSY)◄───┘
                         │
  Firmware I2C bitbang   │
  GPIO12/13 ───────────► │ GPIO_I2C ──I2C bus──► FT6336U ──chardev──► Tauri UI
  (via GPIO matrix)      │              ▲                        (touch events)
                         │              │                        ◄── mouse/keyboard
                         │          GPIO44 (INT)◄──┘
                         └─────────────────────────────────────┘
```

## Files

| File | Target location | Purpose |
|------|----------------|---------|
| `esp32s3_gpspi2.c` | `hw/ssi/` | Minimal GPSPI2 (SPI2) controller: captures firmware SPI transfers (FIFO + DMA shortcut) and pushes bytes to the SSI bus |
| `ssd1677_gdeq0426t82.c` | `hw/display/` | SSD1677 e-ink panel FSM: parses SPI commands, manages 800×480 framebuffer, pushes to chardev on Master Activation |
| `ft6336u.c` | `hw/sensor/` | FT6336U touch controller: I2C slave with minimal register machine, receives touch events from chardev, drives INT pin |
| `gpspi2-meson.fragment` | append to `hw/ssi/meson.build` | Meson build registration for GPSPI2 |
| `gpspi2-Kconfig.fragment` | append to `hw/ssi/Kconfig` | Kconfig for GPSPI2 |
| `display-meson.fragment` | append to `hw/display/meson.build` | Meson build registration for SSD1677 |
| `display-Kconfig.fragment` | append to `hw/display/Kconfig` | Kconfig for SSD1677 |
| `sensor-meson.fragment` | append to `hw/sensor/meson.build` | Meson build registration for FT6336U |
| `sensor-Kconfig.fragment` | append to `hw/sensor/Kconfig` | Kconfig for FT6336U |
| `xtensa-Kconfig.fragment` | append to `hw/xtensa/Kconfig` | `select` lines under `config XTENSA_ESP32S3` to pull all three devices into the build |
| `esp32s3-soc-machine.patch` | apply to `hw/xtensa/esp32s3.c` | SoC machine wiring: instantiate GPSPI2, SSD1677, FT6336U, GPIO_I2C and connect them |

## Mofei pin map (verified)

Source: `lib/hal/HalGPIO.h`, `lib/hal/MofeiDisplay.h`, `.trellis/tasks/04-29-mofei-touch-support/prd.md`

| Function | GPIO | Direction |
|----------|------|-----------|
| EPD SCLK | 4 | GPSPI2 CLK (IO_MUX) |
| EPD MOSI | 3 | GPSPI2 MOSI (IO_MUX) |
| EPD CS | 5 | Firmware GPIO out → SSD1677 SSI CS |
| EPD DC | 6 | Firmware GPIO out → SSD1677 dc input |
| EPD RST | 7 | Firmware GPIO out → SSD1677 reset input |
| EPD BUSY | 8 | SSD1677 busy output → GPIO in |
| Touch SDA | 13 | Bidirectional: GPIO out → GPIO_I2C SDA in, GPIO_I2C SDA out → GPIO in |
| Touch SCL | 12 | GPIO out → GPIO_I2C SCL in |
| Touch INT | 44 | FT6336U int output → GPIO in (active-low) |
| Touch PWR | 45 | (stub — not wired in QEMU) |
| KEY_LOCK | 0 | Discrete key (firmware reads via GPIO) |
| KEY1 | 1 | Discrete key |
| KEY2 | 2 | Discrete key |

## Why a custom GPSPI2 model

The Espressif QEMU fork's existing `TYPE_ESP32S3_SPI` models the SPI flash
controller (SPI0/SPI1) with `SPI_MEM_*` registers. The Mofei firmware's SPI
display bus uses GPSPI2 (SPI2), which has a completely different register
layout (`SPI_CMD`, `SPI_USER`, `SPI_W0..W15`, etc.) at MMIO base `0x60024000`.

Our `esp32s3_gpspi2.c` is a minimal model that:
- Creates an SSI bus for the SSD1677 to sit on
- Handles FIFO-mode transfers (≤ 64 bytes from W0-W15)
- Handles DMA-mode transfers by reading the DMA descriptor chain directly
  from guest memory (bypassing the GDMA controller model)
- Does NOT manage CS — the firmware bit-bangs CS via GPIO5, which is wired
  directly to the SSD1677's SSI_GPIO_CS input

## Reproducing the integration locally

```bash
# After running scripts/build-qemu.sh once:
cd apps/simulator

# Copy peripherals + apply build fragments:
./scripts/integrate-peripherals.sh

# Also apply the SoC machine wiring patch (hand-merge may be needed):
./scripts/integrate-peripherals.sh --soc

# Rebuild QEMU (incremental):
./scripts/build-qemu.sh --force
```

## s37uc board peripherals (UC8253C + CHSC6440)

The s37uc board reuses the shared GPSPI2 controller but swaps the panel and
touch models. Both board peripheral sets compile into the single QEMU binary;
the SoC machine instantiates only the set matching the selected board.

| File | Target location | Purpose |
|------|----------------|---------|
| `uc8253c.c` | `hw/display/` | UC8253C 416×240 calibration e-ink FSM (PSR/PWR/LUT-from-register), direct-guest-fb push, no Y-flip |
| `chsc6440.c` | `hw/sensor/` | CHSC6440 touch (FT6x36-compatible report map), I2C slave addr **0x40** |

Build wiring: `display-meson.fragment` / `display-Kconfig.fragment` register
`UC8253C_S37UC`; `sensor-meson.fragment` / `sensor-Kconfig.fragment` register
`CHSC6440_S37UC`; `xtensa-Kconfig.fragment` `select`s both. `integrate-peripherals.sh`
copies both files.

### s37uc pin map (from docs/hw/new/SCH_Schematic1_1_2026-06-06.pdf + s37uc/board_config.h)

| Function | Net | Notes |
|----------|-----|-------|
| EPD SCLK | INK_CLK | GPSPI2 CLK |
| EPD MOSI | INK_MOSI | GPSPI2 MOSI |
| EPD CS | INK_CS | Firmware GPIO out → UC8253C SSI CS |
| EPD DC | INK_DC | Firmware GPIO out → UC8253C dc input |
| EPD RST | INK_RST | Firmware GPIO out → UC8253C reset input |
| EPD BUSY | INK_BUSY | UC8253C busy output → GPIO in (active-low busy) |
| Touch SDA | IIC_SDA | BITBANG_I2C SDA |
| Touch SCL | IIC_SCL | BITBANG_I2C SCL |
| Touch INT | TOUCH_INT | CHSC6440 int output → GPIO in (active-low) |
| Side keys | SIDEKEY, SIDEKEY1 | discrete keys (firmware reads via GPIO) |

> **OWED (on-host)**: the SoC machine wiring (`hw/xtensa/esp32s3.c`) currently
> instantiates the Mofei set. Board-conditional instantiation of UC8253C +
> CHSC6440 at the s37uc pins, plus s37uc firmware ELF symbol resolution
> (`Uc8253Display` framebuffer addr for the direct-fb push), require the
> espressif/qemu fork built locally and iterative bring-up. The model sources
> above are committed; the running s37uc QEMU machine is not yet wired.

## LilyGo shared SPI2 storage

The LilyGo T5S3 Pro profile routes its one `-drive if=sd` backend through GPSPI2 target 0 (CS12), QEMU's upstream
`ssi-sd` adapter, and the upstream SPI SD card model. Existing profiles keep the DWC SDMMC consumer. Never attach one
`BlockBackend` to both controllers.

Storage peripheral controls use channel `0x06`, protocol version 1, and device 3. Apply mutations only between SPI
transactions. Guest reset preserves externally selected card state; the explicit device reset restores present,
writable, fault-free defaults without replacing raw-image bytes.

Run the focused model and owner gates from the repository root:

```bash
bash test/run_lilygo_simulator_storage_bus_test.sh
python3 scripts/run_simulator_e2e_parallel.py \
  --board lilygo-t5s3-pro \
  --firmware apps/panda-os/device/build-sim-lilygo-storage-owner/murphy_os.elf \
  --case-root test/simulator_board_e2e/lilygo-t5s3-pro \
  --groups storage \
  --jobs 1
```
