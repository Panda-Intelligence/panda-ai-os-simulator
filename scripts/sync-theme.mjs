// Copies the canonical Panda IDE shared stylesheet into this app.
// Single source of truth: apps/shared/panda-ide/panda-ide.css
// Generated destination (git-ignored): apps/simulator/src/panda-ide.css
// Runs as the first step of `dev` / `build` (see package.json).
import { copyFileSync, mkdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const appRoot = resolve(here, "..");
const src = resolve(appRoot, "..", "shared", "panda-ide", "panda-ide.css");
const dest = resolve(appRoot, "src", "panda-ide.css");

mkdirSync(dirname(dest), { recursive: true });
copyFileSync(src, dest);
console.log(`[sync-theme] ${src} -> ${dest}`);
