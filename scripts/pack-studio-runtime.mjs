import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { packageExistingRuntime } from "./pack-existing-runtime.mjs";

const simulator = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const sha = bytes => createHash("sha256").update(bytes).digest("hex");
const args = process.argv.slice(2);
const options = {};
const keys = { "--qemu-dir": "qemuDir", "--ui-dir": "uiDir", "--output-dir": "outputDir", "--sd-image": "sdImage" };
for (let i = 0; i < args.length; i += 2) {
  const key = keys[args[i]];
  if (!key || !args[i + 1] || options[key]) throw new Error("Usage: pack-studio-runtime.mjs --qemu-dir DIR --ui-dir DIR --output-dir DIR --sd-image FILE");
  options[key] = resolve(args[i + 1]);
}
if (Object.keys(options).length !== 4) throw new Error("studio_runtime_explicit_inputs_required");
const development = JSON.parse(readFileSync(join(simulator, "contracts/studio-development-trust.json"), "utf8"));
const manifest = JSON.parse(readFileSync(join(options.qemuDir, "qemu-wasm-artifacts.json"), "utf8"));
const boards = {};
for (const board of ["mofei", "s37uc"]) {
  const entry = manifest.boards?.[board];
  if (entry?.status !== "ready") throw new Error(`studio_firmware_missing:${board}`);
  const binary = readFileSync(join(options.qemuDir, entry.firmware.bin.fileName));
  const symbols = readFileSync(join(options.qemuDir, entry.firmware.symbols.fileName), "utf8");
  const config = readFileSync(join(options.qemuDir, `sdkconfig-${board}`), "utf8");
  // 必须检查真实镜像、链接符号和编译配置；描述符不能替代这些输入。
  if (!binary.includes(Buffer.from(development.rootPublicKeyHex))
    || !symbols.includes("verifySchemaV2Archive")
    || !config.includes(`CONFIG_PANDA_APP_PACKAGE_TRUST_ROOT_PUBLIC_KEY_HEX="${development.rootPublicKeyHex}"`)
    || !/^CONFIG_PANDA_DEBUG_CONSOLE=y$/m.test(config)
    || !/^CONFIG_ESP_CONSOLE_UART_DEFAULT=y$/m.test(config)) throw new Error(`studio_verifier_image_invalid:${board}`);
  boards[board] = { firmwareSha256: sha(binary), sdkconfigSha256: sha(Buffer.from(config)), sourceRevision: entry.provenance.sourceRevision };
}
const result = packageExistingRuntime({ ...options, basePath: "/studio-simulator/", requireBoards: "mofei,s37uc", sdRawBytes: 128 * 1024 * 1024 });
const webManifestPath = join(options.outputDir, "manifest.json");
const webManifest = JSON.parse(readFileSync(webManifestPath, "utf8"));
webManifest.runtime.clockProfile = "studio-functional-icount-v1";
writeFileSync(webManifestPath, JSON.stringify(webManifest, null, 2) + "\n");
writeFileSync(join(options.outputDir, "simulator-trust.json"), JSON.stringify({
  schemaVersion: 1, scope: "studio-simulator-only", verifierEnabled: true,
  rootPublicKeyHex: development.rootPublicKeyHex, boards,
  warning: "Public development trust. Never use this root on physical or production devices.",
}, null, 2) + "\n");
console.log(JSON.stringify({ ...result, runtimeReady: true, executionQualified: false }));
