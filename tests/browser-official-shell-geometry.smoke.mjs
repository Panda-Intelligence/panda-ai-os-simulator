import assert from "node:assert/strict";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

for (const key of ["PLAYWRIGHT_MODULE","SIMULATOR_TEST_URL","CHROME_EXECUTABLE"]) {
  if (!process.env[key]) throw new Error(`Explicit ${key} is required`);
}
const mod = await import(pathToFileURL(resolve(process.env.PLAYWRIGHT_MODULE)).href);
const chromium = mod.chromium ?? mod.default?.chromium;
if (!chromium) throw new Error("PLAYWRIGHT_MODULE does not expose chromium");
const url = process.env.SIMULATOR_TEST_URL;
const base = new URL(url);
assert.ok(["127.0.0.1","localhost"].includes(base.hostname));

const browser = await chromium.launch({headless:true, executablePath:process.env.CHROME_EXECUTABLE});
const context = await browser.newContext({viewport:{width:1440,height:1000},serviceWorkers:"block"});
const page = await context.newPage();
const errors = [];
page.on("pageerror", error => errors.push(String(error)));

const expected = {
  m5papers3: {
    bodyRatio: 67.7 / 121.5,
    bodySize: "121.5×67.7×7.7mm",
    hangingEar: 1,
    controls: 1,
  },
  "lilygo-t5s3-pro": {
    bodyRatio: 69 / 129,
    bodySize: "129×69×11mm",
    homeRing: 1,
    controls: 4,
  },
};

try {
  await page.goto(url,{waitUntil:"domcontentloaded"});
  const results = {};
  for (const [boardId, spec] of Object.entries(expected)) {
    await page.locator('select[name="simulatorBoard"]').selectOption(boardId);
    await page.waitForFunction(id => document.querySelector(".panel-shell")?.getAttribute("data-board") === id, boardId);
    const geometry = await page.locator(".panel-shell").evaluate((shell) => {
      const body = shell.getBoundingClientRect();
      const screen = shell.querySelector(".panel-screen-frame").getBoundingClientRect();
      return {
        bodyWidth: body.width,
        bodyHeight: body.height,
        bodyRatio: body.width / body.height,
        screenWidthRatio: screen.width / body.width,
        screenHeightRatio: screen.height / body.height,
        bodySize: shell.getAttribute("data-physical-size"),
      };
    });
    assert.ok(Math.abs(geometry.bodyRatio - spec.bodyRatio) < 0.008, JSON.stringify({boardId,geometry,spec}));
    assert.equal(geometry.bodySize, spec.bodySize);
    assert.ok(geometry.screenWidthRatio > 0.8 && geometry.screenWidthRatio < 0.9);
    assert.ok(geometry.screenHeightRatio > 0.79 && geometry.screenHeightRatio < 0.88);
    assert.equal(await page.locator(".panel-physical-key").count(), spec.controls);
    if (spec.hangingEar) assert.equal(await page.locator(".panel-hanging-ear").count(), spec.hangingEar);
    if (spec.homeRing) assert.equal(await page.locator(".panel-front-home-ring").count(), spec.homeRing);
    results[boardId] = geometry;
  }
  assert.deepEqual(errors,[]);
  console.log(JSON.stringify({passed:true,results,errors},null,2));
} finally {
  await context.close();
  await browser.close();
}
