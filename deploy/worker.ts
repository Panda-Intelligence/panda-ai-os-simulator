const ISOLATION_HEADERS = {
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
  "Cross-Origin-Resource-Policy": "same-origin",
  "X-Content-Type-Options": "nosniff",
};

const RELEASE_PREFIX = "/releases/";
const SHA256_PATTERN = /^[a-f0-9]{64}$/;
const ETAG_PATTERN = /^[\x21-\x7e]+$/;
const MAX_MANIFEST_BYTES = 1_048_576;
const MAX_MANIFEST_ENTRIES = 1_024;

interface ReleaseEntry {
  path: string;
  key: string;
  sha256: string;
  etag: string;
  contentType?: string;
}

interface ReleaseManifest {
  version: 1;
  entries: ReleaseEntry[];
}

interface ReleaseObject {
  body?: ReadableStream<Uint8Array>;
  size?: number;
  httpEtag?: string;
  httpMetadata?: { contentType?: string };
  checksums?: { sha256?: ArrayBuffer | ArrayBufferView | string };
}

interface ReleaseBucket {
  get(
    key: string,
    options?: { onlyIf?: { etagMatches?: string } },
  ): Promise<ReleaseObject | null>;
}

export interface WorkerEnv {
  ASSETS?: { fetch(request: Request): Promise<Response> };
  RELEASE_BUCKET?: ReleaseBucket;
  RELEASE_MANIFEST?: string;
  RELEASE_MANIFEST_SHA256?: string;
}

function withIsolationHeaders(response: Response): Response {
  const headers = new Headers(response.headers);
  for (const [name, value] of Object.entries(ISOLATION_HEADERS)) {
    headers.set(name, value);
  }
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
}

function errorResponse(message: string, status: number): Response {
  return withIsolationHeaders(
    new Response(`${message}\n`, {
      status,
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    }),
  );
}

function hex(bytes: ArrayBuffer | ArrayBufferView): string {
  const view = bytes instanceof ArrayBuffer
    ? new Uint8Array(bytes)
    : new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return Array.from(view, (byte) => byte.toString(16).padStart(2, "0")).join("");
}

async function sha256(value: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(value));
  return hex(digest);
}

function safeManifestPath(value: unknown): value is string {
  if (typeof value !== "string" || !value.startsWith(RELEASE_PREFIX)) return false;
  if (value.includes("\\") || value.includes("%") || value.includes("\0")) return false;
  const parts = value.slice(1).split("/");
  return parts.every((part) => part.length > 0 && part !== "." && part !== "..");
}

function safeManifestKey(value: unknown): value is string {
  if (typeof value !== "string" || !value.startsWith("releases/")) return false;
  if (value.includes("\\") || value.includes("%") || value.includes("\0")) return false;
  const parts = value.split("/");
  return parts.every((part) => part.length > 0 && part !== "." && part !== "..");
}

function parseManifest(raw: string): ReleaseManifest | null {
  if (new TextEncoder().encode(raw).byteLength > MAX_MANIFEST_BYTES) return null;

  let value: unknown;
  try {
    value = JSON.parse(raw);
  } catch {
    return null;
  }

  if (!value || typeof value !== "object") return null;
  const candidate = value as { version?: unknown; entries?: unknown };
  if (candidate.version !== 1 || !Array.isArray(candidate.entries)) return null;
  if (candidate.entries.length > MAX_MANIFEST_ENTRIES) return null;

  const entries: ReleaseEntry[] = [];
  const paths = new Set<string>();
  for (const item of candidate.entries) {
    if (!item || typeof item !== "object") return null;
    const entry = item as Record<string, unknown>;
    const path = entry.path;
    const key = entry.key;
    const digest = entry.sha256;
    const etag = entry.etag;
    const contentType = entry.contentType;
    if (
      !safeManifestPath(path) ||
      !safeManifestKey(key) ||
      typeof digest !== "string" ||
      !SHA256_PATTERN.test(digest) ||
      typeof etag !== "string" ||
      !ETAG_PATTERN.test(etag) ||
      (contentType !== undefined &&
        (typeof contentType !== "string" || /[\r\n]/.test(contentType)))
    ) {
      return null;
    }
    if (paths.has(path)) return null;
    paths.add(path);
    entries.push({
      path,
      key,
      sha256: digest,
      etag,
      ...(contentType === undefined ? {} : { contentType }),
    });
  }

  return { version: 1, entries };
}

async function verifiedManifest(env: WorkerEnv): Promise<ReleaseManifest | null> {
  const raw = env.RELEASE_MANIFEST;
  const expectedDigest = env.RELEASE_MANIFEST_SHA256;
  if (!raw || new TextEncoder().encode(raw).byteLength > MAX_MANIFEST_BYTES || !expectedDigest || !SHA256_PATTERN.test(expectedDigest)) return null;
  if ((await sha256(raw)) !== expectedDigest) return null;
  return parseManifest(raw);
}

function parseReleasePath(pathname: string): string | null {
  if (!pathname.startsWith(RELEASE_PREFIX)) return null;
  const rawParts = pathname.slice(1).split("/");
  const decoded: string[] = [];
  for (const rawPart of rawParts) {
    if (!rawPart || /%2f|%5c|%25/i.test(rawPart) || rawPart.includes("\\")) return null;
    let part: string;
    try {
      part = decodeURIComponent(rawPart);
    } catch {
      return null;
    }
    if (!part || part === "." || part === ".." || /[\\/\0%]/.test(part)) return null;
    decoded.push(part);
  }
  return `/${decoded.join("/")}`;
}

function checksumFromObject(object: ReleaseObject): string | null {
  const checksum = object.checksums?.sha256;
  if (!checksum) return null;
  if (typeof checksum === "string") return checksum.toLowerCase();
  return hex(checksum).toLowerCase();
}

async function releaseResponse(
  request: Request,
  env: WorkerEnv,
  pathname: string,
): Promise<Response> {
  const releasePath = parseReleasePath(pathname);
  if (!releasePath) return errorResponse("Bad release path", 400);
  if (!env.RELEASE_BUCKET) return errorResponse("Release asset unavailable", 404);

  const manifest = await verifiedManifest(env);
  if (!manifest) return errorResponse("Release manifest unavailable", 503);
  const entry = manifest.entries.find((candidate) => candidate.path === releasePath);
  if (!entry) return errorResponse("Release asset not found", 404);

  const object = await env.RELEASE_BUCKET.get(entry.key, {
    onlyIf: { etagMatches: entry.etag },
  });
  if (!object || !object.body) return errorResponse("Release asset not found", 404);

  const objectChecksum = checksumFromObject(object);
  if (objectChecksum === null || objectChecksum !== entry.sha256) {
    return errorResponse("Release asset verification failed", 502);
  }

  let body: BodyInit | null = object.body;
  const headers = new Headers();
  headers.set("Content-Type", entry.contentType ?? object.httpMetadata?.contentType ?? "application/octet-stream");
  if (object.size !== undefined) headers.set("Content-Length", String(object.size));
  headers.set("ETag", object.httpEtag ?? entry.etag);
  if (request.method === "HEAD") body = null;
  return withIsolationHeaders(new Response(body, { status: 200, headers }));
}

export async function handleRequest(request: Request, env: WorkerEnv): Promise<Response> {
  try {
    if (!["GET", "HEAD"].includes(request.method)) return errorResponse("Method not allowed", 405);
    const url = new URL(request.url);
    if (url.pathname.startsWith(RELEASE_PREFIX)) {
      return await releaseResponse(request, env, url.pathname);
    }
    if (!env.ASSETS) return errorResponse("Static assets unavailable", 503);
    return withIsolationHeaders(await env.ASSETS.fetch(request));
  } catch {
    return errorResponse("Internal server error", 500);
  }
}

export default {
  fetch(request: Request, env: WorkerEnv): Promise<Response> {
    return handleRequest(request, env);
  },
};
