import { createHash } from "node:crypto";
import { readFileSync, readdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { gzipSync } from "node:zlib";
import * as FatFs from "js-fatfs";

// 从空白 FAT32 创建开发沙盒，不继承本机书籍、字体、凭据或已有 SD 内容。
const output = process.argv[2];
const resources = process.argv[3];
if (!output || process.argv.length > 4) throw new Error("Usage: node scripts/create-studio-seed.mjs <new-seed.img.gz> [generated-system-resource-root]");
const bytes = new Uint8Array(128 * 1024 * 1024);
const sectorSize = 512;
const ff = await FatFs.create({ diskio: {
  initialize: () => 0,
  status: () => 0,
  read(fs, _drive, buffer, sector, count) { fs.HEAPU8.set(bytes.subarray(sector * sectorSize, (sector + count) * sectorSize), buffer); return FatFs.RES_OK; },
  write(fs, _drive, buffer, sector, count) { bytes.set(fs.HEAPU8.subarray(buffer, buffer + count * sectorSize), sector * sectorSize); return FatFs.RES_OK; },
  ioctl(fs, _drive, command, buffer) {
    if (command === FatFs.CTRL_SYNC) return FatFs.RES_OK;
    if (command === FatFs.GET_SECTOR_COUNT) fs.setValue(buffer, bytes.length / sectorSize, "i32");
    else if (command === FatFs.GET_SECTOR_SIZE) fs.setValue(buffer, sectorSize, "i16");
    else if (command === FatFs.GET_BLOCK_SIZE) fs.setValue(buffer, 1, "i32");
    else return FatFs.RES_ERROR;
    return FatFs.RES_OK;
  },
} });
const check = (result, operation) => { if (result !== FatFs.FR_OK) throw new Error(`${operation}:${result}`); };
const work = ff.malloc(4096);
const filesystem = ff.malloc(FatFs.sizeof_FATFS);
try {
  check(ff.f_mkfs("", { fmt: FatFs.FM_FAT32 | FatFs.FM_SFD, n_fat: 1, align: 1, n_root: 0, au_size: 512 }, work, 4096), "format");
  check(ff.f_mount(filesystem, "", 1), "mount");
  check(ff.f_mkdir("/PandaDev"), "mkdir");
  const files = [];
  if (resources) {
    check(ff.f_mkdir("/.murphy"), "mkdir-resources");
    check(ff.f_mkdir("/.murphy/fonts"), "mkdir-fonts");
    for (const entry of readdirSync(join(resources, ".murphy/fonts"), { withFileTypes: true })) {
      if (!entry.isFile() || !/\.(mfp|otf|ttf)$/.test(entry.name)) throw new Error("unexpected_system_resource");
      const data = readFileSync(join(resources, ".murphy/fonts", entry.name));
      const file = ff.malloc(FatFs.sizeof_FIL), buffer = ff.malloc(data.length), written = ff.malloc(4);
      try {
        check(ff.f_open(file, `/.murphy/fonts/${entry.name}`, FatFs.FA_WRITE | FatFs.FA_CREATE_NEW), "open-resource");
        ff.HEAPU8.set(data, buffer);
        check(ff.f_write(file, buffer, data.length, written), "write-resource");
        if (ff.getValue(written, "i32") !== data.length) throw new Error("resource_short_write");
        check(ff.f_close(file), "close-resource");
        files.push({ path: `/.murphy/fonts/${entry.name}`, bytes: data.length, sha256: createHash("sha256").update(data).digest("hex") });
      } finally { ff.free(written); ff.free(buffer); ff.free(file); }
    }
  }
  check(ff.f_unmount(""), "unmount");
  const compressed = gzipSync(bytes, { level: 9 });
  writeFileSync(output, compressed, { flag: "wx" });
  writeFileSync(output + ".receipt.json", JSON.stringify({ kind: resources ? "public-system-resources-studio-seed" : "empty-studio-fat32-seed", rawBytes: bytes.length, bytes: compressed.length, sha256: createHash("sha256").update(compressed).digest("hex"), files }, null, 2) + "\n", { flag: "wx" });
  console.log(JSON.stringify({ kind: resources ? "public-system-resources-studio-seed" : "empty-studio-fat32-seed", rawBytes: bytes.length, bytes: compressed.length, sha256: createHash("sha256").update(compressed).digest("hex"), systemResourceFiles: files.length }));
} finally { ff.free(filesystem); ff.free(work); }
