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
