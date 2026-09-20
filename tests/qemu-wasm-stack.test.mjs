import test from "node:test";
import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(fileURLToPath(new URL("..", import.meta.url)));
const patcher = join(root, "scripts", "patch-qemu-wasm-stack.py");
const buildScript = join(root, "scripts", "build-qemu-wasm.sh");
const fixture = `static uintptr_t tcg_qemu_tb_exec_tci(void) {
    uint64_t stack[(TCG_STATIC_CALL_ARGS_SIZE + TCG_STATIC_FRAME_SIZE)
                   / sizeof(uint64_t)];
}
static void init_wasm32(void) {
    ctx.stack = g_malloc(TCG_STATIC_CALL_ARGS_SIZE + TCG_STATIC_FRAME_SIZE);
}`;

test("TCI scratch patch reuses the existing thread buffer and is idempotent", () => {
  const dir = mkdtempSync(join(tmpdir(), "panda-qemu-stack-"));
  try {
    mkdirSync(join(dir, "tcg"));
    const target = join(dir, "tcg", "wasm32.c");
    writeFileSync(target, fixture);
    assert.match(execFileSync("python3", [patcher, dir], { encoding: "utf8" }), /patched/);
    const once = readFileSync(target, "utf8");
    assert.match(once, /panda-wasm-tci-thread-scratch-v1/);
    assert.match(once, /uint64_t \*stack = ctx\.stack;/);
    assert.doesNotMatch(once, /uint64_t stack\[/);
    assert.match(execFileSync("python3", [patcher, dir], { encoding: "utf8" }), /already-patched/);
    assert.equal(readFileSync(target, "utf8"), once);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test("TCI scratch patch refuses an unknown upstream source shape", () => {
  const dir = mkdtempSync(join(tmpdir(), "panda-qemu-stack-shape-"));
  try {
    mkdirSync(join(dir, "tcg"));
    writeFileSync(join(dir, "tcg", "wasm32.c"), fixture.replace("uint64_t stack[", "uint32_t changed["));
    const result = spawnSync("python3", [patcher, dir], { encoding: "utf8" });
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /shape changed/);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test("runtime build keeps continuation ownership, stack guard, and cache revision together", () => {
  const source = readFileSync(buildScript, "utf8");
  assert.match(source, /QEMU_WASM_RUNTIME_REVISION="asyncify-stack-v3-gt911"/);
  assert.match(source, /-sSTACK_OVERFLOW_CHECK=2/);
  assert.match(source, /patch-qemu-wasm-stack\.py/);
  assert.match(source, /patch-wasm-continuations\.mjs/);
  assert.match(source, /manifest\.runtimeBuildRevision === process\.argv\[3\]/);
});