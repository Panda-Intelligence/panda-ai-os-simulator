# Simulated location: London

All interactive launches (Quick Launch, local firmware and full reboot) use a
fixed London preset: 51.5074, -0.1278; firmware timezone Europe%2FLondon. No
geolocation API, permission prompt, IP lookup or host-timezone discovery is used.
The status bar identifies this as simulated rather than a user's real location.

The bundled browser worker updates only /.mofei/simulator_location.json and
/mofei/simulator_location.json in a private copy of the virtual FAT image before
QEMU mounts it. Both writes must succeed before the cached image is replaced.
Template conflicts remain blocked; absent SD remains the existing no-SD mode.
Existing books/settings are retained and the usual SD persistence lifecycle
commits the running image. Old SD templates thus cannot silently keep Taipei.
The source worker is the one instantiated by browserSimulatorRuntime.ts; the
legacy public worker is not used by the current frontend.

Native/Tauri and headless defaults also use London. Existing explicit developer
MOFEI_SIM_LOCATION_* overrides remain for scripted tests; they are not automatic
host detection. The interactive UI always supplies London. Existing hostLocation
field names remain unchanged for protocol compatibility.

Verification uses the real React Device Studio with a recording host bridge and
real FatFs writes to a generated FAT test image. These tests do not claim a
QEMU/firmware boot, live weather, GPS simulation or physical device validation.
LICENSE, cloud deployment, repository settings and unrelated worktrees are not
changed.

## Verified on 2026-09-20

- Node suite: 63/63 (55 baseline plus 8 location/privacy/FAT tests).
- TypeScript + Vite production build passed; existing URL and js-fatfs warnings remain.
- Actual Chrome/React Studio exercised all six boards with browser and desktop
  recording hosts under granted, denied and unavailable location APIs. Across
  96 start/reboot/retry calls every payload was London and geolocation access,
  position requests and permission queries stayed zero. A Tokyo host timezone
  was intentionally used. Cancelled file selection did not launch a guest.
- The baseline component failed the same test by producing Taipei on denial.
- Headless: 100/100 with an explicit existing product-fixture root; Tauri: 18/18.
  The initial standalone headless run was 99/100 because an existing recent-
  reading test needs a product font fixture. No assertion or scope was removed;
  re-running with the read-only fixture source passed the whole suite.
- Real FatFs/WASM editing of generated media preserved a test book and the
  original image, updated both location aliases, and rejected partial writes.

No actual guest firmware or weather/network service was run by these location
checks. Browser tests use production React components and record the host
callback boundary; FAT tests use the production editor with generated media.
