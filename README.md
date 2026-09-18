# Panda AI OS Simulator

**Private extraction candidate — not yet a public open-source release.**

This repository separates the simulator platform from Murphy: React browser UI,
Tauri host, Rust headless harness, and QEMU peripheral/native/WASM tooling.
The current inherited license still applies. Proposed open-source licensing,
ROM and official guest redistribution require the approval tracked in issue #1.
No complete Panda AI OS source tree, user books, fonts, ROM or guest binary is
included. A missing runtime is shown as unavailable, never a fabricated boot.

## Standalone frontend

```sh
npm ci --ignore-scripts --no-audit --no-fund
npm test
npm run build
npm run serve:standalone -- --root ./dist
```

The preview binds only to loopback and serves cross-origin isolation headers.
This builds the real UI, not a running QEMU guest. Supplying reviewed runtime and
guest artifacts is a separate step; without them startup remains unavailable.
Node 24 is the CI target. Native QEMU, Rust/Tauri, browser and product journeys
have separate qualification gates; frontend success does not imply those pass.

## Explicit runtime inputs

Native overrides: `PANDA_SIMULATOR_QEMU`, `PANDA_SIMULATOR_PROJECT_ROOT`, and
`PANDA_SIMULATOR_INTEGRATION`. Product-specific board/firmware paths live in the
consumer repository, not the public board registry. See docs below.

## Package a private site from explicit artifacts

```sh
npm run pack:web -- --manifest ./artifact-input.json \
  --runtime-dir ./approved-runtime --guest-dir ./approved-guest \
  --output-dir ./artifacts/dist --ui-dir ./dist
```

Create the output parent first. Only a new directory or a directory previously
owned by this packer can be replaced. A per-output lock rejects concurrent packs.
A crash can leave a lock/staging directory: inspect it before manual recovery;
this is not a power-loss-durable release installer. Hashes verify bytes, not
publisher identity or copyright. The output remains private/local-only.

`contracts/web-artifact-manifest-v1.schema.json` describes the input. All roles
are explicit; no cached ROM, firmware, private SD or Panda Cloud path is scanned.
The packer holds bounded verified bytes, rejects symlink ancestors, traversals,
wrong digests and excessive aggregate sizes, and optionally copies built UI.
Public deployment is intentionally not configured in `deploy/wrangler.jsonc`.

## Minimal buildable example

`examples/uart-demo` compiles assembly without Murphy/ESP-IDF sources. Set
`XTENSA_CC` to an installed Xtensa compiler and run `scripts/build-example.sh`
with a new output directory. Building its ELF is not a runtime boot assertion.

## Boundaries and participation

See `docs/extraction-boundaries.md`, `docs/licensing-review.md`,
`THIRD_PARTY_NOTICES.md`, `CONTRIBUTING.md`, and `SECURITY.md`.
`provenance/export-manifest.json` records the exact original export; subsequent
commits identify intentional edits. `audit:export` is a pattern scan, not legal
or complete secret clearance. `check:publication` remains blocked pending G0.
The simulator cannot qualify real e-ink ghosting, electrical, radio or USB behavior.
Launch materials under `docs/launch` are embargoed preparation, not posted claims.
