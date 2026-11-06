import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import ts from "typescript";

const root = new URL("../", import.meta.url);
const source = readFileSync(new URL("src/panelScale.ts", root), "utf8");
const javascript = ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ES2022 } }).outputText;
const { snappedPanelCssSize } = await import("data:text/javascript;base64," + Buffer.from(javascript).toString("base64"));
const mofei = { outputWidth: 480, outputHeight: 800 };

test("a Retina frame near one device pixel per panel pixel snaps to an exact 1:1 CSS size", () => {
  // 236x394 CSS px at DPR 2 = 472x788 device px for a 480x800 panel: 0.983, within tolerance → 240x400 CSS px.
  assert.deepEqual(snappedPanelCssSize({ width: 236, height: 394 }, mofei, 2), null, "cannot grow past the frame");
  assert.deepEqual(snappedPanelCssSize({ width: 250, height: 420 }, mofei, 2), { width: 240, height: 400 });
  assert.deepEqual(snappedPanelCssSize({ width: 240, height: 400 }, mofei, 2), { width: 240, height: 400 });
});

test("frames far from an integer scale keep filling the frame (filtered scaling)", () => {
  assert.equal(snappedPanelCssSize({ width: 247, height: 407 }, mofei, 1), null); // 0.51x on a DPR 1 screen
  assert.equal(snappedPanelCssSize({ width: 330, height: 550 }, mofei, 2), null); // 1.375x
  assert.equal(snappedPanelCssSize({ width: 120, height: 200 }, mofei, 1), null); // below 1x: never upscale to 1
});

test("integer multiples above one snap too and the limiting axis decides", () => {
  assert.deepEqual(snappedPanelCssSize({ width: 990, height: 1700 }, mofei, 1), { width: 960, height: 1600 });
  assert.deepEqual(snappedPanelCssSize({ width: 1000, height: 1610 }, mofei, 1), { width: 960, height: 1600 });
  assert.equal(snappedPanelCssSize({ width: 0, height: 0 }, mofei, 2), null);
});
