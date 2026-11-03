export const STUDIO_SIMULATOR_CHANNEL = "panda-studio-simulator-v1";
export const STUDIO_SIMULATOR_MAX_PACKAGE_BYTES = 2 * 1024 * 1024;
export type StudioBoardId = "mofei-480x800" | "x4-pro-480x800" | "compact-240x416";
export type StudioRuntimeDiagnostic = {
  id: string;
  level: "error" | "warn" | "info";
  source: "compiler" | "runtime" | "pos_app";
  message: string;
  line?: number;
  timestamp: string;
};
export type StudioRuntimeSnapshot = {
  status: "ready" | "preparing" | "running" | "stopped" | "failed" | "unavailable";
  boardId: StudioBoardId;
  packageSha256: string | null;
  posAppLogs: string[];
  diagnostics: StudioRuntimeDiagnostic[];
};
export type StudioRunRequest = {
  channel: typeof STUDIO_SIMULATOR_CHANNEL;
  type: "run";
  runId: string;
  boardId: StudioBoardId;
  appId: string;
  version: string;
  packageDigest: string;
  bytes: Uint8Array;
};
export type StudioFrameEvent =
  | { channel: typeof STUDIO_SIMULATOR_CHANNEL; type: "ready" }
  | { channel: typeof STUDIO_SIMULATOR_CHANNEL; type: "snapshot"; runId: string; snapshot: StudioRuntimeSnapshot };

export type StudioResetRequest = { channel: typeof STUDIO_SIMULATOR_CHANNEL; type: "reset"; boardId: StudioBoardId };
export type StudioConnectRequest = { channel: typeof STUDIO_SIMULATOR_CHANNEL; type: "connect" };

export function isStudioConnectRequest(value: unknown): value is StudioConnectRequest {
  const request = value as Partial<StudioConnectRequest> | null;
  return !!request && request.channel === STUDIO_SIMULATOR_CHANNEL && request.type === "connect";
}

export function isStudioResetRequest(value: unknown): value is StudioResetRequest {
  const v = value as Partial<StudioResetRequest> | null;
  return !!v && v.channel === STUDIO_SIMULATOR_CHANNEL && v.type === "reset"
    && ["mofei-480x800", "x4-pro-480x800", "compact-240x416"].includes(v.boardId ?? "");
}

export function simulatorBoardForStudio(id: StudioBoardId): string | undefined {
  if (id === "mofei-480x800") return "mofei";
  if (id === "compact-240x416") return "s37uc";
  return undefined;
}

export function isStudioRunRequest(value: unknown): value is StudioRunRequest {
  if (!value || typeof value !== "object") return false;
  const v = value as Partial<StudioRunRequest>;
  return v.channel === STUDIO_SIMULATOR_CHANNEL && v.type === "run"
    && typeof v.runId === "string" && v.runId.length > 0 && v.runId.length <= 100
    && ["mofei-480x800", "x4-pro-480x800", "compact-240x416"].includes(v.boardId ?? "")
    && typeof v.appId === "string" && /^[A-Za-z0-9][A-Za-z0-9_.-]{1,127}$/.test(v.appId)
    && typeof v.version === "string" && v.version.length > 0 && v.version.length <= 64
    && typeof v.packageDigest === "string" && /^sha256:[0-9a-f]{64}$/.test(v.packageDigest)
    && v.bytes instanceof Uint8Array && v.bytes.byteLength > 0 && v.bytes.byteLength <= STUDIO_SIMULATOR_MAX_PACKAGE_BYTES;
}

export function emptyStudioRuntimeSnapshot(boardId: StudioBoardId): StudioRuntimeSnapshot {
  return { status: "ready", boardId, packageSha256: null, diagnostics: [], posAppLogs: [] };
}
