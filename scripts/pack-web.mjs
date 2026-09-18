import { readFileSync } from "node:fs";
import { assembleWebArtifacts } from "./pack-web-manifest.mjs";

function usage() {
  return "Usage: node scripts/pack-web.mjs --manifest <path> --runtime-dir <path> --guest-dir <path> --output-dir <path> [--ui-dir <built-dist>]";
}

function parseArgs(argv) {
  const values = {};
  for (let index = 0; index < argv.length; index += 1) {
    const flag = argv[index];
    if (!flag.startsWith("--") || !argv[index + 1] || argv[index + 1].startsWith("--")) {
      throw new Error(usage());
    }
    const key = flag.slice(2);
    if (!["manifest", "runtime-dir", "guest-dir", "output-dir", "ui-dir"].includes(key)) {
      throw new Error(`Unknown pack-web option: ${flag}`);
    }
    if (values[key]) throw new Error(`Duplicate pack-web option: ${flag}`);
    values[key] = argv[index + 1];
    index += 1;
  }
  if (!["manifest", "runtime-dir", "guest-dir", "output-dir"].every(key => values[key])) throw new Error(usage());
  return values;
}

try {
  const args = parseArgs(process.argv.slice(2));
  const result = assembleWebArtifacts({
    manifestPath: args.manifest,
    runtimeDir: args["runtime-dir"],
    guestDir: args["guest-dir"],
    outputDir: args["output-dir"],
    uiDir: args["ui-dir"],
  });
  const outputManifest = JSON.parse(readFileSync(result.manifestPath, "utf8"));
  console.log(`[pack-web] private local pack ready: ${result.outputDir}`);
  console.log(`[pack-web] artifacts=${result.artifactCount} visibility=${outputManifest.publication.visibility}`);
} catch (error) {
  console.error(`[pack-web] ${error instanceof Error ? error.message : String(error)}`);
  process.exitCode = 1;
}
