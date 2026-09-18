import { execFileSync } from "node:child_process";
import { lstatSync, readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const files = [...new Set(execFileSync("git", ["ls-files", "-z", "--cached", "--others", "--exclude-standard"], { cwd: root, encoding: "utf8" }).split("\0").filter(Boolean))].sort();
const findings = [], fingerprints = [];
const rules = [
  ["private-key", /-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----/],
  ["github-token", /gh[pousr]_[A-Za-z0-9]{30,}/],
  ["aws-access-key", /AKIA[A-Z0-9]{16}/],
  ["developer-absolute-path", /(?:\/Users\/[^\s/]+\/|[A-Z]:\\Users\\)/],
];
for (const path of files) {
  const absolute = resolve(root, path), info = lstatSync(absolute);
  if (info.isSymbolicLink() || !info.isFile()) { findings.push({ path, category: "non-regular-export" }); continue; }
  if (/\.(?:bin|elf|wasm|img|ttf|otf|woff2?|epub|gb|gba|nes|zip)$/i.test(path)) findings.push({ path, category: "runtime-or-user-asset" });
  if (/(?:^|\/)(?:\.env(?:\..+)?|id_rsa|id_ed25519|credentials\.json)$/.test(path)) findings.push({ path, category: "credential-file" });
  const data = readFileSync(absolute);
  fingerprints.push({ path, bytes: data.length, sha256: createHash("sha256").update(data).digest("hex") });
  if (!data.includes(0)) {
    const text = data.toString("utf8");
    for (const [category, pattern] of rules) {
      const match = pattern.exec(text);
      if (match) findings.push({ path, category, line: text.slice(0, match.index).split("\n").length });
    }
  }
}
const rights = JSON.parse(readFileSync(resolve(root, "provenance/rights-review.json"), "utf8"));
console.log(JSON.stringify({ schemaVersion: 1, fileCount: files.length, findings, fingerprints,
  rightsGate: "pending-human-approval", publicReleaseApproved: false,
  inheritedRightsPending: rights.files.filter(e => e.decision !== "approved").length,
  disclaimer: "Pattern scan only; not complete secret detection, license clearance, or binary publication authority."
}, null, 2));
if (findings.length || process.argv.includes("--publication")) process.exitCode = 1;
