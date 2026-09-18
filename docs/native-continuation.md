# Native continuation — 2026-09-18

Executor: Codex

## Correction

The standalone `run` launcher now resolves only from its own checkout and
explicit caller inputs. `check`, `headless`, and `tauri` accept `--firmware`,
`--qemu`, and `--sd-root`; `PANDA_SIMULATOR_QEMU` and the legacy firmware
environment name remain explicit aliases. The launcher no longer imports the
consumer board-profile module, infers a parent project, builds product
firmware, provisions product fonts, installs npm dependencies, or hides cargo
failures.

Headless E2E default font packs are provisioned only for cases that request a
reader/font fixture. Generic startup and generic E2E cases do not require
`tiny.ttf`, Murphy history, or consumer fixture roots.

## Verification

- `bash -n run` — passed.
- `node --test tests/native-run.test.mjs` — 3 passed. The tests launch from an
  unrelated temporary cwd with a clean environment and verify explicit paths,
  consumer-module independence, and missing-firmware failure.
- `cargo test --manifest-path headless/Cargo.toml --lib` — passed (0 tests in
  the library target).
- `cargo test --manifest-path headless/Cargo.toml --bin mofei-sim-headless` —
  compiled and ran 112 tests; 97 passed and 15 failed because this standalone
  checkout intentionally does not contain consumer-owned `apps/panda-os`,
  `apps/toolchain`, or gitignored `test/epubs` fixture assets. These failures
  are fixture-feature gaps, not ordinary startup failures.
- No QEMU, ESP-IDF, Tauri, package installation, or device build was run.

## CMake and runtime gaps

No CMake or QEMU-core files were changed. Actual guest/UART validation remains
with main using the existing installed QEMU and Panda guest browser. The Tauri
application still owns its existing in-app firmware selection path; this lane
only supplies explicit launcher environment and does not modify `src-tauri`.

The remaining fixture tests need an explicit consumer fixture root or generated
test fixtures before the full native test target can be green in this isolated
checkout. No private hooks, settings, credentials, publication, or hardware
actions were used.
