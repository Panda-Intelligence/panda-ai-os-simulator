import { createHash } from "node:crypto";
import { lstatSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { readVerifiedArtifact } from "./pack-safe-io.mjs";

export function copyBuiltUi(root, stage) {
  const fail = reason => { throw new Error(`web_ui_invalid:${reason}`); };
  if (basename(resolve(root)) !== "dist") fail("expected_built_dist");
  const rootInfo = lstatSync(root);
  if (!rootInfo.isDirectory() || rootInfo.isSymbolicLink()) fail("root_invalid");
  let count = 0, total = 0, indexFound = false;
  function walk(directory = "", depth = 0) {
    if (depth > 12) fail("depth_exceeded");
    for (const name of readdirSync(join(root, directory))) {
      if (name === ".DS_Store") continue; // OS metadata is never a deployable asset.
      if (name.startsWith(".") || /[%?#\\\x00-\x1f]/.test(name)) fail("unsafe_name");
      const path = directory ? `${directory}/${name}` : name;
      // The verified artifact pack, not a previous generated copy, owns this.
      if (!directory && name === "simulator-runtime") continue;
      const info = lstatSync(join(root, path));
      if (info.isSymbolicLink()) fail("symlink_not_allowed");
      if (info.isDirectory()) { walk(path, depth + 1); continue; }
      if (!info.isFile() || !/\.(html|js|css|svg|png|ico|wasm|woff2?)$/.test(name)) fail("unapproved_extension");
      if (++count > 1024 || (total += info.size) > 128 * 1024 * 1024 || info.size > 25 * 1024 * 1024 || info.size < 1) fail("budget_exceeded");
      const sha256 = createHash("sha256").update(readFileSync(join(root,path))).digest("hex");
      const verified = readVerifiedArtifact(root, {path, bytes:info.size, sha256}, "ui");
      const destination = join(stage,path); mkdirSync(dirname(destination),{recursive:true});
      writeFileSync(destination,verified.data,{flag:"wx"});
      if (path === "index.html") indexFound = true;
    }
  }
  walk();
  if (!indexFound) fail("index_missing");
  return { files: count, bytes: total };
}
