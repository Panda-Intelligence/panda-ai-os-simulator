import { browserSimulatorRuntimeHost } from "./browserSimulatorRuntime";
import { createSimulatedLocation } from "./simulatorLocation";

export type PosDevBuild = {
  schemaVersion: 1;
  revision: number;
  appId: string;
  version: string;
  packageDigest: string;
  packageUrl: string;
  board: string;
};

type PosConsoleResult = {
  schemaVersion: 1;
  requestId: string;
  operation: string;
  state: string;
  ok: boolean;
  terminal?: boolean;
  error?: string;
  appId?: string;
  version?: string;
  packageDigest?: string;
  capabilities?: { install?: boolean; open?: boolean };
};

export type PosDevHost = {
  subscribeSerialLog(handler: (line: string) => void): Promise<() => void>;
  subscribeSimulatorError(handler: (line: string) => void): Promise<() => void>;
  sendSerialLine(line: string): Promise<void>;
  stopSim(): Promise<string>;
  startSim(board: string, firmware: string, location: ReturnType<typeof createSimulatedLocation>): Promise<string>;
  chooseFirmware(board: string): Promise<string | undefined>;
  createSdDirectory(board: string, path: string): Promise<void>;
  writeSdFile(board: string, path: string, bytes: Uint8Array): Promise<void>;
};

const pause = (milliseconds: number) => new Promise<void>((complete) => setTimeout(complete, milliseconds));

// 请求只有在固件返回相同 requestId 的实际响应后才完成；发送成功不等于安装成功。
export function createPosConsoleClient(host: Pick<PosDevHost, "subscribeSerialLog" | "sendSerialLine">, timeoutMs = 180000, signal?: AbortSignal) {
  let sequence = 0;
  return async (operation: "capabilities" | "install" | "open", argument?: string): Promise<PosConsoleResult> => {
    const nextRequestId = () => `pos_${Date.now().toString(36)}_${(++sequence).toString(36)}`;
    let requestId = nextRequestId();
    let buffered = "";
    let latest: PosConsoleResult | undefined;
    let terminalResult: PosConsoleResult | undefined;
    let generation = 0;
    const unlisten = await host.subscribeSerialLog((chunk) => {
      buffered += chunk;
      if (buffered.length > 65536) buffered = buffered.slice(-32768);
      const lines = buffered.split(/\r?\n/);
      buffered = lines.pop() ?? "";
      for (const line of lines) {
        const prefix = `POS_APP:${requestId}:`;
        const position = line.indexOf(prefix);
        if (position < 0) continue;
        try {
          const result = JSON.parse(line.slice(position + prefix.length)) as PosConsoleResult;
          if (result.schemaVersion !== 1 || result.requestId !== requestId || typeof result.ok !== "boolean") continue;
          // 来宾启动时可能一次排出多条旧 status；已确认的实际终态不能被其覆盖。
          if (result.operation === operation && (result.terminal || result.state === "completed" || result.state === "failed")) {
            terminalResult ??= result;
          }
          latest = result;
          generation++;
        } catch { /* 控制台其他输出不能冒充完成响应。 */ }
      }
    });
    try {
      if (signal?.aborted) throw new Error("pos_runtime_cancelled");
      const deadline = Date.now() + timeoutMs;
      let sent = false;
      let retryDelayMs = 200;
      let lastRefusal: string | undefined;
      while (Date.now() < deadline) {
        if (signal?.aborted) throw new Error("pos_runtime_cancelled");
        const before = generation;
        try {
          await host.sendSerialLine(sent ? `CMD:POS_APP:${requestId}:status` : `CMD:POS_APP:${requestId}:${operation}${argument ? ":" + argument : ""}`);
          sent = true;
        } catch (error) {
          if (Date.now() + 1000 >= deadline) throw error;
        }
        const replyDeadline = Math.min(deadline, Date.now() + 1000);
        while (generation === before && Date.now() < replyDeadline) {
          if (signal?.aborted) throw new Error("pos_runtime_cancelled");
          await pause(25);
        }
        const result = (terminalResult ?? latest) as PosConsoleResult | undefined;
        if (!result) continue;
        if (!result.ok) {
          if (result.operation === operation && result.terminal && result.state === "failed"
            && (result.error === "worker_busy" || result.error === "busy")) {
            // 固件明确拒绝入队时才换请求 ID 重试；ACK 不明的已发送操作只查询原 ID。
            lastRefusal = result.error;
            const retryAt = Math.min(deadline, Date.now() + retryDelayMs);
            while (Date.now() < retryAt) {
              if (signal?.aborted) throw new Error("pos_runtime_cancelled");
              await pause(Math.min(25, retryAt - Date.now()));
            }
            requestId = nextRequestId();
            latest = undefined;
            terminalResult = undefined;
            sent = false;
            retryDelayMs = Math.min(retryDelayMs * 2, 1000);
            continue;
          }
          if (result.error === "request_not_found") { sent = false; latest = undefined; continue; }
          throw new Error(result.error ?? `pos_runtime_${operation}_failed`);
        }
        if (result.terminal || result.state === "completed" || result.state === "failed") {
          if (result.state === "failed") throw new Error(result.error ?? `pos_runtime_${operation}_failed`);
          return result;
        }
        await pause(100);
      }
      throw new Error(`pos_runtime_${operation}_timeout${lastRefusal ? ": " + lastRefusal : ""}`);
    } finally { unlisten(); }
  };
}

export function isPosDevBuild(value: unknown): value is PosDevBuild {
  if (!value || typeof value !== "object") return false;
  const build = value as Partial<PosDevBuild>;
  return build.schemaVersion === 1 && Number.isSafeInteger(build.revision) && (build.revision ?? 0) > 0
    && typeof build.appId === "string" && /^[A-Za-z0-9][A-Za-z0-9_.-]{1,127}$/.test(build.appId)
    && typeof build.version === "string" && typeof build.board === "string"
    && typeof build.packageDigest === "string" && /^sha256:[0-9a-f]{64}$/.test(build.packageDigest)
    && typeof build.packageUrl === "string" && /^\/__pos\/package\/\d+\.pap$/.test(build.packageUrl);
}

export type PreparedPosDevBuild = { build: PosDevBuild; bytes: Uint8Array; firmware: string; openRequestId: string };

export async function createPosRuntimeHealthMonitor(
  host: Pick<PosDevHost, "subscribeSerialLog">,
  changed: (result: PosConsoleResult) => void,
) {
  let expected: { requestId: string; appId: string; version: string; packageDigest: string } | undefined;
  let buffered = "";
  const recent = new Map<string, PosConsoleResult>();
  const deliver = (result: PosConsoleResult) => {
    if (expected && result.requestId === expected.requestId && result.appId === expected.appId
      && result.version === expected.version && result.packageDigest === expected.packageDigest) changed(result);
  };
  const dispose = await host.subscribeSerialLog((chunk) => {
    buffered += chunk;
    if (buffered.length > 65536) buffered = buffered.slice(-32768);
    const lines = buffered.split(/\r?\n/);
    buffered = lines.pop() ?? "";
    for (const line of lines) {
      const start = line.indexOf("POS_APP:");
      if (start < 0) continue;
      const jsonStart = line.indexOf(":", start + 8);
      if (jsonStart < 0) continue;
      try {
        const result = JSON.parse(line.slice(jsonStart + 1)) as PosConsoleResult;
        if (result.schemaVersion !== 1 || result.operation !== "runtime" || !result.requestId
          || !["running", "failed", "stopped"].includes(result.state) || typeof result.ok !== "boolean") continue;
        recent.set(result.requestId, result);
        if (recent.size > 16) recent.delete(recent.keys().next().value!);
        deliver(result);
      } catch { /* 非结构化日志不改变 Runtime 健康状态。 */ }
    }
  });
  return {
    watch(value: typeof expected) {
      expected = value;
      const previous = value ? recent.get(value.requestId) : undefined;
      if (previous) deliver(previous);
    },
    dispose,
  };
}

export async function applyPosDevBuild(build: PosDevBuild, host: PosDevHost, request: typeof fetch = fetch, signal?: AbortSignal, previous?: PreparedPosDevBuild): Promise<PreparedPosDevBuild> {
  const response = await request(build.packageUrl, { cache: "no-store", signal });
  if (!response.ok) throw new Error(`pos_package_http_${response.status}`);
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (!bytes.byteLength || bytes.byteLength > 4 * 1024 * 1024) throw new Error("pos_package_size_invalid");
  const digest = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes))).map((byte) => byte.toString(16).padStart(2, "0")).join("");
  if (build.packageDigest !== "sha256:" + digest) throw new Error("pos_package_digest_mismatch");
  const firmware = await host.chooseFirmware(build.board);
  if (!firmware) throw new Error("pos_firmware_missing");
  const prepared = { build, bytes, firmware, openRequestId: "" };
  try {
    const opened = await activatePosDevBuild(prepared, host, signal);
    prepared.openRequestId = opened.requestId;
    return prepared;
  } catch (error) {
    if (previous && !signal?.aborted) {
      try { previous.openRequestId = (await activatePosDevBuild(previous, host, signal)).requestId; }
      catch (recoveryError) { throw new Error(`${String(error)}; previous app recovery failed: ${String(recoveryError)}`); }
    }
    throw error;
  }
}

async function activatePosDevBuild(prepared: PreparedPosDevBuild, host: PosDevHost, signal?: AbortSignal): Promise<PosConsoleResult> {
  const { build, bytes, firmware } = prepared;
  const path = `/PandaDev/${build.packageDigest.slice("sha256:".length)}.pap`;
  if (signal?.aborted) throw new Error("pos_runtime_cancelled");
  await host.stopSim();
  if (signal?.aborted) throw new Error("pos_runtime_cancelled");
  await host.createSdDirectory(build.board, "/PandaDev");
  await host.writeSdFile(build.board, path, bytes);
  if (signal?.aborted) throw new Error("pos_runtime_cancelled");
  const started = await host.startSim(build.board, firmware, createSimulatedLocation());
  if (started !== "started" && started !== "already_running") throw new Error(started);
  const command = createPosConsoleClient(host, 180000, signal);
  const capabilities = await command("capabilities");
  if (!capabilities.capabilities?.install || !capabilities.capabilities?.open) throw new Error("pos_runtime_admission_unavailable");
  const installed = await command("install", path);
  if (installed.appId !== build.appId || installed.version !== build.version) throw new Error("pos_installed_identity_mismatch");
  const opened = await command("open", build.appId);
  if (opened.appId !== build.appId) throw new Error("pos_opened_identity_mismatch");
  return opened;
}

// 仅由本地 pos dev URL 启用；普通浏览器模拟器不连接开发服务器。
export async function startPosDevBridge(host: PosDevHost = browserSimulatorRuntimeHost): Promise<(() => void) | undefined> {
  const location = new URL(globalThis.location.href);
  if (location.searchParams.get("posDev") !== "1") return;
  if (!["127.0.0.1", "localhost", "[::1]"].includes(location.hostname)) throw new Error("pos_development_origin_required");
  let disposed = false;
  let pending: PosDevBuild | undefined;
  let active = false;
  let activeBuild: PosDevBuild | undefined;
  let appliedRevision = 0;
  let lastRunning: PreparedPosDevBuild | undefined;
  const controller = new AbortController();
  let logBuffer = "";
  let logTimer: ReturnType<typeof setTimeout> | undefined;
  let flushingLog = false;
  const report = async (message: string, revision?: number, status?: "running" | "error" | "stopped") => {
    if (disposed) return;
    await fetch("/__pos/runtime", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ revision, status, message: message.slice(0, 4096) }), signal: controller.signal,
    }).catch(() => undefined);
  };
  const state = (build: PosDevBuild, running: boolean, error?: string) => {
    globalThis.dispatchEvent(new CustomEvent("pos-dev-state", { detail: { board: build.board, running, error, message: running ? `pos dev: ${build.appId} (build ${build.revision})` : "pos dev: preparing app" } }));
  };
  const health = await createPosRuntimeHealthMonitor(host, (result) => {
    if (!lastRunning || disposed) return;
    const failed = !result.ok || result.state === "failed";
    const running = result.state === "running" && result.ok;
    const message = failed ? result.error ?? "runtime_error" : running ? `Runtime running ${result.appId}` : `Runtime stopped ${result.appId}`;
    state(lastRunning.build, running, failed ? message : undefined);
    void report(message, lastRunning.build.revision, failed ? "error" : running ? "running" : "stopped");
  });
  const watchHealth = (prepared: PreparedPosDevBuild | undefined) => health.watch(prepared ? {
    requestId: prepared.openRequestId, appId: prepared.build.appId, version: prepared.build.version, packageDigest: prepared.build.packageDigest,
  } : undefined);
  const apply = async () => {
    if (active || disposed) return;
    active = true;
    try {
      while (pending && !disposed) {
        const build = pending;
        pending = undefined;
        if (build.revision <= appliedRevision) continue;
        activeBuild = build;
        watchHealth(undefined);
        state(build, false);
        try {
          const prepared = await applyPosDevBuild(build, host, fetch, controller.signal, lastRunning);
          if (disposed) return;
          appliedRevision = build.revision;
          lastRunning = prepared;
          state(build, true);
          await report(`Runtime opened ${build.appId}@${build.version} (build ${build.revision})`, build.revision, "running");
          watchHealth(prepared);
        } catch (error) {
          const message = error instanceof Error ? error.message : String(error);
          state(build, false, message);
          await report(message, build.revision, "error");
          if (lastRunning && !disposed && !message.includes("previous app recovery failed:")) {
            state(lastRunning.build, true, message);
            watchHealth(lastRunning);
          }
        }
      }
    } finally { active = false; activeBuild = undefined; }
  };
  const queue = (value: unknown) => {
    if (!isPosDevBuild(value) || value.revision <= appliedRevision) return;
    pending = value;
    void apply();
  };
  // UART 可能逐字符输出；批量转发日志，避免每个字符各占一个 HTTP 请求。
  const flushLog = async () => {
    if (flushingLog || disposed) return;
    flushingLog = true;
    try {
      while (logBuffer && !disposed) {
        const message = logBuffer.slice(0, 4096);
        logBuffer = logBuffer.slice(4096);
        await report(message);
      }
    } finally { flushingLog = false; }
  };
  const unlistenLog = await host.subscribeSerialLog((message) => {
    logBuffer += message;
    if (logBuffer.length > 65536) logBuffer = "[pos dev] Earlier serial output omitted\n" + logBuffer.slice(-61440);
    if (logTimer) return;
    logTimer = setTimeout(() => { logTimer = undefined; void flushLog(); }, 100);
  });
  const unlistenError = await host.subscribeSimulatorError((message) => { void report(message, activeBuild?.revision ?? lastRunning?.build.revision, "error"); });
  const events = new EventSource("/__pos/events");
  const dispose = () => {
    disposed = true;
    pending = undefined;
    if (logTimer) clearTimeout(logTimer);
    logBuffer = "";
    controller.abort();
    events.close();
    unlistenLog();
    unlistenError();
    health.dispose();
    void host.stopSim().catch(() => undefined);
  };
  events.addEventListener("shutdown", dispose);
  events.addEventListener("build", (event) => {
    try { queue(JSON.parse((event as MessageEvent<string>).data)); }
    catch { void report("pos_dev_event_invalid"); }
  });
  events.addEventListener("diagnostic", (event) => {
    try {
      const diagnostic = JSON.parse((event as MessageEvent<string>).data);
      globalThis.dispatchEvent(new CustomEvent("pos-dev-state", { detail: { error: diagnostic.error } }));
    } catch { /* 无效诊断不会改变正在运行的应用。 */ }
  });
  try {
    const response = await fetch("/__pos/state", { signal: controller.signal });
    if (!response.ok) throw new Error(`pos_state_http_${response.status}`);
    queue(await response.json());
  } catch (error) { await report(String(error)); }
  return dispose;
}
