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

Install the committed npm lockfile before runtime compilation. The build now
checks the host-side TypeScript dependency before any source checkout or Docker
work, rather than failing during post-link continuation patching.

```sh
npm ci --ignore-scripts
npm run build
node scripts/pack-existing-runtime.mjs \
  --qemu-dir /explicit/coherent-runtime-and-guests \
  --ui-dir ./dist --output-dir /explicit/owned-preview/dist \
  --board-map /explicit/murphy-boards.json --require-boards all \
  --require-runtime-revision asyncify-stack-v4-papers3-sdspi \
  --sd-image /explicit/review-seed.img.gz --sd-raw-bytes 134217728
```
