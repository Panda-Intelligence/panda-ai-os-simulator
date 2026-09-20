import { createSimulatedLocation } from "../simulatorLocation";
import { useCallback, useEffect, useRef, useState } from "react";
import { PanelCanvas } from "./PanelCanvas";
import {
  BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE,
  browserSimulatorHostBridge,
  resolveDefaultSimulatorHostBridge,
  type SimulatorSdEntry,
  type SimulatorFirmwareOption,
  type SimulatorHostBridge,
} from "../simulatorBridge";
import { DEFAULT_SIMULATOR_BOARD_ID, getSimulatorBoard, SIMULATOR_BOARDS, shortSimulatorBoardName, summarizeSimulatorBoard } from "../boards";
import {
  resolveInitialSimulatorLocale,
  saveSimulatorLocalePreference,
  simulatorT,
  SimulatorLocale,
  SimulatorTranslationKey,
  SUPPORTED_SIMULATOR_LOCALES,
} from "../i18n";

const MAX_LOG_LINES = 1000;
const STICKY_LOG_PREFIXES = ["[QEMU-SIM]", "[QEMU-DBG] ELF load result:", "[BOOT-FRAME]"];
const BUTTON_CLICK_HOLD_MS = 250;
const BUTTON_CLICK_SETTLE_MS = 50;

const formatByteSize = (bytes: number) => {
  if (!Number.isFinite(bytes) || bytes < 0) return "";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(bytes < 10 * 1024 ? 1 : 0)} KiB`;
  return `${(bytes / (1024 * 1024)).toFixed(bytes < 10 * 1024 * 1024 ? 1 : 0)} MiB`;
};

type PanelDisplayMode = "fit" | "1x" | "2x";

type StatusState = { kind: "message"; text: string } | { kind: "translation"; key: SimulatorTranslationKey };
type SdBusyState = "idle" | "refresh" | "write" | "read" | "delete" | "backup" | "restore";

const boardButtonIdByLabel = (
  keyMap: { id: number; label: string; aliases?: string[] }[],
  labels: string[],
  fallbackId: number,
) => {
  const accepted = new Set(labels.map((label) => label.trim().toLowerCase()));
  return keyMap.find(
    (key) =>
      accepted.has(key.label.trim().toLowerCase()) ||
      (key.aliases ?? []).some((alias) => accepted.has(alias.trim().toLowerCase())),
  )?.id ?? fallbackId;
};

type SimulatorDevicePaneProps = {
  hostBridge?: SimulatorHostBridge | Promise<SimulatorHostBridge>;
};

export function SimulatorDevicePane({ hostBridge: hostBridgeOverride }: SimulatorDevicePaneProps = {}) {
  const [resolvedHostBridge, setResolvedHostBridge] = useState<SimulatorHostBridge>(browserSimulatorHostBridge);
  const [running, setRunning] = useState(false);
  const [locale, setLocale] = useState<SimulatorLocale>(() => resolveInitialSimulatorLocale());
  const [boardId, setBoardId] = useState<string>(DEFAULT_SIMULATOR_BOARD_ID);
  const board = getSimulatorBoard(boardId);
  const [status, setStatus] = useState<StatusState>({ kind: "translation", key: "statusIdle" });
  const [error, setError] = useState<string | null>(null);
  const [sdRoot, setSdRoot] = useState("");
  const [logLines, setLogLines] = useState<string[]>([]);
  const [sidebarOpen, setSidebarOpen] = useState(true);
  const [panelOpen, setPanelOpen] = useState(true);
  const [panelDisplayMode, setPanelDisplayMode] = useState<PanelDisplayMode>("fit");
  const [firmwareName, setFirmwareName] = useState<string | null>(null);
  const [firmwareOptions, setFirmwareOptions] = useState<SimulatorFirmwareOption[]>([]);
  const [selectedFirmwarePath, setSelectedFirmwarePath] = useState("");
  const [sdDropActive, setSdDropActive] = useState(false);
  const [sdPath, setSdPath] = useState("/");
  const [sdEntries, setSdEntries] = useState<SimulatorSdEntry[]>([]);
  const [sdBusy, setSdBusy] = useState<SdBusyState>("idle");
  const [sdMessage, setSdMessage] = useState("");
  const [newFolderName, setNewFolderName] = useState("");
  const serialConsoleRef = useRef<HTMLDivElement | null>(null);
  const logEndRef = useRef<HTMLDivElement | null>(null);
  const fileInputRef = useRef<HTMLInputElement | null>(null);
  const folderInputRef = useRef<HTMLInputElement | null>(null);
  const sdImageInputRef = useRef<HTMLInputElement | null>(null);
  const firmwarePathRef = useRef("");
  const buttonClickTailRef = useRef<Promise<void>>(Promise.resolve());
  const pressedPointerButtonsRef = useRef<Set<number>>(new Set());
  const t = (key: SimulatorTranslationKey) => simulatorT(locale, key);
  const statusText = status.kind === "message" ? status.text : t(status.key);
  const statusLabel = status.kind === "translation" && status.key === "statusRuntimeUnavailable" ? "NA" : statusText;
  const hostBridge = resolvedHostBridge;
  const browserSdAvailable = sdRoot.includes("Browser sandbox");
  const browserSdTransferAvailable = browserSdAvailable
    && typeof hostBridge.exportSdImage === "function"
    && typeof hostBridge.importSdImage === "function";
  const firmwareCatalogAvailable = typeof hostBridge.listFirmwareOptions === "function";
  const matchingFirmware = firmwareOptions.filter((option) => option.boardId === board.id);
  const launchAvailable = !firmwareCatalogAvailable || matchingFirmware.some((option) => option.path === selectedFirmwarePath);
  const panelDisplayScale = panelDisplayMode === "1x" ? 1 : panelDisplayMode === "2x" ? 2 : 0;
  const mainClassName = panelDisplayMode === "1x" ? "ide-main ide-main--display-one-to-one" : "ide-main";
  const deviceStageClassName = panelDisplayScale > 0
    ? "ide-device-stage ide-device-stage--scroll-scale"
    : "ide-device-stage";

  useEffect(() => {
    let mounted = true;
    Promise.resolve(hostBridgeOverride ?? resolveDefaultSimulatorHostBridge())
      .then((bridge) => {
        if (mounted) {
          setResolvedHostBridge(bridge);
        }
      })
      .catch(() => {
        if (mounted) {
          setResolvedHostBridge(browserSimulatorHostBridge);
          setStatus({ kind: "translation", key: "statusRuntimeUnavailable" });
        }
      });
    return () => {
      mounted = false;
    };
  }, [hostBridgeOverride]);

  useEffect(() => {
    document.documentElement.lang = locale;
  }, [locale]);

  useEffect(() => {
    let serialUnlisten: (() => void) | null = null;
    let errorUnlisten: (() => void) | null = null;
    let disposed = false;

    (async () => {
      try {
        const nextSerialUnlisten = await hostBridge.subscribeSerialLog((payload) => {
          setLogLines((prev) => {
            const incoming = payload.split("\n");
            const next = [...prev, ...incoming];
            if (next.length > MAX_LOG_LINES) {
              const sticky = next.filter((line) => STICKY_LOG_PREFIXES.some((prefix) => line.startsWith(prefix)));
              const tail = next.slice(next.length - MAX_LOG_LINES);
              return [...sticky.filter((line) => !tail.includes(line)), ...tail];
            }
            return next;
          });
        });
        const nextErrorUnlisten = await hostBridge.subscribeSimulatorError((payload) => {
          setError(payload);
          if (payload.startsWith("browser_wasm_worker_fatal:")) {
            setRunning(false);
            setStatus({ kind: "translation", key: "statusStartError" });
          }
        });
        if (disposed) {
          nextSerialUnlisten();
          nextErrorUnlisten();
          return;
        }
        serialUnlisten = nextSerialUnlisten;
        errorUnlisten = nextErrorUnlisten;
      } catch {
        serialUnlisten = null;
        errorUnlisten = null;
      }
    })();

    return () => {
      disposed = true;
      if (serialUnlisten) serialUnlisten();
      if (errorUnlisten) errorUnlisten();
    };
  }, [hostBridge]);

  useEffect(() => {
    let cancelled = false;
    if (!hostBridge.listFirmwareOptions) {
      setFirmwareOptions([]);
      setSelectedFirmwarePath("");
      return () => { cancelled = true; };
    }
    setFirmwareOptions([]);
    setSelectedFirmwarePath("");
    void hostBridge.listFirmwareOptions(board.id)
      .then((options) => {
        if (cancelled) return;
        options = options.filter((option) => option.boardId === board.id);
        setFirmwareOptions(options);
        setStatus((current) => options.length === 0
          ? { kind: "translation", key: "statusRuntimeUnavailable" }
          : current.kind === "translation" && current.key === "statusRuntimeUnavailable"
            ? { kind: "translation", key: "statusIdle" } : current);
        setSelectedFirmwarePath((current) => options.some((option) => option.path === current)
          ? current
          : (options[0]?.path ?? ""));
      })
      .catch(() => {
        if (!cancelled) {
          setFirmwareOptions([]);
          setSelectedFirmwarePath("");
          setStatus({ kind: "translation", key: "statusRuntimeUnavailable" });
        }
      });
    return () => { cancelled = true; };
  }, [board.id, hostBridge]);

  useEffect(() => {
    void hostBridge.readSdRootPath()
      .then((path) => {
        setSdRoot(path);
      })
      .catch(() => {
        setSdRoot("");
      });
  }, [hostBridge]);

  const normalizeSdChildPath = (basePath: string, leaf: string) => {
    const cleanLeaf = leaf.split("/").filter((part) => part && part !== "." && part !== "..").join("/");
    return basePath === "/" ? `/${cleanLeaf}` : `${basePath}/${cleanLeaf}`;
  };

  const refreshSdDirectory = useCallback(async (path: string) => {
    setSdBusy("refresh");
    setSdMessage("");
    try {
      const directory = await hostBridge.listSdDirectory(board.id, path);
      setSdPath(directory.path);
      setSdEntries(directory.entries);
      setSdMessage(`${simulatorT(locale, "sdStatusReady")}: ${directory.entries.length}`);
    } catch (e) {
      const msg = typeof e === "string" ? e : (e as Error).message ?? String(e);
      setSdMessage(msg);
    } finally {
      setSdBusy("idle");
    }
  }, [board.id, hostBridge, locale]);

  useEffect(() => {
    if (browserSdAvailable) {
      void refreshSdDirectory("/");
    }
  }, [browserSdAvailable, refreshSdDirectory]);

  useEffect(() => {
    const serialConsole = serialConsoleRef.current;
    if (serialConsole) {
      serialConsole.scrollTop = serialConsole.scrollHeight;
    }
  }, [logLines]);

  const quickStartSim = async () => {
    if (!launchAvailable) {
      setStatus({ kind: "translation", key: "statusRuntimeUnavailable" });
      return;
    }
    setError(null);
    try {
      const hostLocation = createSimulatedLocation();
      const firmwarePath = selectedFirmwarePath;
      const res = await hostBridge.startSim(board.id, firmwarePath, hostLocation);
      setStatus({ kind: "message", text: `start_sim: ${res}` });
      if (res === "started" || res === "already_running") {
        firmwarePathRef.current = firmwarePath;
        const selected = firmwareOptions.find((option) => option.path === firmwarePath);
        setFirmwareName(selected?.label ?? (firmwarePath ? firmwarePath.split(/[\\/]/).pop() || firmwarePath : null));
        setRunning(true);
      } else if (res === BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE) {
        setStatus({ kind: "translation", key: "statusRuntimeUnavailable" });
      }
    } catch (e) {
      const msg = typeof e === "string" ? e : (e as Error).message ?? String(e);
      setError(msg);
      setStatus({ kind: "translation", key: "statusStartError" });
    }
  };

  const startSim = async () => {
    setError(null);
    try {
      const firmwarePath = await hostBridge.chooseFirmware(board.id, t("filePickerFirmwareFilter"));
      if (firmwarePath === undefined) {
        return;
      }

      const hostLocation = createSimulatedLocation();
      const res = await hostBridge.startSim(board.id, firmwarePath, hostLocation);
      setStatus({ kind: "message", text: `start_sim: ${res}` });
      if (res === "started" || res === "already_running") {
        firmwarePathRef.current = firmwarePath;
        setFirmwareName(firmwarePath.split(/[\\/]/).pop() || firmwarePath);
        setRunning(true);
      } else if (res === BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE) {
        setStatus({ kind: "translation", key: "statusRuntimeUnavailable" });
      }
    } catch (e) {
      const msg = typeof e === "string" ? e : (e as Error).message ?? String(e);
      setError(msg);
      setStatus({ kind: "translation", key: "statusStartError" });
    }
  };

  const stopSim = async () => {
    setError(null);
    try {
      pressedPointerButtonsRef.current.clear();
      await hostBridge.releaseButtons().catch(() => false);
      const res = await hostBridge.stopSim();
      setStatus({ kind: "message", text: `stop_sim: ${res}` });
      setRunning(false);
    } catch (e) {
      const msg = typeof e === "string" ? e : (e as Error).message ?? String(e);
      setError(msg);
      setStatus({ kind: "translation", key: "statusStopError" });
    }
  };

  const clearLog = () => setLogLines([]);

  const handleLocaleChange = (nextLocale: SimulatorLocale) => {
    setLocale(nextLocale);
    saveSimulatorLocalePreference(nextLocale);
  };

  const handleBoardChange = (nextBoardId: string) => {
    if (running || nextBoardId === boardId) return;
    const nextBoard = getSimulatorBoard(nextBoardId);
    setBoardId(nextBoard.id);
    firmwarePathRef.current = "";
    setFirmwareName(null);
    setSelectedFirmwarePath("");
    setSdPath("/");
    setSdEntries([]);
  };

  const openSdDirectory = (path: string) => {
    if (running) return;
    void refreshSdDirectory(path);
  };

  const openSdParent = () => {
    if (sdPath === "/" || running) return;
    const parent = sdPath.slice(0, sdPath.lastIndexOf("/")) || "/";
    void refreshSdDirectory(parent);
  };

  const importSdFiles = async (files: FileList | null, preserveRelativePath: boolean) => {
    if (!files || files.length === 0 || running) return;
    setSdBusy("write");
    setSdMessage("");
    try {
      for (const file of Array.from(files)) {
        const relativePath = preserveRelativePath
          ? (file as File & { webkitRelativePath?: string }).webkitRelativePath || file.name
          : file.name;
        const targetPath = normalizeSdChildPath(sdPath, relativePath);
        const bytes = new Uint8Array(await file.arrayBuffer());
        await hostBridge.writeSdFile(board.id, targetPath, bytes);
      }
      setSdMessage(`${t("sdStatusImported")}: ${files.length}`);
      await refreshSdDirectory(sdPath);
    } catch (e) {
      const msg = typeof e === "string" ? e : (e as Error).message ?? String(e);
      setSdMessage(msg);
    } finally {
      setSdBusy("idle");
      if (fileInputRef.current) fileInputRef.current.value = "";
      if (folderInputRef.current) folderInputRef.current.value = "";
    }
  };

  const createSdFolder = async () => {
    const folder = newFolderName.trim();
    if (!folder || running) return;
    setSdBusy("write");
    setSdMessage("");
    try {
      await hostBridge.createSdDirectory(board.id, normalizeSdChildPath(sdPath, folder));
      setNewFolderName("");
      await refreshSdDirectory(sdPath);
    } catch (e) {
      const msg = typeof e === "string" ? e : (e as Error).message ?? String(e);
      setSdMessage(msg);
    } finally {
      setSdBusy("idle");
    }
  };

  const downloadSdFile = async (entry: SimulatorSdEntry) => {
    if (entry.kind !== "file" || running) return;
    setSdBusy("read");
    setSdMessage("");
    try {
      const file = await hostBridge.readSdFile(board.id, entry.path);
      const blob = new Blob([file.bytes]);
      const url = URL.createObjectURL(blob);
      const anchor = document.createElement("a");
      anchor.href = url;
      anchor.download = entry.name;
      anchor.click();
      URL.revokeObjectURL(url);
      setSdMessage(`${t("sdStatusDownloaded")}: ${entry.name}`);
    } catch (e) {
      const msg = typeof e === "string" ? e : (e as Error).message ?? String(e);
      setSdMessage(msg);
    } finally {
      setSdBusy("idle");
    }
  };

  const downloadSdImage = async () => {
    if (running || !hostBridge.exportSdImage) return;
    setSdBusy("backup");
    setSdMessage("");
    try {
      const image = await hostBridge.exportSdImage(board.id);
      const blob = new Blob([image.bytes], { type: "application/octet-stream" });
      const url = URL.createObjectURL(blob);
      const anchor = document.createElement("a");
      anchor.href = url;
      anchor.download = `panda-${board.id}-sd-card.img`;
      anchor.click();
      URL.revokeObjectURL(url);
      setSdMessage(t(image.templateConflict ? "sdStatusConflictExported" : "sdStatusBackupDownloaded"));
    } catch (e) {
      const msg = typeof e === "string" ? e : (e as Error).message ?? String(e);
      setSdMessage(msg);
    } finally {
      setSdBusy("idle");
    }
  };

  const importSdImage = async (file: File | null) => {
    if (!file || running || !hostBridge.importSdImage) return;
    if (!window.confirm(t("sdImportConfirm"))) {
      if (sdImageInputRef.current) sdImageInputRef.current.value = "";
      return;
    }
    setSdBusy("restore");
    setSdMessage("");
    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      await hostBridge.importSdImage(board.id, bytes);
      await refreshSdDirectory(sdPath);
      setSdMessage(t("sdStatusRestored"));
    } catch (e) {
      const msg = typeof e === "string" ? e : (e as Error).message ?? String(e);
      setSdMessage(msg);
    } finally {
      setSdBusy("idle");
      if (sdImageInputRef.current) sdImageInputRef.current.value = "";
    }
  };

  const deleteSdEntry = async (entry: SimulatorSdEntry) => {
    if (running) return;
    setSdBusy("delete");
    setSdMessage("");
    try {
      await hostBridge.deleteSdPath(board.id, entry.path, entry.kind === "directory");
      await refreshSdDirectory(sdPath);
    } catch (e) {
      const msg = typeof e === "string" ? e : (e as Error).message ?? String(e);
      setSdMessage(msg);
    } finally {
      setSdBusy("idle");
    }
  };

  const sendButton = useCallback((button: number, pressed: boolean) =>
    hostBridge.injectButton(button, pressed).catch(() => false), [hostBridge]);

  const sendButtonClick = useCallback(async (button: number) => {
    const click = buttonClickTailRef.current
      .catch(() => undefined)
      .then(async () => {
        await sendButton(button, true);
        await new Promise((r) => setTimeout(r, BUTTON_CLICK_HOLD_MS));
        await sendButton(button, false);
        await new Promise((r) => setTimeout(r, BUTTON_CLICK_SETTLE_MS));
      });
    buttonClickTailRef.current = click;
    await click;
  }, [sendButton]);

  const pressHardwareButton = (button: number) => {
    pressedPointerButtonsRef.current.add(button);
    void sendButton(button, true);
  };

  const releaseHardwareButton = (button: number) => {
    if (!pressedPointerButtonsRef.current.has(button)) return;
    pressedPointerButtonsRef.current.delete(button);
    void sendButton(button, false);
  };

  const resetSim = async () => {
    setError(null);
    try {
      pressedPointerButtonsRef.current.clear();
      await hostBridge.releaseButtons().catch(() => false);
      setRunning(false);
      setStatus({ kind: "translation", key: "statusResetRebooting" });
      const firmwarePath = firmwarePathRef.current;
      const hostLocation = createSimulatedLocation();
      const res = await hostBridge.fullRebootSim(board.id, firmwarePath, hostLocation);
      setStatus({ kind: "message", text: `reset_sim: ${res}` });
      if (res.startsWith("started") || res.startsWith("already_running")) {
        setRunning(true);
      }
    } catch (e) {
      const msg = typeof e === "string" ? e : (e as Error).message ?? String(e);
      setError(msg);
      setStatus({ kind: "translation", key: "statusResetError" });
    }
  };

  useEffect(() => {
    const uiWidth = board.outputWidth;
    const uiHeight = board.outputHeight;
    const centerX = uiWidth / 2;
    const centerY = uiHeight / 2;
    const swipePixels = Math.round(Math.min(uiWidth, uiHeight) * 0.4);
    const hardwareButtonIds = board.keyMap.map((key) => key.id);
    const backButtonId = boardButtonIdByLabel(board.keyMap, ["esc", "back", "sidekey", "lock"], 0);
    const confirmButtonId = boardButtonIdByLabel(board.keyMap, ["enter", "confirm", "key1", "sidekey1"], 1);
    const sendTouch = (action: number, x: number, y: number) => {
      return hostBridge.injectTouch({ action, x, y, fingerId: 0 }).catch(() => false);
    };

    const sendSwipe = async (dx: number, dy: number) => {
      const endX = Math.max(0, Math.min(uiWidth - 1, centerX + dx));
      const endY = Math.max(0, Math.min(uiHeight - 1, centerY + dy));
      await sendTouch(1, centerX, centerY);
      for (let i = 1; i <= 3; i++) {
        const t = i / 3;
        await new Promise((r) => setTimeout(r, 50));
        await sendTouch(2, Math.round(centerX + dx * t), Math.round(centerY + dy * t));
      }
      await sendTouch(3, endX, endY);
    };

    const sendTap = async () => {
      await sendTouch(1, centerX, centerY);
      await new Promise((r) => setTimeout(r, 50));
      await sendTouch(3, centerX, centerY);
    };

    const onKeyDown = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement | null)?.tagName?.toLowerCase();
      if (tag === "input" || tag === "textarea") return;

      switch (e.key) {
        case "ArrowLeft":
          e.preventDefault();
          void sendSwipe(-swipePixels, 0);
          break;
        case "ArrowRight":
          e.preventDefault();
          void sendSwipe(swipePixels, 0);
          break;
        case "ArrowUp":
          e.preventDefault();
          void sendSwipe(0, -swipePixels);
          break;
        case "ArrowDown":
          e.preventDefault();
          void sendSwipe(0, swipePixels);
          break;
        case " ":
          e.preventDefault();
          void sendTap();
          break;
        case "Enter":
        case "NumpadEnter":
          if (e.repeat) break;
          e.preventDefault();
          void sendButtonClick(confirmButtonId);
          break;
        case "Escape":
          if (e.repeat) break;
          e.preventDefault();
          void sendButtonClick(backButtonId);
          break;
        case "1":
          e.preventDefault();
          if (hardwareButtonIds[0] !== undefined) void sendButton(hardwareButtonIds[0], true);
          break;
        case "2":
          e.preventDefault();
          if (hardwareButtonIds[1] !== undefined) void sendButton(hardwareButtonIds[1], true);
          break;
        case "3":
          e.preventDefault();
          if (hardwareButtonIds[2] !== undefined) void sendButton(hardwareButtonIds[2], true);
          break;
        default:
          break;
      }
    };

    const onKeyUp = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement | null)?.tagName?.toLowerCase();
      if (tag === "input" || tag === "textarea") return;

      switch (e.key) {
        case "1":
          if (hardwareButtonIds[0] !== undefined) void sendButton(hardwareButtonIds[0], false);
          break;
        case "2":
          if (hardwareButtonIds[1] !== undefined) void sendButton(hardwareButtonIds[1], false);
          break;
        case "3":
          if (hardwareButtonIds[2] !== undefined) void sendButton(hardwareButtonIds[2], false);
          break;
        default:
          break;
      }
    };

    window.addEventListener("keydown", onKeyDown);
    window.addEventListener("keyup", onKeyUp);
    return () => {
      window.removeEventListener("keydown", onKeyDown);
      window.removeEventListener("keyup", onKeyUp);
    };
  }, [hostBridge, sendButton, sendButtonClick, board.keyMap, board.outputWidth, board.outputHeight]);

  return (
    <div className="pds-root ide-shell">
      <header className="ide-titlebar">
        <div className="ide-titlebar__title">
          <span className="ide-titlebar__name">{t("appTitle")}</span>
          <span className="ide-titlebar__subtitle">{t("appSubtitle")}</span>
        </div>
        <div className="ide-titlebar__selectors">
          <label className="ide-toolbar-field">
            <span>{t("deviceLabel")}</span>
            <select
              className="pds-select ide-toolbar-select ide-toolbar-select--board"
              name="simulatorBoard"
              value={boardId}
              disabled={running}
              onChange={(event) => handleBoardChange(event.currentTarget.value)}
            >
              {SIMULATOR_BOARDS.map((candidate) => (
                <option key={candidate.id} value={candidate.id}>{shortSimulatorBoardName(candidate)}</option>
              ))}
            </select>
          </label>
          <label className="ide-toolbar-field ide-toolbar-field--firmware">
            <span>{t("firmwareLabel")}</span>
            {firmwareCatalogAvailable ? (
              <select
                className="pds-select ide-toolbar-select ide-toolbar-select--firmware"
                name="simulatorFirmware"
                value={selectedFirmwarePath}
                disabled={running || firmwareOptions.length === 0}
                onChange={(event) => {
                  const path = event.currentTarget.value;
                  setSelectedFirmwarePath(path);
                  setFirmwareName(firmwareOptions.find((option) => option.path === path)?.label ?? null);
                }}
              >
                {firmwareOptions.length > 0 ? firmwareOptions.map((option) => (
                  <option key={option.id} value={option.path}>{option.label}</option>
                )) : <option value="">{t("firmwareUnavailable")}</option>}
              </select>
            ) : (
              <button type="button" className="pds-btn ide-toolbar-firmware-button" disabled={running} onClick={startSim}>
                {t("buttonChooseFirmware")}
              </button>
            )}
          </label>
        </div>
        <div className="ide-titlebar__actions" aria-label={t("runControlsAria")}>
          {!running ? (
            <button className="pds-btn pds-btn--primary" disabled={!launchAvailable} onClick={quickStartSim}>
              {t("buttonQuickLaunch")}
            </button>
          ) : (
            <button className="pds-btn pds-btn--danger" onClick={stopSim}>
              {t("buttonStop")}
            </button>
          )}
          <button className="pds-btn" disabled={!running} title={t("hardwareKeyResetTitle")} onClick={() => void resetSim()}>
            {t("hardwareKeyReset")}
          </button>
        </div>
      </header>

      <div className="ide-body">
        <nav className="pds-activitybar" aria-label="Simulator views">
          <div className="pds-activitybar__brand" aria-hidden="true">P</div>
          <button
            type="button"
            className={sidebarOpen ? "pds-activitybar__btn pds-activitybar__btn--active" : "pds-activitybar__btn"}
            aria-label={t("toggleSidebarAria")}
            aria-pressed={sidebarOpen}
            title={t("toggleSidebarAria")}
            onClick={() => setSidebarOpen((open) => !open)}
          >
            <DeviceIcon />
          </button>
          <button
            type="button"
            className={panelOpen ? "pds-activitybar__btn pds-activitybar__btn--active" : "pds-activitybar__btn"}
            aria-label={t("togglePanelAria")}
            aria-pressed={panelOpen}
            title={t("togglePanelAria")}
            onClick={() => setPanelOpen((open) => !open)}
          >
            <TerminalIcon />
          </button>
        </nav>

        {sidebarOpen && (
          <aside className="ide-sidebar ide-sidebar--boards" aria-label={t("devicePickerAria")}>
            <section className="ide-sidebar__group ide-session-summary">
              <div className="ide-session-summary__row">
                <span className={running ? "ide-statusbar__dot ide-statusbar__dot--on" : "ide-statusbar__dot"} aria-hidden="true" />
                <strong title={statusText}>{statusLabel}</strong>
              </div>
              <span className="ide-session-summary__firmware">{firmwareName ?? t("firmwareQuickLaunch")}</span>
            </section>
            <section className="ide-sidebar__group ide-sidebar__group--boards">
              <h2 className="pds-section-title">{t("deviceLabel")}</h2>
              <div className="ide-board-list" aria-label={t("devicePickerAria")}>
                {SIMULATOR_BOARDS.map((candidate) => (
                  <button
                    key={candidate.id}
                    type="button"
                    className={candidate.id === board.id ? "ide-board-card ide-board-card--active" : "ide-board-card"}
                    disabled={running}
                    onClick={() => handleBoardChange(candidate.id)}
                  >
                    <span className="ide-board-card__name">{shortSimulatorBoardName(candidate)}</span>
                    <span className="ide-board-card__meta">
                      {candidate.outputWidth}×{candidate.outputHeight} · {candidate.framebufferFormat}
                    </span>
                  </button>
                ))}
              </div>
            </section>
          </aside>
        )}

        <main className={mainClassName}>
          <div className={deviceStageClassName} aria-label={t("panelAria")}>
            {status.kind === "translation" && status.key === "statusRuntimeUnavailable" ? (
              <div className="ide-device-stage__notice" role="status">
                {t("cloudRuntimeNotice")}
              </div>
            ) : null}
            {error && <pre className="pds-error ide-device-stage__error">{error}</pre>}
            <PanelCanvas
              ariaLabel={t("panelAria")}
              hostBridge={hostBridge}
              board={board}
              displayScale={panelDisplayScale}
              onReset={running ? () => void resetSim() : undefined}
            />
          </div>

          {panelOpen && (
            <section className="ide-bottom" aria-label={t("serialConsoleAria")}>
              <div className="ide-bottom__head">
                <div className="pds-tabs">
                  <button type="button" className="pds-tab pds-tab--active">
                    {t("serialTabLabel")}
                  </button>
                </div>
                <div className="ide-bottom__actions" aria-label={t("serialConsoleAria")}>
                  <button
                    type="button"
                    className="pds-btn pds-btn--ghost ide-bottom__action"
                    onClick={() => {
                      void navigator.clipboard.writeText(logLines.join("\n"));
                    }}
                  >
                    {t("buttonCopyLog")}
                  </button>
                  <button type="button" className="pds-btn pds-btn--ghost ide-bottom__action" onClick={clearLog}>
                    {t("buttonClearLog")}
                  </button>
                </div>
              </div>
              <div ref={serialConsoleRef} className="pds-console ide-bottom__console">
                {logLines.length === 0 ? (
                  <span className="pds-console__empty">{t("serialNoOutput")}</span>
                ) : (
                  logLines.map((line, i) => <div key={i}>{line || "\u00a0"}</div>)
                )}
                <div ref={logEndRef} />
              </div>
            </section>
          )}
        </main>
        <aside className="ide-inspector" aria-label="Inspector">
          <div className="ide-inspector__header">
            <div>
              <strong>{shortSimulatorBoardName(board)}</strong>
              <span>{summarizeSimulatorBoard(board)}</span>
            </div>
            <span className={running ? "pds-pill pds-pill--running" : "pds-pill"} title={statusText} aria-label={statusText}>{statusLabel}</span>
          </div>
          <section className="ide-inspector__group">
            <div className="ide-inspector__group-head">
              <h2 className="pds-section-title">{t("displayModeLabel")}</h2>
              <span>{board.outputWidth}×{board.outputHeight}</span>
            </div>
            <select
              className="pds-select"
              name="panelDisplayMode"
              value={panelDisplayMode}
              aria-label={t("displayModeAria")}
              onChange={(event) => setPanelDisplayMode(event.currentTarget.value as PanelDisplayMode)}
            >
              <option value="fit">{t("displayModeFit")}</option>
              <option value="1x">{t("displayModeOneToOne")}</option>
              <option value="2x">{t("displayModeTwoX")}</option>
            </select>
          </section>
            <section className="ide-inspector__group">
              <h2 className="pds-section-title">{t("hardwareKeysTitle")}</h2>
              <div className="ide-inspector__keys" aria-label={t("hardwareKeysAria")}>
                {board.keyMap.map(({ id, label }) => (
                  <button
                    key={id}
                    type="button"
                    className="pds-btn ide-key"
                    onPointerDown={() => pressHardwareButton(id)}
                    onPointerUp={() => releaseHardwareButton(id)}
                    onPointerCancel={() => releaseHardwareButton(id)}
                    onPointerLeave={() => releaseHardwareButton(id)}
                  >
                    {label}
                  </button>
                ))}
              </div>
            </section>

            <section className="ide-inspector__group">
              <h2 className="pds-section-title">{t("localeLabel")}</h2>
              <select
                className="pds-select"
                name="simulatorLocale"
                value={locale}
                onChange={(event) => handleLocaleChange(event.currentTarget.value as SimulatorLocale)}
              >
                {SUPPORTED_SIMULATOR_LOCALES.map((supportedLocale) => (
                  <option key={supportedLocale.code} value={supportedLocale.code}>
                    {supportedLocale.label}
                  </option>
                ))}
              </select>
            </section>

            {browserSdAvailable && (
            <section className="ide-inspector__group ide-inspector__group--sd">
              <h2 className="pds-section-title">{t("sdCardTitle")}</h2>
              <div className="ide-sdcard__path">{sdPath}</div>
              {browserSdTransferAvailable && (
                <div className="ide-sdcard__transfer">
                  <p className="ide-inspector__hint">{t("sdTransferDescription")}</p>
                  <div className="ide-sdcard__actions">
                    <button
                      type="button"
                      className="pds-btn ide-sdcard__action"
                      disabled={running || sdBusy !== "idle"}
                      onClick={() => void downloadSdImage()}
                    >
                      {t("sdButtonDownloadBackup")}
                    </button>
                    <button
                      type="button"
                      className="pds-btn ide-sdcard__action"
                      disabled={running || sdBusy !== "idle"}
                      onClick={() => {
                        if (sdImageInputRef.current) sdImageInputRef.current.value = "";
                        sdImageInputRef.current?.click();
                      }}
                    >
                      {t("sdButtonImportBackup")}
                    </button>
                  </div>
                  <input
                    ref={sdImageInputRef}
                    className="ide-sdcard__file-input"
                    type="file"
                    accept=".img,application/octet-stream"
                    onChange={(event) => void importSdImage(event.currentTarget.files?.[0] ?? null)}
                  />
                </div>
              )}
              <div className="ide-sdcard__actions">
                <button
                  type="button"
                  className="pds-btn pds-btn--ghost ide-sdcard__action"
                  disabled={running || sdBusy !== "idle" || sdPath === "/"}
                  onClick={openSdParent}
                >
                  {t("sdButtonUp")}
                </button>
                <button
                  type="button"
                  className="pds-btn pds-btn--ghost ide-sdcard__action"
                  disabled={running || sdBusy !== "idle"}
                  onClick={() => void refreshSdDirectory(sdPath)}
                >
                  {t("sdButtonRefresh")}
                </button>
              </div>
              <div
                className={sdDropActive ? "ide-sdcard__dropzone ide-sdcard__dropzone--active" : "ide-sdcard__dropzone"}
                onDragEnter={(event) => { event.preventDefault(); if (!running) setSdDropActive(true); }}
                onDragOver={(event) => { event.preventDefault(); if (!running) setSdDropActive(true); }}
                onDragLeave={(event) => {
                  if (!event.currentTarget.contains(event.relatedTarget as Node | null)) setSdDropActive(false);
                }}
                onDrop={(event) => {
                  event.preventDefault();
                  setSdDropActive(false);
                  if (!running) void importSdFiles(event.dataTransfer.files, false);
                }}
              >
                <strong>{t("sdDropTitle")}</strong>
                <span>{t("sdDropHint")}</span>
                <div className="ide-sdcard__actions">
                  <button
                    type="button"
                    className="pds-btn ide-sdcard__action"
                    disabled={running || sdBusy !== "idle"}
                    onClick={() => fileInputRef.current?.click()}
                  >
                    {t("sdButtonImportFiles")}
                  </button>
                  <button
                    type="button"
                    className="pds-btn ide-sdcard__action"
                    disabled={running || sdBusy !== "idle"}
                    onClick={() => folderInputRef.current?.click()}
                  >
                    {t("sdButtonImportFolder")}
                  </button>
                </div>
              </div>
              <input
                ref={fileInputRef}
                className="ide-sdcard__file-input"
                type="file"
                multiple
                onChange={(event) => void importSdFiles(event.currentTarget.files, false)}
              />
              <input
                ref={folderInputRef}
                className="ide-sdcard__file-input"
                type="file"
                multiple
                {...({ webkitdirectory: "", directory: "" } as Record<string, string>)}
                onChange={(event) => void importSdFiles(event.currentTarget.files, true)}
              />
              <div className="ide-sdcard__new-folder">
                <input
                  className="pds-input ide-sdcard__folder-input"
                  value={newFolderName}
                  disabled={running || sdBusy !== "idle"}
                  placeholder={t("sdNewFolderPlaceholder")}
                  onChange={(event) => setNewFolderName(event.currentTarget.value)}
                  onKeyDown={(event) => {
                    if (event.key === "Enter") {
                      void createSdFolder();
                    }
                  }}
                />
                <button
                  type="button"
                  className="pds-btn pds-btn--ghost ide-sdcard__action"
                  disabled={running || sdBusy !== "idle" || newFolderName.trim().length === 0}
                  onClick={() => void createSdFolder()}
                >
                  {t("sdButtonCreateFolder")}
                </button>
              </div>
              <div className="ide-sdcard__list" aria-label={t("sdCardTitle")}>
                {sdEntries.length === 0 ? (
                  <span className="ide-inspector__hint">{sdBusy === "idle" ? t("sdEmpty") : t("sdBusy")}</span>
                ) : (
                  sdEntries.map((entry) => (
                    <div key={entry.path} className="ide-sdcard__entry">
                      <button
                        type="button"
                        className="ide-sdcard__entry-name"
                        disabled={running || sdBusy !== "idle"}
                        onClick={() => entry.kind === "directory" ? openSdDirectory(entry.path) : void downloadSdFile(entry)}
                      >
                        <span className="ide-sdcard__entry-kind">{entry.kind === "directory" ? "DIR" : "FILE"}</span>
                        <span className="ide-sdcard__entry-label">{entry.name}</span>
                        {entry.kind === "file" ? (
                          <span className="ide-sdcard__entry-size">{formatByteSize(entry.size)}</span>
                        ) : null}
                      </button>
                      <button
                        type="button"
                        className="pds-btn pds-btn--ghost pds-btn--icon ide-sdcard__delete"
                        title={t("sdButtonDelete")}
                        disabled={running || sdBusy !== "idle"}
                        onClick={() => void deleteSdEntry(entry)}
                      >
                        x
                      </button>
                    </div>
                  ))
                )}
              </div>
              {sdMessage ? <p className="ide-inspector__hint" role="status" aria-live="polite">{sdMessage}</p> : null}
              {running ? <p className="ide-inspector__hint">{t("sdLockedWhileRunning")}</p> : null}
            </section>
            )}

        </aside>

      </div>

      <footer className="pds-statusbar ide-statusbar">
        <span className="pds-statusbar__item">
          <span className={running ? "ide-statusbar__dot ide-statusbar__dot--on" : "ide-statusbar__dot"} aria-hidden="true" />
          <span title={statusText}>{statusLabel}</span>
        </span>
        <span className="pds-statusbar__item">
          {t("statusSdLabel")}: {sdRoot || t("statusSdResolving")}
        </span>
        <span className="pds-statusbar__item" data-simulated-location="London" title="Europe/London · 51.5074, -0.1278">
          {t("simulatedLocationLabel")}: London
        </span>
        <span className="pds-statusbar__item ide-statusbar__keys">
          {t("statusKeysLabel")}: {t("statusKeysHelp")}
        </span>
      </footer>
    </div>
  );
}

function DeviceIcon() {
  return (
    <svg className="pds-icon" viewBox="0 0 24 24" aria-hidden="true">
      <rect x="7" y="3" width="10" height="18" rx="2" />
      <path d="M11 18h2" />
    </svg>
  );
}

function TerminalIcon() {
  return (
    <svg className="pds-icon" viewBox="0 0 24 24" aria-hidden="true">
      <path d="M4 6l5 5-5 5" />
      <path d="M13 17h7" />
    </svg>
  );
}
