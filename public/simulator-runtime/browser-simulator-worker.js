const RUNTIME_STATUS_READY = "ready";
const RUNTIME_STATUS_RUNNING = "running";
const QEMU_ARTIFACT_MISSING = "qemu_wasm_artifact_missing";
const QEMU_START_FAILED = "qemu_wasm_start_failed";
const FLASH_SIZE_BYTES = 16 * 1024 * 1024;
const BOOTLOADER_OFFSET = 0x0000;
const PARTITION_TABLE_OFFSET = 0x8000;
const PARTITION_ENTRY_SIZE = 32;
const PARTITION_MAGIC = 0x50aa;
const PARTITION_END_MAGIC = 0xebeb;
const IPC_FRAMEBUFFER_CHANNEL = 0x01;
const IPC_DEBUG_CHANNEL = 0x02;
const IPC_CONTROL_CHANNEL = 0x06;
const SD_CARD_DB_NAME = "panda-browser-simulator-sd-card";
const SD_CARD_DB_VERSION = 1;
const SD_CARD_STORE_NAME = "images";
const SD_CARD_PERSIST_INTERVAL_MS = 15000;
const qemuIpcOwner = { instance: null };

let runtimeManifest = null;
let qemuModulePromise = null;
let qemuInstance = null;
const qemuPty = createBrowserQemuPty();
let running = false;
let lastBoardId = "mofei";
let activeSdCard = null;
let sdCardPersistTimer = null;

postMessage({
  type: "ready",
  manifest: {
    kind: "wasm-worker",
    status: RUNTIME_STATUS_READY,
    workerScript: "/simulator/app/simulator-runtime/browser-simulator-worker.js",
    firmwareArtifact: null,
  },
});

addEventListener("message", (event) => {
  void handleCommand(event.data);
});

async function handleCommand(command) {
  if (!command || typeof command !== "object" || typeof command.type !== "string") {
    emitError("Ignoring malformed browser simulator runtime command.");
    return;
  }

  if (command.manifest && typeof command.manifest === "object") {
    runtimeManifest = command.manifest;
  }

  switch (command.type) {
    case "start":
      await startRuntime(command);
      return;
    case "fullReboot":
      await startRuntime(command);
      return;
    case "stop":
      await stopRuntime(command.requestId, true);
      return;
    case "releaseButtons":
      releaseButtons();
      result(command.requestId, true, running ? "released" : "not_running");
      return;
    case "injectButton":
      injectButton(command.buttonId, command.pressed);
      result(command.requestId, true, running ? "button_injected" : "not_running");
      return;
    case "injectTouch":
      injectTouch(command.input);
      result(command.requestId, true, running ? "touch_injected" : "not_running");
      return;
    case "peripheralControl":
      injectPeripheralControl(command.payload);
      result(command.requestId, true, running ? "control_injected" : "not_running");
      return;
    default:
      result(command.requestId, false, undefined, `unsupported_command:${command.type}`);
  }
}

async function startRuntime(command) {
  lastBoardId = command.boardId || lastBoardId;
  if (running) {
    result(command.requestId, true, "already_running");
    return;
  }

  const manifest = runtimeManifest;
  if (!manifest || manifest.kind !== "wasm-worker") {
    result(command.requestId, false, undefined, "browser_wasm_runtime_manifest_missing");
    return;
  }

  if (
    !manifest.qemuScript ||
    !manifest.qemuWasm ||
    !manifest.qemuKernel ||
    !manifest.qemuSymbols ||
    !manifest.qemuBootloader ||
    !manifest.qemuPartitionTable ||
    !manifest.qemuOtaData
  ) {
    result(command.requestId, false, undefined, QEMU_ARTIFACT_MISSING);
    return;
  }

  try {
    const boardArtifacts = manifest.firmwareArtifacts ? manifest.firmwareArtifacts[lastBoardId] : null;
    const firmwareUrl = command.firmwarePath || (boardArtifacts && boardArtifacts.bin) || manifest.firmwareArtifact;
    const kernelUrl = (boardArtifacts && boardArtifacts.kernel) || manifest.qemuKernel;
    const symbolsUrl = (boardArtifacts && boardArtifacts.symbols) || manifest.qemuSymbols;
    const firmware = await fetchFirmware(firmwareUrl);
    const kernel = await fetchKernel(kernelUrl);
    const symbols = await fetchSymbols(symbolsUrl);
    const bootloader = await fetchBootloader(manifest.qemuBootloader);
    const partitionTable = await fetchPartitionTable(manifest.qemuPartitionTable);
    const otaData = await fetchOtaData(manifest.qemuOtaData);
    const flashImage = buildFlashImage({ bootloader, partitionTable, otaData, firmware });
    const rom = await fetchRom(manifest.qemuRom);
    const sdImage = await fetchSdImage(manifest.qemuSdImage);
    qemuInstance = await loadQemuModule(manifest, firmware, kernel, symbols, bootloader, partitionTable, otaData, flashImage, rom, sdImage);
    startQemuMain(qemuInstance, kernel.path, flashImage.path, sdImage?.path ?? null);
    running = true;
    log(`[browser-qemu] qemu-wasm runtime loaded for board=${lastBoardId}`);
    log(`[browser-qemu] firmware artifact=${firmwareUrl || "default"}`);
    log(`[browser-qemu] kernel artifact=${kernelUrl || "default"}`);
    log(`[browser-qemu] symbols artifact=${symbolsUrl || "default"}`);
    log(`[browser-qemu] flash image mounted=${flashImage.path}`);
    log(`[browser-qemu] rom artifact=${manifest.qemuRom || "default"}`);
    log(
      sdImage
        ? `[browser-qemu] SD image mounted=${describeSdImageArtifact(manifest.qemuSdImage)}${sdImage.persisted ? " (persisted browser image)" : " (template image)"}`
        : "[browser-qemu] SD image unavailable; booting without browser SD card image.",
    );
    startSdCardPersistence();
    result(command.requestId, true, "started");
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    emitError(`${QEMU_START_FAILED}: ${message}`);
    result(command.requestId, false, undefined, `${QEMU_START_FAILED}:${message}`);
  }
}

async function stopRuntime(requestId, emitResult) {
  await persistActiveSdCard("stop");
  stopSdCardPersistence();
  running = false;
  if (emitResult) {
    result(requestId, true, "stopped");
  }
}

async function loadQemuModule(manifest, firmware, kernel, symbols, bootloader, partitionTable, otaData, flashImage, rom, sdImage) {
  if (qemuModulePromise) {
    return qemuModulePromise;
  }

  qemuModulePromise = (async () => {
    await assertFetchOk(manifest.qemuWasm, "qemu wasm");
    await assertFetchOk(manifest.qemuScript, "qemu module script");

    self._mofeiWasmIpcSend = function _workerBridge(channel, flags, payloadPtr, payloadLen) {
      handleQemuIpc(qemuIpcOwner.instance, channel, flags, payloadPtr, payloadLen);
    };

    const qemuModule = await import(cacheBustUrl(absoluteUrl(manifest.qemuScript)));
    const factory = qemuModule.default;
    if (typeof factory === "function") {
      return factory({
        noInitialRun: true,
        noExitRuntime: true,
        ENV: {
          MOFEI_ROM_BINARY: rom.path,
          MOFEI_FIRMWARE_ELF: kernel.path,
          MOFEI_FIRMWARE_SYMBOLS: symbols.path,
          MOFEI_SIM_BOARD: lastBoardId === "mofei" ? "mofei" : lastBoardId,
        },
        pty: qemuPty,
        preRun: [
          (moduleInstance) => {
            const target = moduleInstance || qemuInstance;
            if (!target || !target.FS) {
              throw new Error("qemu module filesystem is unavailable");
            }
            qemuIpcOwner.instance = target;
            ensureDirectory(target.FS, "/mofei");
            ensureDirectory(target.FS, "/murphy-os");
            target.FS.writeFile(firmware.path, firmware.bytes);
            target.FS.writeFile(kernel.path, kernel.bytes);
            target.FS.writeFile(symbols.path, symbols.bytes);
            target.FS.writeFile(bootloader.path, bootloader.bytes);
            target.FS.writeFile(partitionTable.path, partitionTable.bytes);
            target.FS.writeFile(otaData.path, otaData.bytes);
            target.FS.writeFile(flashImage.path, flashImage.bytes);
            target.FS.writeFile(rom.path, rom.bytes);
            if (sdImage) {
              target.FS.writeFile(sdImage.path, sdImage.bytes);
              activeSdCard = {
                path: sdImage.path,
                storageKey: sdImage.storageKey,
                templateFingerprint: sdImage.templateFingerprint,
              };
            } else {
              activeSdCard = null;
            }
            if (typeof target.ENV === "object" && target.ENV) {
              target.ENV.MOFEI_ROM_BINARY = rom.path;
              target.ENV.MOFEI_FIRMWARE_ELF = kernel.path;
              target.ENV.MOFEI_FIRMWARE_SYMBOLS = symbols.path;
              target.ENV.MOFEI_SIM_BOARD = lastBoardId === "mofei" ? "mofei" : lastBoardId;
            }
          },
        ],
        locateFile(path) {
          return locateQemuFile(path, manifest);
        },
        _mofeiWasmIpcSend(channel, flags, payloadPtr, payloadLen) {
          handleQemuIpc(qemuIpcOwner.instance, channel, flags, payloadPtr, payloadLen);
        },
        print(text) {
          log(String(text));
        },
        printErr(text) {
          emitError(String(text));
        },
      });
    }
    throw new Error(`qemu_wasm_start_failed: factory is not a function, got ${typeof factory}`);
  })();

  return qemuModulePromise;
}

function createBrowserQemuPty() {
  const stdin = [];
  const readableListeners = new Set();
  const signalListeners = new Set();
  let termios = {
    iflag: 0,
    oflag: 1,
    cflag: 191,
    lflag: 35387,
    cc: new Array(32).fill(0),
  };

  const notifyReadable = () => {
    for (const listener of readableListeners) {
      listener();
    }
  };

  return {
    get readable() {
      return stdin.length > 0;
    },
    get writable() {
      return true;
    },
    read(length) {
      if (!Number.isFinite(length) || length <= 0 || stdin.length === 0) {
        return [];
      }
      return stdin.splice(0, length);
    },
    write(bytes) {
      if (!Array.isArray(bytes) || bytes.length === 0) {
        return;
      }
      log(new TextDecoder().decode(new Uint8Array(bytes)));
    },
    ioctl(name, value) {
      if (name === "TCGETS") {
        return {
          iflag: termios.iflag,
          oflag: termios.oflag,
          cflag: termios.cflag,
          lflag: termios.lflag,
          cc: [...termios.cc],
        };
      }
      if (name === "TCSETS") {
        termios = {
          iflag: value?.iflag ?? termios.iflag,
          oflag: value?.oflag ?? termios.oflag,
          cflag: value?.cflag ?? termios.cflag,
          lflag: value?.lflag ?? termios.lflag,
          cc: Array.isArray(value?.cc) ? [...value.cc] : [...termios.cc],
        };
        return 0;
      }
      if (name === "TIOCGWINSZ") {
        return [24, 80];
      }
      return 0;
    },
    onReadable(listener) {
      return subscribe(readableListeners, listener);
    },
    onSignal(listener) {
      return subscribe(signalListeners, listener);
    },
    pushInput(bytes) {
      if (!bytes || bytes.length === 0) {
        return;
      }
      stdin.push(...bytes);
      notifyReadable();
    },
    signal(name) {
      for (const listener of signalListeners) {
        listener(name);
      }
    },
  };
}

function subscribe(listeners, listener) {
  if (typeof listener !== "function") {
    return { dispose() {} };
  }
  listeners.add(listener);
  return {
    dispose() {
      listeners.delete(listener);
    },
  };
}

function startQemuMain(instance, firmwarePath, flashImagePath, sdImagePath) {
  if (!instance || typeof instance.callMain !== "function") {
    throw new Error("qemu module loaded without callMain");
  }

  const args = [
    "-machine",
    "esp32s3",
    "-smp",
    "1",
    "-chardev",
    "null,id=mofei",
    "-display",
    "none",
    "-serial",
    "stdio",
    "-semihosting-config",
    "enable=on,target=native",
    "-L",
    "/mofei",
  ];
  args.push("-kernel", firmwarePath);
  if (flashImagePath) {
    args.push("-drive", `file=${flashImagePath},if=mtd,format=raw`);
  }
  if (sdImagePath) {
    args.push("-drive", `file=${sdImagePath},if=sd,format=raw`);
  }
  instance.callMain(args);
}

async function fetchFirmware(firmwareUrl) {
  if (!firmwareUrl) {
    throw new Error("browser qemu firmware artifact is missing");
  }

  const bytes = await fetchBinaryArtifact(firmwareUrl, "firmware artifact");
  return {
    path: "/murphy-os/panda_os.bin",
    bytes,
  };
}

async function fetchKernel(kernelUrl) {
  if (!kernelUrl) {
    throw new Error("browser qemu kernel artifact is missing");
  }

  const bytes = await fetchBinaryArtifact(kernelUrl, "kernel artifact");
  return {
    path: "/murphy-os/murphy_os-kernel.img",
    bytes,
  };
}

async function fetchSymbols(symbolsUrl) {
  if (!symbolsUrl) {
    throw new Error("browser qemu symbols artifact is missing");
  }

  const response = await fetch(symbolsUrl, { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`symbols artifact fetch failed: ${response.status}`);
  }

  const bytes = new TextEncoder().encode(await response.text());
  return {
    path: "/murphy-os/murphy_os-symbols.txt",
    bytes,
  };
}

async function fetchRom(romUrl) {
  if (!romUrl) {
    throw new Error("browser qemu ROM artifact is missing");
  }

  const bytes = await fetchBinaryArtifact(romUrl, "ROM artifact");
  return {
    path: "/mofei/esp32s3_rev0_rom.bin",
    bytes,
  };
}

async function fetchSdImage(sdImageUrl) {
  const artifactUrls = normalizeSdImageArtifactUrls(sdImageUrl);
  if (!artifactUrls) {
    return null;
  }

  const artifactBytes = await fetchSdImageArtifacts(artifactUrls);
  const templateBytes = await decodeSdImageArtifact(artifactUrls[0], artifactBytes);
  const templateFingerprint = fingerprintBytes(templateBytes);
  const storageKey = sdImageStorageKey(lastBoardId, artifactUrls);
  const stored = await readStoredSdCard(storageKey);
  const canUseStored =
    stored &&
    stored.templateFingerprint === templateFingerprint &&
    stored.byteLength === templateBytes.byteLength &&
    stored.bytes instanceof Uint8Array;
  const bytes = canUseStored ? stored.bytes : templateBytes;
  return {
    path: "/mofei/sdcard.img",
    bytes,
    storageKey,
    templateFingerprint,
    persisted: Boolean(canUseStored),
  };
}

function normalizeSdImageArtifactUrls(value) {
  const candidates = Array.isArray(value) ? value : [value];
  const urls = candidates.filter((entry) => typeof entry === "string" && entry.trim());
  return urls.length === candidates.length && urls.length > 0 ? urls : null;
}

function describeSdImageArtifact(value) {
  const urls = normalizeSdImageArtifactUrls(value);
  return urls ? (urls.length === 1 ? urls[0] : `${urls.length} compressed parts`) : "unavailable";
}

function sdImageStorageKey(boardId, artifactUrls) {
  const artifactIdentity = artifactUrls.length === 1 ? artifactUrls[0] : JSON.stringify(artifactUrls);
  return `sdcard:${boardId}:${artifactIdentity}`;
}

async function fetchSdImageArtifacts(urls) {
  const chunks = await Promise.all(urls.map((url) => fetchBinaryArtifact(url, "SD image artifact")));
  const totalBytes = chunks.reduce((total, chunk) => total + chunk.byteLength, 0);
  const artifactBytes = new Uint8Array(totalBytes);
  let offset = 0;
  for (const chunk of chunks) {
    artifactBytes.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return artifactBytes;
}

async function fetchBootloader(bootloaderUrl) {
  if (!bootloaderUrl) {
    throw new Error("browser qemu bootloader artifact is missing");
  }

  return {
    path: "/murphy-os/bootloader.bin",
    bytes: await fetchBinaryArtifact(bootloaderUrl, "bootloader artifact"),
  };
}

async function fetchPartitionTable(partitionTableUrl) {
  if (!partitionTableUrl) {
    throw new Error("browser qemu partition table artifact is missing");
  }

  return {
    path: "/murphy-os/partition-table.bin",
    bytes: await fetchBinaryArtifact(partitionTableUrl, "partition table artifact"),
  };
}

async function fetchOtaData(otaDataUrl) {
  if (!otaDataUrl) {
    throw new Error("browser qemu OTA data artifact is missing");
  }

  return {
    path: "/murphy-os/ota_data_initial.bin",
    bytes: await fetchBinaryArtifact(otaDataUrl, "OTA data artifact"),
  };
}

async function fetchBinaryArtifact(url, label) {
  const response = await fetch(url, { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`${label} fetch failed: ${response.status}`);
  }
  return new Uint8Array(await response.arrayBuffer());
}

async function decodeSdImageArtifact(url, bytes) {
  if (!isGzipArtifact(url, bytes)) {
    return bytes;
  }
  if (typeof DecompressionStream !== "function") {
    throw new Error("browser_sd_card_gzip_unsupported");
  }
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

function isGzipArtifact(url, bytes) {
  return String(url).endsWith(".gz") || (bytes.length >= 2 && bytes[0] === 0x1f && bytes[1] === 0x8b);
}

function startSdCardPersistence() {
  stopSdCardPersistence();
  if (!activeSdCard) {
    return;
  }
  sdCardPersistTimer = setInterval(() => {
    void persistActiveSdCard("interval");
  }, SD_CARD_PERSIST_INTERVAL_MS);
}

function stopSdCardPersistence() {
  if (sdCardPersistTimer !== null) {
    clearInterval(sdCardPersistTimer);
    sdCardPersistTimer = null;
  }
}

async function persistActiveSdCard(reason) {
  if (!activeSdCard || !qemuInstance || !qemuInstance.FS) {
    return;
  }
  try {
    const bytes = qemuInstance.FS.readFile(activeSdCard.path);
    if (!(bytes instanceof Uint8Array) || bytes.length === 0) {
      return;
    }
    await writeStoredSdCard(activeSdCard.storageKey, {
      bytes,
      byteLength: bytes.byteLength,
      templateFingerprint: activeSdCard.templateFingerprint,
      savedAt: Date.now(),
    });
    if (reason !== "interval") {
      log(`[browser-qemu] persisted browser SD image reason=${reason} bytes=${bytes.byteLength}`);
    }
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    emitError(`[browser-qemu] failed to persist browser SD image: ${message}`);
  }
}

function fingerprintBytes(bytes) {
  let hash = 0x811c9dc5;
  const mix = (value) => {
    hash ^= value & 0xff;
    hash = Math.imul(hash, 0x01000193) >>> 0;
  };
  const length = bytes.byteLength >>> 0;
  mix(length & 0xff);
  mix((length >> 8) & 0xff);
  mix((length >> 16) & 0xff);
  mix((length >> 24) & 0xff);
  const sampleWindow = Math.min(65536, bytes.byteLength);
  for (let index = 0; index < sampleWindow; index += 1) {
    mix(bytes[index]);
  }
  const tailStart = Math.max(sampleWindow, bytes.byteLength - sampleWindow);
  for (let index = tailStart; index < bytes.byteLength; index += 1) {
    mix(bytes[index]);
  }
  return `${bytes.byteLength}:${hash.toString(16).padStart(8, "0")}`;
}

async function readStoredSdCard(storageKey) {
  const db = await openSdCardDb().catch(() => null);
  if (!db) {
    return null;
  }
  return new Promise((resolve) => {
    const tx = db.transaction(SD_CARD_STORE_NAME, "readonly");
    const request = tx.objectStore(SD_CARD_STORE_NAME).get(storageKey);
    request.onsuccess = () => {
      const value = request.result;
      if (!value || !(value.bytes instanceof ArrayBuffer)) {
        resolve(null);
        return;
      }
      resolve({
        templateFingerprint: value.templateFingerprint,
        byteLength: value.byteLength,
        bytes: new Uint8Array(value.bytes),
      });
    };
    request.onerror = () => resolve(null);
    tx.oncomplete = () => db.close();
    tx.onerror = () => db.close();
    tx.onabort = () => db.close();
  });
}

async function writeStoredSdCard(storageKey, value) {
  const db = await openSdCardDb();
  return new Promise((resolve, reject) => {
    const tx = db.transaction(SD_CARD_STORE_NAME, "readwrite");
    const request = tx.objectStore(SD_CARD_STORE_NAME).put(
      {
        templateFingerprint: value.templateFingerprint,
        byteLength: value.byteLength,
        bytes: value.bytes.buffer.slice(value.bytes.byteOffset, value.bytes.byteOffset + value.bytes.byteLength),
        savedAt: value.savedAt,
      },
      storageKey,
    );
    request.onerror = () => reject(request.error || new Error("indexeddb sdcard write failed"));
    tx.oncomplete = () => {
      db.close();
      resolve();
    };
    tx.onerror = () => {
      db.close();
      reject(tx.error || new Error("indexeddb sdcard transaction failed"));
    };
    tx.onabort = () => {
      db.close();
      reject(tx.error || new Error("indexeddb sdcard transaction aborted"));
    };
  });
}

function openSdCardDb() {
  if (!self.indexedDB) {
    return Promise.reject(new Error("indexeddb unavailable"));
  }
  return new Promise((resolve, reject) => {
    const request = self.indexedDB.open(SD_CARD_DB_NAME, SD_CARD_DB_VERSION);
    request.onupgradeneeded = () => {
      const db = request.result;
      if (!db.objectStoreNames.contains(SD_CARD_STORE_NAME)) {
        db.createObjectStore(SD_CARD_STORE_NAME);
      }
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error || new Error("indexeddb open failed"));
  });
}

function buildFlashImage({ bootloader, partitionTable, otaData, firmware }) {
  if (!bootloader.bytes.length || !partitionTable.bytes.length || !otaData.bytes.length || !firmware.bytes.length) {
    throw new Error("browser qemu flash image source artifact is empty");
  }
  if (firmware.bytes[0] !== 0xe9) {
    throw new Error("browser qemu firmware artifact is not an ESP image");
  }

  const layout = parseFlashLayout(partitionTable.bytes);
  const bootloaderBytes = patchBootloaderResetCopy(bootloader.bytes);
  const firmwareBytes = patchAdcCalibrationCopy(firmware.bytes);
  const bytes = new Uint8Array(FLASH_SIZE_BYTES);
  bytes.fill(0xff);
  const occupied = [];
  copyFlashSegment(bytes, occupied, BOOTLOADER_OFFSET, bootloaderBytes, "bootloader");
  copyFlashSegment(bytes, occupied, PARTITION_TABLE_OFFSET, partitionTable.bytes, "partition-table");
  copyFlashSegment(bytes, occupied, layout.bootApp0Offset, otaData.bytes, "ota_data_initial");
  copyFlashSegment(bytes, occupied, layout.appOffset, firmwareBytes, "firmware");
  return {
    path: "/murphy-os/flash.img",
    bytes,
  };
}

function patchBootloaderResetCopy(source) {
  const bytes = new Uint8Array(source);
  forEachEspImageSegment(bytes, (loadAddress, segmentOffset, segmentSize) => {
    if (loadAddress !== 0x403cb700) {
      return false;
    }
    const resetLoopSegmentOffset = 0x19e9;
    if (resetLoopSegmentOffset + 3 > segmentSize) {
      return false;
    }
    const patchOffset = segmentOffset + resetLoopSegmentOffset;
    if (bytes[patchOffset] === 0x06 && bytes[patchOffset + 1] === 0xff && bytes[patchOffset + 2] === 0xff) {
      bytes[patchOffset] = 0x1d;
      bytes[patchOffset + 1] = 0xf0;
      bytes[patchOffset + 2] = 0x00;
      log("[browser-qemu] patched bootloader reset loop for flash image");
    }
    return false;
  });
  return bytes;
}

function patchAdcCalibrationCopy(source) {
  const bytes = new Uint8Array(source);
  forEachEspImageSegment(bytes, (loadAddress, segmentOffset, segmentSize) => {
    if (loadAddress !== 0x42000020) {
      return false;
    }
    for (let index = 0; index + 4 <= segmentSize; index += 2) {
      const patchOffset = segmentOffset + index;
      if (
        bytes[patchOffset] === 0x36 &&
        bytes[patchOffset + 1] === 0xc1 &&
        bytes[patchOffset + 2] === 0x00 &&
        bytes[patchOffset + 3] === 0x7d
      ) {
        bytes[patchOffset] = 0x02;
        bytes[patchOffset + 1] = 0x0c;
        bytes[patchOffset + 2] = 0x1d;
        bytes[patchOffset + 3] = 0xf0;
        log("[browser-qemu] patched ADC calibration for flash image");
        return true;
      }
    }
    return false;
  });
  return bytes;
}

function forEachEspImageSegment(bytes, visitor) {
  if (bytes.length < 24 || bytes[0] !== 0xe9) {
    return;
  }
  const segments = bytes[1];
  let offset = 24;
  for (let segmentIndex = 0; segmentIndex < segments; segmentIndex += 1) {
    if (offset + 8 > bytes.length) {
      return;
    }
    const loadAddress = readLeU32(bytes, offset);
    const segmentSize = readLeU32(bytes, offset + 4);
    offset += 8;
    if (offset + segmentSize > bytes.length) {
      return;
    }
    if (visitor(loadAddress, offset, segmentSize)) {
      return;
    }
    offset += segmentSize;
  }
}

function parseFlashLayout(partitionTable) {
  let bootApp0Offset = null;
  let appOffset = null;
  let firstAppOffset = null;

  for (let offset = 0; offset + PARTITION_ENTRY_SIZE <= partitionTable.length; offset += PARTITION_ENTRY_SIZE) {
    const entry = partitionTable.subarray(offset, offset + PARTITION_ENTRY_SIZE);
    const magic = entry[0] | (entry[1] << 8);
    if (magic === PARTITION_END_MAGIC) {
      break;
    }
    if (magic !== PARTITION_MAGIC) {
      throw new Error(`invalid partition table magic ${entry[0].toString(16).padStart(2, "0")}${entry[1].toString(16).padStart(2, "0")}`);
    }

    const entryType = entry[2];
    const entrySubtype = entry[3];
    const partitionOffset = readLeU32(entry, 4);
    const label = partitionLabel(entry);

    if (entryType === 0x00) {
      if (firstAppOffset === null) {
        firstAppOffset = partitionOffset;
      }
      if (label === "app0" || entrySubtype === 0x10) {
        appOffset = partitionOffset;
      }
    } else if (label === "otadata" || (entryType === 0x01 && entrySubtype === 0x00)) {
      bootApp0Offset = partitionOffset;
    }
  }

  if (bootApp0Offset === null) {
    throw new Error("partition table has no otadata entry for ota_data_initial.bin");
  }
  if (appOffset === null && firstAppOffset === null) {
    throw new Error("partition table has no app entry for firmware.bin");
  }

  return {
    bootApp0Offset,
    appOffset: appOffset ?? firstAppOffset,
  };
}

function partitionLabel(entry) {
  const label = entry.subarray(12, 28);
  const end = label.indexOf(0);
  return new TextDecoder().decode(end >= 0 ? label.subarray(0, end) : label);
}

function readLeU32(bytes, offset) {
  return (
    bytes[offset] |
    (bytes[offset + 1] << 8) |
    (bytes[offset + 2] << 16) |
    (bytes[offset + 3] << 24)
  ) >>> 0;
}

function copyFlashSegment(image, occupied, offset, data, name) {
  const end = offset + data.length;
  if (!Number.isSafeInteger(offset) || offset < 0 || end > image.length) {
    throw new Error(`${name} too large for flash offset 0x${offset.toString(16)}`);
  }
  for (const existing of occupied) {
    if (offset < existing.end && end > existing.offset) {
      throw new Error(
        `${name} at 0x${offset.toString(16)}..0x${end.toString(16)} overlaps ${existing.name} at 0x${existing.offset.toString(16)}..0x${existing.end.toString(16)}`,
      );
    }
  }
  image.set(data, offset);
  occupied.push({ offset, end, name });
}

function ensureDirectory(fs, path) {
  try {
    fs.mkdir(path);
  } catch (error) {
    if (!error || error.code !== "EEXIST") {
      throw error;
    }
  }
}

function handleQemuIpc(instance, channel, flags, payloadPtr, payloadLen) {
  const bytes = readHeapBytes(instance, payloadPtr, payloadLen);
  if (bytes === null) {
    emitError(
      `[browser-qemu] dropped invalid IPC payload channel=${channel} ptr=${payloadPtr} len=${payloadLen}`,
    );
    return;
  }
  if (channel === IPC_FRAMEBUFFER_CHANNEL) {
    emitFramebuffer(flags, bytes);
    return;
  }
  if (channel === IPC_DEBUG_CHANNEL) {
    log(new TextDecoder().decode(bytes));
    return;
  }
  if (channel === IPC_CONTROL_CHANNEL) {
    postMessage({ type: "peripheralControl", payload: Array.from(bytes) });
  }
}

function readHeapBytes(instance, ptr, len) {
  if (!instance || !instance.HEAPU8 || !Number.isFinite(ptr) || !Number.isFinite(len) || len < 0) {
    return null;
  }
  const start = ptr >>> 0;
  const length = len >>> 0;
  const end = start + length;
  if (length === 0) {
    return new Uint8Array();
  }
  if (end < start || end > instance.HEAPU8.length) {
    return null;
  }
  return instance.HEAPU8.slice(start, end);
}

function framebufferProfile(boardId) {
  if (boardId === "s37uc") return { width: 416, height: 240, format: "mono1" };
  if (boardId === "m5papers3") return { width: 540, height: 960, format: "gray16" };
  if (boardId === "lilygo-t5s3-pro") return { width: 540, height: 960, format: "gray16" };
  return { width: 800, height: 480, format: "mono1" };
}

function emitFramebuffer(flags, framebuffer) {
  const { width, height, format } = framebufferProfile(lastBoardId);
  const expectedBytes = format === "gray16" ? Math.ceil(width / 2) * height : Math.ceil((width * height) / 8);
  if (framebuffer.length !== expectedBytes) {
    emitError(
      `[browser-qemu] dropped ${format} framebuffer payload len=${framebuffer.length}, expected=${expectedBytes}`,
    );
    return;
  }
  const rgba = format === "gray16" ? gray16ToRgba(framebuffer, width, height) : oneBitToRgba(framebuffer, width, height);
  postMessage({
    type: "framebuffer",
    payload: {
      kind: flags & 0x01 ? "partial" : "full",
      width,
      height,
      rgba_b64: bytesToBase64(rgba),
    },
  });
}

function gray16ToRgba(framebuffer, width, height) {
  const rgba = new Uint8Array(width * height * 4);
  const stride = Math.ceil(width / 2);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const packed = framebuffer[y * stride + (x >> 1)] ?? 0;
      const level = (x & 1) === 0 ? packed >> 4 : packed & 0x0f;
      const value = 255 - level * 17;
      const idx = (y * width + x) * 4;
      rgba[idx] = value;
      rgba[idx + 1] = value;
      rgba[idx + 2] = value;
      rgba[idx + 3] = 255;
    }
  }
  return rgba;
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

function releaseButtons() {
  if (!qemuInstance) {
    return;
  }
  const fn = qemuInstance._mofei_wasm_release_buttons;
  if (typeof fn === "function") {
    fn();
  }
}

function injectButton(buttonId, pressed) {
  if (!qemuInstance) {
    return;
  }
  const fn = qemuInstance._mofei_wasm_inject_button;
  if (typeof fn === "function") {
    fn(buttonId >>> 0, pressed ? 1 : 0);
  }
}

function injectTouch(input) {
  if (!qemuInstance || !input || typeof input !== "object") {
    return;
  }
  const fn = qemuInstance._mofei_wasm_inject_touch;
  if (typeof fn === "function") {
    fn(input.action >>> 0, input.x >>> 0, input.y >>> 0, input.fingerId >>> 0);
  }
}

function injectPeripheralControl(payload) {
  if (!qemuInstance || !Array.isArray(payload)) {
    return;
  }
  const bytes = Uint8Array.from(payload);
  const fn = qemuInstance._mofei_wasm_i2c_control;
  if (typeof fn !== "function") {
    return;
  }
  const ptr = qemuInstance._malloc(bytes.length);
  try {
    qemuInstance.HEAPU8.set(bytes, ptr);
    fn(ptr, bytes.length);
  } finally {
    qemuInstance._free(ptr);
  }
}

function locateQemuFile(path, manifest) {
  if (path.endsWith(".wasm") && manifest.qemuWasm) {
    return manifest.qemuWasm;
  }
  if (path.endsWith(".worker.js") && manifest.qemuWorkerScript) {
    return manifest.qemuWorkerScript;
  }
  return new URL(path, absoluteUrl(manifest.qemuScript)).toString();
}

async function assertFetchOk(url, label) {
  const response = await fetch(url, { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`${label} fetch failed: ${response.status}`);
  }
}

function absoluteUrl(path) {
  return new URL(path, self.location.href).toString();
}

function cacheBustUrl(path) {
  const url = new URL(path);
  url.searchParams.set("browser-worker-import", "1");
  return url.toString();
}

function result(requestId, ok, status, error) {
  if (typeof requestId !== "number") {
    return;
  }
  if (ok) {
    postMessage({ type: "result", requestId, ok: true, status });
    return;
  }
  postMessage({ type: "result", requestId, ok: false, error: error || "browser_qemu_runtime_error" });
}

function log(payload) {
  postMessage({ type: "serialLog", payload });
}

function emitError(payload) {
  postMessage({ type: "simulatorError", payload });
}

function bytesToBase64(bytes) {
  let binary = "";
  const chunkSize = 0x8000;
  for (let offset = 0; offset < bytes.length; offset += chunkSize) {
    const chunk = bytes.subarray(offset, offset + chunkSize);
    binary += String.fromCharCode(...chunk);
  }
  return btoa(binary);
}
