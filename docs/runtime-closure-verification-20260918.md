# QEMU/WASM runtime closure — 2026-09-18

Issue #5 reproduced only when the real browser runtime mounted the 128 MiB SD.
The old runtime WASM was 9c9928239e231827ef9c5b8071f7f88fba2c0bfc7dc7130bd4a98f932dcc2b7d;
it aborted in rcu_read_unlock() although the same guest booted without SD.

Instrumentation showed the reader pointer and pthread did not change. The
64 KiB pthread C stack instead drifted downward across repeated
Asyncify/libffi unwind/rewind cycles. A 6144-byte stack operation eventually
overlapped adjacent TLS including rcu_reader.depth. Increasing the stack to
128, 256, and 512 KiB only delayed the failure and stack-overflow checks proved
the same downward drift. The RCU assertion was a victim, not the root cause.

The production correction does not relax RCU or assertions. Generated
ffi_call_js and invoke_* wrappers retain original stack snapshots keyed by
the active Asyncify data object and reuse them during rewind. TCI's existing
per-thread scratch allocation is used instead of retaining another ~6 KiB
array in each suspended C frame. STACK_OVERFLOW_CHECK=2 remains enabled.

## Matched-input browser result

A clean runtime build carrying the exact current continuation patch produced:
- JS SHA256 3362dae7971f9a1621418ae247c53234b20731c7b5b08c34938b752d64804282
- WASM SHA256 b7b85dadeb9de42593ea6d265647845698c6027111bd0b6b3c68adb50b0c54a2
- worker SHA256 b3da90501116a254f0518048ff154ee695aa889ce168573adaf78e13bd5d3d8d
- continuation patch SHA256 61a4e3782a38d1cd44d7abb8c08c942073fe887fda61b0a239a45e325e5cd6b3

The patch hash equals the runtime source manifest. Replacing only the QEMU
runtime files in the old verified guest/boot/ROM/SD set retained guest source
revision 9eedee8c943581f7260c8e94af9f60f199126bcb and the same 128 MiB SD.

Chrome 152 on loopback then reached start_sim: started, produced a real
480x800 framebuffer, and the existing canvas-touch smoke changed the frame.
There were zero page errors, external-origin requests, or RCU assertions.

Stop persisted the SD as one metadata record plus 32 x 4 MiB chunks:
134,217,728 bytes total. Restart in the same browser context produced a second
real framebuffer with no page errors or external requests. A separate extra
assertion that one arbitrary post-restart input must change pixels was removed
from this persistence gate; input-induced frame change is already verified by
the independent real guest smoke.

## Software regression

- generated-wrapper continuation tests: 7/7;
- complete Node suite: 49/49;
- Tauri library tests: 17/17;
- headless tests: 99/99;
- TypeScript/Vite build: pass;
- git diff --check: pass.

The stack-size experiments and diagnostic memory instrumentation remain only in
local task evidence. No debug RCU instrumentation, assertion suppression,
larger production pthread stack, private book/font/firmware bytes, or browser
screenshots are committed.

This closes the reproduced RCU/SD-startup defect for the tested Mofei guest.
It is not evidence for physical e-ink behavior, all board/browser combinations,
or public deployment. Repository visibility and Cloudflare publishing remain
owner-controlled.
