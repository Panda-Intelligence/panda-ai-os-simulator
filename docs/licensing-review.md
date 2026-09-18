# Publication and rights review

Status: **private extraction candidate; G0 not approved**. The existing root
Panda Public Source license is retained. This is not an OSI-open-source release.
Execution approval is not evidence that third-party or historical contributor
rights have been reassigned. Proposed MIT reauthorization of wholly Panda-owned
files must be recorded by the rights owner, separately from QEMU GPL and guest,
ROM, font and media redistribution approval. No public license switch is made.

The tracked export manifest records every imported file against Murphy commit
972e16c3e0fc7d45097454a5eb5e99d40ca2cd3f. Only simulator sources and the explicit shared
style were exported; no original Git history, ignored caches, runtime binaries,
user books, font files, credentials or device data were copied. Files carrying
their own upstream license retain it; the root does not replace that license.

Before public conversion: validate every file's provenance and ownership; record
external contributor permissions; retain QEMU notices and matching corresponding
source/build instructions for each runtime release; determine ROM redistribution
rights; separately approve the Panda OS guest and seed. DCO on future PRs does
not retroactively resolve historical contributions. A public website distributes
its JS/WASM and guest bytes; changing file extensions is not a security boundary.

`provenance/rights-review.json` is an auditable queue, not legal clearance.
No automated scanner certifies ownership or complete absence of secrets.
Official reference: https://www.qemu.org/docs/master/about/license.html
