import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { runInNewContext } from "node:vm";
import ts from "typescript";

const boards = JSON.parse(await readFile(new URL("../boards.json", import.meta.url), "utf8"));
const boardSource = await readFile(new URL("../src/boards.ts", import.meta.url), "utf8");
const paneSource = await readFile(new URL("../src/components/SimulatorDevicePane.tsx", import.meta.url), "utf8");
const panelSource = await readFile(new URL("../src/components/PanelCanvas.tsx", import.meta.url), "utf8");
const runtimeSource = await readFile(new URL("../src/browserSimulatorRuntime.ts", import.meta.url), "utf8");
const workerSource = await readFile(new URL("../src/browserSimulatorRuntimeWorker.js", import.meta.url), "utf8");
const i18nSource = await readFile(new URL("../src/i18n.ts", import.meta.url), "utf8");

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

test("English is the first locale and the language switch lives in the title bar", () => {
  assert.match(i18nSource, /DEFAULT_SIMULATOR_LOCALE: SimulatorLocale = "en"/);
  assert.match(i18nSource, /if \(isSimulatorLocale\(saved\)\) return saved;\s+return DEFAULT_SIMULATOR_LOCALE/);
  const titlebarActions = paneSource.indexOf('className="ide-titlebar__actions"');
  const languageSwitch = paneSource.indexOf('className="pds-select ide-titlebar__language-select"');
  const inspector = paneSource.indexOf('className="ide-inspector"');
  assert.ok(titlebarActions >= 0 && languageSwitch > titlebarActions && languageSwitch < inspector);
  assert.doesNotMatch(paneSource.slice(inspector), /name="simulatorLocale"/);
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

test("official-reference skins carry public physical dimensions and structures", async () => {
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
  assert.match(panelSource,/getPhysicalControls/);
  const controlSource = await readFile(new URL("../src/boardPhysicalControls.ts", import.meta.url), "utf8");
  assert.match(controlSource,/name:"RST"/);
  assert.match(panelSource,/screenDiagonalMm/);
  assert.match(panelSource,/screenWidthRatio/);
  assert.match(panelSource,/screenHeightRatio/);
  assert.match(panelSource,/--panel-screen-width-ratio/);
  assert.match(panelSource,/--panel-screen-height-ratio/);
});

test("all 11 hardware boards are declared in boards.json, boards.ts, and boardPhysicalControls.ts", async () => {
  const expectedBoardIds = [
    "mofei",
    "s3r8",
    "s3wroom",
    "s37uc",
    "m5papers3",
    "lilygo-t5s3-pro",
    "read-pico",
    "molink",
    "x4pro",
    "metalio-eink4",
    "eego-a4"
  ];
  assert.deepEqual(
    boards.boards.map((b) => b.id),
    expectedBoardIds
  );

  const controlSource = await readFile(new URL("../src/boardPhysicalControls.ts", import.meta.url), "utf8");
  const transpiledControls = ts.transpileModule(controlSource, {
    compilerOptions: {
      module: ts.ModuleKind.CommonJS,
      target: ts.ScriptTarget.ES2022
    }
  }).outputText;
  const controlExports = {};
  const controlMod = { exports: controlExports };
  runInNewContext(transpiledControls, {
    exports: controlExports,
    module: controlMod
  });

  const newBoardIds = ["read-pico", "molink", "x4pro", "metalio-eink4", "eego-a4"];
  for (const id of newBoardIds) {
    assert.ok(controlMod.exports.RAW_CONTROL_PROFILES[id], `missing RAW_CONTROL_PROFILES[${id}]`);
    assert.ok(controlMod.exports.RAW_CONTROL_PROFILES[id].controls.length > 0, `empty RAW_CONTROL_PROFILES[${id}]`);
  }
  for (const calibratedId of ["m5papers3", "lilygo-t5s3-pro"]) {
    const board = boards.boards.find((b) => b.id === calibratedId);
    assert.ok(controlMod.exports.getPhysicalControls(board)?.length > 0);
  }
});

test("runtime worker handles read-pico/eego-a4 slicing, molink/x4pro SSD1677 unscrambling, and x4pro touch swap", () => {
  const postedMessages = [];
  const touchCalls = [];
  const postMessage = (msg) => {
    postedMessages.push(msg);
  };
  const btoa = (str) => Buffer.from(str, "binary").toString("base64");
  const atob = (b64) => Buffer.from(b64, "base64").toString("binary");
  const context = {
    postMessage,
    btoa,
    atob,
    addEventListener() {},
    self: {
      postMessage,
      btoa,
      atob,
      addEventListener() {}
    },
    Uint8Array,
    Uint8ClampedArray,
    Int32Array,
    ArrayBuffer,
    SharedArrayBuffer: typeof SharedArrayBuffer !== "undefined" ? SharedArrayBuffer : ArrayBuffer,
    DataView,
    TextDecoder,
    TextEncoder,
    console,
    Math,
    Date,
    Number,
    String,
    Boolean,
    Object,
    Array,
    Map,
    Set,
    Promise,
    setTimeout,
    clearTimeout
  };

  const executableWorker =
    workerSource.replace(/^import[^\n]+\n/gm, "") +
    `\nself.__test = {
      qemuSimBoardFor,
      framebufferProfile,
      unscrambleSsd1677WireToGuestBytes,
      emitFramebuffer,
      injectTouch,
      setLastBoardId(id) { lastBoardId = id; },
      setQemuInstance(inst) { qemuInstance = inst; }
    };`;
  runInNewContext(executableWorker, context);
  const workerTest = context.self.__test;

  assert.equal(workerTest.qemuSimBoardFor("read-pico"), "lilygo-t5s3-pro");
  assert.equal(workerTest.qemuSimBoardFor("eego-a4"), "lilygo-t5s3-pro");
  assert.equal(workerTest.qemuSimBoardFor("molink"), "molink");
  assert.equal(workerTest.qemuSimBoardFor("x4pro"), "x4pro");
  assert.equal(workerTest.qemuSimBoardFor("metalio-eink4"), "metalio-eink4");

  // 1. Direct framebuffer slicing for read-pico (684x1216 -> 104448 bytes) and eego-a4 (552x768 -> 52992 bytes)
  const fullDirectBuffer = new Uint8Array(259200);
  fullDirectBuffer.fill(0xff);
  fullDirectBuffer[0] = 0x00; // first 8 pixels black (value 16)

  workerTest.setLastBoardId("read-pico");
  postedMessages.length = 0;
  workerTest.emitFramebuffer(0, fullDirectBuffer);
  assert.equal(postedMessages.length, 1);
  assert.equal(postedMessages[0].payload.width, 684);
  assert.equal(postedMessages[0].payload.height, 1216);
  const readPicoRgba = Buffer.from(postedMessages[0].payload.rgba_b64, "base64");
  assert.equal(readPicoRgba.length, 684 * 1216 * 4);
  assert.equal(readPicoRgba[0], 16);

  workerTest.setLastBoardId("eego-a4");
  postedMessages.length = 0;
  workerTest.emitFramebuffer(0, fullDirectBuffer);
  assert.equal(postedMessages.length, 1);
  assert.equal(postedMessages[0].payload.width, 552);
  assert.equal(postedMessages[0].payload.height, 768);
  const eegoRgba = Buffer.from(postedMessages[0].payload.rgba_b64, "base64");
  assert.equal(eegoRgba.length, 552 * 768 * 4);
  assert.equal(eegoRgba[0], 16);

  // 2. SSD1677 wire unscramble for molink & x4pro
  const guestBytes = new Uint8Array(48000);
  for (let i = 0; i < guestBytes.length; i += 1) {
    guestBytes[i] = (i * 73 + 19) & 0xff;
  }
  const wireBytes = new Uint8Array(48000);
  wireBytes.fill(0xff);
  for (let y = 0; y < 800; y += 1) {
    const srcRowOffset = y * 60;
    const dstColByte = y >> 3;
    const dstMask = 0x80 >> (y & 7);
    for (let x = 0; x < 480; x += 1) {
      const srcByte = guestBytes[srcRowOffset + (x >> 3)];
      if ((srcByte & (0x80 >> (x & 7))) === 0) {
        const dstIndex = (479 - x) * 100 + dstColByte;
        wireBytes[dstIndex] &= ~dstMask;
      }
    }
  }

  const recoveredGuest = workerTest.unscrambleSsd1677WireToGuestBytes(wireBytes);
  assert.deepEqual(Array.from(recoveredGuest), Array.from(guestBytes));

  for (const landscapeBoard of ["molink", "x4pro"]) {
    workerTest.setLastBoardId(landscapeBoard);
    postedMessages.length = 0;
    workerTest.emitFramebuffer(0, wireBytes);
    assert.equal(postedMessages.length, 1);
    assert.equal(postedMessages[0].payload.width, 800);
    assert.equal(postedMessages[0].payload.height, 480);
    const rgba = Buffer.from(postedMessages[0].payload.rgba_b64, "base64");
    assert.equal(rgba.length, 800 * 480 * 4);
  }

  // 3. Touch coordinate swap + bit 7 flag for x4pro
  workerTest.setQemuInstance({
    _mofei_wasm_inject_touch(action, x, y, fingerId) {
      touchCalls.push({ action, x, y, fingerId });
    }
  });
  workerTest.setLastBoardId("x4pro");
  workerTest.injectTouch({ action: 1, x: 640, y: 340, fingerId: 2 });
  assert.equal(touchCalls.length, 1);
  assert.deepEqual(touchCalls[0], {
    action: 1,
    x: 340,
    y: 640,
    fingerId: 0x82
  });
});

test("PanelCanvas transforms framebuffers only when output dimensions are transposed from framebuffer dimensions", () => {
  const transpiledPanel = ts.transpileModule(panelSource, {
    compilerOptions: {
      module: ts.ModuleKind.CommonJS,
      target: ts.ScriptTarget.ES2022,
      jsx: ts.JsxEmit.ReactJSX
    }
  }).outputText;

  const panelExports = {};
  const panelMod = { exports: panelExports };
  runInNewContext(transpiledPanel, {
    exports: panelExports,
    module: panelMod,
    Uint8Array,
    require(id) {
      if (id.includes("react")) {
        return {
          useEffect() {},
          useCallback(fn) { return fn; },
          useMemo(fn) { return fn(); },
          useRef() { return { current: null }; },
          useState(init) { return [init, () => {}]; },
          jsx() { return null; },
          jsxs() { return null; }
        };
      }
      return {};
    }
  });

  const { shouldRotate90Clockwise, transformFramebuffer } = panelMod.exports;
  const boardById = Object.fromEntries(boards.boards.map((b) => [b.id, b]));

  // Rotated portrait boards (fb 800x480 -> output 480x800)
  assert.equal(shouldRotate90Clockwise({ width: 800, height: 480 }, boardById["mofei"]), true);
  assert.equal(shouldRotate90Clockwise({ width: 800, height: 480 }, boardById["metalio-eink4"]), true);

  // Native landscape boards (fb 800x480 -> output 800x480)
  assert.equal(shouldRotate90Clockwise({ width: 800, height: 480 }, boardById["molink"]), false);
  assert.equal(shouldRotate90Clockwise({ width: 800, height: 480 }, boardById["x4pro"]), false);

  // Native portrait boards (fb == output)
  assert.equal(shouldRotate90Clockwise({ width: 684, height: 1216 }, boardById["read-pico"]), false);
  assert.equal(shouldRotate90Clockwise({ width: 552, height: 768 }, boardById["eego-a4"]), false);

  const sampleRgba = new Uint8Array(800 * 480 * 4);
  sampleRgba[0] = 123;
  const molinkTransformed = transformFramebuffer(sampleRgba, 800, 480, boardById["molink"]);
  assert.equal(molinkTransformed, sampleRgba);

  const metalioTransformed = transformFramebuffer(sampleRgba, 800, 480, boardById["metalio-eink4"]);
  assert.ok(metalioTransformed instanceof Uint8Array);
  assert.notEqual(metalioTransformed, sampleRgba);
  assert.equal(metalioTransformed.length, 480 * 800 * 4);
});

test("SimulatorDevicePane supports live board switching while running", () => {
  assert.match(paneSource, /const \[startPending, setStartPending\] = useState\(false\)/);
  assert.match(paneSource, /const wasRunning = running;/);
  assert.match(paneSource, /await hostBridge\.stopSim\(\)/);
  assert.match(paneSource, /await hostBridge\.startSim\(nextBoard\.id, nextFirmwarePath, createSimulatedLocation\(\)\)/);
  assert.doesNotMatch(paneSource, /name="simulatorBoard"[\s\S]{0,120}disabled=\{running\}/);
});
