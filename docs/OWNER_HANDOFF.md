# Owner handoff: configuration is separate from engineering acceptance

The owner will adjust GitHub repository settings and Cloudflare publication
later. This change does not make those changes or require them to run local tests.
No production routes/accounts, DNS, visibility, access policy or license grants
were changed. Public guest/ROM/media authorization remains its own review item.

## Local independent checks

```sh
npm ci --ignore-scripts
npm test
npm run build
npm run test:headless
cargo test --locked --manifest-path src-tauri/Cargo.toml --lib
XTENSA_CC=xtensa-esp32s3-elf-gcc scripts/build-example.sh <new-output>
```

The minimal example has actually emitted PANDA under the installed ESP32-S3 QEMU.
A full Panda guest requires its matching runtime and explicit artifact manifest.
Use `npm run pack:existing -- --help`; use immutable version output directories.
The packer intentionally refuses to delete/replace an unowned legacy directory.
Preserve any previous generated site outside the serving root before migrating it.

Murphy keeps product mappings, seeds and test fixtures. Its wrapper pins an exact
upstream SHA; full fixture tests require the consumer-owned inputs and the
explicit `product-fixtures` feature. These are not silently skipped upstream.

## Do not publish as a working reading demo yet

Issue #5 still blocks SD-backed guest reading with the supplied runtime set.
Changing GitHub or Cloudflare settings cannot resolve that QEMU RCU assertion.
The 128 MiB IndexedDB serialization defect is independently fixed and tested,
but the SD-backed emulator boot must pass separately before release. Keep both
PRs draft until that runtime and consumer journey gate is complete.
