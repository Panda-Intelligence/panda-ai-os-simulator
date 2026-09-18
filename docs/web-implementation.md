# Standalone web simulator implementation plan

Date: 2026-09-18
Owner: Codex
Lane: web

## Objective

Complete one standalone web assembly seam for the private simulator extraction. The browser runtime will consume a manifest and prebuilt runtime and guest artifacts supplied by explicit local build arguments. It will not discover Murphy paths, call Panda Cloud, or fabricate a boot result.

## Scoped files

- `scripts/sync-theme.mjs`
- `scripts/pack-web.mjs` and `scripts/pack-web-*` helpers
- `src/browserSimulatorRuntime.ts`
- `contracts/*`
- `tests/web-*`
- `docs/web-*`

`package.json`, lockfiles, `README.md`, `LICENSE`, provenance, native/QEMU sources, and other worker lanes are out of scope.

## Implementation contract

1. Theme synchronization copies `packages/ui-tokens/panda-ide.css` and fails clearly when that explicit source is absent.
2. The runtime manifest consumer keeps the historical manifest v1 runtime fields and protocol identifiers. New source and digest metadata is optional for old consumers and represented without changing command or framebuffer event names.
3. Browser manifest lookup defaults to `import.meta.env.BASE_URL` plus `manifest.json`. An explicit manifest URL is accepted only when it is a relative URL or same-origin URL without private filesystem paths, credentials, or unsupported schemes.
4. Web packing accepts explicit input and output arguments. The assembly helper copies only the declared allowlist of runtime and guest artifact roles, using `lstat` and bounded byte lengths, and verifies every declared SHA256 digest.
5. A coherent set requires the QEMU module JavaScript, QEMU WASM, worker support when declared, bootloader, partition table, OTA data, ROM, and at least one guest board with firmware, kernel, symbols, and provenance metadata. SD images are copied only when explicitly declared, never auto-discovered.
6. Unknown entries, duplicate paths, absolute paths, URL paths, private-path segments, traversal, symlinks, missing files, zero-byte files, oversize files, bad hashes, and incomplete required sets are rejected.
7. Assembly stages into a uniquely named sibling directory. The prior output remains untouched until validation and copying complete. Failure only removes the helper-owned staging directory, never an arbitrary user destination.
8. Public publication is denied unless the caller supplies the explicit local/private pack mode and the trusted build context required by the contract. A JSON `public: true` value alone cannot open the gate.

## Verification

- Run the scoped `node --test tests/web-*.test.mjs` suite.
- Run direct Node smoke checks for theme synchronization and pack argument validation where available.
- Do not run full Vite, QEMU, Rust, Tauri, device, or hardware builds.
- Record commands, results, and remaining limitations in `.codex/testing.md`, `verification.md`, and `/private/tmp/panda-simulator-oss-20260918/web.result.md`.
