# Panda AI OS Simulator

**Open-source simulator repository — Panda-owned code is licensed under MIT.**

This repository separates the simulator platform from Murphy: React browser UI,
Tauri host, Rust headless harness, and QEMU peripheral/native/WASM tooling.
The MIT license covers Panda-owned simulator code. QEMU/Espressif sources,
dependencies, ROMs, guest firmware, fonts, media, and other third-party
materials remain governed by their own notices and distribution rights; no
unresolved rights are cleared by this repository license. No complete Panda AI
OS source tree, user books, fonts, ROM or guest binary is included. A missing
runtime is shown as unavailable, never a fabricated boot.

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

## Package a standalone site from explicit artifacts

```sh
npm run pack:web -- --manifest ./artifact-input.json \
  --runtime-dir ./approved-runtime --guest-dir ./approved-guest \
  --output-dir ./artifacts/dist --ui-dir ./dist
```

Create the output parent first. Only a new directory or a directory previously
owned by this packer can be replaced. A per-output lock rejects concurrent packs.
A crash can leave a lock/staging directory: inspect it before manual recovery;
this is not a power-loss-durable release installer. Hashes verify bytes, not
publisher identity or copyright. The output is an explicit artifact package;
guest and runtime distribution still require their applicable rights review.

`contracts/web-artifact-manifest-v1.schema.json` describes the input. All roles
are explicit; no cached ROM, firmware, private SD or Panda Cloud path is scanned.
The packer holds bounded verified bytes, rejects symlink ancestors, traversals,
wrong digests and excessive aggregate sizes, and optionally copies built UI.
The standalone Worker publication target is declared in
`deploy/wrangler.jsonc`; this repository change configures it but does not
deploy the Worker or upload release artifacts.

## Minimal buildable example

`examples/uart-demo` compiles assembly without Murphy/ESP-IDF sources. Set
`XTENSA_CC` to an installed Xtensa compiler and run `scripts/build-example.sh`
with a new output directory. Building its ELF is not a runtime boot assertion.

## Boundaries and participation

See `docs/extraction-boundaries.md`, `docs/licensing-review.md`,
`THIRD_PARTY_NOTICES.md`, `CONTRIBUTING.md`, and `SECURITY.md`.
`provenance/export-manifest.json` records the exact original export; subsequent
commits identify intentional edits. `audit:export` is a pattern scan, not legal
or complete secret clearance. `check:publication` remains an explicit artifact
and rights gate; it does not certify third-party, guest, or ROM distribution.
The simulator cannot qualify real e-ink ghosting, electrical, radio or USB behavior.
Launch materials under `docs/launch` are embargoed preparation, not posted claims.

## Continued split and runtime evidence

See `docs/continuation-verification-20260918.md` and `docs/OWNER_HANDOFF.md`.
The native minimal guest and no-SD browser guest have actually executed.
Atomic chunked browser persistence passed a real 128 MiB IndexedDB test.
A separate supplied-runtime RCU assertion still blocks SD-backed reading (issue #5).
The source repository is open under MIT for Panda-owned code, while guest/ROM
rights and runtime qualification remain separate gates. The standalone Worker
is deployed at https://simulator.pandacat.ai/; deployment does not relicense
third-party runtime materials or certify the remaining qualification gates.
