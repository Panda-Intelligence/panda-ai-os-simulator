# Standalone native foundation implementation plan

Date: 2026-09-18
Executor: Codex

## Scope

Extract the standalone board contract and native path resolution without changing
the framebuffer IPC, QEMU machine name, firmware file formats, or frontend
bridge method signatures.

## Implementation

1. Make `boards.json` contain only standalone board identity, display geometry,
   QEMU peripheral identifiers, capabilities, and key labels/aliases.
2. Seed `the consumer-owned tools/simulator-integration/murphy-boards.json` with the removed product firmware and
   legacy build metadata. It is opt-in through
   `PANDA_SIMULATOR_INTEGRATION`; there is no implicit parent-repository lookup.
3. Add a dependency-free Rust resolver used by the Tauri and headless paths.
   `PANDA_SIMULATOR_PROJECT_ROOT` resolves relative legacy firmware paths and
   `PANDA_SIMULATOR_QEMU` overrides the native QEMU binary.
4. Keep empty frontend quicklaunch firmware explicit: without an opt-in
   integration mapping, the native launcher reports that a firmware path is
   required instead of guessing one.
5. Parameterize browser-QEMU firmware inputs with a coherent explicit firmware
   directory while keeping individual artifact overrides available.

## Verification

- Validate the JSON registry shape and TypeScript compilation without changing
  package metadata.
- Run dependency-free resolver tests with `rustc --test`.
- Run shell syntax and `build-qemu-wasm.sh --help`; do not build QEMU, Rust, or
  Tauri artifacts in this lane.

## Public board schema contract

The public root has `defaultBoard` and `boards`. Each board has `id`,
`displayName`, raw/framebuffer/output dimensions, `framebufferFormat`,
`touchI2cAddr`, `qemu.displayType`, `qemu.touchType`, `capabilities`, and
`keyMap`. A key has `id`, `label`, and optional `aliases`. Firmware paths,
Murphy board names, headless binary paths, and case roots are not public board
fields.
