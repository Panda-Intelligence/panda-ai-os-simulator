import test from "node:test";
import assert from "node:assert/strict";

import { MAX_BYTES, validateSdImageBytes, writeSdImage } from "../public/simulator-runtime/sd-card-store.js";

function makeFat12Image(totalSectors = 300) {
  const bytes = new Uint8Array(totalSectors * 512);
  const view = new DataView(bytes.buffer);
  bytes.set([0xeb, 0x3c, 0x90], 0);
  view.setUint16(11, 512, true);
  bytes[13] = 1;
  view.setUint16(14, 1, true);
  bytes[16] = 1;
  view.setUint16(17, 16, true);
  view.setUint16(19, totalSectors, true);
  bytes[21] = 0xf8;
  view.setUint16(22, 1, true);
  bytes[510] = 0x55;
  bytes[511] = 0xaa;
  bytes.set([0xf8, 0xff, 0xff], 512);
  return bytes;
}

test("validates bounded raw FAT image geometry", () => {
  const image = makeFat12Image();
  const shape = validateSdImageBytes(image, image.byteLength);
  assert.equal(shape.fatType, 12);
  assert.equal(shape.sectorSize, 512);
  assert.throws(() => validateSdImageBytes(image, image.byteLength + 512), /browser_sd_image_size_mismatch/);
  assert.throws(() => validateSdImageBytes(new Uint8Array(512), 512), /browser_sd_fat_sector_size_invalid/);
  assert.throws(() => validateSdImageBytes(new Uint8Array(MAX_BYTES + 1)), /browser_sd_image_size_invalid/);
});

test("rejects a missing FAT boot signature before replacement", () => {
  const image = makeFat12Image();
  image[510] = 0;
  assert.throws(() => validateSdImageBytes(image), /browser_sd_fat_signature_invalid/);
});

test("write rejects corrupt bytes before opening storage", async () => {
  const image = makeFat12Image();
  image[510] = 0;
  let opened = false;
  await assert.rejects(
    writeSdImage(() => {
      opened = true;
      throw new Error("storage should not be opened");
    }, "images", "board", { bytes: image, byteLength: image.byteLength }),
    /browser_sd_fat_signature_invalid/,
  );
  assert.equal(opened, false);
});

test("write rejects a mismatched transfer before storage mutation", async () => {
  const image = makeFat12Image();
  let opened = false;
  await assert.rejects(
    writeSdImage(() => {
      opened = true;
      throw new Error("storage should not be opened");
    }, "images", "board", { bytes: image, byteLength: image.byteLength + 512 }),
    /browser_sd_write_size_invalid/,
  );
  assert.equal(opened, false);
});
