import { applyPosDevPackage, createPosRuntimeHealthMonitor, type PosDevHost } from "../posDevBridge";
import { emptyStudioRuntimeSnapshot, simulatorBoardForStudio, type StudioRunRequest, type StudioRuntimeSnapshot } from "./contract";

export class StudioSimulatorSession {
  private controller?: AbortController;
  private tail: Promise<void> = Promise.resolve();
  private cleanup: Array<() => void> = [];
  private disposed = false;

  constructor(private host: PosDevHost, private changed: (runId: string, snapshot: StudioRuntimeSnapshot) => void) {}

  run(request: StudioRunRequest): Promise<void> {
    this.controller?.abort();
    const controller = new AbortController();
    this.controller = controller;
    this.tail = this.tail.catch(() => undefined).then(() => this.activate(request, controller.signal));
    return this.tail;
  }

  async dispose() {
    this.disposed = true;
    await this.stop();
  }

  stop(): Promise<void> {
    this.controller?.abort();
    this.tail = this.tail.catch(() => undefined).then(async () => {
      this.release();
      await this.host.stopSim();
    });
    return this.tail;
  }

  private release() {
    for (const dispose of this.cleanup.splice(0)) dispose();
  }

  private async activate(request: StudioRunRequest, signal: AbortSignal) {
    if (signal.aborted || this.disposed) return;
    this.release();
    const snapshot = emptyStudioRuntimeSnapshot(request.boardId);
    snapshot.packageSha256 = request.packageDigest;
    snapshot.status = "preparing";
    let logBuffer = "";
    let failed = false;
    let diagnosticSequence = 0;
    let flushTimer: ReturnType<typeof setTimeout> | undefined;
    const emit = () => {
      if (!signal.aborted && !this.disposed) this.changed(request.runId, {
        ...snapshot, posAppLogs: [...snapshot.posAppLogs], diagnostics: [...snapshot.diagnostics],
      });
    };
    const flush = () => {
      flushTimer = undefined;
      if (logBuffer) {
        snapshot.posAppLogs = [...snapshot.posAppLogs, logBuffer].slice(-100);
        logBuffer = "";
      }
      emit();
    };
    const fail = (message: string, unavailable = false) => {
      failed = true;
      snapshot.status = unavailable ? "unavailable" : "failed";
      if (!snapshot.diagnostics.some(item => item.message === message)) {
        snapshot.diagnostics = [...snapshot.diagnostics, { id: `${request.runId}-${++diagnosticSequence}`, level: "error" as const, source: "runtime" as const, message: message.slice(0, 8192), timestamp: new Date().toISOString() }].slice(-100);
      }
      flush();
      emit();
    };
    emit();
    try {
      const board = simulatorBoardForStudio(request.boardId);
      if (!board) { fail("simulator_board_unavailable: X4 Pro", true); await this.host.stopSim(); return; }
      this.cleanup.push(await this.host.subscribeSerialLog(chunk => {
        logBuffer += chunk;
        // 启动日志可在一次事件循环中突发输出；分块保留，不能丢掉最早的真实错误。
        while (logBuffer.length >= 8192) {
          snapshot.posAppLogs = [...snapshot.posAppLogs, logBuffer.slice(0, 8192)].slice(-100);
          logBuffer = logBuffer.slice(8192);
        }
        if (!flushTimer) flushTimer = setTimeout(flush, 120);
      }));
      this.cleanup.push(() => { if (flushTimer) clearTimeout(flushTimer); });
      this.cleanup.push(await this.host.subscribeSimulatorError(message => fail(message)));
      // 必须先监听，再打开应用，以保留 open 终态之前发生的启动错误。
      const health = await createPosRuntimeHealthMonitor(this.host, result => {
        if (!result.ok || result.state === "failed") fail(result.error || "runtime_failed");
        else if (!failed) { snapshot.status = result.state === "running" ? "running" : "stopped"; emit(); }
      });
      this.cleanup.push(health.dispose);
      const prepared = await applyPosDevPackage({
        schemaVersion: 1, revision: 1, appId: request.appId, version: request.version,
        packageDigest: request.packageDigest, packageUrl: "/__pos/package/1.pap", board,
      }, request.bytes, this.host, signal);
      health.watch({ requestId: prepared.openRequestId, appId: request.appId, version: request.version, packageDigest: request.packageDigest });
    } catch (error) {
      if (!signal.aborted) {
        const message = error instanceof Error ? error.message : String(error);
        fail(message, /unavailable|firmware_missing/.test(message));
      }
    }
  }
}
