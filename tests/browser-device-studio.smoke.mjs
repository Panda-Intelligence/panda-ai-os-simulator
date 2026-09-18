import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

for (const key of ["PLAYWRIGHT_MODULE","SIMULATOR_TEST_URL","SIMULATOR_TEST_OUT","CHROME_EXECUTABLE"]) {
  if (!process.env[key]) throw new Error(`Explicit ${key} is required`);
}
const mod = await import(pathToFileURL(resolve(process.env.PLAYWRIGHT_MODULE)).href);
const chromium = mod.chromium ?? mod.default?.chromium;
if (!chromium) throw new Error("PLAYWRIGHT_MODULE does not expose chromium");
const base = new URL(process.env.SIMULATOR_TEST_URL);
assert.ok(["127.0.0.1","localhost"].includes(base.hostname));
const browser = await chromium.launch({headless:true,executablePath:process.env.CHROME_EXECUTABLE});
const context = await browser.newContext({viewport:{width:1440,height:1100},serviceWorkers:"block"});
const page = await context.newPage();
const errors = [], external = [];
page.on("pageerror", error => errors.push(String(error)));
await context.route("**/*", async route => {
  const url = new URL(route.request().url());
  if (["http:","https:"].includes(url.protocol) && url.origin !== base.origin) {
    external.push(url.origin);
    await route.abort();
    return;
  }
  await route.continue();
});

const report = {};
try {
  await page.goto(base.href,{waitUntil:"networkidle"});
  assert.equal(await page.evaluate(() => crossOriginIsolated), true);
  const cards = page.locator(".ide-board-card");
  assert.equal(await cards.count(), 6);

  const firmware = page.locator('select[name="simulatorFirmware"]');
  await firmware.waitFor({state:"visible",timeout:10000});
  assert.ok((await firmware.locator("option").count()) >= 1);
  report.mofeiFirmware = await firmware.inputValue();

  const board = page.locator('select[name="simulatorBoard"]');
  await board.selectOption("m5papers3");
  await page.waitForFunction(() => document.querySelector(".panel-shell")?.getAttribute("data-board") === "m5papers3");
  await page.waitForFunction(() => document.querySelector('select[name="simulatorFirmware"]')?.value.includes("m5papers3"));
  assert.equal(await page.locator(".panel-physical-key").count(), 1);
  report.paperS3Firmware = await firmware.inputValue();
  await board.selectOption("lilygo-t5s3-pro");
  await page.waitForFunction(() => document.querySelector(".panel-shell")?.getAttribute("data-board") === "lilygo-t5s3-pro");
  await page.waitForFunction(() => document.querySelector('select[name="simulatorFirmware"]')?.value.includes("lilygo-t5s3-pro"));
  assert.equal(await page.locator(".panel-physical-key").count(), 3);
  report.lilygoFirmware = await firmware.inputValue();
  const physicalKey = page.locator(".panel-physical-key").first();
  const box = await physicalKey.boundingBox();
  assert.ok(box);
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
  const pressedTransform = await physicalKey.evaluate((node) => getComputedStyle(node).transform);
  await page.mouse.up();
  assert.notEqual(pressedTransform, "none");
  report.physicalKeyPressedEffect = true;

  await board.selectOption("mofei");
  await page.waitForFunction(() => document.querySelector(".panel-shell")?.getAttribute("data-board") === "mofei");
  const dropzone = page.locator(".ide-sdcard__dropzone");
  await dropzone.waitFor({state:"visible",timeout:10000});

  await page.evaluate(() => {
    const dropzone = document.querySelector(".ide-sdcard__dropzone");
    if (!dropzone) throw new Error("missing SD dropzone");
    const dataTransfer = new DataTransfer();
    dataTransfer.items.add(new File(["Panda Simulator drag upload\n"], "browser-drop.txt", {type:"text/plain"}));
    dropzone.dispatchEvent(new DragEvent("dragenter", {bubbles:true,cancelable:true,dataTransfer}));
    dropzone.dispatchEvent(new DragEvent("drop", {bubbles:true,cancelable:true,dataTransfer}));
  });
  await page.getByText("browser-drop.txt",{exact:true}).waitFor({state:"visible",timeout:15000});
  assert.ok((await page.locator(".ide-sdcard__entry-size").allTextContents()).some(value => value.includes("B")));
  report.sdDragDropVisible = true;
  assert.equal(errors.length,0,errors.join("\n"));
  assert.deepEqual([...new Set(external)],[]);
  report.boardCards = await cards.count();
  report.crossOriginIsolated = true;
  report.physicalShell = true;
  report.blockedExternalOrigins = [];
  report.errors = [];
  report.passed = true;
  await mkdir(resolve(process.env.SIMULATOR_TEST_OUT),{recursive:true});
  await writeFile(resolve(process.env.SIMULATOR_TEST_OUT,"result.json"),JSON.stringify(report,null,2)+"\n");
  console.log(JSON.stringify(report,null,2));
} finally {
  await context.close();
  await browser.close();
}
