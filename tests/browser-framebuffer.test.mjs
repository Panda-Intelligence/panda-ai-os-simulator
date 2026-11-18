import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { runInNewContext } from "node:vm";

const workerSource = readFileSync(new URL("../src/browserSimulatorRuntimeWorker.js", import.meta.url), "utf8")
  .replace(/^import[^\n]+\n/gm, "");

function framebufferWorker() {
  const messages = [];
  const send = runInNewContext(
    `${workerSource}
    (bytes, flags, boardId) => {
      lastBoardId = boardId;
      handleQemuIpc({ HEAPU8: bytes }, 0x01, flags, 0, bytes.length);
    }
    `,
    { addEventListener() {}, postMessage: (message) => messages.push(message), btoa },
  );
  assert.equal(messages.shift().type, "ready");
  return (bytes, flags, boardId = "mofei") => {
    send(bytes, flags, boardId);
    assert.equal(messages.length, 1);
    return messages.shift();
  };
}

function checkFrame(message, { kind, width, height, levels }) {
  assert.equal(message.type, "framebuffer");
  const payload = message.payload;
  assert.equal(payload.kind, kind);
  assert.equal(payload.width, width);
  assert.equal(payload.height, height);
  const rgba = Buffer.from(payload.rgba_b64, "base64");
  assert.equal(rgba.length, width * height * 4);
  assert.deepEqual([...rgba.subarray(0, levels.length * 4)], levels.flatMap(value => [value, value, value, 255]));
}

test("browser IPC keeps Gray16 local to one frame and preserves the partial flag", () => {
  const send = framebufferWorker();
  const gray = new Uint8Array(800 * 480 / 2);
  gray.set([0x05, 0xAF]);
  const mono = new Uint8Array(800 * 480 / 8);
  mono[0] = 0xAA;

  checkFrame(send(gray, 0x03), { kind: "partial", width: 800, height: 480, levels: [255, 170, 85, 0] });
  const shortFrame = send(mono, 0x02);
  assert.equal(shortFrame.type, "simulatorError");
  assert.match(shortFrame.payload, /dropped gray16 framebuffer payload/);
  assert.equal(send(gray, 0x00).type, "simulatorError");
  checkFrame(send(mono, 0x00), { kind: "full", width: 800, height: 480, levels: [255, 16, 255, 16] });
});

test("browser IPC still decodes unflagged Gray16 frames for Gray16 boards", () => {
  const send = framebufferWorker();
  const gray = new Uint8Array(540 * 960 / 2);
  gray.set([0x05, 0xAF]);
  checkFrame(send(gray, 0x00, "lilygo-t5s3-pro"), {
    kind: "full", width: 540, height: 960, levels: [255, 170, 85, 0],
  });
});
