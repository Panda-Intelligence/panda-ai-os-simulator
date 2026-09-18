import test from "node:test";
import assert from "node:assert/strict";
import { chmodSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const repo = new URL("..", import.meta.url).pathname.replace(/\/$/, "");
const launcher = join(repo, "run");

function runLauncher(args, env = {}) {
  return spawnSync(launcher, args, {
    cwd: mkdtempSync(join(tmpdir(), "panda-native-cwd-")),
    env: { PATH: process.env.PATH, HOME: process.env.HOME, ...env },
    encoding: "utf8",
  });
}

test("check is independent of cwd and consumer environment", () => {
  const root = mkdtempSync(join(tmpdir(), "panda-native-inputs-"));
  const firmware = join(root, "firmware.elf");
  const sdRoot = join(root, "sd");
  writeFileSync(firmware, "fixture");

  const result = runLauncher([
    "check",
    "--firmware",
    firmware,
    "--qemu",
    "/bin/sh",
    "--sd-root",
    sdRoot,
  ], { PANDA_SIMULATOR_PROJECT_ROOT: "", PANDA_SIMULATOR_INTEGRATION: "" });

  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /QEMU binary: \/bin\/sh/);
  assert.match(result.stdout, /Firmware:/);
  assert.doesNotMatch(`${result.stdout}\n${result.stderr}`, /simulator_board_profiles|ESP-IDF|provision-default-fonts/);
});

test("headless rejects missing firmware before cargo or QEMU startup", () => {
  const result = runLauncher([
    "headless",
    "--qemu",
    "/bin/sh",
    "--sd-root",
    mkdtempSync(join(tmpdir(), "panda-native-sd-")),
  ]);

  assert.notEqual(result.status, 0);
  assert.match(`${result.stdout}\n${result.stderr}`, /Firmware is required/);
});

test("launcher remains a shell entrypoint", () => {
  chmodSync(launcher, 0o755);
  const result = spawnSync("bash", ["-n", launcher], { encoding: "utf8" });
  assert.equal(result.status, 0, result.stderr);
});
