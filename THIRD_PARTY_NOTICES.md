# Third-party notices — review in progress

Panda-owned simulator code is licensed under MIT, while all imported per-file
notices remain in force. QEMU/Espressif files are not relicensed by the root
Panda license and remain governed by their explicit upstream terms.
Native QEMU and qemu-wasm upstream repositories/pins are in qemu-runtime.env and
scripts/build-qemu-wasm.sh. Binary distribution remains blocked until exact
corresponding source, patches, toolchain/build instructions and notices are paired.

JS/Rust dependencies remain governed by their own packaged licenses and pinned
lockfiles. React, Tauri, js-fatfs and Rust dependencies must appear in the final
release SBOM, including transitive dependencies. No dependency is relicensed.
The simulator fixture and brand assets are still subject to ownership review.
No third-party ROM, real SD image, book or font file is included in this export.
