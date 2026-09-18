# Standalone simulator web lane result

Date: 2026-09-18
Owner: Codex
Branch: `feat/standalone-foundation`

## Actual files

- `docs/web-implementation.md`
- `contracts/web-artifact-manifest-v1.schema.json`
- `scripts/pack-web.mjs`
- `scripts/pack-web-manifest.mjs`
- `scripts/sync-theme.mjs`
- `src/browserSimulatorRuntime.ts`
- `tests/web-pack.test.mjs`
- `docs/web-operations.md`
- `docs/web-testing.md`
- `docs/web-verification.md`
- `docs/web-review.md`

## Commands and results

- `node_modules/.bin/tsc --noEmit`: passed.
- `node --test tests/web-*.test.mjs`: passed, 3/3.
- `node scripts/sync-theme.mjs`: passed; output matches `packages/ui-tokens/panda-ide.css`.
- `node scripts/pack-web.mjs` without arguments: rejected with exit 1 and usage text, as expected.
- `git diff --check`: passed.

## Completed seam

The web lane now has an explicit private pack command and helper that validates bounded allowlisted runtime and guest entries, rejects unsafe paths/symlinks/duplicates/missing files/oversize or bad-digest files, requires a coherent QEMU plus guest set, copies only declared artifacts, and preserves the prior output on failure. The emitted manifest keeps runtime manifest v1 fields and adds source/digest metadata with a private local-only publication label.

The browser consumer now derives its default manifest URL from `import.meta.env.BASE_URL`, validates optional explicit manifest URLs, and resolves same-origin artifact paths relative to the manifest. The theme sync no longer references `../shared`.

## Remaining limitations

- Main still needs to add the package script wiring and run the full browser/Vite integration build.
- This worker did not run QEMU, device, Rust, Tauri, hardware, deployment, or guest boot/framebuffer verification.
- The packer requires main’s prebuilt artifacts and a source manifest supplied through explicit arguments; no real production artifact pack was run here.
- No public publish path is implemented or authorized. The output is intentionally private/local-only pending main’s publication controls.
