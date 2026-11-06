import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const sha = bytes => createHash("sha256").update(bytes).digest("hex");

export function patchWasmPtySources({ ptySource, syscallSource, outputDir, receiptPath }) {
  const manifest = JSON.parse(readFileSync(join(here, "wasm-pty-source.json"), "utf8"));
  const patchPath = join(here, "wasm-pty-source.patch");
  const inputs = { pty: resolve(ptySource), syscall: resolve(syscallSource) };
  const states = manifest.files.map(file => {
    const input = inputs[file.key];
    const bytes = readFileSync(input), digest = sha(bytes);
    const previousPatched = digest !== file.afterSha256 && (file.previousPatchedSha256 ?? []).includes(digest);
    if (digest !== file.beforeSha256 && digest !== file.afterSha256 && !previousPatched) {
      throw new Error(`wasm_pty_source_version_mismatch:${file.key}:${digest}`);
    }
    return { ...file, input, bytes, alreadyPatched: digest === file.afterSha256, previousPatched };
  });
  const stage = mkdtempSync(join(tmpdir(), "panda-pty-source-"));
  try {
    for (const file of states) {
      const target = join(stage, file.staged);
      mkdirSync(dirname(target), { recursive: true });
      const backup = file.input + ".panda-pty-upstream";
      if (file.previousPatched || (file.alreadyPatched && states.some(other => !other.alreadyPatched))) {
        if (!existsSync(backup) || sha(readFileSync(backup)) !== file.beforeSha256) {
          throw new Error(`wasm_pty_partial_source_without_upstream:${file.key}`);
        }
        copyFileSync(backup, target);
      } else writeFileSync(target, file.bytes);
    }
    if (states.some(file => !file.alreadyPatched)) {
      execFileSync("git", ["apply", "--check", patchPath], { cwd: stage, stdio: "pipe" });
      execFileSync("git", ["apply", patchPath], { cwd: stage, stdio: "pipe" });
    }
    for (const file of states) {
      if (sha(readFileSync(join(stage, file.staged))) !== file.afterSha256) {
        throw new Error(`wasm_pty_patched_source_mismatch:${file.key}`);
      }
    }
    const sourceSetDigest = sha(JSON.stringify(states.map(file => [file.key, file.afterSha256])));
    const libraryDir = join(resolve(outputDir), sourceSetDigest);
    mkdirSync(libraryDir, { recursive: true });
    for (const file of states) {
      const backup = file.input + ".panda-pty-upstream";
      if (!file.alreadyPatched) {
        if (!existsSync(backup)) copyFileSync(file.input, backup);
        if (sha(readFileSync(backup)) !== file.beforeSha256) throw new Error(`wasm_pty_upstream_backup_mismatch:${file.key}`);
        copyFileSync(join(stage, file.staged), file.input);
      }
    }
    const libraryPath = join(libraryDir, "emscripten-pty.js");
    copyFileSync(inputs.pty, libraryPath);
    const receipt = {
      schemaVersion: 1,
      xtermPtyVersion: manifest.xtermPtyVersion,
      emscriptenVersion: manifest.emscriptenVersion,
      sourceUrls: manifest.sourceUrls,
      patchSha256: sha(readFileSync(patchPath)),
      sourceSetDigest,
      libraryPath,
      files: states.map(file => ({
        key: file.key, input: file.input, upstreamSha256: file.beforeSha256,
        patchedSha256: sha(readFileSync(file.input)),
        previousPatchedSha256: file.previousPatched ? sha(file.bytes) : null,
      })),
    };
    if (receiptPath) writeFileSync(receiptPath, JSON.stringify(receipt, null, 2) + "\n");
    return receipt;
  } finally {
    rmSync(stage, { recursive: true, force: true });
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const args = process.argv.slice(2), options = {};
  for (let i = 0; i < args.length; i += 2) {
    if (!["--pty-source", "--syscall-source", "--output-dir", "--receipt"].includes(args[i]) || !args[i + 1]) {
      throw new Error("Usage: patch-wasm-pty-source.mjs --pty-source FILE --syscall-source FILE --output-dir DIR --receipt FILE");
    }
    options[args[i]] = args[i + 1];
  }
  if (Object.keys(options).length !== 4) throw new Error("wasm_pty_explicit_source_paths_required");
  console.log(JSON.stringify(patchWasmPtySources({
    ptySource: options["--pty-source"], syscallSource: options["--syscall-source"],
    outputDir: options["--output-dir"], receiptPath: options["--receipt"],
  })));
}
