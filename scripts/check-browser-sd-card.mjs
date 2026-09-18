#!/usr/bin/env node

import assert from "node:assert/strict";
import { existsSync, readFileSync, readdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { gunzipSync } from "node:zlib";
import createFatFs, * as FatFs from "js-fatfs";

const runtimeDir = resolve(dirname(fileURLToPath(import.meta.url)), "../../panda-cloud/public/simulator/app/simulator-runtime/qemu");
const defaultImagePath = resolve(runtimeDir, "sdcard.img.gz");
const imagePaths = process.argv.length > 2
  ? process.argv.slice(2).map((path) => resolve(path))
  : existsSync(defaultImagePath)
    ? [defaultImagePath]
    : readdirSync(runtimeDir)
        .filter((name) => /^sdcard\.img\.gz\.part\d+$/.test(name))
        .sort()
        .map((name) => resolve(runtimeDir, name));
if (imagePaths.length === 0) {
  throw new Error(`Browser SD image is missing from ${runtimeDir}`);
}
const sectorSizeFallback = 512;
const fileChunkBytes = 64 * 1024;
// 浏览器介质只保留规范默认载荷中的阅读器通用回退字体；完整 UI 字体保留在发行介质。
const browserFallbackFontPacks = [
  "notosans_tc_16_japanese_common.mfp",
  "notosans_tc_20_japanese_common.mfp",
  "notosans_tc_24_japanese_common.mfp",
  "notosans_tc_26_japanese_common.mfp",
  "notosans_tc_32_japanese_common.mfp",
];

class RawSdImageDisk {
  constructor(image) {
    this.image = image;
    const sectorSize = image.length >= 13 ? image[11] | (image[12] << 8) : sectorSizeFallback;
    this.sectorSize = sectorSize > 0 ? sectorSize : sectorSizeFallback;
  }

  initialize() {
    return FatFs.RES_OK;
  }

  status() {
    return FatFs.RES_OK;
  }

  read(ff, _pdrv, buff, sector, count) {
    const offset = sector * this.sectorSize;
    const byteLength = count * this.sectorSize;
    if (offset < 0 || offset + byteLength > this.image.byteLength) {
      return FatFs.RES_PARERR;
    }
    ff.HEAPU8.set(this.image.subarray(offset, offset + byteLength), buff);
    return FatFs.RES_OK;
  }

  write(ff, _pdrv, buff, sector, count) {
    const offset = sector * this.sectorSize;
    const byteLength = count * this.sectorSize;
    if (offset < 0 || offset + byteLength > this.image.byteLength) {
      return FatFs.RES_PARERR;
    }
    this.image.set(ff.HEAPU8.subarray(buff, buff + byteLength), offset);
    return FatFs.RES_OK;
  }

  ioctl(ff, _pdrv, cmd, buff) {
    switch (cmd) {
      case FatFs.CTRL_SYNC:
        return FatFs.RES_OK;
      case FatFs.GET_SECTOR_COUNT:
        ff.setValue(buff, Math.floor(this.image.byteLength / this.sectorSize), "i32");
        return FatFs.RES_OK;
      case FatFs.GET_SECTOR_SIZE:
        ff.setValue(buff, this.sectorSize, "i16");
        return FatFs.RES_OK;
      case FatFs.GET_BLOCK_SIZE:
        ff.setValue(buff, 1, "i32");
        return FatFs.RES_OK;
      default:
        return FatFs.RES_PARERR;
    }
  }
}

function checkFatResult(code, label) {
  if (code !== FatFs.FR_OK) {
    throw new Error(`FatFs error ${code}: ${label}`);
  }
}

function statPath(ff, path) {
  const info = ff.malloc(FatFs.sizeof_FILINFO);
  try {
    checkFatResult(ff.f_stat(path, info), `stat ${path}`);
    return {
      path,
      size: ff.FILINFO_fsize(info),
      attributes: ff.FILINFO_fattrib(info),
    };
  } finally {
    ff.free(info);
  }
}

function readFile(ff, path) {
  const info = statPath(ff, path);
  const file = ff.malloc(FatFs.sizeof_FIL);
  const readPtr = ff.malloc(4);
  const bufferPtr = ff.malloc(fileChunkBytes);
  const out = new Uint8Array(info.size);
  let offset = 0;
  try {
    checkFatResult(ff.f_open(file, path, FatFs.FA_READ | FatFs.FA_OPEN_EXISTING), `open ${path}`);
    while (offset < out.length) {
      const wanted = Math.min(fileChunkBytes, out.length - offset);
      checkFatResult(ff.f_read(file, bufferPtr, wanted, readPtr), `read ${path}`);
      const readBytes = ff.getValue(readPtr, "i32") >>> 0;
      if (readBytes === 0) break;
      out.set(ff.HEAPU8.subarray(bufferPtr, bufferPtr + readBytes), offset);
      offset += readBytes;
    }
    return offset === out.length ? out : out.slice(0, offset);
  } finally {
    ff.f_close(file);
    ff.free(bufferPtr);
    ff.free(readPtr);
    ff.free(file);
  }
}

function writeFile(ff, path, bytes) {
  const file = ff.malloc(FatFs.sizeof_FIL);
  const writtenPtr = ff.malloc(4);
  const bufferPtr = ff.malloc(fileChunkBytes);
  try {
    checkFatResult(ff.f_open(file, path, FatFs.FA_WRITE | FatFs.FA_CREATE_ALWAYS), `open write ${path}`);
    for (let offset = 0; offset < bytes.length; offset += fileChunkBytes) {
      const chunk = bytes.subarray(offset, offset + fileChunkBytes);
      ff.HEAPU8.set(chunk, bufferPtr);
      checkFatResult(ff.f_write(file, bufferPtr, chunk.length, writtenPtr), `write ${path}`);
      assert.equal(ff.getValue(writtenPtr, "i32") >>> 0, chunk.length, `short write for ${path}`);
    }
    checkFatResult(ff.f_sync(file), `sync ${path}`);
  } finally {
    ff.f_close(file);
    ff.free(bufferPtr);
    ff.free(writtenPtr);
    ff.free(file);
  }
}

const artifactBytes = Buffer.concat(imagePaths.map((path) => readFileSync(path)));
const image = new Uint8Array(
  imagePaths[0].includes(".gz") || (artifactBytes.length >= 2 && artifactBytes[0] === 0x1f && artifactBytes[1] === 0x8b)
    ? gunzipSync(artifactBytes)
    : artifactBytes,
);
const ff = await createFatFs({ diskio: new RawSdImageDisk(image) });
const fatfs = ff.malloc(FatFs.sizeof_FATFS);

try {
  checkFatResult(ff.f_mount(fatfs, "", 1), "mount");

  const expectedFiles = [
    "/fonts/LXGWWenKaiLite-Regular.ttf",
    ...browserFallbackFontPacks.map((name) => `/.murphy/fonts/${name}`),
    "/Books/quick-launch.txt",
  ];
  const stats = expectedFiles.map((path) => statPath(ff, path));
  for (const stat of stats) {
    assert.ok(stat.size > 0, `${stat.path} should not be empty`);
  }

  const quickLaunch = new TextDecoder().decode(readFile(ff, "/Books/quick-launch.txt"));
  assert.match(quickLaunch, /Panda Browser Simulator/);

  const probeDir = "/codex-sd-check";
  const probeFile = `${probeDir}/probe.txt`;
  const probeBytes = new TextEncoder().encode("browser sd card read/write probe\n");
  const mkdirResult = ff.f_mkdir(probeDir);
  if (mkdirResult !== FatFs.FR_OK && mkdirResult !== FatFs.FR_EXIST) {
    checkFatResult(mkdirResult, `mkdir ${probeDir}`);
  }
  writeFile(ff, probeFile, probeBytes);
  assert.deepEqual(readFile(ff, probeFile), probeBytes);
  checkFatResult(ff.f_unlink(probeFile), `unlink ${probeFile}`);
  checkFatResult(ff.f_unlink(probeDir), `unlink ${probeDir}`);

  console.log(`browser SD card contract ok: ${imagePaths.join(", ")}`);
  for (const stat of stats) {
    console.log(`${stat.path} ${stat.size} bytes`);
  }
} finally {
  ff.f_unmount("");
  ff.free(fatfs);
}
