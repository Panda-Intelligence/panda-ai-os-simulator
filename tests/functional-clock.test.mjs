import test from "node:test";
import assert from "node:assert/strict";
import { functionalClockArgs } from "../src/browserQemuPlatform.js";
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
import ts from "typescript";

test("Studio declares one fixed instruction-clock profile and keeps ordinary simulator defaults", () => {
  assert.deepEqual(functionalClockArgs(undefined), []);
  assert.deepEqual(functionalClockArgs("studio-functional-icount-v1"), ["-icount", "shift=3,align=off,sleep=on"]);
  assert.throws(() => functionalClockArgs("auto"), /clock_profile_unsupported/);
});

test("runtime manifest parsing and URL normalization preserve the explicit clock profile", () => {
  const source = readFileSync(new URL("../src/browserSimulatorRuntime.ts", import.meta.url), "utf8")
    .replaceAll("import.meta.env.BASE_URL", '"/studio-simulator/"')
    .replaceAll("import.meta.url", '"http://localhost/studio-simulator/module.js"');
  const code = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } }).outputText;
  const exports = {};
  runInNewContext(code, { exports, URL, console });
  const input = { runtime: { kind: "wasm-worker", status: "ready", workerScript: "simulator-runtime/worker.js", clockProfile: "studio-functional-icount-v1" } };
  const parsed = exports.parseBrowserSimulatorRuntimeManifest(input);
  assert.equal(parsed.clockProfile, "studio-functional-icount-v1");
  const resolved = exports.resolveBrowserSimulatorRuntimeManifestUrls(parsed, "http://localhost/studio-simulator/manifest.json");
  assert.equal(resolved.clockProfile, parsed.clockProfile);
  assert.deepEqual(functionalClockArgs(resolved.clockProfile), ["-icount", "shift=3,align=off,sleep=on"]);
  assert.equal(exports.parseBrowserSimulatorRuntimeManifest({ runtime: { ...input.runtime, clockProfile: "auto" } }).kind, "unavailable");
});
