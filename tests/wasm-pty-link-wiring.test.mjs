import test from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const build = readFileSync(new URL("../scripts/build-qemu-wasm.sh", import.meta.url), "utf8");
const helper = build.slice(build.indexOf("patch_python_if_present() {"), build.indexOf("\nappend_if_missing() {"));
const start = build.indexOf('  QEMU_WASM_SCRIPT_MOUNT="${QEMU_WASM_SCRIPT_MOUNT}" patch_python_if_present "${meson_build}"');
const patch = build.slice(start, build.indexOf("\n'", start) + 2);
const original = `elif host_os == 'emscripten'
  if get_option('optimization') != 'plain'
    emulator_link_args += ['-O' + get_option('optimization')]
  endif
endif
`;
const legacy = "    '--js-library=/build/node_modules/xterm-pty/emscripten-pty.js',\n";

for (const cached of [false, true]) {
  test(`canonical Meson patch has no second PTY library for ${cached ? "cached" : "fresh"} sources`, () => {
    const dir = mkdtempSync(join(tmpdir(), "panda-pty-link-"));
    try {
      const path = join(dir, "meson.build");
      const input = cached ? original.replace("\nendif\n", `\n  emulator_link_args += [\n${legacy}    '--js-library=/murphy-scripts/qemu-wasm-mofei-bridge.js',\n  ]\nendif\n`) : original;
      writeFileSync(path, input);
      const run = () => execFileSync("bash", ["-c", `${helper}\nmeson_build=$1\nQEMU_WASM_SCRIPT_MOUNT=/murphy-scripts\n${patch}`, "pty-link-test", path], { stdio: "pipe" });
      run();
      const first = readFileSync(path, "utf8");
      assert.ok(first.includes("--js-library=/murphy-scripts/qemu-wasm-mofei-bridge.js"));
      assert.ok(!first.includes("--js-library=/build/node_modules/xterm-pty/emscripten-pty.js"));
      assert.ok(first.includes("get_option('optimization')"));
      run();
      assert.equal(readFileSync(path, "utf8"), first);
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  });
}
