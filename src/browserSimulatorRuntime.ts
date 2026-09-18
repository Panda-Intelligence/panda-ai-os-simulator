import type {
  SimulatorFramebufferEvent,
  SimulatorFirmwareOption,
  SimulatorHostLocation,
  SimulatorSdDirectory,
  SimulatorSdEntry,
  SimulatorSdFile,
  SimulatorSdImage,
  SimulatorTouchInput,
  SimulatorUnlisten,
} from "./simulatorBridgeTypes";

export const BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE = "browser_wasm_runtime_unavailable";

export type BrowserSimulatorRuntimeStatus = "unavailable" | "ready" | "running" | "error";

export type BrowserSimulatorRuntimeFirmwareArtifacts = Record<
  string,
  { bin: string; kernel: string | null; symbols: string | null; bootloader?: string | null; partitionTable?: string | null; otaData?: string | null }
>;
export type BrowserSimulatorRuntimeSdImage = string | string[];
export type BrowserSimulatorRuntimeSource = {
  kind: string;
  revision: string | null;
  label: string | null;
  artifactManifestSha256: string | null;
};

export type BrowserSimulatorRuntimeManifest = {
  kind: "unavailable" | "wasm-worker";
  status: BrowserSimulatorRuntimeStatus;
  workerScript: string | null;
  firmwareArtifact: string | null;
  firmwareArtifacts: BrowserSimulatorRuntimeFirmwareArtifacts | null;
  qemuScript?: string | null;
  qemuWasm?: string | null;
  qemuKernel?: string | null;
  qemuSymbols?: string | null;
  qemuBootloader?: string | null;
  qemuPartitionTable?: string | null;
  qemuOtaData?: string | null;
  qemuRom?: string | null;
  qemuSdImage?: BrowserSimulatorRuntimeSdImage | null;
  qemuSdRawBytes?: number | null;
  qemuWorkerScript?: string | null;
  artifactManifest?: string | null;
  source?: BrowserSimulatorRuntimeSource | null;
  digests?: Record<string, string> | null;
  reason?: string;
};

export type BrowserSimulatorRuntimeCommand =
  | { type: "start"; requestId: number; boardId: string; firmwarePath: string; hostLocation: SimulatorHostLocation }
  | { type: "stop"; requestId: number }
  | { type: "fullReboot"; requestId: number; boardId: string; firmwarePath: string; hostLocation: SimulatorHostLocation }
  | { type: "releaseButtons"; requestId: number }
  | { type: "injectButton"; requestId: number; buttonId: number; pressed: boolean }
  | { type: "injectTouch"; requestId: number; input: SimulatorTouchInput }
  | { type: "peripheralControl"; requestId: number; payload: number[] }
  | { type: "listSdDirectory"; requestId: number; boardId: string; path: string }
  | { type: "readSdFile"; requestId: number; boardId: string; path: string }
  | { type: "writeSdFile"; requestId: number; boardId: string; path: string; bytes: Uint8Array }
  | { type: "createSdDirectory"; requestId: number; boardId: string; path: string }
  | { type: "deleteSdPath"; requestId: number; boardId: string; path: string; recursive?: boolean }
  | { type: "exportSdImage"; requestId: number; boardId: string }
  | { type: "importSdImage"; requestId: number; boardId: string; bytes: Uint8Array };

export type BrowserSimulatorRuntimeEvent =
  | { type: "ready"; manifest: BrowserSimulatorRuntimeManifest }
  | { type: "result"; requestId: number; ok: true; status: string; data?: unknown }
  | { type: "result"; requestId: number; ok: false; error: string }
  | { type: "serialLog"; payload: string }
  | { type: "simulatorError"; payload: string }
  | { type: "framebuffer"; payload: SimulatorFramebufferEvent }
  | { type: "peripheralControl"; payload: number[] };

type RuntimeEventMap = {
  serialLog: string;
  simulatorError: string;
  framebuffer: SimulatorFramebufferEvent;
  peripheralControl: number[];
};

type RuntimeEventHandler<K extends keyof RuntimeEventMap> = (payload: RuntimeEventMap[K]) => void;
type RuntimeEvent = {
  [K in keyof RuntimeEventMap]: { type: K; payload: RuntimeEventMap[K] };
}[keyof RuntimeEventMap];
type RuntimeEventEmitter = (event: RuntimeEvent) => void;
type BrowserSimulatorRuntimeResultEvent = Extract<BrowserSimulatorRuntimeEvent, { type: "result" }>;

type BrowserSimulatorRuntimeAdapter = {
  startSim(boardId: string, firmwarePath: string, hostLocation: SimulatorHostLocation): Promise<string>;
  stopSim(): Promise<string>;
  fullRebootSim(boardId: string, firmwarePath: string, hostLocation: SimulatorHostLocation): Promise<string>;
  releaseButtons(): Promise<boolean>;
  injectButton(buttonId: number, pressed: boolean): Promise<boolean>;
  injectTouch(input: SimulatorTouchInput): Promise<boolean>;
  injectPeripheralControl(payload: number[]): Promise<boolean>;
  listSdDirectory(boardId: string, path: string): Promise<SimulatorSdDirectory>;
  readSdFile(boardId: string, path: string): Promise<SimulatorSdFile>;
  writeSdFile(boardId: string, path: string, bytes: Uint8Array): Promise<void>;
  createSdDirectory(boardId: string, path: string): Promise<void>;
  deleteSdPath(boardId: string, path: string, recursive?: boolean): Promise<void>;
  exportSdImage(boardId: string): Promise<SimulatorSdImage>;
  importSdImage(boardId: string, bytes: Uint8Array): Promise<void>;
};

const DEFAULT_RUNTIME_MANIFEST: BrowserSimulatorRuntimeManifest = {
  kind: "unavailable",
  status: "unavailable",
  workerScript: null,
  firmwareArtifact: null,
  firmwareArtifacts: null,
  reason: "WASM firmware runtime artifact is not available yet.",
};

const DEFAULT_MANIFEST_URL = `${import.meta.env.BASE_URL.replace(/\/?$/, "/")}manifest.json`;
const BROWSER_SD_LABEL = "Browser sandbox (qemu-wasm runtime)";
const WORKER_NAME = "panda-browser-simulator-runtime";
const PERIPHERAL_CONTROL_MAX_PAYLOAD = 64;
const PRIVATE_RUNTIME_PATH = /(^|\/)(?:private|users|home|murphy|panda-cloud|shared|\.git|node_modules)(?:\/|$)/i;

class UnavailableBrowserSimulatorRuntimeAdapter implements BrowserSimulatorRuntimeAdapter {
  constructor(
    private readonly manifest: BrowserSimulatorRuntimeManifest,
    private readonly emit: RuntimeEventEmitter,
  ) {}

  async startSim(boardId: string, firmwarePath: string, hostLocation: SimulatorHostLocation): Promise<string> {
    void boardId;
    void firmwarePath;
    void hostLocation;
    return this.unavailableStatus("start");
  }

  async stopSim(): Promise<string> {
    return this.unavailableStatus("stop");
  }

  async fullRebootSim(boardId: string, firmwarePath: string, hostLocation: SimulatorHostLocation): Promise<string> {
    void boardId;
    void firmwarePath;
    void hostLocation;
    return this.unavailableStatus("fullReboot");
  }

  async releaseButtons(): Promise<boolean> {
    this.emit({ type: "serialLog", payload: "[browser] releaseButtons ignored because WASM runtime is unavailable." });
    return false;
  }

  async injectButton(buttonId: number, pressed: boolean): Promise<boolean> {
    this.emit({
      type: "serialLog",
      payload: `[browser] injectButton ignored button_id=${buttonId} pressed=${pressed} because WASM runtime is unavailable.`,
    });
    return false;
  }

  async injectTouch(input: SimulatorTouchInput): Promise<boolean> {
    this.emit({
      type: "serialLog",
      payload: `[browser] injectTouch ignored action=${input.action} x=${input.x} y=${input.y} because WASM runtime is unavailable.`,
    });
    return false;
  }

  async injectPeripheralControl(payload: number[]): Promise<boolean> {
    this.emit({
      type: "serialLog",
      payload: `[browser] injectPeripheralControl ignored len=${payload.length} because WASM runtime is unavailable.`,
    });
    return false;
  }

  async listSdDirectory(boardId: string, path: string): Promise<SimulatorSdDirectory> {
    void boardId;
    void path;
    throw new Error(BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE);
  }

  async readSdFile(boardId: string, path: string): Promise<SimulatorSdFile> {
    void boardId;
    void path;
    throw new Error(BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE);
  }

  async writeSdFile(boardId: string, path: string, bytes: Uint8Array): Promise<void> {
    void boardId;
    void path;
    void bytes;
    throw new Error(BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE);
  }

  async createSdDirectory(boardId: string, path: string): Promise<void> {
    void boardId;
    void path;
    throw new Error(BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE);
  }

  async deleteSdPath(boardId: string, path: string, recursive?: boolean): Promise<void> {
    void boardId;
    void path;
    void recursive;
    throw new Error(BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE);
  }

  async exportSdImage(boardId: string): Promise<SimulatorSdImage> {
    void boardId;
    throw new Error(BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE);
  }

  async importSdImage(boardId: string, bytes: Uint8Array): Promise<void> {
    void boardId;
    void bytes;
    throw new Error(BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE);
  }

  private unavailableStatus(command: string): string {
    const reason = this.manifest.reason ? ` (${this.manifest.reason})` : "";
    this.emit({
      type: "serialLog",
      payload: `[browser] ${command} unavailable: ${this.manifest.kind}/${this.manifest.status}${reason}`,
    });
    return BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE;
  }
}

class WorkerBrowserSimulatorRuntimeAdapter implements BrowserSimulatorRuntimeAdapter {
  private readonly pendingRequests = new Map<number, (event: BrowserSimulatorRuntimeResultEvent) => void>();
  private worker: Worker | null = null;
  private nextRequestId = 1;

  constructor(
    private readonly manifest: BrowserSimulatorRuntimeManifest,
    private readonly emit: RuntimeEventEmitter,
  ) {}

  async startSim(boardId: string, firmwarePath: string, hostLocation: SimulatorHostLocation): Promise<string> {
    const result = await this.sendCommand({
      type: "start",
      requestId: this.allocateRequestId(),
      boardId,
      firmwarePath,
      hostLocation,
    });
    return this.statusOrThrow("start", result);
  }

  async stopSim(): Promise<string> {
    const result = await this.sendCommand({ type: "stop", requestId: this.allocateRequestId() });
    return this.statusOrThrow("stop", result);
  }

  async fullRebootSim(boardId: string, firmwarePath: string, hostLocation: SimulatorHostLocation): Promise<string> {
    await this.stopSim();
    const result = await this.sendCommand({
      type: "fullReboot",
      requestId: this.allocateRequestId(),
      boardId,
      firmwarePath,
      hostLocation,
    });
    return this.statusOrThrow("fullReboot", result);
  }

  async releaseButtons(): Promise<boolean> {
    const result = await this.sendCommand({ type: "releaseButtons", requestId: this.allocateRequestId() });
    return this.booleanResult("releaseButtons", result);
  }

  async injectButton(buttonId: number, pressed: boolean): Promise<boolean> {
    const result = await this.sendCommand({
      type: "injectButton",
      requestId: this.allocateRequestId(),
      buttonId,
      pressed,
    });
    return this.booleanResult("injectButton", result);
  }

  async injectTouch(input: SimulatorTouchInput): Promise<boolean> {
    const result = await this.sendCommand({ type: "injectTouch", requestId: this.allocateRequestId(), input });
    return this.booleanResult("injectTouch", result);
  }

  async injectPeripheralControl(payload: number[]): Promise<boolean> {
    const result = await this.sendCommand({
      type: "peripheralControl",
      requestId: this.allocateRequestId(),
      payload,
    });
    return this.booleanResult("injectPeripheralControl", result);
  }

  async listSdDirectory(boardId: string, path: string): Promise<SimulatorSdDirectory> {
    const result = await this.sendCommand({
      type: "listSdDirectory",
      requestId: this.allocateRequestId(),
      boardId,
      path,
    });
    if (!result.ok) throw this.commandError("listSdDirectory", result);
    return parseSdDirectory(result.data);
  }

  async readSdFile(boardId: string, path: string): Promise<SimulatorSdFile> {
    const result = await this.sendCommand({
      type: "readSdFile",
      requestId: this.allocateRequestId(),
      boardId,
      path,
    });
    if (!result.ok) throw this.commandError("readSdFile", result);
    return parseSdFile(result.data);
  }

  async writeSdFile(boardId: string, path: string, bytes: Uint8Array): Promise<void> {
    const result = await this.sendCommand({
      type: "writeSdFile",
      requestId: this.allocateRequestId(),
      boardId,
      path,
      bytes,
    });
    if (!result.ok) throw this.commandError("writeSdFile", result);
  }

  async createSdDirectory(boardId: string, path: string): Promise<void> {
    const result = await this.sendCommand({
      type: "createSdDirectory",
      requestId: this.allocateRequestId(),
      boardId,
      path,
    });
    if (!result.ok) throw this.commandError("createSdDirectory", result);
  }

  async deleteSdPath(boardId: string, path: string, recursive?: boolean): Promise<void> {
    const result = await this.sendCommand({
      type: "deleteSdPath",
      requestId: this.allocateRequestId(),
      boardId,
      path,
      recursive,
    });
    if (!result.ok) throw this.commandError("deleteSdPath", result);
  }

  async exportSdImage(boardId: string): Promise<SimulatorSdImage> {
    const result = await this.sendCommand({
      type: "exportSdImage",
      requestId: this.allocateRequestId(),
      boardId,
    });
    if (!result.ok) throw this.commandError("exportSdImage", result);
    return parseSdImage(result.data);
  }

  async importSdImage(boardId: string, bytes: Uint8Array): Promise<void> {
    const transferBytes = bytes.slice();
    const result = await this.sendCommand({
      type: "importSdImage",
      requestId: this.allocateRequestId(),
      boardId,
      bytes: transferBytes,
    }, [transferBytes.buffer]);
    if (!result.ok) throw this.commandError("importSdImage", result);
  }

  private allocateRequestId(): number {
    const requestId = this.nextRequestId;
    this.nextRequestId += 1;
    return requestId;
  }

  private async sendCommand(command: BrowserSimulatorRuntimeCommand, transfer: Transferable[] = []): Promise<BrowserSimulatorRuntimeResultEvent> {
    const worker = this.ensureWorker();
    if (!worker) {
      return {
        type: "result",
        requestId: command.requestId,
        ok: false,
        error: "browser_wasm_worker_unavailable",
      };
    }

    return new Promise((resolve) => {
      this.pendingRequests.set(command.requestId, resolve);
      worker.postMessage({
        ...command,
        manifest: this.manifest,
      }, transfer);
    });
  }

  private ensureWorker(): Worker | null {
    if (this.worker) {
      return this.worker;
    }
    if (typeof Worker !== "function" || !this.manifest.workerScript) {
      this.emit({
        type: "simulatorError",
        payload: "Browser Worker support is required for the WASM simulator runtime.",
      });
      return null;
    }

    try {
      this.worker = new Worker(new URL("./browserSimulatorRuntimeWorker.js", import.meta.url), {
        type: "module",
        name: WORKER_NAME,
      });
      this.worker.addEventListener("message", this.handleWorkerMessage);
      this.worker.addEventListener("error", this.handleWorkerError);
      this.emit({
        type: "serialLog",
        payload: `[browser] WASM runtime worker connected: ${this.manifest.workerScript}`,
      });
      return this.worker;
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      this.emit({ type: "simulatorError", payload: `Failed to create WASM simulator runtime worker: ${message}` });
      return null;
    }
  }

  private readonly handleWorkerMessage = (event: MessageEvent<BrowserSimulatorRuntimeEvent>) => {
    const runtimeEvent = event.data;
    if (!isBrowserSimulatorRuntimeEvent(runtimeEvent)) {
      this.emit({ type: "simulatorError", payload: "Ignoring malformed browser simulator runtime event." });
      return;
    }

    switch (runtimeEvent.type) {
      case "ready":
        this.emit({ type: "serialLog", payload: `[browser] WASM runtime ready: ${runtimeEvent.manifest.status}` });
        return;
      case "result":
        this.resolvePendingResult(runtimeEvent);
        return;
      case "serialLog":
        this.emit({ type: "serialLog", payload: runtimeEvent.payload });
        return;
      case "simulatorError":
        this.emit({ type: "simulatorError", payload: runtimeEvent.payload });
        return;
      case "framebuffer":
        this.emit({ type: "framebuffer", payload: runtimeEvent.payload });
        return;
      case "peripheralControl":
        this.emit({ type: "peripheralControl", payload: runtimeEvent.payload });
        return;
      default:
        return;
    }
  };

  private readonly handleWorkerError = (event: ErrorEvent) => {
    const message = `browser_wasm_worker_fatal: ${event.message || "worker terminated unexpectedly"}`;
    for (const [requestId, resolve] of this.pendingRequests) {
      resolve({ type: "result", requestId, ok: false, error: message });
    }
    this.pendingRequests.clear();
    this.terminateWorker();
    this.emit({ type: "simulatorError", payload: message });
  };

  private resolvePendingResult(result: BrowserSimulatorRuntimeResultEvent) {
    const resolve = this.pendingRequests.get(result.requestId);
    if (!resolve) {
      return;
    }
    this.pendingRequests.delete(result.requestId);
    resolve(result);
  }

  private terminateWorker() {
    if (!this.worker) {
      return;
    }
    this.worker.removeEventListener("message", this.handleWorkerMessage);
    this.worker.removeEventListener("error", this.handleWorkerError);
    this.worker.terminate();
    this.worker = null;
    this.pendingRequests.clear();
  }

  private statusOrThrow(command: string, result: BrowserSimulatorRuntimeResultEvent): string {
    if (result.ok && command === "stop") {
      this.terminateWorker();
    }
    if (result.ok) {
      return result.status;
    }
    this.emit({ type: "simulatorError", payload: `[browser] ${command} failed: ${result.error}` });
    throw new Error(result.error);
  }

  private booleanResult(command: string, result: BrowserSimulatorRuntimeResultEvent): boolean {
    if (result.ok) {
      return true;
    }
    this.emit({ type: "serialLog", payload: `[browser] ${command} ignored: ${result.error}` });
    return false;
  }

  private commandError(command: string, result: Extract<BrowserSimulatorRuntimeResultEvent, { ok: false }>): Error {
    this.emit({ type: "simulatorError", payload: `[browser] ${command} failed: ${result.error}` });
    return new Error(result.error);
  }
}

export class BrowserSimulatorRuntimeHost {
  private readonly manifestUrl: string;
  private manifestPromise: Promise<BrowserSimulatorRuntimeManifest> | null = null;
  private adapterPromise: Promise<BrowserSimulatorRuntimeAdapter> | null = null;
  private readonly serialLogHandlers = new Set<RuntimeEventHandler<"serialLog">>();
  private readonly simulatorErrorHandlers = new Set<RuntimeEventHandler<"simulatorError">>();
  private readonly framebufferHandlers = new Set<RuntimeEventHandler<"framebuffer">>();
  private readonly peripheralControlHandlers = new Set<RuntimeEventHandler<"peripheralControl">>();
  private startupLogged = false;

  constructor(manifestUrl = DEFAULT_MANIFEST_URL) {
    this.manifestUrl = validateBrowserSimulatorManifestUrl(manifestUrl);
  }

  async listFirmwareOptions(boardId: string): Promise<SimulatorFirmwareOption[]> {
    const manifest = await this.runtimeManifest();
    if (manifest.kind !== "wasm-worker") return [];
    const boardArtifact = manifest.firmwareArtifacts?.[boardId];
    const path = boardArtifact?.bin
      ?? ((boardId === "mofei" || boardId === "s3r8") ? manifest.firmwareArtifact : null);
    if (!path) return [];
    const sourceLabel = manifest.source?.label
      || (manifest.source?.revision ? manifest.source.revision.slice(0, 10) : "bundled");
    const fileName = path.split("/").pop() || path;
    return [{
      id: "bundled:" + boardId + ":" + fileName,
      boardId,
      label: fileName + " · " + sourceLabel,
      path,
      bundled: true,
      sourceLabel,
    }];
  }

  async chooseFirmware(boardId: string): Promise<string | undefined> {
    const manifest = await this.runtimeManifest();
    if (manifest.kind === "wasm-worker") {
      const boardArtifact = manifest.firmwareArtifacts?.[boardId];
      if (boardArtifact?.bin) {
        return boardArtifact.bin;
      }
      if (boardId === "mofei" || boardId === "s3r8") {
        return manifest.firmwareArtifact || undefined;
      }
      throw new Error(
        `Browser simulator firmware for '${boardId}' is missing; rebuild with apps/panda-os/tools/release-beta.sh`,
      );
    }
    return undefined;
  }

  async readSdRootPath(): Promise<string> {
    return BROWSER_SD_LABEL;
  }

  async listSdDirectory(boardId: string, path: string): Promise<SimulatorSdDirectory> {
    return (await this.runtimeAdapter()).listSdDirectory(boardId, path);
  }

  async readSdFile(boardId: string, path: string): Promise<SimulatorSdFile> {
    return (await this.runtimeAdapter()).readSdFile(boardId, path);
  }

  async writeSdFile(boardId: string, path: string, bytes: Uint8Array): Promise<void> {
    return (await this.runtimeAdapter()).writeSdFile(boardId, path, bytes);
  }

  async createSdDirectory(boardId: string, path: string): Promise<void> {
    return (await this.runtimeAdapter()).createSdDirectory(boardId, path);
  }

  async deleteSdPath(boardId: string, path: string, recursive?: boolean): Promise<void> {
    return (await this.runtimeAdapter()).deleteSdPath(boardId, path, recursive);
  }

  async exportSdImage(boardId: string): Promise<SimulatorSdImage> {
    return (await this.runtimeAdapter()).exportSdImage(boardId);
  }

  async importSdImage(boardId: string, bytes: Uint8Array): Promise<void> {
    return (await this.runtimeAdapter()).importSdImage(boardId, bytes);
  }

  async startSim(boardId: string, firmwarePath: string, hostLocation: SimulatorHostLocation): Promise<string> {
    return (await this.runtimeAdapter()).startSim(boardId, firmwarePath, hostLocation);
  }

  async stopSim(): Promise<string> {
    return (await this.runtimeAdapter()).stopSim();
  }

  async fullRebootSim(boardId: string, firmwarePath: string, hostLocation: SimulatorHostLocation): Promise<string> {
    return (await this.runtimeAdapter()).fullRebootSim(boardId, firmwarePath, hostLocation);
  }

  async releaseButtons(): Promise<boolean> {
    return (await this.runtimeAdapter()).releaseButtons();
  }

  async injectButton(buttonId: number, pressed: boolean): Promise<boolean> {
    return (await this.runtimeAdapter()).injectButton(buttonId, pressed);
  }

  async injectTouch(input: SimulatorTouchInput): Promise<boolean> {
    return (await this.runtimeAdapter()).injectTouch(input);
  }

  async injectPeripheralControl(payload: number[]): Promise<boolean> {
    return (await this.runtimeAdapter()).injectPeripheralControl(payload);
  }

  async subscribeSerialLog(handler: RuntimeEventHandler<"serialLog">): Promise<SimulatorUnlisten> {
    this.serialLogHandlers.add(handler);
    this.logStartupOnce();
    return () => {
      this.serialLogHandlers.delete(handler);
    };
  }

  async subscribeSimulatorError(handler: RuntimeEventHandler<"simulatorError">): Promise<SimulatorUnlisten> {
    this.simulatorErrorHandlers.add(handler);
    return () => {
      this.simulatorErrorHandlers.delete(handler);
    };
  }

  async subscribeFramebuffer(handler: RuntimeEventHandler<"framebuffer">): Promise<SimulatorUnlisten> {
    this.framebufferHandlers.add(handler);
    return () => {
      this.framebufferHandlers.delete(handler);
    };
  }

  async subscribePeripheralControl(handler: RuntimeEventHandler<"peripheralControl">): Promise<SimulatorUnlisten> {
    this.peripheralControlHandlers.add(handler);
    return () => {
      this.peripheralControlHandlers.delete(handler);
    };
  }

  async runtimeManifest(): Promise<BrowserSimulatorRuntimeManifest> {
    if (!this.manifestPromise) {
      this.manifestPromise = this.loadRuntimeManifest();
    }
    return this.manifestPromise;
  }

  private async runtimeAdapter(): Promise<BrowserSimulatorRuntimeAdapter> {
    if (!this.adapterPromise) {
      this.adapterPromise = this.runtimeManifest().then((manifest) => this.createRuntimeAdapter(manifest));
    }
    return this.adapterPromise;
  }

  private createRuntimeAdapter(manifest: BrowserSimulatorRuntimeManifest): BrowserSimulatorRuntimeAdapter {
    if (manifest.kind === "wasm-worker" && manifest.workerScript) {
      return new WorkerBrowserSimulatorRuntimeAdapter(manifest, this.emitRuntimeEvent);
    }
    return new UnavailableBrowserSimulatorRuntimeAdapter(manifest, this.emitRuntimeEvent);
  }

  private async loadRuntimeManifest(): Promise<BrowserSimulatorRuntimeManifest> {
    if (typeof fetch !== "function") {
      return DEFAULT_RUNTIME_MANIFEST;
    }

    try {
      const response = await fetch(this.manifestUrl, { cache: "no-store" });
      if (!response.ok) {
        return DEFAULT_RUNTIME_MANIFEST;
      }
      const payload = await response.json();
      return resolveBrowserSimulatorRuntimeManifestUrls(parseBrowserSimulatorRuntimeManifest(payload), this.manifestUrl);
    } catch {
      return DEFAULT_RUNTIME_MANIFEST;
    }
  }

  private logStartupOnce() {
    if (this.startupLogged) {
      return;
    }
    this.startupLogged = true;
    this.emitRuntimeEvent({ type: "serialLog", payload: "[browser] simulator UI loaded" });
    void this.runtimeManifest().then((manifest) => {
      if (manifest.kind === "wasm-worker" && manifest.workerScript) {
        this.emitRuntimeEvent({
          type: "serialLog",
          payload: `[browser] WASM runtime manifest discovered: ${manifest.workerScript}`,
        });
        return;
      }
      this.emitRuntimeEvent({
        type: "serialLog",
        payload: `[browser] WASM firmware runtime is unavailable: ${manifest.reason ?? manifest.status}`,
      });
    });
  }

  private readonly emitRuntimeEvent: RuntimeEventEmitter = (event) => {
    switch (event.type) {
      case "serialLog":
        for (const handler of this.serialLogHandlers) {
          handler(event.payload);
        }
        return;
      case "simulatorError":
        for (const handler of this.simulatorErrorHandlers) {
          handler(event.payload);
        }
        return;
      case "framebuffer":
        for (const handler of this.framebufferHandlers) {
          handler(event.payload);
        }
        return;
      case "peripheralControl":
        for (const handler of this.peripheralControlHandlers) {
          handler(event.payload);
        }
        return;
      default:
        return;
    }
  };
}

export const browserSimulatorRuntimeHost = new BrowserSimulatorRuntimeHost();

export function validateBrowserSimulatorManifestUrl(value: string, baseUrl = import.meta.env.BASE_URL): string {
  if (typeof value !== "string" || !value.trim()) {
    throw new Error("browser_runtime_manifest_url_missing");
  }
  const locationBase = typeof globalThis.location?.href === "string" ? globalThis.location.href : "http://localhost/";
  const base = new URL(baseUrl, locationBase);
  const resolved = new URL(value, base);
  if (resolved.protocol !== "http:" && resolved.protocol !== "https:") {
    throw new Error("browser_runtime_manifest_url_scheme_invalid");
  }
  if (resolved.origin !== base.origin || resolved.username || resolved.password || PRIVATE_RUNTIME_PATH.test(resolved.pathname)) {
    throw new Error("browser_runtime_manifest_url_origin_invalid");
  }
  return resolved.toString();
}

export function parseBrowserSimulatorRuntimeManifest(payload: unknown): BrowserSimulatorRuntimeManifest {
  if (!payload || typeof payload !== "object") {
    return DEFAULT_RUNTIME_MANIFEST;
  }

  const record = payload as Record<string, unknown>;
  const runtime = record.runtime;
  if (!runtime || typeof runtime !== "object") {
    return DEFAULT_RUNTIME_MANIFEST;
  }

  const runtimeRecord = runtime as Record<string, unknown>;
  const kind = runtimeRecord.kind === "wasm-worker" ? "wasm-worker" : "unavailable";
  const status = parseRuntimeStatus(runtimeRecord.status);
  const workerScript = typeof runtimeRecord.workerScript === "string" && runtimeRecord.workerScript.trim()
    ? runtimeRecord.workerScript
    : null;
  const firmwareArtifact = typeof runtimeRecord.firmwareArtifact === "string" && runtimeRecord.firmwareArtifact.trim()
    ? runtimeRecord.firmwareArtifact
    : null;
  const firmwareArtifacts = parseFirmwareArtifacts(runtimeRecord.firmwareArtifacts);
  const qemuScript = typeof runtimeRecord.qemuScript === "string" && runtimeRecord.qemuScript.trim()
    ? runtimeRecord.qemuScript
    : null;
  const qemuWasm = typeof runtimeRecord.qemuWasm === "string" && runtimeRecord.qemuWasm.trim()
    ? runtimeRecord.qemuWasm
    : null;
  const qemuKernel = typeof runtimeRecord.qemuKernel === "string" && runtimeRecord.qemuKernel.trim()
    ? runtimeRecord.qemuKernel
    : null;
  const qemuSymbols = typeof runtimeRecord.qemuSymbols === "string" && runtimeRecord.qemuSymbols.trim()
    ? runtimeRecord.qemuSymbols
    : null;
  const qemuBootloader =
    typeof runtimeRecord.qemuBootloader === "string" && runtimeRecord.qemuBootloader.trim()
      ? runtimeRecord.qemuBootloader
      : null;
  const qemuPartitionTable =
    typeof runtimeRecord.qemuPartitionTable === "string" && runtimeRecord.qemuPartitionTable.trim()
      ? runtimeRecord.qemuPartitionTable
      : null;
  const qemuOtaData =
    typeof runtimeRecord.qemuOtaData === "string" && runtimeRecord.qemuOtaData.trim()
      ? runtimeRecord.qemuOtaData
      : null;
  const qemuRom = typeof runtimeRecord.qemuRom === "string" && runtimeRecord.qemuRom.trim()
    ? runtimeRecord.qemuRom
    : null;
  const qemuSdImage = parseSdImageArtifacts(runtimeRecord.qemuSdImage);
  const qemuSdRawBytes = typeof runtimeRecord.qemuSdRawBytes === "number" && Number.isSafeInteger(runtimeRecord.qemuSdRawBytes) && runtimeRecord.qemuSdRawBytes >= 512 && runtimeRecord.qemuSdRawBytes <= 256*1024*1024 ? runtimeRecord.qemuSdRawBytes : null;
  const qemuWorkerScript =
    typeof runtimeRecord.qemuWorkerScript === "string" && runtimeRecord.qemuWorkerScript.trim()
      ? runtimeRecord.qemuWorkerScript
      : null;
  const artifactManifest =
    typeof runtimeRecord.artifactManifest === "string" && runtimeRecord.artifactManifest.trim()
      ? runtimeRecord.artifactManifest
      : null;
  const source = parseRuntimeSource(runtimeRecord.source);
  const digests = parseRuntimeDigests(runtimeRecord.digests);
  const reason = typeof runtimeRecord.reason === "string" && runtimeRecord.reason.trim()
    ? runtimeRecord.reason
    : undefined;

  if (kind !== "wasm-worker" || !workerScript) {
    return {
      kind: "unavailable",
      status: "unavailable",
      workerScript: null,
      firmwareArtifact,
      firmwareArtifacts,
      qemuScript,
      qemuWasm,
      qemuKernel,
      qemuSymbols,
      qemuBootloader,
      qemuPartitionTable,
      qemuOtaData,
      qemuRom,
      qemuSdImage,
      qemuSdRawBytes,
      qemuWorkerScript,
      artifactManifest,
      source,
      digests,
      reason: reason ?? DEFAULT_RUNTIME_MANIFEST.reason,
    };
  }

  return {
    kind,
    status,
    workerScript,
    firmwareArtifact,
    firmwareArtifacts,
    qemuScript,
    qemuWasm,
    qemuKernel,
    qemuSymbols,
    qemuBootloader,
    qemuPartitionTable,
    qemuOtaData,
    qemuRom,
    qemuSdImage,
    qemuSdRawBytes,
    qemuWorkerScript,
    artifactManifest,
    source,
    digests,
    reason,
  };
}

export function resolveBrowserSimulatorRuntimeManifestUrls(
  manifest: BrowserSimulatorRuntimeManifest,
  manifestUrl: string,
): BrowserSimulatorRuntimeManifest {
  if (manifest.kind !== "wasm-worker") {
    return manifest;
  }
  const resolveAsset = (value: string | null | undefined): string | null | undefined => {
    if (!value) return value;
    const resolved = new URL(value, manifestUrl);
    const base = new URL(manifestUrl);
    if (resolved.origin !== base.origin || resolved.username || resolved.password || PRIVATE_RUNTIME_PATH.test(resolved.pathname)) {
      throw new Error("browser_runtime_asset_url_invalid");
    }
    return resolved.toString();
  };
  return {
    ...manifest,
    workerScript: resolveAsset(manifest.workerScript) ?? null,
    firmwareArtifact: resolveAsset(manifest.firmwareArtifact) ?? null,
    firmwareArtifacts: manifest.firmwareArtifacts
      ? Object.fromEntries(Object.entries(manifest.firmwareArtifacts).map(([boardId, artifacts]) => [boardId, {
        bin: resolveAsset(artifacts.bin) ?? "",
        kernel: resolveAsset(artifacts.kernel) ?? null,
        symbols: resolveAsset(artifacts.symbols) ?? null,
        bootloader: resolveAsset(artifacts.bootloader) ?? null,
        partitionTable: resolveAsset(artifacts.partitionTable) ?? null,
        otaData: resolveAsset(artifacts.otaData) ?? null,
      }]))
      : null,
    qemuScript: resolveAsset(manifest.qemuScript) ?? null,
    qemuWasm: resolveAsset(manifest.qemuWasm) ?? null,
    qemuKernel: resolveAsset(manifest.qemuKernel) ?? null,
    qemuSymbols: resolveAsset(manifest.qemuSymbols) ?? null,
    qemuBootloader: resolveAsset(manifest.qemuBootloader) ?? null,
    qemuPartitionTable: resolveAsset(manifest.qemuPartitionTable) ?? null,
    qemuOtaData: resolveAsset(manifest.qemuOtaData) ?? null,
    qemuRom: resolveAsset(manifest.qemuRom) ?? null,
    qemuSdImage: Array.isArray(manifest.qemuSdImage)
      ? manifest.qemuSdImage.map((value) => resolveAsset(value) ?? "")
      : typeof manifest.qemuSdImage === "string"
        ? resolveAsset(manifest.qemuSdImage) ?? null
        : null,
    qemuWorkerScript: resolveAsset(manifest.qemuWorkerScript) ?? null,
    artifactManifest: resolveAsset(manifest.artifactManifest) ?? null,
  };
}

function parseSdImageArtifacts(value: unknown): BrowserSimulatorRuntimeSdImage | null {
  if (typeof value === "string" && value.trim()) {
    return value;
  }
  if (!Array.isArray(value) || value.length === 0) {
    return null;
  }
  const artifacts = value.filter((entry): entry is string => typeof entry === "string" && Boolean(entry.trim()));
  return artifacts.length === value.length ? artifacts : null;
}

function parseFirmwareArtifacts(value: unknown): BrowserSimulatorRuntimeFirmwareArtifacts | null {
  if (!value || typeof value !== "object") {
    return null;
  }
  const artifacts: BrowserSimulatorRuntimeFirmwareArtifacts = {};
  for (const [boardId, entry] of Object.entries(value as Record<string, unknown>)) {
    if (!entry || typeof entry !== "object") {
      continue;
    }
    const record = entry as Record<string, unknown>;
    const bin = typeof record.bin === "string" && record.bin.trim() ? record.bin : null;
    if (!bin) {
      continue;
    }
    artifacts[boardId] = {
      bin,
      kernel: typeof record.kernel === "string" && record.kernel.trim() ? record.kernel : null,
      symbols: typeof record.symbols === "string" && record.symbols.trim() ? record.symbols : null,
      bootloader: typeof record.bootloader === "string" && record.bootloader.trim() ? record.bootloader : null,
      partitionTable: typeof record.partitionTable === "string" && record.partitionTable.trim() ? record.partitionTable : null,
      otaData: typeof record.otaData === "string" && record.otaData.trim() ? record.otaData : null,
    };
  }
  return Object.keys(artifacts).length > 0 ? artifacts : null;
}

function parseRuntimeStatus(status: unknown): BrowserSimulatorRuntimeStatus {
  if (status === "ready" || status === "running" || status === "error") {
    return status;
  }
  return "unavailable";
}

function parseRuntimeSource(value: unknown): BrowserSimulatorRuntimeSource | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const record = value as Record<string, unknown>;
  return {
    kind: typeof record.kind === "string" && record.kind ? record.kind : "unknown",
    revision: typeof record.revision === "string" && record.revision ? record.revision : null,
    label: typeof record.label === "string" && record.label ? record.label : null,
    artifactManifestSha256:
      typeof record.artifactManifestSha256 === "string" && /^[0-9a-f]{64}$/i.test(record.artifactManifestSha256)
        ? record.artifactManifestSha256
        : null,
  };
}

function parseRuntimeDigests(value: unknown): Record<string, string> | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const entries = Object.entries(value as Record<string, unknown>);
  if (entries.some(([key, digest]) => !key || typeof digest !== "string" || !/^[0-9a-f]{64}$/i.test(digest))) {
    return null;
  }
  return Object.fromEntries(entries.map(([key, digest]) => [key, digest as string]));
}

function parseSdDirectory(value: unknown): SimulatorSdDirectory {
  if (!value || typeof value !== "object") {
    throw new Error("browser_sd_directory_payload_invalid");
  }
  const record = value as Record<string, unknown>;
  const path = typeof record.path === "string" && record.path ? record.path : "/";
  const entries = Array.isArray(record.entries)
    ? record.entries.flatMap((entry) => {
        if (!entry || typeof entry !== "object") return [];
        const entryRecord = entry as Record<string, unknown>;
        const name = typeof entryRecord.name === "string" ? entryRecord.name : "";
        const entryPath = typeof entryRecord.path === "string" ? entryRecord.path : "";
        const kind: SimulatorSdEntry["kind"] | null =
          entryRecord.kind === "directory" ? "directory" : entryRecord.kind === "file" ? "file" : null;
        const size = typeof entryRecord.size === "number" && Number.isFinite(entryRecord.size) ? entryRecord.size : 0;
        if (!name || !entryPath || !kind) return [];
        return [{
          name,
          path: entryPath,
          kind,
          size,
          attributes: typeof entryRecord.attributes === "number" ? entryRecord.attributes : undefined,
        }];
      })
    : [];
  return { path, entries };
}

function parseSdFile(value: unknown): SimulatorSdFile {
  if (!value || typeof value !== "object") {
    throw new Error("browser_sd_file_payload_invalid");
  }
  const record = value as Record<string, unknown>;
  const path = typeof record.path === "string" && record.path ? record.path : "/";
  const bytes = record.bytes instanceof ArrayBuffer
    ? record.bytes
    : ArrayBuffer.isView(record.bytes)
      ? record.bytes.buffer.slice(record.bytes.byteOffset, record.bytes.byteOffset + record.bytes.byteLength)
      : null;
  if (!bytes) {
    throw new Error("browser_sd_file_bytes_invalid");
  }
  return { path, bytes };
}

function parseSdImage(value: unknown): SimulatorSdImage {
  if (!value || typeof value !== "object") {
    throw new Error("browser_sd_image_payload_invalid");
  }
  const record = value as Record<string, unknown>;
  const parsed = parseSdFile(value);
  const byteLength = typeof record.byteLength === "number" && Number.isSafeInteger(record.byteLength)
    ? record.byteLength
    : parsed.bytes.byteLength;
  if (byteLength !== parsed.bytes.byteLength || byteLength < 1) {
    throw new Error("browser_sd_image_size_invalid");
  }
  return {
    path: parsed.path,
    byteLength,
    bytes: parsed.bytes,
    templateFingerprint: typeof record.templateFingerprint === "string" ? record.templateFingerprint : null,
    storedTemplateFingerprint: typeof record.storedTemplateFingerprint === "string" ? record.storedTemplateFingerprint : null,
    templateConflict: record.templateConflict === true,
  };
}

function isBrowserSimulatorRuntimeEvent(payload: unknown): payload is BrowserSimulatorRuntimeEvent {
  if (!payload || typeof payload !== "object") {
    return false;
  }

  const record = payload as Record<string, unknown>;
  switch (record.type) {
    case "ready":
      return Boolean(record.manifest && typeof record.manifest === "object");
    case "result":
      return typeof record.requestId === "number" && typeof record.ok === "boolean";
    case "serialLog":
    case "simulatorError":
      return typeof record.payload === "string";
    case "framebuffer":
      return Boolean(record.payload && typeof record.payload === "object");
    case "peripheralControl":
      return isPeripheralControlPayload(record.payload);
    default:
      return false;
  }
}

function isPeripheralControlPayload(payload: unknown): payload is number[] {
  return Array.isArray(payload)
    && payload.length <= PERIPHERAL_CONTROL_MAX_PAYLOAD
    && payload.every((byte) => Number.isInteger(byte) && byte >= 0 && byte <= 0xff);
}
