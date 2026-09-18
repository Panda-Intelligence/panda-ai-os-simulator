# LilyGo T5S3 Pro simulator runtime

Updated: 2026-07-11
Author: Codex

LilyGo T5S3 Pro uses the project-patched Espressif ESP32-S3 QEMU; LilyGo does not provide a full board simulator. The
upstream repository, tag, commit, and minimum free-space floor live in `apps/simulator/qemu-runtime.env`.
`apps/simulator/run setup` acquires that exact commit, synchronizes the reviewed QEMU source mirrors, verifies the
patches, and builds the runtime for the current host architecture.

The first setup requires at least 8 GiB free. Install the macOS dependencies with Homebrew:

```bash
brew install ninja glib pixman libgcrypt pkg-config gnutls
apps/simulator/run setup
```

Linux needs the equivalent `ninja-build`, GLib, Pixman, libgcrypt, pkg-config, and GnuTLS development packages. Check
the cache, revision, remote, binary architecture, ESP32-S3 machine, and patch integrity with:

```bash
apps/simulator/run check
```

When the cached remote or revision differs, the check rejects it and prints a repair command. It never switches a dirty
checkout silently.

Run the complete LilyGo firmware boot smoke with:

```bash
bash test/run_lilygo_simulator_boot_smoke_test.sh
```

This command builds `apps/panda-os/device/build-sim-lilygo-t5s3-pro`, boots the real firmware with
`--board lilygo-t5s3-pro`, injects one safe non-interactive edge touch, and writes
`.simulator-e2e-lilygo-boot/transcript.log` plus
`.simulator-e2e-lilygo-boot/screenshot.png`. The smoke verifies board identity, firmware selection, the firmware touch
read, a non-blank 540x960 grayscale PNG, and at least two output tones.

For a targeted input diagnostic, override the reference tap with `PANDA_LILYGO_SIM_TAP_X` and
`PANDA_LILYGO_SIM_TAP_Y`. The committed default remains `10,10` so the input round-trip does not navigate away from the
boot frame before the final screenshot is captured.
