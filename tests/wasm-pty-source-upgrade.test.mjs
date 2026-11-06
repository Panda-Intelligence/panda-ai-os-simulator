import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { copyFileSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { patchWasmPtySources } from "../scripts/patch-wasm-pty-source.mjs";

const fixture = fileURLToPath(new URL("./fixtures/wasm-pty-source/", import.meta.url));
const manifest = JSON.parse(readFileSync(new URL("../scripts/wasm-pty-source.json", import.meta.url)));
const sha = data => createHash("sha256").update(data).digest("hex");

function withPreviousSource(check) {
  const dir = mkdtempSync(join(tmpdir(), "panda-pty-source-upgrade-"));
  try {
    const options = { ptySource: join(dir, "emscripten-pty.js"), syscallSource: join(dir, "library_syscall.js"), outputDir: join(dir, "output") };
    copyFileSync(join(fixture, "emscripten-pty.js"), options.ptySource);
    copyFileSync(join(fixture, "library_syscall.js"), options.syscallSource);
    patchWasmPtySources(options);
    copyFileSync(join(fixture, "previous-reviewed-pty.js"), options.ptySource);
    check(options);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

test("reviewed previous PTY sources upgrade from their verified upstream backup", () => withPreviousSource(options => {
  const previous = sha(readFileSync(options.ptySource));
  assert.equal(previous, "c55d62e47a6311146797841f0107b678adba512bdd1e361019d51c2d53f44c21");
  const receipt = patchWasmPtySources(options);
  assert.equal(receipt.files.find(file => file.key === "pty").previousPatchedSha256, previous);
  assert.equal(sha(readFileSync(options.ptySource)), manifest.files.find(file => file.key === "pty").afterSha256);
  assert.equal(sha(readFileSync(options.ptySource + ".panda-pty-upstream")), manifest.files.find(file => file.key === "pty").beforeSha256);
  const current = readFileSync(options.ptySource);
  patchWasmPtySources(options);
  assert.deepEqual(readFileSync(options.ptySource), current);
}));

test("previous PTY source upgrade requires the original upstream bytes", () => withPreviousSource(options => {
  const previous = readFileSync(options.ptySource);
  writeFileSync(options.ptySource + ".panda-pty-upstream", "wrong backup");
  assert.throws(() => patchWasmPtySources(options), /source_without_upstream/);
  assert.deepEqual(readFileSync(options.ptySource), previous);
}));

test("an upstream backup does not admit an unknown modified PTY source", () => withPreviousSource(options => {
  const modified = Buffer.concat([readFileSync(options.ptySource), Buffer.from("\n/* unknown source */\n")]);
  writeFileSync(options.ptySource, modified);
  assert.throws(() => patchWasmPtySources(options), /source_version_mismatch/);
  assert.deepEqual(readFileSync(options.ptySource), modified);
}));
