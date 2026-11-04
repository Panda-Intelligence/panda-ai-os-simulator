import assert from "node:assert/strict";
import test from "node:test";

import { CHUNK_BYTES, readSdImage, writeSdImage } from "../public/simulator-runtime/sd-card-store.js";

// Minimal IndexedDB double: one object store, array keys, async request delivery and
// auto-commit once a transaction has no pending request (like the browser).
class MemoryIdb {
  constructor() {
    this.values = new Map();
    this.puts = [];
    this.deletes = [];
  }
  open() {
    const owner = this;
    return Promise.resolve({
      close() {},
      transaction() {
        let pending = 0;
        let finished = false;
        const tx = { oncomplete: null, onerror: null, onabort: null, error: null, abort() { finished = true; tx.onabort?.(); } };
        const settle = () => setTimeout(() => {
          if (!finished && pending === 0) { finished = true; tx.oncomplete?.(); }
        }, 0);
        const request = (run) => {
          const req = { result: undefined, onsuccess: null, onerror: null };
          pending++;
          setTimeout(() => {
            if (finished) return;
            req.result = run();
            req.onsuccess?.();
            pending--;
            settle();
          }, 0);
          return req;
        };
        tx.objectStore = () => ({
          get: (key) => request(() => owner.values.get(JSON.stringify(key))),
          put: (value, key) => request(() => { owner.puts.push(key); owner.values.set(JSON.stringify(key), value); }),
          delete: (key) => request(() => { owner.deletes.push(key); owner.values.delete(JSON.stringify(key)); }),
        });
        return tx;
      },
    });
  }
}

// FAT12 image spanning several chunks (validated by the store before any write).
function makeImage(chunks = 3) {
  const image = new Uint8Array(chunks * CHUNK_BYTES);
  const view = new DataView(image.buffer);
  view.setUint16(11, 512, true);
  image[13] = 64;
  view.setUint16(14, 1, true);
  image[16] = 2;
  view.setUint16(17, 224, true);
  view.setUint16(19, image.byteLength / 512 > 0xffff ? 0 : image.byteLength / 512, true);
  if (image.byteLength / 512 > 0xffff) view.setUint32(32, image.byteLength / 512, true);
  image[21] = 0xf8;
  view.setUint16(22, 2, true);
  image[510] = 0x55;
  image[511] = 0xaa;
  image.set([0xf8, 0xff, 0xff], 512);
  return image;
}

const chunkPuts = (db) => db.puts.filter((key) => key[0] === "panda-sd-chunk-v2");

test("an unchanged save writes no chunks and a one-byte change rewrites exactly one", async () => {
  const db = new MemoryIdb();
  const open = () => db.open();
  const image = makeImage(3);
  await writeSdImage(open, "images", "board", { bytes: image, byteLength: image.byteLength });
  assert.equal(chunkPuts(db).length, 3, "first save writes every chunk");

  db.puts.length = 0;
  db.deletes.length = 0;
  await writeSdImage(open, "images", "board", { bytes: image.slice(), byteLength: image.byteLength });
  assert.equal(chunkPuts(db).length, 0, "identical image reuses stored chunks");
  assert.equal(db.deletes.length, 0);

  const changed = image.slice();
  changed[CHUNK_BYTES + 4096] ^= 0xff;
  db.puts.length = 0;
  await writeSdImage(open, "images", "board", { bytes: changed, byteLength: changed.byteLength });
  assert.deepEqual(chunkPuts(db).map((key) => key[3]), [1], "only the modified chunk is written");
  assert.deepEqual(db.deletes.map((key) => key[3]), [1], "the replaced chunk is deleted");

  const restored = await readSdImage(open, "images", "board");
  assert.deepEqual(restored.bytes, changed);
});

test("a v2 record still reads and is upgraded with a full rewrite", async () => {
  const db = new MemoryIdb();
  const open = () => db.open();
  const image = makeImage(2);
  const generation = "legacy-generation-0001";
  for (let index = 0; index < 2; index++) {
    db.values.set(JSON.stringify(["panda-sd-chunk-v2", "board", generation, index]),
      image.slice(index * CHUNK_BYTES, (index + 1) * CHUNK_BYTES).buffer);
  }
  db.values.set(JSON.stringify(["panda-sd-meta-v2", "board"]), {
    format: "chunks-v2", generation, chunkBytes: CHUNK_BYTES, chunkCount: 2, byteLength: image.byteLength, savedAt: 1,
  });
  assert.deepEqual((await readSdImage(open, "images", "board")).bytes, image);

  await writeSdImage(open, "images", "board", { bytes: image, byteLength: image.byteLength });
  assert.equal(chunkPuts(db).length, 2, "v2 has no digests, so every chunk is rewritten once");
  assert.equal(db.deletes.length, 2, "the v2 generation is removed");
  const meta = db.values.get(JSON.stringify(["panda-sd-meta-v2", "board"]));
  assert.equal(meta.format, "chunks-v3");
  assert.deepEqual((await readSdImage(open, "images", "board")).bytes, image);
});
