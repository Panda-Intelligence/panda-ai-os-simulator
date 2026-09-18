// 将独立 UI token 样式表复制到应用生成样式表，避免依赖仓库外路径。
import { copyFileSync, mkdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const appRoot = resolve(here, "..");
const src = resolve(appRoot, "packages", "ui-tokens", "panda-ide.css");
const dest = resolve(appRoot, "src", "panda-ide.css");

mkdirSync(dirname(dest), { recursive: true });
copyFileSync(src, dest);
console.log(`[sync-theme] ${src} -> ${dest}`);
