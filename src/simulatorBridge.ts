import { browserSimulatorHostBridge } from "./browserSimulatorHostBridge";
import type {
  SimulatorFramebufferEvent,
  SimulatorFirmwareOption,
  SimulatorHostBridge,
  SimulatorHostLocation,
  SimulatorSdDirectory,
  SimulatorSdEntry,
  SimulatorSdFile,
  SimulatorSdImage,
  SimulatorTouchInput,
  SimulatorUnlisten,
} from "./simulatorBridgeTypes";

export type {
  SimulatorFramebufferEvent,
  SimulatorFirmwareOption,
  SimulatorHostBridge,
  SimulatorHostLocation,
  SimulatorSdDirectory,
  SimulatorSdEntry,
  SimulatorSdFile,
  SimulatorSdImage,
  SimulatorTouchInput,
  SimulatorUnlisten,
};

export { BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE, browserSimulatorHostBridge } from "./browserSimulatorHostBridge";

export async function resolveDefaultSimulatorHostBridge(): Promise<SimulatorHostBridge> {
  if (typeof window !== "undefined" && "__TAURI_INTERNALS__" in window) {
    const { tauriSimulatorHostBridge } = await import("./tauriSimulatorHostBridge");
    return tauriSimulatorHostBridge;
  }
  return browserSimulatorHostBridge;
}
