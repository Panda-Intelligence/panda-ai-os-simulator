# Pinned PTY source contracts

These are the actual xterm-pty 0.10.1 and Emscripten 3.1.50 source libraries from
the pinned QEMU toolchain image. Their original licenses are included. Source
URLs, image identity, and digests are recorded in `provenance.json`.

`emitted-red.js` and `emitted-green.js` are untouched compiler outputs from the
same small C program and compiler flags. The green build applies the canonical
source dependency patch before compilation. The tests extract the real emitted
functions and use the actual browser worker PTY implementation; they do not
copy a replacement poll/read implementation into the test.

Regenerate both with the exact locally installed toolchain image:

```sh
node scripts/emit-wasm-pty-test-fixtures.mjs sha256:203f421e68b8180a3986da47bd665af89a20acaca3478f7da280bfb2c56e90c4
node --test tests/wasm-pty-poll.test.mjs
```

The compiler's Wasm binaries remain temporary and are not fixtures. These
contracts establish the three PTY defects independently of the release stall;
only the same-guest replay and complete Studio checks can establish causality.
