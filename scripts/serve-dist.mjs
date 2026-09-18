#!/usr/bin/env node

import { createServer } from "node:http";
import { lstat, open, realpath } from "node:fs/promises";
import { constants } from "node:fs";
import { basename, extname, join, relative, resolve } from "node:path";

const ISOLATION_HEADERS = {
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
  "Cross-Origin-Resource-Policy": "same-origin",
  "X-Content-Type-Options": "nosniff",
};

const MIME_TYPES = new Map([
  [".css", "text/css; charset=utf-8"],
  [".gif", "image/gif"],
  [".html", "text/html; charset=utf-8"],
  [".ico", "image/x-icon"],
  [".jpeg", "image/jpeg"],
  [".jpg", "image/jpeg"],
  [".js", "text/javascript; charset=utf-8"],
  [".json", "application/json; charset=utf-8"],
  [".png", "image/png"],
  [".svg", "image/svg+xml"],
  [".wasm", "application/wasm"],
  [".webp", "image/webp"],
]);

const BLOCKED_EXTENSIONS = new Set([
  ".c",
  ".cc",
  ".cpp",
  ".d.ts",
  ".h",
  ".hpp",
  ".jsx",
  ".map",
  ".py",
  ".rs",
  ".scss",
  ".svelte",
  ".swift",
  ".ts",
  ".tsx",
  ".vue",
]);

function usage(message) {
  if (message) console.error(message);
  console.error("Usage: node scripts/serve-dist.mjs --root /explicit/path/to/dist [--port 4173]");
  process.exitCode = 2;
}

function parseArgs(argv) {
  let root;
  let port = 4173;
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === "--root") {
      root = argv[++index];
      if (!root) return usage("--root requires an explicit directory path");
    } else if (argument === "--port") {
      const portValue = argv[++index];
      if (!portValue) return usage("--port requires a value");
      port = Number(portValue);
    }
    else if (argument === "--help") return { help: true };
    else return usage(`Unknown argument: ${argument}`);
  }
  if (!root) return usage("--root is required");
  if (!Number.isInteger(port) || port < 0 || port > 65535) return usage("Port must be an integer from 0 through 65535");
  return { root: resolve(root), port };
}

function addIsolationHeaders(response, headers = {}) {
  for (const [name, value] of Object.entries(ISOLATION_HEADERS)) response.setHeader(name, value);
  for (const [name, value] of Object.entries(headers)) response.setHeader(name, value);
}

function send(response, status, body, headers = {}) {
  addIsolationHeaders(response, { "Content-Type": "text/plain; charset=utf-8", ...headers });
  response.statusCode = status;
  response.end(body);
}

function decodePath(rawUrl) {
  const rawPath = rawUrl.split("?", 1)[0];
  if (!rawPath.startsWith("/") || rawPath.includes("\\")) return null;
  const rawParts = rawPath.slice(1).split("/");
  if (rawParts.length === 1 && rawParts[0] === "") return [];
  const decoded = [];
  for (const rawPart of rawParts) {
    if (!rawPart || /%2f|%5c|%25/i.test(rawPart)) return null;
    let part;
    try {
      part = decodeURIComponent(rawPart);
    } catch {
      return null;
    }
    if (!part || part === "." || part === ".." || /[\\/\0%]/.test(part)) return null;
    decoded.push(part);
  }
  return decoded;
}

async function assertDirectory(root) {
  const rootStat = await lstat(root);
  if (!rootStat.isDirectory() || rootStat.isSymbolicLink()) throw new Error("Static root must be a real directory");
  if (basename(root) !== "dist") throw new Error("Static root must be a directory named dist");
  return realpath(root);
}

async function findRegularFile(root, parts) {
  const candidate = join(root, ...parts);
  const escaped = relative(root, candidate);
  if (escaped.startsWith("..")) return { status: 400 };
  let current = root;
  for (const part of parts) {
    current = join(current, part);
    let stat;
    try {
      stat = await lstat(current);
    } catch (error) {
      if (error?.code === "ENOENT") return { status: 404 };
      throw error;
    }
    if (stat.isSymbolicLink()) return { status: 403 };
  }
  let stat;
  try {
    stat = await lstat(candidate);
  } catch (error) {
    if (error?.code === "ENOENT") return { status: 404 };
    throw error;
  }
  if (!stat.isFile()) return { status: 404 };
  return { path: candidate, stat };
}

function isBlockedSource(path) {
  const lower = path.toLowerCase();
  for (const extension of BLOCKED_EXTENSIONS) {
    if (lower.endsWith(extension)) return true;
  }
  return false;
}

async function serve(request, response, root) {
  if (request.method !== "GET" && request.method !== "HEAD") {
    send(response, 405, "Method not allowed\n", { Allow: "GET, HEAD" });
    return;
  }
  const parts = decodePath(request.url ?? "");
  if (!parts) {
    send(response, 400, "Bad path\n");
    return;
  }
  const fileParts = parts.length === 0 ? ["index.html"] : parts;
  const result = await findRegularFile(root, fileParts);
  if (result.status) {
    send(response, result.status, result.status === 403 ? "Forbidden\n" : "Not found\n");
    return;
  }
  if (isBlockedSource(result.path)) {
    send(response, 404, "Not found\n");
    return;
  }
  const handle = await open(result.path, constants.O_RDONLY);
  addIsolationHeaders(response, {
    "Content-Length": String(result.stat.size),
    "Content-Type": MIME_TYPES.get(extname(result.path).toLowerCase()) ?? "application/octet-stream",
  });
  response.statusCode = 200;
  if (request.method === "HEAD") {
    await handle.close();
    response.end();
    return;
  }
  handle.createReadStream().pipe(response);
}

const options = parseArgs(process.argv.slice(2));
if (options?.help) {
  console.log("Usage: node scripts/serve-dist.mjs --root /explicit/path/to/dist [--port 4173]");
} else if (options) {
  try {
    const root = await assertDirectory(options.root);
    const server = createServer((request, response) => {
      serve(request, response, root).catch(() => send(response, 500, "Internal server error\n"));
    });
    server.on("error", (error) => {
      console.error(error.message);
      process.exitCode = 1;
    });
    server.listen(options.port, "127.0.0.1", () => {
      const address = server.address();
      const actualPort = typeof address === "object" && address ? address.port : options.port;
      console.log(`listening http://127.0.0.1:${actualPort}`);
    });
  } catch (error) {
    console.error(error instanceof Error ? error.message : String(error));
    process.exitCode = 1;
  }
}

export { decodePath };
