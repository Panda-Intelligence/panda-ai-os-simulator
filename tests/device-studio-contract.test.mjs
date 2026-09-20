import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const boards = JSON.parse(await readFile(new URL("../boards.json", import.meta.url), "utf8"));
const boardSource = await readFile(new URL("../src/boards.ts", import.meta.url), "utf8");
const paneSource = await readFile(new URL("../src/components/SimulatorDevicePane.tsx", import.meta.url), "utf8");
const panelSource = await readFile(new URL("../src/components/PanelCanvas.tsx", import.meta.url), "utf8");
const runtimeSource = await readFile(new URL("../src/browserSimulatorRuntime.ts", import.meta.url), "utf8");

test("every declared board has an explicit physical visual profile", () => {
  for (const board of boards.boards) {
    assert.ok(boardSource.includes(board.id), "missing visual metadata for " + board.id);
  }
  assert.match(panelSource,/panel-physical-key/);
  assert.match(panelSource,/getSimulatorBoardVisual/);
});

test("device studio exposes board-filtered firmware selection", () => {
  assert.match(paneSource,/name="simulatorFirmware"/);
  assert.match(paneSource,/firmwareOptions\.find/);
  assert.match(runtimeSource,/listFirmwareOptions\(boardId/);
  assert.match(runtimeSource,/firmwareArtifacts\?\.\[boardId\]/);
});

test("virtual SD UI supports drag-drop and browser-side file management", () => {
  assert.match(paneSource,/ide-sdcard__dropzone/);
  assert.match(paneSource,/event\.dataTransfer\.files/);
  assert.match(paneSource,/writeSdFile\(board\.id/);
  assert.match(paneSource,/deleteSdPath\(board\.id/);
});

test("workspace distributes primary controls across top, left navigation and right inspector", () => {
  assert.match(paneSource,/ide-titlebar__selectors/);
  assert.match(paneSource,/ide-sidebar--boards/);
  assert.match(paneSource,/ide-inspector/);
  assert.match(paneSource,/ide-inspector__group--sd/);
  const topBoard = paneSource.indexOf('className="pds-select ide-toolbar-select ide-toolbar-select--board"');
  const topFirmware = paneSource.indexOf('className="pds-select ide-toolbar-select ide-toolbar-select--firmware"');
  const inspector = paneSource.indexOf('className="ide-inspector"');
  assert.ok(topBoard > 0 && topFirmware > 0 && inspector > 0);
});

test("official-reference skins carry public physical dimensions and structures", () => {
  assert.match(boardSource,/121\.5/);
  assert.match(boardSource,/67\.7/);
  assert.match(boardSource,/7\.7/);
  assert.match(boardSource,/129/);
  assert.match(boardSource,/69/);
  assert.match(boardSource,/11/);
  assert.match(boardSource,/docs\.m5stack\.com\/en\/core\/PaperS3/);
  assert.match(boardSource,/wiki\.lilygo\.cc\/products\/t5-series\/t5-e-paper-s3-pro/);
  assert.match(panelSource,/panel-hanging-ear/);
  assert.match(panelSource,/panel-front-home-ring/);
  assert.match(panelSource,/RST/);
  assert.match(panelSource,/screenDiagonalMm/);
  assert.match(panelSource,/screenWidthRatio/);
  assert.match(panelSource,/screenHeightRatio/);
  assert.match(panelSource,/--panel-screen-width-ratio/);
  assert.match(panelSource,/--panel-screen-height-ratio/);
});
