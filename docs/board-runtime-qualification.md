# Per-board browser runtime qualification

The full local preview is a product package, not the bare Vite `dist` directory.
A UI build alone cannot boot firmware. Keep the runtime, its source manifest,
matching per-board guest/boot sidecars, and SD seed as separate explicit inputs.

## Fail-closed build and packaging

The PaperS3 GT911 correction requires a freshly compiled runtime. The cache
revision is `asyncify-stack-v4-papers3-sdspi`; an existing v2 binary can display PaperS3
frames yet still route its touch to the wrong model. Changing the UI does not
repair that binary. `--require-runtime-revision` rejects stale/absent revision
metadata, while normal artifact SHA-256 and guest provenance checks still run.
Revision metadata is not a publisher signature or a substitute for execution.

Install the committed Bun lockfile before runtime compilation. The build now
checks the host-side TypeScript dependency before any source checkout or Docker
work, rather than failing during post-link continuation patching.

```sh
bun install --frozen-lockfile
bun run build
node scripts/pack-existing-runtime.mjs \
  --qemu-dir /explicit/coherent-runtime-and-guests \
  --ui-dir ./dist --output-dir /explicit/owned-preview/dist \
  --board-map /explicit/murphy-boards.json --require-boards all \
  --require-runtime-revision asyncify-stack-v4-papers3-sdspi \
  --sd-image /explicit/review-seed.img.gz --sd-raw-bytes 134217728
```

## PaperS3: positive storage acceptance, not just first paint

Touch and storage were separate faults. The v3 build corrected GT911 input and
passed six-board paint/input plus seven same-page Stop/restart sessions. Visual
inspection then showed PaperS3's guest library reporting no storage. Browser
IndexedDB persistence did not prove the firmware had mounted the card.

The v4 machine graph attaches `ssi-sd` to PaperS3 SPI2, routes its actual GPIO47
chip select, preserves the GPIO write side effect in the guest helper, and does
not attach the same card backend a second time to SDMMC. LilyGo wiring stays
unchanged. Source mirrors and native patches are both updated. The guard is
`asyncify-stack-v4-papers3-sdspi`.

The real-browser test now additionally requires the guest-generated
`E2E:STORAGE:event=sdspi_mounted` event on both PaperS3 and LilyGo. Merely seeing
`sdspi_begin`, a framebuffer, or browser SD chunks is insufficient. This new
assertion failed on v3 and passed on v4. The PaperS3 guest library screenshot
then showed the original local `Simulator-demo.txt` file instead of no media.

## Final local qualification

Guest build revision: `94355027c6f1e42db4aeae871a04796d54293873` (unchanged).
WASM runtime build source: `47a66cafaeb4291473d85e90e3e33d7b8bc4fa52`, clean.
The compile used pinned QEMU/native revisions and Emscripten 3.1.50. The runtime
was transferred between the owner's authorized computers using a one-shot,
source-IP-limited TLS endpoint with a verified public-key pin and archive hash.
No runtime, ROM, private seed, screenshot or font is committed here.

Runtime artifact identities (before packaging adapter):

| File | SHA-256 |
|---|---|
| QEMU JS | `7133372b18fb97e4e5e764daec3d7632c6c1d053f1578b8331fa5f0c781ba494` |
| QEMU WASM | `ab46571f8ab7b4c3528bf1c378033662a7af838867ae6438933233805eaec00f` |
| pthread worker | `b3da90501116a254f0518048ff154ee695aa889ce168573adaf78e13bd5d3d8d` |

Executed on real Chrome with the explicit full local package:

- All six boards booted the corresponding guest, emitted actual Worker
  framebuffers, and changed visible content after a real pointer gesture.
- PaperS3 and LilyGo both emitted the positive SPI-SD mounted event and opened
  the original local sample from the guest library into the Reader activity.
- Seven sessions in one page (PaperS3, LilyGo, S37UC, Mofei, S3R8, S3-WROOM,
  PaperS3 again) passed launch/input/Stop; Stop terminated the actual Worker and
  persisted 134,217,728 bytes before the next board loaded.
- The browser never accessed geolocation or fetched an external origin.
- Circle/NA validation: 18 board/viewport/scale combinations; equal circle axes,
  centered geometry, compact NA, Fit clearance and fixed-scale scroll reach.
- Existing button 18-geometry/15-activation and 96 London launch/reboot callback
  regressions passed separately; these are not additional guest boots.

The complete Node suite has 95 tests, all passing. TypeScript/Vite passed with
existing URL/js-fatfs warnings. QEMU completed 1,269 build steps; its existing
libffi warnings remain. The GPIO test compiles the actual helper body and checks
PaperS3 CS47 alongside unchanged LilyGo pins; both native mirror patches also
passed `git apply --check` on the pinned native QEMU source.

## Read Pico, Molink, X4Pro, Eego A4 and Metalio: input and boot windows

The boot test no longer sends one canvas click to every board. Boards with a qualified touch gesture
(Mofei, S3R8, S3-WROOM, S37UC, PaperS3, LilyGo) keep it. Every other board is driven with its own keys,
taken from `boards.json` `keyMap` and pressed through the on-screen `.panel-physical-key` control for 250 ms,
navigation keys first (Down/PageDown, Right, Up, Left, Confirm, Back, Power last). A board passes when one of
its keys changes the framebuffer; if none does, the failure lists every key tried. Touch-capable boards outside
the qualified set also get one tap, which is recorded in the report but not asserted.

Measured on the 2.5.2 deployment, Chromium headless, host load between 56 and 165 across runs (first frame counted
from Quick Launch; one value per run):

| Board | First frame | Key response | Touch tap at 50% x, 20% y |
|---|---|---|---|
| molink | 62.8 s, 63.4 s | Down 0.3 s | no touch on this board |
| x4pro | 67.5 s, 59.7 s | PageDown 0.3 s | no change within 15 s |
| eego-a4 | 17.8 s, 14.0 s | PageDown 0.3 s | no change within 15 s |
| metalio-eink4 | 58.4 s, 58.4 s, 67.4 s | Power 0.8-0.9 s | changed after 4.5-4.7 s |

Painting after the first frame took about 0.36 s on every board. Boot windows are a table in the test, not a
blanket increase: the default is 60 s, and Molink, X4Pro and Metalio carry twice their slowest measured first
frame, rounded up (130 s, 140 s, 140 s). Their first frames sit within a few seconds of 60 s whatever the host
load, so the default window passes or fails them by chance. Key and touch responses stay on the 15 s default.
`SIMULATOR_TEST_WINDOW_SCALE=<n>` stretches all windows further on an overloaded host. The earlier ad-hoc
"240 s boot, 60 s waits" was not needed by any board in these runs.

A loopback run of the rebuilt runtime (this change, host load 56-86) passed all eleven boards with the table
above: first frames at 3.1 s for Read Pico, S3-WROOM, PaperS3, LilyGo and Eego A4, 15.2 s for S37UC and 51-54 s for
Mofei, S3R8, Molink, X4Pro and Metalio; Read Pico answered PageDown with a changed frame after 0.3 s and
its Virtual SD card listed the seeded folders.

Read-only runs against a deployed simulator need `SIMULATOR_TEST_ALLOW_REMOTE=1`; the default stays loopback only.

## Read Pico browser boot (fixed 2026-11-09)

Two faults kept Read Pico from ever painting in the browser:

1. The worker started it as the `lilygo-t5s3-pro` QEMU kind. That kind attaches the SD card on SPI2 and none
   on SDMMC, while the Read Pico firmware mounts SDMMC (1-bit CLK 38 / CMD 42 / D0 44), so every command
   timed out (`SD card init failed: ESP_ERR_TIMEOUT`). It now starts as the native `read-pico` kind.
2. The Read Pico UI framebuffer is Gray16 684x1216 (stride 342, 415872 bytes). The direct capture refused
   anything above the LilyGo 259200-byte ceiling (`[LILYGO-FB] ... read=0`), and the worker and `boards.json`
   described Mono1. The capture ceiling is now `MOFEI_DIRECT_FRAMEBUFFER_MAX_BYTES` and Read Pico is `gray16`.

Both changes live in the QEMU peripherals (`mofei-sim-addrs.h`, `target_xtensa_exc_helper.c` and its patch
mirror) and the worker, so a deployed simulator needs a rebuilt QEMU wasm runtime, not just a new UI bundle.

## Reproduction and scope

Use `tests/browser-all-board-runtime.smoke.mjs` for the per-board positive
framebuffer/input/storage matrix (`SIMULATOR_TEST_BOARDS` selects boards). Set `SIMULATOR_TEST_READ_SAMPLE=1` only with
the documented single-book seed to additionally exercise Library -> Reader.
`tests/browser-board-lifecycle.smoke.mjs` checks real same-page Stop/restart;
`tests/browser-round-status.smoke.mjs` checks the actual UI when artifacts are
absent. All require explicit local Playwright/Chrome paths and loopback URLs.

Build infrastructure retries are recorded rather than hidden: a Docker parser
fetch and Docker-network npm fetch failed; the task-local image used the
installed Docker frontend and exactly xterm-pty 0.10.1 fetched on the authorized
host. The locked dependencies/toolchain versions were preserved. An initial
native-patch hunk recount and a sample-reader test locator were corrected before
passing their respective validation; no assertions or guest checks were removed.

The local preview is not a public release or a complete browser/platform
qualification. No existing LICENSE, repository access setting, Cloudflare
configuration, hardware device or original SD data is changed. The PaperS3
bottom circle represents its mechanical accessory, not a new guest input.
