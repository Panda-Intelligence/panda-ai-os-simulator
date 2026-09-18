# Foundation verification — 2026-09-18

This is an initial private engineering candidate, not completion of G0–G4.

Executed by the integrator after parallel scoped implementation:

| Gate | Result |
|---|---|
| Locked isolated npm dependency installation | Passed; 30 packages, scripts disabled |
| Frontend TypeScript + Vite build | Passed |
| Node integration tests | 12/12, no skipped or failed cases |
| Worker strict TypeScript check | Passed separately from frontend tsconfig |
| Dependency-free native Rust resolver tests | 4/4 |
| Actual headless Cargo check, locked/offline | Passed after fixing String-to-anyhow error conversion |
| Minimal assembly guest build | Passed with explicit installed Xtensa compiler |
| Tauri library Cargo check, locked/offline | Blocked: required aho-corasick 1.1.4 absent from local cache |
| Real QEMU native/WASM guest run | Not executed |
| Public deployment/social posts | Not executed |

The baseline frontend build first failed on the implicit parent-directory theme;
the internal UI-token source fixes it. The initial headless compile found two
invalid `?` conversions, which were fixed before the successful check. Vite
retains its new-URL warning and js-fatfs browser-externalized `module` warning;
headless retains a shared resolver dead-code warning. Not warning-free.

Filesystem tests cover digest mismatch, publication flag refusal, symlink parent,
source/output overlap, aggregate size, traversal, unowned output, concurrent-pack
lock, owned replacement and built-UI source-map rejection. These generated tiny
fixtures test assembly, not actual runtime bytecode or guest compatibility.
