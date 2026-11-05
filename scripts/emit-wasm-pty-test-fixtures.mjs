import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { patchWasmPtySources } from "./patch-wasm-pty-source.mjs";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const fixture = join(root, "tests/fixtures/wasm-pty-source");
const image = process.argv[2];
if (!image || process.argv.length !== 3) throw new Error("Usage: node scripts/emit-wasm-pty-test-fixtures.mjs <pinned-toolchain-image>");
const imageId = execFileSync("docker", ["image", "inspect", "--format", "{{.Id}}", image], { encoding: "utf8" }).trim();
const expected = JSON.parse(readFileSync(join(fixture, "provenance.json"), "utf8"));
if (imageId !== expected.toolchainImageId) throw new Error("wasm_pty_test_toolchain_image_mismatch");
const sha = bytes => createHash("sha256").update(bytes).digest("hex");
const work = mkdtempSync(join(tmpdir(), "panda-pty-emission-"));
const flags = ["-O1", "-g", "-pthread", "-sPROXY_TO_PTHREAD=1", "-sASYNCIFY=1", "-sFORCE_FILESYSTEM=1", "-sWASM_BIGINT=1", "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sENVIRONMENT=node"];
const emissions = [];
try {
  for (const mode of ["red", "green"]) {
    const dir = join(work, mode), source = join(dir, "source");mkdirSync(source, { recursive: true });
    copyFileSync(join(fixture, "probe.c"), join(dir, "probe.c"));
    copyFileSync(join(fixture, "probe-layout.js"), join(dir, "probe-layout.js"));
    for (const name of ["emscripten-pty.js", "library_syscall.js"]) copyFileSync(join(fixture, name), join(source, name));
    let library = "/source/emscripten-pty.js";
    if (mode === "green") {
      const receipt = patchWasmPtySources({ ptySource: join(source, "emscripten-pty.js"), syscallSource: join(source, "library_syscall.js"), outputDir: join(source, "library") });
      library = "/source/library/" + receipt.sourceSetDigest + "/emscripten-pty.js";
    }
    execFileSync("docker", ["run", "--rm", "--network", "none", "--cpus", "2", "-e", "EMCC_CFLAGS=",
      "-v", dir + ":/out", "-v", source + ":/source:ro", "-v", join(source, "library_syscall.js") + ":/emsdk/upstream/emscripten/src/library_syscall.js:ro",
      imageId, "emcc", "/out/probe.c", ...flags, '-sEXPORTED_FUNCTIONS=["_main","_malloc","_free"]', '-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=["$PANDA_PTY_TIMEVAL_LAYOUT"]', "--js-library=" + library, "--js-library=/out/probe-layout.js", "-o", "/out/probe.js"], { stdio: "inherit" });
    const bytes = readFileSync(join(dir, "probe.js"));writeFileSync(join(fixture, `emitted-${mode}.js`), bytes);
    emissions.push({ mode, jsSha256: sha(bytes), cSha256: sha(readFileSync(join(fixture, "probe.c"))), compilerExit: 0,
      libraryInputs: mode === "red" ? "pristine pinned source" : "canonical source patch applied before emcc", flags });
  }
  const manifest = JSON.parse(readFileSync(join(root, "scripts/wasm-pty-source.json"), "utf8"));
  writeFileSync(join(fixture, "emission.json"), JSON.stringify({ generatedAt: new Date().toISOString(), toolchainImageId: imageId,
    emscriptenVersion: manifest.emscriptenVersion, xtermPtyVersion: manifest.xtermPtyVersion,
    sourcePatchSha256: sha(readFileSync(join(root, "scripts/wasm-pty-source.patch"))), emissions, sourceLibraries: manifest.files }, null, 2) + "\n");
} finally { rmSync(work, { recursive: true, force: true }); }
