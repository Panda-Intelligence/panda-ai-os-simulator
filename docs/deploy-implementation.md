# Deploy lane implementation plan

Date: 2026-09-18
Owner: Codex
Branch: `feat/standalone-foundation`

## Scope

Implement one independent deployment seam without changing the application,
provenance, or runtime source:

- a Cloudflare Worker entry point that delegates same-origin static requests to
  the Workers Static Assets binding;
- an opt-in same-origin R2 release route constrained by a digest-verified,
  path allowlisted manifest;
- a loopback-only Node preview server for an explicit built-output directory;
- real HTTP smoke tests for success, errors, traversal, MIME, and isolation
  headers.

## Constraints and decisions

- `workers_dev` and `preview_urls` remain disabled. The production Worker name,
  custom domain, static-assets binding, and observability settings are declared
  in `deploy/wrangler.jsonc`; this repository change does not mutate Cloudflare
  accounts, DNS, credentials, or deployed state.
- Every Worker and preview response, including errors, receives COOP,
  COEP, CORP, and `nosniff` headers.
- R2 is never a generic key proxy. The route requires both a manifest and its
  SHA-256 digest, accepts only exact manifest paths, rejects encoded separators
  and traversal, and verifies the returned object bytes against the entry
  digest before responding.
- The preview server binds to `127.0.0.1`, rejects symlinked paths and
  traversal, serves only files below a resolved `dist`-named root, exposes no
  directory listings, and blocks source-map/source-file extensions.
- No generated simulator assets are created by this lane; missing assets stay
  missing and return an error.

The production deployment created from this configuration is
`panda-simulator` at `https://simulator.pandacat.ai/`. The deployment is an
explicit owner-authorized operation; it does not change the source or
third-party license terms.

## Verification

Run the scoped test directly with the repository's existing Node runtime:

```text
node --test tests/deploy-preview.test.mjs
```

The test starts the actual preview process and uses `fetch` over loopback. A
separate Worker contract test exercises the exported fetch handler with small
in-memory bindings and does not require Wrangler, QEMU, Rust, Tauri, or a
deployment.
