import assert from "node:assert/strict";
import test from "node:test";

import { CHUNK_BYTES, readSdImage, writeSdImage } from "../public/simulator-runtime/sd-card-store.js";

// Minimal IndexedDB double: one object store, array keys, async request delivery and
// auto-commit once a transaction has no pending request (like the browser).
class MemoryIdb {
  constructor() {
    this.name = crypto.randomUUID();
    this.values = new Map();
    this.puts = [];
    this.deletes = [];
    this.beforePut = null;
    this.transactionTail = Promise.resolve();
  }
  open() {
    const owner = this;
    return Promise.resolve({
      name: this.name,
      close() {},
      transaction() {
        let pending = 0;
        let finished = false;
        const preceding = owner.transactionTail;
        const turn = Promise.withResolvers();
        owner.transactionTail = turn.promise;
        const tx = { oncomplete: null, onerror: null, onabort: null, error: null, abort() {
          if (finished) return;
          finished = true;
          tx.onabort?.();
          turn.resolve();
        } };
        const settle = () => setTimeout(() => {
          if (!finished && pending === 0) {
            finished = true;
            tx.oncomplete?.();
            turn.resolve();
          }
        }, 0);
        const request = (run) => {
          const req = { result: undefined, onsuccess: null, onerror: null };
          pending++;
          setTimeout(async () => {
            await preceding;
            if (finished) return;
            try {
              req.result = await run();
              req.onsuccess?.();
            } catch (error) {
              req.error = error;
              tx.error = error;
              req.onerror?.();
              tx.abort();
            } finally {
              pending--;
              settle();
            }
          }, 0);
          return req;
        };
        tx.objectStore = () => ({
          get: (key) => request(() => owner.values.get(JSON.stringify(key))),
          put: (value, key) => request(async () => {
            await owner.beforePut?.(value, key);
            owner.puts.push(key);
            owner.values.set(JSON.stringify(key), value);
          }),
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

test("overlapping saves cannot remove chunks reused by the later snapshot", async () => {
  const db = new MemoryIdb();
  const open = () => db.open();
  const initial = makeImage(2);
  await writeSdImage(open, "images", "board", { bytes: initial, byteLength: initial.byteLength });

  const interval = initial.slice();
  interval[4096] ^= 0xff;
  const beforeStop = initial.slice();
  beforeStop[CHUNK_BYTES + 4096] ^= 0xff;

  const entered = Promise.withResolvers();
  const release = Promise.withResolvers();
  db.beforePut = async (_value, key) => {
    if (key[0] === "panda-sd-chunk-v2" && key[3] === 0) {
      entered.resolve();
      await release.promise;
    }
  };
  const intervalSave = writeSdImage(open, "images", "board", {
    bytes: interval, byteLength: interval.byteLength,
  });
  await entered.promise;
  const stopSave = writeSdImage(open, "images", "board", {
    bytes: beforeStop, byteLength: beforeStop.byteLength,
  });
  release.resolve();
  await Promise.all([intervalSave, stopSave]);

  assert.deepEqual((await readSdImage(open, "images", "board")).bytes, beforeStop);
});

test("same-database saves are ordered even through different open callbacks", async () => {
  const db = new MemoryIdb();
  const firstOpen = () => db.open();
  const secondOpen = () => db.open();
  const initial = makeImage(2);
  await writeSdImage(firstOpen, "images", "board", { bytes: initial, byteLength: initial.byteLength });
  const first = initial.slice();
  first[4096] ^= 0xff;
  const second = initial.slice();
  second[CHUNK_BYTES + 4096] ^= 0xff;

  await Promise.all([
    writeSdImage(firstOpen, "images", "board", { bytes: first, byteLength: first.byteLength }),
    writeSdImage(secondOpen, "images", "board", { bytes: second, byteLength: second.byteLength }),
  ]);

  assert.deepEqual((await readSdImage(firstOpen, "images", "board")).bytes, second);
});

test("a failed save keeps its previous snapshot readable and can be retried", async () => {
  const db = new MemoryIdb();
  const open = () => db.open();
  const initial = makeImage(2);
  await writeSdImage(open, "images", "board", { bytes: initial, byteLength: initial.byteLength });
  const changed = initial.slice();
  changed[4096] ^= 0xff;
  changed[CHUNK_BYTES + 4096] ^= 0xff;
  db.beforePut = (_value, key) => {
    if (key[0] === "panda-sd-meta-v2") throw new Error("injected SD write failure");
  };

  await assert.rejects(
    writeSdImage(open, "images", "board", { bytes: changed, byteLength: changed.byteLength }),
    /injected SD write failure/,
  );
  assert.deepEqual((await readSdImage(open, "images", "board")).bytes, initial);
  db.beforePut = null;
  await writeSdImage(open, "images", "board", { bytes: changed, byteLength: changed.byteLength });
  assert.deepEqual((await readSdImage(open, "images", "board")).bytes, changed);
});

test("a queued save still runs after the preceding save rejects", async () => {
  const db = new MemoryIdb();
  const open = () => db.open();
  const initial = makeImage(2);
  await writeSdImage(open, "images", "board", { bytes: initial, byteLength: initial.byteLength });
  const failed = initial.slice();
  failed[4096] ^= 0xff;
  const next = initial.slice();
  next[CHUNK_BYTES + 4096] ^= 0xff;
  let rejectNextPut = true;
  db.beforePut = () => {
    if (rejectNextPut) {
      rejectNextPut = false;
      throw new Error("injected queued SD write failure");
    }
  };

  const outcomes = await Promise.allSettled([
    writeSdImage(open, "images", "board", { bytes: failed, byteLength: failed.byteLength }),
    writeSdImage(open, "images", "board", { bytes: next, byteLength: next.byteLength }),
  ]);
  assert.equal(outcomes[0].status, "rejected");
  assert.match(outcomes[0].reason.message, /injected queued SD write failure/);
  assert.equal(outcomes[1].status, "fulfilled");
  assert.deepEqual((await readSdImage(open, "images", "board")).bytes, next);
});

test("a blocked save does not delay another card or database", async () => {
  const db = new MemoryIdb();
  const otherDb = new MemoryIdb();
  const otherOpen = () => otherDb.open();
  const image = makeImage(1);
  const entered = Promise.withResolvers();
  const release = Promise.withResolvers();
  let opens = 0;
  const open = async () => {
    if (++opens === 2) {
      entered.resolve();
      await release.promise;
    }
    return db.open();
  };
  const blocked = writeSdImage(open, "images", "blocked", { bytes: image, byteLength: image.byteLength });
  try {
    await entered.promise;
    await Promise.all([
      writeSdImage(open, "images", "other", { bytes: image, byteLength: image.byteLength }),
      writeSdImage(otherOpen, "images", "blocked", { bytes: image, byteLength: image.byteLength }),
    ]);
    assert.deepEqual((await readSdImage(open, "images", "other")).bytes, image);
    assert.deepEqual((await readSdImage(otherOpen, "images", "blocked")).bytes, image);
  } finally {
    release.resolve();
    await blocked;
  }
  assert.deepEqual((await readSdImage(open, "images", "blocked")).bytes, image);
});
for (const identicalChanges of [false, true]) {
  test(`independent store modules rebase ${identicalChanges ? "identical" : "different"} concurrent snapshots`, async () => {
    const otherStore = await import(`../public/simulator-runtime/sd-card-store.js?writer-${identicalChanges}`);
    const db = new MemoryIdb();
    const open = () => db.open();
    const initial = makeImage(2);
    await writeSdImage(open, "images", "board", { bytes: initial, byteLength: initial.byteLength });
    const first = initial.slice();
    first[4096] ^= 0xff;
    const second = identicalChanges ? first.slice() : initial.slice();
    if (!identicalChanges) second[CHUNK_BYTES + 4096] ^= 0xff;
    const firstReady = Promise.withResolvers();
    const secondReady = Promise.withResolvers();
    const releaseFirst = Promise.withResolvers();
    const releaseSecond = Promise.withResolvers();
    const pausedOpen = (ready, release) => {
      let opens = 0;
      return async () => {
        if (++opens === 3) {
          ready.resolve();
          await release.promise;
        }
        return db.open();
      };
    };
    const firstSave = writeSdImage(pausedOpen(firstReady, releaseFirst), "images", "board", {
      bytes: first, byteLength: first.byteLength,
    });
    let secondSave;
    try {
      await firstReady.promise;
      secondSave = otherStore.writeSdImage(pausedOpen(secondReady, releaseSecond), "images", "board", {
        bytes: second, byteLength: second.byteLength,
      });
      await secondReady.promise;
      releaseFirst.resolve();
      await firstSave;
      releaseSecond.resolve();
      await secondSave;

      assert.deepEqual((await readSdImage(open, "images", "board")).bytes, second);
      const storedChunks = [...db.values.keys()].filter(key => JSON.parse(key)[0] === "panda-sd-chunk-v2");
      assert.equal(storedChunks.length, 2, "successful conflicts leave only the published chunks");
    } finally {
      releaseFirst.resolve();
      releaseSecond.resolve();
      await Promise.allSettled([firstSave, secondSave]);
    }
  });
}
