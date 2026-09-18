# Web lane testing

Date: 2026-09-18
Owner: Codex

`node_modules/.bin/tsc --noEmit`: passed with exit code 0.

`node --test tests/web-*.test.mjs`: passed, 3 tests, 0 failures.

- assembles a real filesystem fixture and emits a private v1 runtime manifest
- rejects a digest mismatch and preserves the previous output
- does not let a JSON public boolean bypass the private publication gate

`node scripts/sync-theme.mjs && cmp -s packages/ui-tokens/panda-ide.css src/panda-ide.css`: passed. The generated ignored stylesheet matches the canonical local token source.

`node scripts/pack-web.mjs`: failed as expected with exit code 1 and the explicit usage message because all four required paths were intentionally omitted.

`git diff --check`: passed.

No full Vite/browser bundle, QEMU, Rust, Tauri, device, hardware, deployment, or credential-related command was run. Main coordinates those builds.
