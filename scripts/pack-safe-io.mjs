import { createHash } from "node:crypto";
import { closeSync, constants, fstatSync, lstatSync, openSync, readSync, realpathSync } from "node:fs";
import { join, resolve, sep } from "node:path";

// Hold one bounded snapshot from validation through publication, not a second
// path-based copy. Validate ancestors as well as the final directory entry.
export function readVerifiedArtifact(root, artifact, label) {
  const fail = reason => { throw new Error(`web_artifact_manifest_invalid:${label}_${reason}`); };
  const declared = resolve(root), rootInfo = lstatSync(declared);
  if (rootInfo.isSymbolicLink() || !rootInfo.isDirectory()) fail("root_invalid");
  const canonical = realpathSync(declared);
  let path = canonical;
  const parts = artifact.path.split("/");
  for (const [index, part] of parts.entries()) {
    if (!part || part === "." || part === "..") fail("unsafe_component");
    path = join(path, part);
    let info;
    try { info = lstatSync(path); } catch { fail("missing"); }
    if (info.isSymbolicLink()) fail("symlink_not_allowed");
    if (index < parts.length - 1 && !info.isDirectory()) fail("parent_not_directory");
  }
  const actual = realpathSync(path);
  if (!actual.startsWith(canonical + sep)) fail("escapes_root");
  const fd = openSync(actual, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
  try {
    const before = fstatSync(fd);
    if (!before.isFile()) fail("not_regular_file");
    if (before.size !== artifact.bytes) fail("byte_length_mismatch");
    if (!Number.isSafeInteger(artifact.bytes) || artifact.bytes < 1 || artifact.bytes > 256 * 1024 * 1024) fail("size_invalid");
    const bytes = Buffer.alloc(artifact.bytes + 1);
    let used = 0;
    while (used < bytes.length) {
      const count = readSync(fd, bytes, used, bytes.length - used, null);
      if (!count) break;
      used += count;
    }
    const after = fstatSync(fd);
    if (used !== artifact.bytes || after.size !== before.size || after.mtimeMs !== before.mtimeMs) fail("changed_during_read");
    const data = bytes.subarray(0, used);
    const digest = createHash("sha256").update(data).digest("hex");
    if (digest !== artifact.sha256) fail("sha256_mismatch");
    return { sourcePath: actual, bytes: used, sha256: digest, data };
  } finally { closeSync(fd); }
}
