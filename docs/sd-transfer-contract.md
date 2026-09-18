# Local SD transfer contract

Date: 2026-09-18
Executor: Codex

## Scope

The browser simulator exposes a local, user-driven backup and restore path for
the simulator SD card. The guest must be stopped before either operation.

An export is a browser download of the current SD image from the current
origin. An import is a user-selected file read by that origin. Neither path
performs a cross-origin request, sends the image to a server, or attempts to
read storage owned by another origin. Moving data between origins is therefore
an explicit export-then-import action.

## File format and bounds

The transfer file is a raw FAT volume image. It has no simulator-specific
header, JSON envelope, compression, or hidden payload. The downloaded `.img`
file is the exact byte sequence mounted by QEMU. The importer accepts a raw
`ArrayBuffer` from a local `File` only.

The configured `qemuSdRawBytes` value is the required byte length when it is
present. Otherwise the current same-origin template length is the required
length. The shared storage upper bound is 256 MiB; the approved simulator
template remains the existing 128 MiB image. A payload that is empty, larger
than the bound, or not exactly the configured size is rejected before storage
is changed.

The shared validator checks the FAT boot-sector signature, sector geometry,
reserved/FAT/root/data regions, FAT type geometry, and that the filesystem fits
inside the supplied bytes. The bundled source worker additionally mounts the
candidate through `js-fatfs` before it can be committed. Invalid bytes never
replace the previous image.

## Storage and migration

The only persistent store read by this feature is the known `images` object
store in the simulator's IndexedDB database. Current images use the existing
4 MiB chunk format and one metadata record committed in one read-write
transaction. The old single-record key remains readable and is never deleted
as part of migration. A quota error or transaction abort leaves the previous
metadata and all of its chunks intact.

Each board has a stable storage slot whose metadata records the template
fingerprint used when the image was saved. The reader also checks the existing
URL-derived key from the prior runtime for one-time compatibility. It does not
scan unrelated databases, stores, or keys.

If the stored image's length or template fingerprint differs from the current
template, the old user image remains the selected image and the runtime reports
a visible template conflict. It never silently returns the new seed over those
bytes. The raw old image remains exportable while stopped. A successful,
explicit raw import validates first and then records the imported image against
the current template fingerprint.

Cancellation, an unreadable file, invalid bytes, a mismatched size, a failed
FAT mount, and a quota or transaction failure all leave the last complete image
available. The UI reports the failure and does not claim that a restore
completed.

## Host capability

Raw transfer is an optional browser host capability. The Tauri bridge does not
emulate it with a native path; it leaves the optional methods absent. The
browser UI renders the transfer controls only when the browser capability is
present, while existing native simulator controls remain unchanged.
