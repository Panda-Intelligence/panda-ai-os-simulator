import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { resolve, join } from "node:path";
import { pathToFileURL } from "node:url";

for (const name of ["PLAYWRIGHT_MODULE", "SIMULATOR_TEST_URL", "SIMULATOR_TEST_OUT", "CHROME_EXECUTABLE"]) {
  if (!process.env[name]) throw new Error(`Explicit ${name} is required`);
}
const modulePath = /\.m?js$/.test(process.env.PLAYWRIGHT_MODULE)
  ? process.env.PLAYWRIGHT_MODULE : join(process.env.PLAYWRIGHT_MODULE, "index.js");
const playwright = await import(pathToFileURL(resolve(modulePath)).href);
const chromium = playwright.chromium ?? playwright.default?.chromium;
if (!chromium) throw new Error("PLAYWRIGHT_MODULE does not expose chromium");

const base = new URL(process.env.SIMULATOR_TEST_URL);
assert.ok(["127.0.0.1", "localhost"].includes(base.hostname));
const browser = await chromium.launch({headless:true, executablePath:process.env.CHROME_EXECUTABLE});
const context = await browser.newContext({viewport:{width:1280,height:1100},serviceWorkers:"block"});
const page = await context.newPage(), errors = [], blocked = [];
page.on("pageerror", error => errors.push(String(error)));
await context.route("**/*", async route => {
  const url = new URL(route.request().url());
  if (["http:","https:"].includes(url.protocol) && url.origin !== base.origin) {
    blocked.push(url.origin); await route.abort(); return;
  }
  await route.continue();
});
const waitStarted = async () => {
  await page.waitForFunction(() => document.querySelector(".pds-pill")?.textContent?.trim() === "start_sim: started", null, {timeout:180000});
  await page.waitForFunction(() => {
    const canvas = document.querySelector("canvas.panel-canvas");
    if (!canvas) return false;
    const pixels = canvas.getContext("2d").getImageData(0,0,canvas.width,canvas.height).data;
    let dark = 0, light = 0;
    for (let i=0;i<pixels.length;i+=4) { dark += pixels[i] < 64; light += pixels[i] > 192; }
    return dark > 100 && light > 100;
  }, null, {timeout:180000});
};
const readStorage = () => page.evaluate(async () => {
  const request = indexedDB.open("panda-browser-simulator-sd-card");
  const db = await new Promise((resolve,reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
  const tx = db.transaction("images","readonly"), store = tx.objectStore("images");
  const keys = await new Promise((resolve,reject) => {
    const query = store.getAllKeys(); query.onsuccess = () => resolve(query.result); query.onerror = () => reject(query.error);
  });
  const metaKey = keys.find(key => Array.isArray(key) && key[0] === "panda-sd-meta-v2");
  if (!metaKey) throw new Error("missing v2 SD metadata");
  const meta = await new Promise((resolve,reject) => {
    const query = store.get(metaKey); query.onsuccess = () => resolve(query.result); query.onerror = () => reject(query.error);
  });
  db.close(); return {keyCount:keys.length, meta};
});
const report = {};
try {
  await page.goto(base.href,{waitUntil:"networkidle"});
  assert.equal(await page.evaluate(() => crossOriginIsolated), true);
  await page.getByRole("button",{name:/Quick Launch|快速啟動|快速启动/}).click();
  await waitStarted();
  report.firstFrame = await page.locator("canvas.panel-canvas").evaluate(canvas => ({width:canvas.width,height:canvas.height,urlLength:canvas.toDataURL().length}));
  await page.getByRole("button",{name:/^Stop$/}).click();
  await page.waitForFunction(() => document.querySelector(".pds-pill")?.textContent?.trim() === "stop_sim: stopped", null, {timeout:60000});
  const stored = await readStorage();
  assert.equal(stored.meta.format,"chunks-v2");
  assert.equal(stored.meta.byteLength,134217728);
  assert.equal(stored.meta.chunkBytes,4*1024*1024);
  assert.equal(stored.meta.chunkCount,32);
  assert.ok(stored.keyCount >= 33);
  await page.getByRole("button",{name:/Quick Launch|快速啟動|快速启动/}).click();
  await waitStarted();
  report.secondFrame = await page.locator("canvas.panel-canvas").evaluate(canvas => ({width:canvas.width,height:canvas.height,urlLength:canvas.toDataURL().length}));
  await page.locator("canvas.panel-canvas").click({position:{x:100,y:100}});
  await page.waitForTimeout(500);
  assert.equal(await page.locator(".pds-pill").textContent(), "start_sim: started");
  assert.equal(errors.length,0,errors.join("\n"));
  assert.deepEqual([...new Set(blocked)],[]);
  report.passed = true;
  report.storage = {keyCount:stored.keyCount,byteLength:stored.meta.byteLength,chunkBytes:stored.meta.chunkBytes,chunkCount:stored.meta.chunkCount};
  report.restartFramebuffer = report.secondFrame.width === 480 && report.secondFrame.height === 800;
  report.inputAcceptedAfterRestart = true;
  report.errors = errors;
  report.blockedExternalOrigins = [];
  const out = resolve(process.env.SIMULATOR_TEST_OUT);
  await mkdir(out,{recursive:true});
  await writeFile(join(out,"result.json"),JSON.stringify(report,null,2)+"\n");
  console.log(JSON.stringify(report,null,2));
} finally {
  await context.close();
  await browser.close();
}
