# Device Studio interaction

Panda-owned simulator code is licensed under MIT. QEMU/Espressif sources,
guest firmware, ROMs and other third-party materials retain their own notices
and distribution terms.

## Board selection

The session sidebar exposes every board declared in `boards.json` as both a
native select control and a compact board card. Changing boards is disabled
while the guest is running. The selected board drives framebuffer dimensions,
touch coordinates, physical-key mapping, firmware filtering, and the visual
device shell.

The shell is a CSS-rendered product representation, not a claim of exact
industrial dimensions or e-ink optical simulation. Mofei/S3R8/S3-WROOM,
S37UC, M5Stack PaperS3, and LilyGo T5S3 Pro have distinct enclosure families,
finishes, branding marks, key placement, and press feedback. The rendered
side keys call the same `injectButton` bridge as sidebar hardware controls.

## Firmware library

Browser builds expose only firmware declared for the selected board by the
verified runtime manifest. The selected firmware path is passed explicitly to
`startSim`; the worker still rejects a path that does not match that board.
Desktop/Tauri keeps the explicit local .elf/.bin picker instead.

A firmware drop-down showing one entry is intentional when a package contains
only one verified firmware for that board; the UI contract supports additional
manifest-backed choices without allowing an unrelated board image.

## Virtual SD card

The browser SD is a FAT image persisted in IndexedDB. While stopped, users can
browse directories, upload files or folders, create folders, download files,
delete entries, drag files onto the drop zone, and import/export a raw image
backup. Files are mounted into QEMU on the next start.

The storage implementation keeps its existing bounds: 256 MiB maximum,
4 MiB persistence chunks, atomic metadata publication, FAT/size validation,
legacy-read retention, and explicit template-conflict handling. Editing is
disabled while the simulator is running.
