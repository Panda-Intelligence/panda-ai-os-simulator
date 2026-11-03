import { useEffect, useMemo, useState } from "react";
import { createRoot } from "react-dom/client";
import { browserSimulatorHostBridge } from "../browserSimulatorHostBridge";
import { browserSimulatorRuntimeHost } from "../browserSimulatorRuntime";
import { getSimulatorBoard } from "../boards";
import { PanelCanvas } from "../components/PanelCanvas";
import { emptyStudioRuntimeSnapshot, isStudioConnectRequest, isStudioResetRequest, isStudioRunRequest, simulatorBoardForStudio, STUDIO_SIMULATOR_CHANNEL, type StudioFrameEvent } from "./contract";
import { StudioSimulatorSession } from "./session";
import "./studio.css";

function StudioFrame() {
  const [boardId, setBoardId] = useState("mofei");
  const session = useMemo(() => new StudioSimulatorSession(browserSimulatorRuntimeHost, (runId, snapshot) => {
    const event: StudioFrameEvent = { channel: STUDIO_SIMULATOR_CHANNEL, type: "snapshot", runId, snapshot };
    parent.postMessage(event, location.origin);
  }), []);
  useEffect(() => {
    const ready = () => parent.postMessage({ channel: STUDIO_SIMULATOR_CHANNEL, type: "ready" } satisfies StudioFrameEvent, location.origin);
    const receive = (event: MessageEvent) => {
      if (event.origin !== location.origin || event.source !== parent) return;
      // iframe 可能早于父页面 hydration 就绪，收到连接请求时重新确认，避免一次性通知丢失。
      if (isStudioConnectRequest(event.data)) { ready(); return; }
      if (isStudioResetRequest(event.data)) {
        setBoardId(simulatorBoardForStudio(event.data.boardId) ?? "mofei");
        void session.stop();
        return;
      }
      if (!isStudioRunRequest(event.data)) return;
      const request = event.data;
      if (!crossOriginIsolated) {
        parent.postMessage({ channel: STUDIO_SIMULATOR_CHANNEL, type: "snapshot", runId: request.runId, snapshot: {
          ...emptyStudioRuntimeSnapshot(request.boardId), status: "unavailable", packageSha256: request.packageDigest,
          diagnostics: [{ id: "isolation", level: "error", source: "runtime", message: "browser_cross_origin_isolation_required", timestamp: new Date().toISOString() }],
        } } satisfies StudioFrameEvent, location.origin);
        return;
      }
      setBoardId(simulatorBoardForStudio(request.boardId) ?? "mofei");
      void session.run(request);
    };
    window.addEventListener("message", receive);
    ready();
    return () => { window.removeEventListener("message", receive); void session.dispose(); };
  }, [session]);
  return <main className="studio-frame"><PanelCanvas key={boardId} ariaLabel="Panda firmware display" hostBridge={browserSimulatorHostBridge} board={getSimulatorBoard(boardId)} displayScale={0} /></main>;
}

createRoot(document.getElementById("root")!).render(<StudioFrame />);
