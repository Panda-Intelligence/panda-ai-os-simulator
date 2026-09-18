# Simulator runtime closure — 2026-09-18

User requests all remaining simulator engineering defects be completed. The
owner retains GitHub configuration and Cloudflare publication decisions.
Upstream main merged PR3 before its continuation commit; this branch merges
main and retains 1f87d1b. Consumer similarly preserves the continuation after
PR187 while syncing develop through 394a5c72c. New PRs will carry these changes.

1. Reproduce issue5 with the same guest/SD and preserve its exact artifact hash.
2. Inspect pinned QEMU/WASM + Espressif source and toolchain. Rebuild in a unique
   task-owned container, never remove another container or disable RCU checks.
3. Diagnose and correct the source-level RCU/coroutine/IO ownership; prove a
   matched-input before/after and real reading, input, stop/restart persistence.
4. Complete local SD export/import and legacy snapshot safety without touching
   other origins, requiring user data uploads, or silently overwriting errors.
5. Check remaining consumer pack/asset validation and explicit fixture paths.
6. Re-run independent Node/Rust/Tauri/browser contracts, then update exact
   upstream pin and source-bound evidence. Only passing work can close issues.

No speculative claim of compatibility or public readiness. Exact failed tests,
source/runtime identities, test inputs and remaining gaps must be retained.
No device/serial operation, deployment, licence switch, repository settings or
social posting. User books/fonts/firmware/ROM remain local private test inputs.
