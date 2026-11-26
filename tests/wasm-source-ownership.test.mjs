import test from "node:test";
import assert from "node:assert/strict";
import {chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, symlinkSync, writeFileSync} from "node:fs";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {spawn, spawnSync} from "node:child_process";
import {once} from "node:events";

const script = new URL("../scripts/build-qemu-wasm.sh", import.meta.url).pathname;
const text = readFileSync(script, "utf8");
const functions = text.slice(text.indexOf("acquire_source_lock()"), text.indexOf("is_full_sha()")) +
  text.slice(text.indexOf("start_container()"), text.indexOf("patch_qemu_wasm_pty_link_input()"));
const prefix = String.raw`
set -euo pipefail
SOURCE_LOCK_DIR=""
SOURCE_LOCK_TOKEN=""
SOURCE_LOCK_HELD=0
CONTAINER_STARTED=0
CONTAINER_ID=""
step(){ printf '[fixture] %s\n' "$*"; }
die(){ printf 'ERROR: %s\n' "$*" >&2; exit 1; }
require_tool(){ command -v "$1" >/dev/null; }
`;

function fixture() {
  const root = realpathSync(mkdtempSync(join(tmpdir(), "wasm-source-owner-")));
  const source = join(root, "cache", "source");
  mkdirSync(join(root, "cache"));
  const bin = join(root, "bin");
  mkdirSync(bin);
  const docker = join(bin, "docker");
  writeFileSync(docker, String.raw`#!/usr/bin/env python3
import json, os, signal, subprocess, sys
args = sys.argv[1:]
with open(os.environ["FAKE_DOCKER_LOG"], "a") as log:
    log.write(json.dumps(args) + "\n")
mode = os.environ.get("FAKE_DOCKER_MODE", "")
if args[:2] == ["image", "inspect"]:
    print("sha256:" + "b" * 64)
elif args[0] == "ps":
    if "{{.Names}}" in args and mode == "collision":
        print("shared-container")
    elif "{{.ID}}" in args:
        if mode == "list-failed":
            sys.exit(1)
        print("a" * 64)
        print("c" * 64)
elif args[0] == "run":
    print("a" * 64)
elif args[0] == "rm" and mode == "remove-failed":
    sys.exit(1)
elif args[0] == "exec" and mode in ("configure-failed", "configured", "configure-interrupted", "compile-failed"):
    if "emconfigure" in args and mode == "configure-failed":
        print("configure rejected atomic64", file=sys.stderr)
        sys.exit(19)
    if "emconfigure" in args and mode == "configure-interrupted":
        print("OWNER_READY", flush=True)
        signal.pause()
    if "emmake" in args and mode == "compile-failed":
        print("fixture compiler failure", file=sys.stderr)
        sys.exit(27)
    if "/bin/sh" in args:
        sys.exit(subprocess.call(args[args.index("/bin/sh"):]))
    if "node" in args and "-e" in args:
        print("/fixture/pty-library.js")
    if "python3" in args:
        sys.exit(subprocess.call(args[args.index("python3"):]))
`);
  chmodSync(docker, 0o755);
  const env = {
    ...process.env,
    PATH: bin + ":" + process.env.PATH,
    QEMU_WASM_SRC_DIR: source,
    QEMU_WASM_CACHE_DIR: join(root, "other-wrapper-cache"),
    QEMU_WASM_KEEP_BUILD: "0",
    QEMU_WASM_CONTAINER: "shared-container",
    QEMU_WASM_REF: "d".repeat(40),
    QEMU_WASM_IMAGE: "fixture-image",
    QEMU_WASM_MALLOC: "emmalloc",
    QEMU_WASM_SCRIPT_MOUNT: "/scripts",
    SCRIPT_DIR: root,
    FAKE_DOCKER_LOG: join(root, "docker.jsonl"),
  };
  const run = (body, overrides = {}) => spawnSync("bash", ["-c", prefix + functions +
    "\ntrap stop_container EXIT\ntrap 'exit 130' INT\ntrap 'exit 143' TERM\n" + body],
  {env: {...env, ...overrides}, encoding: "utf8"});
  const calls = () => existsSync(env.FAKE_DOCKER_LOG) ?
    readFileSync(env.FAKE_DOCKER_LOG, "utf8").trim().split("\n").filter(Boolean).map(JSON.parse) : [];
  return {root, source, env, run, calls, lock: source + ".panda-wasm-lock"};
}

async function owner(env, body) {
  const child = spawn("bash", ["-c", prefix + functions +
    "\ntrap stop_container EXIT\ntrap 'exit 130' INT\ntrap 'exit 143' TERM\n" + body],
  {env, detached: true, stdio: ["pipe", "pipe", "pipe"]});
  const closed = once(child, "close");
  let output = "";
  child.stderr.on("data", chunk => { output += chunk; });
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => { process.kill(-child.pid, "SIGKILL"); reject(new Error("owner rendezvous stalled: " + output)); }, 10000);
    child.stdout.on("data", chunk => {
      output += chunk;
      if (output.includes("OWNER_READY\n")) { clearTimeout(timer); resolve(); }
    });
    child.once("error", error => { clearTimeout(timer); reject(error); });
    child.once("close", code => {
      if (!output.includes("OWNER_READY\n")) { clearTimeout(timer); reject(new Error("owner exited " + code + ": " + output)); }
    });
  });
  return {child, closed, output: () => output};
}

test("the actual entry rejects a second wrapper cache for the same canonical source before cleanup", async () => {
  const f = fixture();
  let held;
  try {
    held = await owner(f.env, 'acquire_source_lock; printf "OWNER_READY\\n"; read -r release');
    const alias = join(f.root, "cache-alias");
    symlinkSync(join(f.root, "cache"), alias);
    mkdirSync(f.source);
    writeFileSync(join(f.source, "sentinel"), "active checkout");
    const partial = f.source + ".partial.fixture";
    mkdirSync(partial);
    writeFileSync(join(partial, "sentinel"), "active clone");
    const token = readFileSync(join(f.lock, "owner"), "utf8");
    const result = spawnSync("bash", [script, "--no-build"], {
      env: {...f.env, QEMU_WASM_SRC_DIR: join(alias, "source")},
      encoding: "utf8",
    });
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /source cache is owned/);
    assert.equal(readFileSync(join(f.lock, "owner"), "utf8"), token);
    assert.equal(readFileSync(join(f.source, "sentinel"), "utf8"), "active checkout");
    assert.equal(readFileSync(join(partial, "sentinel"), "utf8"), "active clone");
    assert.deepEqual(f.calls(), []);
    held.child.stdin.end("\n");
    assert.deepEqual(await held.closed, [0, null]);
    held = null;
    assert.equal(existsSync(f.lock), false);
    const next = f.run("acquire_source_lock");
    assert.equal(next.status, 0, next.stderr);
    assert.equal(existsSync(f.lock), false);
  } finally {
    if (held) { held.child.kill("SIGTERM"); await held.closed; }
    rmSync(f.root, {recursive: true, force: true});
  }
});

test("changed ownership is preserved and makes an otherwise successful exit fail", () => {
  const f = fixture();
  try {
    const result = f.run('acquire_source_lock; printf "replacement-owner\\n" > "$SOURCE_LOCK_DIR/owner"');
    assert.equal(result.status, 1);
    assert.match(result.stderr, /source lock owner changed/);
    assert.equal(readFileSync(join(f.lock, "owner"), "utf8"), "replacement-owner\n");
  } finally { rmSync(f.root, {recursive: true, force: true}); }
});

test("an existing one-shot container is never removed or started", () => {
  const f = fixture();
  try {
    const result = f.run("acquire_source_lock; start_container", {FAKE_DOCKER_MODE: "collision"});
    assert.equal(result.status, 1);
    assert.match(result.stderr, /container name already exists/);
    assert.equal(f.calls().some(args => ["run", "rm", "start", "stop"].includes(args[0])), false);
    assert.equal(existsSync(f.lock), false);
  } finally { rmSync(f.root, {recursive: true, force: true}); }
});

test("cleanup uses the recorded container ID and preserves the original build failure", () => {
  const f = fixture();
  try {
    const result = f.run("acquire_source_lock; start_container; exit 23");
    assert.equal(result.status, 23, result.stderr);
    assert.deepEqual(f.calls().filter(args => args[0] === "rm"), [["rm", "-f", "a".repeat(64)]]);
    assert.equal(existsSync(f.lock), false);
  } finally { rmSync(f.root, {recursive: true, force: true}); }
});

test("unknown or failed container cleanup retains the source lock and cannot return success", () => {
  for (const mode of ["list-failed", "remove-failed"]) {
    for (const status of [0, 23]) {
      const f = fixture();
      try {
        const result = f.run("acquire_source_lock; start_container; exit " + status, {FAKE_DOCKER_MODE: mode});
        assert.equal(result.status, status || 1, result.stderr);
        assert.match(result.stderr, /container cleanup failed; preserving source lock/);
        assert.equal(existsSync(join(f.lock, "owner")), true);
      } finally { rmSync(f.root, {recursive: true, force: true}); }
    }
  }
});

test("TERM stops the owned container before releasing its source lock", async () => {
  const f = fixture();
  let held;
  try {
    held = await owner(f.env, 'acquire_source_lock; start_container; printf "OWNER_READY\\n"; read -r release');
    held.child.kill("SIGTERM");
    assert.deepEqual(await held.closed, [143, null]);
    held = null;
    assert.deepEqual(f.calls().filter(args => args[0] === "rm"), [["rm", "-f", "a".repeat(64)]]);
    assert.equal(existsSync(f.lock), false);
  } finally {
    if (held) { held.child.kill("SIGTERM"); await held.closed; }
    rmSync(f.root, {recursive: true, force: true});
  }
});

test("a configure failure preserves full probe diagnostics and never retries or reaches compilation", () => {
  const f = fixture();
  try {
    const build = join(f.root, "build");
    mkdirSync(join(build, "meson-logs"), {recursive: true});
    writeFileSync(join(build, "config.log"), "full compiler command\natomic64 probe failed\nfinal config line\n");
    writeFileSync(join(build, "meson-logs/meson-log.txt"), "full Meson probe source\ncompiler stderr\n");
    const configure = text.slice(text.indexOf("configure_and_make()"), text.indexOf("patch_qemu_wasm_runtime_env_bridge()"));
    const result = f.run(configure +
      '\npatch_qemu_wasm_pty_link_input(){ :; }\nacquire_source_lock; start_container; configure_and_make',
    {FAKE_DOCKER_MODE: "configure-failed", QEMU_WASM_BUILD_DIR: build, JOBS: "1"});
    assert.equal(result.status, 1);
    assert.match(result.stdout, /full compiler command\natomic64 probe failed\nfinal config line/);
    assert.match(result.stdout, /full Meson probe source\ncompiler stderr/);
    assert.match(result.stderr, /qemu-wasm configure failed/);
    assert.equal(f.calls().filter(args => args.includes("emconfigure")).length, 1);
    assert.equal(f.calls().filter(args => args.includes("emmake")).length, 0);
    assert.equal(existsSync(f.lock), false);
  } finally { rmSync(f.root, {recursive: true, force: true}); }
});

test("make records the generated atomic64 result and preserves the supported fallback without rewriting it", () => {
  for (const header of [
    null,
    "#undef CONFIG_ATOMIC64\n",
    "#define CONFIG_ATOMIC64 0\n",
    "#define CONFIG_ATOMIC64\n#undef CONFIG_ATOMIC64\n",
    "#define CONFIG_ATOMIC64\n#define CONFIG_ATOMIC64 1\n",
    "#define CONFIG_ATOMIC64\n",
    "#define CONFIG_ATOMIC64 1\n",
  ]) {
    const f = fixture();
    try {
      const build = join(f.root, "build");
      mkdirSync(build);
      const path = join(build, "config-host.h");
      if (header !== null) writeFileSync(path, header);
      writeFileSync(join(build, "config.log"), "complete configure probe output\n");
      const configure = text.slice(text.indexOf("configure_and_make()"), text.indexOf("patch_qemu_wasm_runtime_env_bridge()"));
      const result = f.run(configure +
        '\npatch_qemu_wasm_pty_link_input(){ :; }\nacquire_source_lock; start_container; configure_and_make',
      {FAKE_DOCKER_MODE: "configured", QEMU_WASM_BUILD_DIR: build, JOBS: "1"});
      const enabled = header === "#define CONFIG_ATOMIC64\n" || header === "#define CONFIG_ATOMIC64 1\n";
      const valid = enabled || header === "#undef CONFIG_ATOMIC64\n";
      assert.equal(result.status, valid ? 0 : 1, result.stderr);
      assert.equal(f.calls().filter(args => args.includes("emconfigure")).length, 1);
      assert.equal(f.calls().filter(args => args.includes("emmake")).length, valid ? 1 : 0);
      if (valid) {
        assert.ok(result.stdout.includes(JSON.stringify({CONFIG_ATOMIC64: enabled}).replace(":", ": ")));
      } else {
        assert.match(result.stderr, /configure produced an invalid CONFIG_ATOMIC64 result/);
        assert.match(result.stdout, /complete configure probe output/);
      }
      assert.equal(existsSync(path), header !== null);
      if (header !== null) assert.equal(readFileSync(path, "utf8"), header);
      assert.equal(existsSync(f.lock), false);
    } finally { rmSync(f.root, {recursive: true, force: true}); }
  }
});

test("interrupting the actual configure control path preserves diagnostics and exits without make", async () => {
  const f = fixture();
  let held;
  try {
    const build = join(f.root, "build");
    mkdirSync(join(build, "meson-logs"), {recursive: true});
    writeFileSync(join(build, "config.log"), "first interrupted configure diagnostics\n");
    writeFileSync(join(build, "meson-logs/meson-log.txt"), "interrupted compiler probe stderr\n");
    const configure = text.slice(text.indexOf("configure_and_make()"), text.indexOf("patch_qemu_wasm_runtime_env_bridge()"));
    held = await owner({...f.env, FAKE_DOCKER_MODE: "configure-interrupted", QEMU_WASM_BUILD_DIR: build, JOBS: "1"},
      configure + '\npatch_qemu_wasm_pty_link_input(){ :; }\nacquire_source_lock; start_container; configure_and_make');
    process.kill(-held.child.pid, "SIGTERM");
    assert.deepEqual(await held.closed, [143, null]);
    assert.match(held.output(), /first interrupted configure diagnostics/);
    assert.match(held.output(), /interrupted compiler probe stderr/);
    held = null;
    assert.equal(f.calls().filter(args => args.includes("emconfigure")).length, 1);
    assert.equal(f.calls().filter(args => args.includes("emmake")).length, 0);
    assert.deepEqual(f.calls().filter(args => args[0] === "rm"), [["rm", "-f", "a".repeat(64)]]);
    assert.equal(existsSync(f.lock), false);
  } finally {
    if (held) { process.kill(-held.child.pid, "SIGTERM"); await held.closed; }
    rmSync(f.root, {recursive: true, force: true});
  }
});

test("a compile failure retains the configure result and full probe diagnostics with its original exit", () => {
  const f = fixture();
  try {
    const build = join(f.root, "build");
    mkdirSync(build);
    writeFileSync(join(build, "config-host.h"), "#undef CONFIG_ATOMIC64\n");
    writeFileSync(join(build, "config.log"), "complete fallback configure diagnostics\n");
    const configure = text.slice(text.indexOf("configure_and_make()"), text.indexOf("patch_qemu_wasm_runtime_env_bridge()"));
    const result = f.run(configure +
      '\npatch_qemu_wasm_pty_link_input(){ :; }\nacquire_source_lock; start_container; configure_and_make',
    {FAKE_DOCKER_MODE: "compile-failed", QEMU_WASM_BUILD_DIR: build, JOBS: "1"});
    assert.equal(result.status, 27);
    assert.match(result.stdout, /"CONFIG_ATOMIC64": false/);
    assert.match(result.stdout, /complete fallback configure diagnostics/);
    assert.match(result.stderr, /fixture compiler failure/);
    assert.equal(f.calls().filter(args => args.includes("emconfigure")).length, 1);
    assert.equal(f.calls().filter(args => args.includes("emmake")).length, 1);
    assert.equal(existsSync(f.lock), false);
  } finally { rmSync(f.root, {recursive: true, force: true}); }
});
