# Physical button alignment — 2026-09-20

## Scope and reproduced defect

User requested a direct code fix and PR for shifted PaperS3 / LILYGO controls.
Baseline: `45b0e54ea71064a6d9491bdbf081eeefec34c3ef`.
The actual DOM already places buttons directly under `.panel-physical-keys`;
the earlier claim that screen/branding siblings broke `nth-child` was incorrect.
The faults were inaccurate physical coordinates, all LILYGO keys placed on one
edge, duplicate percentage/pixel CSS overrides, and an independently spaced
legend. The old rendered PaperS3 button center was about 64.34% down the body.
No suggested decorative spans / `pointer-events:none` replacement was applied.

## Manufacturer references (portrait front view)

PaperS3 official model drawing, page 1, frontal and side elevations:
https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/517/C139.pdf
Side photograph (loop end at the left; invert its long axis for portrait top):
https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/products/core/PaperS3/10.webp

LILYGO official front/back oblique photograph (RST/PWR on front-right):
https://wiki.lilygo.cc/products/t5-series/t5-e-paper-s3-pro/index/image/t5-e-paper-s3-pro2.jpg
Official pin/side-view sheet (BOOT/IO48 on front-left):
https://wiki.lilygo.cc/products/t5-series/t5-e-paper-s3-pro/index/image/t5-e-paper-s3-pro-en.jpg

The numbers below are **image-derived UI calibration**, not manufacturer-
dimensioned button coordinates or CAD tolerances. Approximate visual placement
is intentionally separate from the published chassis dimensions. We retain the
current license and redistribute neither vendor photographs nor drawings.

| Board / control | Front-view edge | Center from top | Cap length / body height |
|---|---|---:|---:|
| PaperS3 PWR | right | 82.5% | 8.2% |
| LILYGO RST | right | 59.0% | 4.2% |
| LILYGO PWR | right | 68.0% | 4.2% |
| LILYGO BOOT | left | 57.5% | 4.2% |
| LILYGO IO48 | left | 66.5% | 4.2% |

The adjacent LILYGO TF slot is below PWR (79–91% height); the old 66–84%
slot overlapped the corrected PWR cap. QWIIC/rear features are not fabricated.

## Implementation

`boardPhysicalControls.ts` is the single named-control placement table.
Logical ids are resolved from the existing board keyMap, not array indices.
RST remains the reset callback and is disabled when it is unavailable.
`PhysicalDeviceKey` renders an operable button with a scaled cap and caption
sharing its center. CSS handles appearance only. A transparent minimum 24px
hit region enlarges interaction without changing the visible hardware outline.

Pointer capture, cancel/lost capture, keyboard Space/Enter, window blur and
component disposal have balanced down/up injection. Keyboard activation does
not also reach the Studio global shortcuts. No RCU/WASM, guest, SD data, license,
repository settings, Cloudflare configuration or Murphy pin is changed.
Other board skins/key layouts retain their existing renderer.

## Verification commands and limitations

- `bun run test` runs the existing suite plus executable geometry/mapping tests.
- `bun run build` checks TypeScript and production bundling.
- `tests/browser-physical-buttons.smoke.mjs` creates its own loopback Vite
  component fixture. It needs explicit PLAYWRIGHT_MODULE, CHROME_EXECUTABLE and
  SIMULATOR_TEST_OUT environment values. No fixed developer filesystem path.
- The fixture renders the actual PanelCanvas/PhysicalDeviceKey and records the
  bridge callbacks. It does not supply fake guest framebuffer output or claim
  a QEMU boot. It tests three viewports × Fit/1x/2x × two boards, edge/center/
  seam/caption alignment, TF overlap, real mouse hit tests, Space/Enter, blur,
  disposal, disabled reset and independence from DOM sibling/keyMap order.

The pre-fix geometry test fails at PaperS3 center 0.6434 versus the reference
0.825. The first harness attempt needed Vite's React preamble; that harness
failure is not reported as product reproduction. Logs remain local.
