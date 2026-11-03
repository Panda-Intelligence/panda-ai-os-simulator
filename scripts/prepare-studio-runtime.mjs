import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const simulator = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const required = ["--engine-dir", "--mofei-build", "--compact-build", "--source-root", "--output-dir"];
const keys = new Set([...required, "--seed-image"]);
const options = {};
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i += 2) {
  if (!keys.has(argv[i]) || !argv[i + 1] || options[argv[i]]) throw new Error("studio_runtime_explicit_inputs_required");
  options[argv[i]] = resolve(argv[i + 1]);
}
if (required.some(key => !options[key])) throw new Error("Usage: bun run prepare:studio --engine-dir DIR --mofei-build DIR --compact-build DIR --source-root DIR --output-dir DIR [--seed-image GENERATED_PUBLIC_SEED]");
const sha = bytes => createHash("sha256").update(bytes).digest("hex");
const root = options["--source-root"];
const sourceRevision = execFileSync("git", ["-C", root, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
const engineRoot = options["--engine-dir"];
const engineIsRaw = existsSync(join(engineRoot, "qemu-wasm-artifacts.json"));
const engineManifest = engineIsRaw ? JSON.parse(readFileSync(join(engineRoot, "qemu-wasm-artifacts.json"), "utf8")) : (() => {
  const bytes = readFileSync(join(engineRoot, "runtime-source.json"));
  const built = JSON.parse(bytes);
  if (built.kind !== "local-runtime-build" || !built.qemu?.commit || !built.toolchainImageId
    || !["emmalloc", "dlmalloc", "mimalloc"].includes(built.wasmAllocator) || !built.runtimeBuildRevision) throw new Error("studio_engine_build_provenance_invalid");
  return { schemaVersion: 2, status: "ready", runtimeBuildRevision: built.runtimeBuildRevision, wasmAllocator: built.wasmAllocator,
    source: built, sourceArtifactManifestSha256: sha(bytes), artifacts: built.artifacts.map(entry => ({ ...entry, fileName: entry.path })) };
})();
if (engineManifest.status !== "ready" || !engineManifest.runtimeBuildRevision) throw new Error("studio_engine_not_ready");
const engineFiles = new Map(engineManifest.artifacts.map(entry => [entry.fileName, entry]));
const work = mkdtempSync(join(tmpdir(), "panda-studio-runtime-"));
const qemu = join(work, "qemu");
const ui = join(work, "dist");
mkdirSync(qemu);
const receipts = { sourceRevision, engine: [], engineSource: engineIsRaw ? { kind: "qemu-artifact-manifest", sha256: sha(readFileSync(join(engineRoot, "qemu-wasm-artifacts.json"))) } : { kind: "local-runtime-build", source: engineManifest.source, sha256: engineManifest.sourceArtifactManifestSha256 }, boards: {} };
const run = (file, args, env = {}) => execFileSync(file, args, { cwd: simulator, env: { ...process.env, ...env }, stdio: "inherit" });
try {
  for (const name of ["qemu-system-xtensa.js", "qemu-system-xtensa.wasm", "qemu-system-xtensa.worker.js", "esp32s3_rev0_rom.bin"]) {
    const expected = engineFiles.get(name);
    const input = join(engineRoot, name);
    const bytes = readFileSync(input);
    if (!expected || expected.bytes !== bytes.length || expected.sha256 !== sha(bytes)) throw new Error(`studio_engine_digest_mismatch:${name}`);
    copyFileSync(input, join(qemu, name));
    receipts.engine.push({ path: name, bytes: bytes.length, sha256: sha(bytes) });
  }
  writeFileSync(join(qemu, "qemu-wasm-artifacts.json"), JSON.stringify(engineManifest));
  for (const [board, flag, runtimeBoard] of [["mofei", "--mofei-build", "default"], ["s37uc", "--compact-build", "s37uc"]]) {
    const build = options[flag];
    const cache = readFileSync(join(build, "CMakeCache.txt"), "utf8");
    const config = readFileSync(join(build, "sdkconfig"), "utf8");
    // 使用既有模拟器性能配置；实体固件的 SIZE 覆盖会放大指令解释与软件验签开销。
    if (!/^CONFIG_COMPILER_OPTIMIZATION_PERF=y$/m.test(config)
      || /^CONFIG_COMPILER_OPTIMIZATION_SIZE=y$/m.test(config)
      || !/^CONFIG_MBEDTLS_COMPILER_OPTIMIZATION_PERF=y$/m.test(config)
      || /^CONFIG_MBEDTLS_COMPILER_OPTIMIZATION_SIZE=y$/m.test(config)) throw new Error(`studio_simulator_perf_profile_required:${board}`);
    if (!/^PANDA_SIMULATOR:[^=]+=1$/m.test(cache) || !/^PANDA_PACKAGE_V2_YYJSON_SPIKE:[^=]+=(ON|1)$/m.test(cache)
      || !new RegExp(`^BOARD:[^=]+=${runtimeBoard}$`, "m").test(cache)) throw new Error(`studio_build_configuration_invalid:${board}`);
    const commands = JSON.parse(readFileSync(join(build, "compile_commands.json"), "utf8"));
    const verifier = commands.find(entry => entry.file.endsWith("/runtime/AppPackageVerifierV2.cpp"));
    if (!verifier?.command.includes("PANDA_PACKAGE_V2_YYJSON_SPIKE=1")) throw new Error(`studio_build_verifier_disabled:${board}`);
    const plan = existsSync(join(build, "studio-release-profile.json")) ? join(build, "studio-release-profile.json") : build + ".release-profile/resolved-release-profile.json";
    if (JSON.parse(readFileSync(plan, "utf8")).sourceRevision !== sourceRevision) throw new Error(`studio_build_source_revision_mismatch:${board}`);
    run("bash", ["scripts/build-qemu-wasm.sh", "--firmware-only", `--firmware-dir=${build}`], {
      BOARD: runtimeBoard, QEMU_WASM_OUTPUT_DIR: qemu, QEMU_WASM_REQUIRED_BOARDS: "default s37uc",
      QEMU_WASM_SOURCE_REVISION: sourceRevision, QEMU_WASM_PROFILE_PLAN: plan,
    });
    copyFileSync(join(build, "sdkconfig"), join(qemu, `sdkconfig-${board}`));
    receipts.boards[board] = Object.fromEntries(["panda_os.bin", "panda_os.elf", "sdkconfig", "CMakeCache.txt"].map(path => {
      const bytes = readFileSync(join(build, path)); return [path, { bytes: bytes.length, sha256: sha(bytes) }];
    }));
  }
  run(process.execPath, ["scripts/write-qemu-wasm-artifact-manifest.mjs", qemu, join(qemu, "qemu-wasm-artifacts.json"), "--require-boards", "default", "s37uc"], {
    PANDA_SIMULATOR_PROJECT_ROOT: root, QEMU_WASM_SOURCE_REVISION: sourceRevision, QEMU_WASM_RUNTIME_REVISION: engineManifest.runtimeBuildRevision,
  });
  run(process.execPath, ["node_modules/vite/bin/vite.js", "build", "--config", "studio.vite.config.ts", "--outDir", ui]);
  copyFileSync(join(ui, "studio.html"), join(ui, "index.html"));
  const seed = options["--seed-image"] || join(work, "seed.img.gz");
  const resources = join(work, "system-resources");
  if (options["--seed-image"]) {
    const receipt = JSON.parse(readFileSync(seed + ".receipt.json", "utf8"));
    const authority = JSON.parse(readFileSync(join(root, "apps/panda-os/tools/default-font-payload.json"), "utf8"));
    const names = new Set([...authority.packs, ...authority.streamSources].map(item => `/.murphy/fonts/${item.file}`));
    if (receipt.kind !== "public-system-resources-studio-seed" || receipt.sha256 !== sha(readFileSync(seed)) || receipt.rawBytes !== 128 * 1024 * 1024
      || receipt.files.length !== names.size || receipt.files.some(item => !names.has(item.path))) throw new Error("studio_seed_receipt_invalid");
  } else {
    run("bash", [join(root, "apps/panda-os/tools/provision-default-fonts.sh"), "--root", resources]);
    run(process.execPath, ["scripts/create-studio-seed.mjs", seed, resources]);
  }
  run(process.execPath, ["scripts/pack-studio-runtime.mjs", "--qemu-dir", qemu, "--ui-dir", ui, "--sd-image", seed, "--output-dir", options["--output-dir"]]);
  // 源码差异也必须包含未提交的新固件文件；Git HEAD 本身不足以描述开发构建。
  const sourcePaths = execFileSync("git", ["-C", root, "ls-files", "--cached", "--others", "--exclude-standard", "--", "apps/panda-os"], { encoding: "utf8" }).trim().split("\n").filter(Boolean).sort();
  const sources = sourcePaths.map(path => ({ path, sha256: sha(readFileSync(join(root, path))) }));
  receipts.sourceFilesSha256 = sha(Buffer.from(JSON.stringify(sources)));
  receipts.seedSha256 = sha(readFileSync(seed));
  receipts.systemResourceAuthority = {
    manifest: "apps/panda-os/tools/default-font-payload.json",
    manifestSha256: sha(readFileSync(join(root, "apps/panda-os/tools/default-font-payload.json"))),
    provenance: "apps/panda-cloud/scripts/lib/font-payload.mjs",
    provenanceSha256: sha(readFileSync(join(root, "apps/panda-cloud/scripts/lib/font-payload.mjs"))),
  };
  const output = options["--output-dir"];
  writeFileSync(join(output, "studio-source-files.json"), JSON.stringify(sources));
  copyFileSync(seed + ".receipt.json", join(output, "studio-system-resources.json"));
  writeFileSync(join(output, "studio-build-receipt.json"), JSON.stringify({ ...receipts, runtimeReady: true, executionQualified: false }, null, 2) + "\n");
  console.log(JSON.stringify({ output, runtimeReady: true, executionQualified: false }));
} finally { rmSync(work, { recursive: true, force: true }); }
