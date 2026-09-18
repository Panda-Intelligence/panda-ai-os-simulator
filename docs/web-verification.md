# Web lane verification

Date: 2026-09-18
Owner: Codex

The standalone seam is verified at the scoped level:

- Theme source is the explicit in-repository `packages/ui-tokens/panda-ide.css`.
- Pack input requires explicit manifest, runtime directory, guest directory, and output directory arguments.
- Artifact paths are relative, normalized, bounded, and reject URL, traversal, private-path, duplicate, unknown, and unsafe forms.
- `lstat` rejects symlinks and non-regular files. Declared byte lengths and SHA256 digests are checked before copying.
- Required QEMU and guest artifact sets are declared and checked. SD image copying is opt-in only.
- Assembly copies only fixed allowlisted roles into a helper-owned sibling staging directory and commits after all checks pass.
- Failed assembly does not replace or remove the prior output directory.
- Output is explicitly labeled private and local-only. An arbitrary `public: true` JSON field is rejected as an unknown key.
- Runtime manifest v1 fields and historical browser command/event identifiers remain in place. Source and digest metadata are optional extensions.
- Default manifest lookup is based on `import.meta.env.BASE_URL`; explicit URLs are constrained to same-origin HTTP(S) locations without credentials or private path segments.

Remaining integration verification belongs to main: full browser build, actual prebuilt QEMU/guest pack, and guest boot/framebuffer execution.
