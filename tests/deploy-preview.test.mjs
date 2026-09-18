import { once } from "node:events";
import { createHash } from "node:crypto";
import { mkdtemp, mkdir, symlink, writeFile } from "node:fs/promises";
import { request as httpRequest } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawn } from "node:child_process";
import test from "node:test";
import assert from "node:assert/strict";

const previewScript = join(process.cwd(), "scripts", "serve-dist.mjs");
const requiredHeaders = {
  "cross-origin-opener-policy": "same-origin",
  "cross-origin-embedder-policy": "require-corp",
  "cross-origin-resource-policy": "same-origin",
  "x-content-type-options": "nosniff",
};

function assertIsolation(response) {
  for (const [name, value] of Object.entries(requiredHeaders)) {
    assert.equal(response.headers.get(name), value, `${name} must be present`);
  }
}

async function rawRequest(port, path, method = "GET") {
  return new Promise((resolve, reject) => {
    const request = httpRequest({ host: "127.0.0.1", port, path, method }, (response) => {
      const chunks = [];
      response.on("data", (chunk) => chunks.push(chunk));
      response.on("end", () => resolve({
        status: response.statusCode,
        headers: response.headers,
        body: Buffer.concat(chunks),
      }));
    });
    request.on("error", reject);
    request.end();
  });
}

async function startPreview(root) {
  const processHandle = spawn(process.execPath, [previewScript, "--root", root, "--port", "0"], {
    cwd: process.cwd(),
    stdio: ["ignore", "pipe", "pipe"],
  });
  let output = "";
  const listening = new Promise((resolve, reject) => {
    processHandle.stdout.on("data", (chunk) => {
      output += chunk;
      const match = output.match(/listening http:\/\/127\.0\.0\.1:(\d+)/);
      if (match) resolve(Number(match[1]));
    });
    processHandle.stderr.on("data", (chunk) => {
      output += chunk;
    });
    processHandle.once("error", reject);
    processHandle.once("exit", (code) => reject(new Error(`preview exited early (${code}): ${output}`)));
  });
  const port = await listening;
  return {
    port,
    async close() {
      processHandle.kill("SIGTERM");
      await once(processHandle, "exit");
    },
  };
}

test("loopback preview serves built assets and isolation errors over real HTTP", async () => {
  const temporaryRoot = await mkdtemp(join(tmpdir(), "panda-deploy-"));
  const root = join(temporaryRoot, "dist");
  await mkdir(root);
  await mkdir(join(root, "nested"));
  await writeFile(join(root, "index.html"), "<!doctype html><title>simulator</title>");
  await writeFile(join(root, "runtime.wasm"), Buffer.from([0, 97, 115, 109]));
  await writeFile(join(root, "source.ts"), "export const notPublic = true;");
  await writeFile(join(temporaryRoot, "outside.txt"), "outside");
  await symlink(join(temporaryRoot, "outside.txt"), join(root, "linked.txt"));

  const preview = await startPreview(root);
  try {
    const base = `http://127.0.0.1:${preview.port}`;
    const index = await fetch(`${base}/`);
    assert.equal(index.status, 200);
    assert.equal(index.headers.get("content-type"), "text/html; charset=utf-8");
    assertIsolation(index);
    assert.match(await index.text(), /simulator/);

    const wasm = await fetch(`${base}/runtime.wasm`);
    assert.equal(wasm.status, 200);
    assert.equal(wasm.headers.get("content-type"), "application/wasm");
    assertIsolation(wasm);
    assert.deepEqual([...new Uint8Array(await wasm.arrayBuffer())], [0, 97, 115, 109]);

    const head = await fetch(`${base}/runtime.wasm`, { method: "HEAD" });
    assert.equal(head.status, 200);
    assert.equal(head.headers.get("content-length"), "4");
    assert.equal(await head.text(), "");
    assertIsolation(head);

    const directory = await fetch(`${base}/nested`);
    assert.equal(directory.status, 404);
    assertIsolation(directory);

    const missing = await fetch(`${base}/missing.js`);
    assert.equal(missing.status, 404);
    assertIsolation(missing);

    const source = await fetch(`${base}/source.ts`);
    assert.equal(source.status, 404);
    assertIsolation(source);

    const traversal = await rawRequest(preview.port, "/%2e%2e/outside.txt");
    assert.equal(traversal.status, 400);
    assert.equal(traversal.headers["cross-origin-embedder-policy"], "require-corp");

    const encodedSlash = await rawRequest(preview.port, "/runtime%2Fwasm");
    assert.equal(encodedSlash.status, 400);

    const symlink = await fetch(`${base}/linked.txt`);
    assert.equal(symlink.status, 403);
    assertIsolation(symlink);
  } finally {
    await preview.close();
  }
});

test("Worker static and R2 seams preserve isolation headers and deny unlisted keys", async () => {
  const { default: worker, handleRequest } = await import("../deploy/worker.ts");
  const staticEnv = {
    ASSETS: {
      fetch: async (request) => request.url.endsWith("missing")
        ? new Response("not found", { status: 404 })
        : new Response("built app", { status: 200, headers: { "Content-Type": "text/html" } }),
    },
  };

  const staticResponse = await worker.fetch(new Request("https://simulator.invalid/"), staticEnv);
  assert.equal(staticResponse.status, 200);
  assertIsolation(staticResponse);
  assert.equal(await staticResponse.text(), "built app");

  const staticError = await handleRequest(new Request("https://simulator.invalid/missing"), staticEnv);
  assert.equal(staticError.status, 404);
  assertIsolation(staticError);

  const bytes = new TextEncoder().encode("guest-release");
  const assetDigest = createHash("sha256").update(bytes).digest("hex");
  const manifest = JSON.stringify({
    version: 1,
    entries: [{
      path: "/releases/v1/runtime.wasm",
      key: "releases/v1/runtime.wasm",
      sha256: assetDigest,
      etag: "\"release-v1\"",
      contentType: "application/wasm",
    }],
  });
  const manifestDigest = createHash("sha256").update(manifest).digest("hex");
  let requestedKey;
  let requestedEtag;
  const releaseEnv = {
    RELEASE_MANIFEST: manifest,
    RELEASE_MANIFEST_SHA256: manifestDigest,
    RELEASE_BUCKET: {
      async get(key, options) {
        requestedKey = key;
        requestedEtag = options?.onlyIf?.etagMatches;
        return {
          body: new Response(bytes).body,
          size: bytes.byteLength,
          httpEtag: '"release-v1"',
          httpMetadata: { contentType: "application/octet-stream" },
          checksums: { sha256: Buffer.from(assetDigest, "hex") },
        };
      },
    },
  };

  const release = await handleRequest(
    new Request("https://simulator.invalid/releases/v1/runtime.wasm"),
    releaseEnv,
  );
  assert.equal(release.status, 200);
  assert.equal(release.headers.get("content-type"), "application/wasm");
  assertIsolation(release);
  assert.equal(await release.text(), "guest-release");
  assert.equal(requestedKey, "releases/v1/runtime.wasm");
  assert.equal(requestedEtag, '"release-v1"');

  const unverifiedObject = await handleRequest(
    new Request("https://simulator.invalid/releases/v1/runtime.wasm"),
    {
      ...releaseEnv,
      RELEASE_BUCKET: {
        async get() {
          return { body: new Response(bytes).body };
        },
      },
    },
  );
  assert.equal(unverifiedObject.status, 502);
  assertIsolation(unverifiedObject);

  const unlisted = await handleRequest(
    new Request("https://simulator.invalid/releases/v1/other.wasm"),
    releaseEnv,
  );
  assert.equal(unlisted.status, 404);
  assertIsolation(unlisted);

  const encodedTraversal = await handleRequest(
    new Request("https://simulator.invalid/releases/v1/%252e%252e/other.wasm"),
    releaseEnv,
  );
  assert.equal(encodedTraversal.status, 400);
  assertIsolation(encodedTraversal);

  const missingAssets = await handleRequest(new Request("https://simulator.invalid/"), {});
  assert.equal(missingAssets.status, 503);
  assertIsolation(missingAssets);
});
