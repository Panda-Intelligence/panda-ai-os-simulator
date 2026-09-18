#!/usr/bin/env node

import assert from "node:assert/strict";

const logicalWidth = 480;
const logicalHeight = 800;
const panelWidth = 800;
const panelHeight = 480;
const logicalStride = logicalWidth / 8;
const panelStride = panelWidth / 8;

function makeWhite1bpp(width, height) {
  return new Uint8Array((width * height) / 8).fill(0xff);
}

function getBit(buffer, stride, x, y) {
  const byteIdx = y * stride + (x >> 3);
  const bit = 7 - (x & 7);
  return (buffer[byteIdx] >> bit) & 1;
}

function setBit(buffer, stride, x, y, white) {
  const byteIdx = y * stride + (x >> 3);
  const mask = 0x80 >> (x & 7);
  if (white) {
    buffer[byteIdx] |= mask;
  } else {
    buffer[byteIdx] &= ~mask;
  }
}

function copyMurphyLogicalFramebufferToPanelRam(logical) {
  const panel = makeWhite1bpp(panelWidth, panelHeight);
  for (let y = 0; y < logicalHeight; y++) {
    for (let x = 0; x < logicalWidth; x++) {
      const white = getBit(logical, logicalStride, x, y) !== 0;
      setBit(panel, panelStride, y, x, white);
    }
  }
  return panel;
}

function pushFramebuffer(panelRam) {
  const flipped = new Uint8Array(panelRam.length);
  for (let row = 0; row < panelHeight; row++) {
    const srcStart = (panelHeight - 1 - row) * panelStride;
    flipped.set(panelRam.subarray(srcStart, srcStart + panelStride), row * panelStride);
  }
  return flipped;
}

function oneBitToRgba(oneBitFramebuffer, width, height) {
  const rgba = new Uint8Array(width * height * 4);
  const pixels = width * height;
  for (let pixel = 0; pixel < pixels; pixel++) {
    const byte = oneBitFramebuffer[pixel >> 3] ?? 0xff;
    const bit = 7 - (pixel & 7);
    const white = (byte >> bit) & 1;
    const value = white ? 255 : 16;
    const idx = pixel * 4;
    rgba[idx] = value;
    rgba[idx + 1] = value;
    rgba[idx + 2] = value;
    rgba[idx + 3] = 255;
  }
  return rgba;
}

function rotate90CW(src, srcW, srcH) {
  const dst = new Uint8Array(src.length);
  const dstW = srcH;
  for (let dy = 0; dy < srcW; dy++) {
    for (let dx = 0; dx < dstW; dx++) {
      const sx = dy;
      const sy = srcH - 1 - dx;
      const srcIdx = (sy * srcW + sx) * 4;
      const dstIdx = (dy * dstW + dx) * 4;
      dst[dstIdx] = src[srcIdx];
      dst[dstIdx + 1] = src[srcIdx + 1];
      dst[dstIdx + 2] = src[srcIdx + 2];
      dst[dstIdx + 3] = src[srcIdx + 3];
    }
  }
  return dst;
}

function pixelValue(rgba, width, x, y) {
  return rgba[(y * width + x) * 4];
}

const logical = makeWhite1bpp(logicalWidth, logicalHeight);
const blackPixels = [
  [0, 0],
  [12, 34],
  [123, 456],
  [479, 799],
];

for (const [x, y] of blackPixels) {
  setBit(logical, logicalStride, x, y, false);
}
for (let y = 90; y < 98; y++) {
  for (let x = 200; x < 214; x++) {
    setBit(logical, logicalStride, x, y, false);
  }
}

const panelRam = copyMurphyLogicalFramebufferToPanelRam(logical);
const rawPanelPayload = pushFramebuffer(panelRam);
const rgbaRaw = oneBitToRgba(rawPanelPayload, panelWidth, panelHeight);
const canvas = rotate90CW(rgbaRaw, panelWidth, panelHeight);

for (const [x, y] of blackPixels) {
  assert.equal(pixelValue(canvas, logicalWidth, x, y), 16, `expected black pixel at logical ${x},${y}`);
}
assert.equal(pixelValue(canvas, logicalWidth, 199, 90), 255, "left edge outside rectangle should stay white");
assert.equal(pixelValue(canvas, logicalWidth, 200, 90), 16, "rectangle left edge should be black");
assert.equal(pixelValue(canvas, logicalWidth, 213, 97), 16, "rectangle lower-right should be black");
assert.equal(pixelValue(canvas, logicalWidth, 214, 97), 255, "right edge outside rectangle should stay white");

console.log("simulator framebuffer contract ok");
