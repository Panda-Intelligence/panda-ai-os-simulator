# Continued extraction: source-bound verification

User retains repository configuration and Cloudflare publication. No repository
visibility/permission settings, licenses, DNS, bucket configuration, deployment,
social posts or hardware operations were changed in this continuation.

## Engineering changes

The standalone launcher no longer imports consumer Python modules, requires
ESP-IDF or provisions product fonts. Consumer fixture access is explicit; native
engine tests and product-fixture tests have separate named gates without deleting
assertions. Cloud delegates generic assembly and the guarded QEMU JS patch to
upstream. Murphy retains only product seed selection and release mappings.
The seed builder now copies only manifest-listed files and rejects path, filename,
symlink and duplicate-entry escapes; generated FAT output is outside its input root.

Existing runtime import preserves the supplied guest revision/digests, validates
board geometry, and retains versioned boot artifacts. Single/split gzip SD
transport and base paths remain supported with bounded decoded sizes.
A shared browser store replaces oversized IndexedDB records with atomic 4 MiB
chunks. Legacy records are retained, and missing/corrupt chunks fail explicitly.
This preserves an older readable snapshot, not automatic compatibility of new
edits with an old UI release; full cross-origin/rollback migration remains open.

## Executed evidence

- Native minimal example now uses the ESP32-S3-specific compiler. The earlier
  generic driver produced a big-endian ELF that QEMU rejected. The corrected
  little-endian ELF actually executed and emitted `PANDA` on the installed QEMU.
- Real Chrome 152.0.7977.83 loaded the standalone site, received a real 480x800
  guest frame, and an input changed that frame; no external requests/page errors
  in the no-SD case. This was not a synthetic UI or firmware timing benchmark.
- Real Chromium IndexedDB roundtripped 134,217,728 bytes using 4,194,304-byte
  records. Injected transaction failure preserved the prior image; legacy reads,
  preservation of the legacy snapshot and rejection of a missing chunk passed.
- Headless standalone gate: 97/97. Product gate with explicit isolated fixture
  input: 112/112; the latter includes the former, not 209 unique cases. Product
  assets were copied privately; missing historical TC fixture was replaced only
  in the isolated test input with original multi-paragraph TC content. These are
  seeding assertions, not a performance result on the historical benchmark book.
- Tauri library gate: 15/15. Node filesystem/packaging/launcher/route tests: 21/21.
  Frontend/Vite and separate strict Worker type checks passed. Product seed
  safety tests in Murphy: 5/5; mapping/environment/theme tests: 6/6.

## Remaining runtime blocker (issue #5)

The supplied older guest/runtime set belongs to Murphy source
`9eedee8c943581f7260c8e94af9f60f199126bcb`. It is not latest develop firmware.
Its WASM SHA256 is
`9c9928239e231827ef9c5b8071f7f88fba2c0bfc7dc7130bd4a98f932dcc2b7d`.
With a generated 128 MiB SD image, actual guest startup aborts at
`rcu_read_unlock`: `p_rcu_reader->depth != 0`. This remains reproducible after
fixing the separate IndexedDB value-size error. No RCU assertion, resource
limit or firmware security check was disabled. Attribution to the extraction
versus that runtime is not established; matched-source rebuild/comparison and
an SD-backed reading/restart journey are still required.

Failed compiler-target selection, first UI status-selector mismatch, absent
fixture inputs and runtime failures are retained separately. No public runtime,
all-browser compatibility, optical behavior, latest-firmware performance or
complete release qualification is claimed. Private raw logs, images and test
font/book inputs stay local and are not part of GitHub source commits.
