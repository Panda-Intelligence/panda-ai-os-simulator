import test from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { runInNewContext } from "node:vm";

test("multi-board provenance requires unchanged firmware inputs across real Git revisions", () => {
  const dir = mkdtempSync(join(tmpdir(), "panda-artifact-provenance-"));
  const git = (...args) => execFileSync("git", ["-C", dir, ...args], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }).trim();
  try {
    git("init", "-q");
    git("config", "user.name", "Simulator Test");
    git("config", "user.email", "simulator-test@example.invalid");
    writeFileSync(join(dir, "firmware.c"), "int firmware = 1;\n");
    git("add", ".");
    git("commit", "-qm", "firmware baseline");
    const baseline = git("rev-parse", "HEAD");
    writeFileSync(join(dir, "metadata.json"), "{}\n");
    git("add", ".");
    git("commit", "-qm", "metadata only");
    const metadata = git("rev-parse", "HEAD");
    writeFileSync(join(dir, "firmware.c"), "int firmware = 2;\n");
    git("add", ".");
    git("commit", "-qm", "firmware changed");
    const changed = git("rev-parse", "HEAD");

    const source = readFileSync(new URL("../scripts/write-qemu-wasm-artifact-manifest.mjs", import.meta.url), "utf8");
    const start = source.indexOf("function sourceInputsMatchCurrentRevision(");
    const end = source.indexOf("\nconst sharedRequired", start);
    assert.ok(start >= 0 && end > start);
    const check = (revision, explicit = true) => runInNewContext(
      `${source.slice(start, end)}\nsourceInputsMatchCurrentRevision(baseline);`,
      { execFileSync, baseline, currentSourceRevision: revision, sourceInputCompatibility: new Map(),
        explicitProductRoot: explicit ? dir : undefined, repoRoot: dir,
        product: { sourceInputs: ["firmware.c"] }, requiredBuildBoards: ["default", "s37uc"] },
    );
    assert.equal(check(baseline), true);
    assert.equal(check(metadata), true);
    assert.equal(check(changed), false);
    assert.equal(check(metadata, false), false);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
