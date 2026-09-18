import { browserSimulatorRuntimeHost } from "./browserSimulatorRuntime";
import type { SimulatorHostBridge } from "./simulatorBridgeTypes";

export { BROWSER_SIMULATOR_RUNTIME_UNAVAILABLE } from "./browserSimulatorRuntime";

export const browserSimulatorHostBridge: SimulatorHostBridge = {
  async listFirmwareOptions(boardId) {
    return browserSimulatorRuntimeHost.listFirmwareOptions(boardId);
  },
  async chooseFirmware(boardId, filterName) {
    void filterName;
    return browserSimulatorRuntimeHost.chooseFirmware(boardId);
  },
  async readSdRootPath() {
    return browserSimulatorRuntimeHost.readSdRootPath();
  },
  async listSdDirectory(boardId, path) {
    return browserSimulatorRuntimeHost.listSdDirectory(boardId, path);
  },
  async readSdFile(boardId, path) {
    return browserSimulatorRuntimeHost.readSdFile(boardId, path);
  },
  async writeSdFile(boardId, path, bytes) {
    return browserSimulatorRuntimeHost.writeSdFile(boardId, path, bytes);
  },
  async createSdDirectory(boardId, path) {
    return browserSimulatorRuntimeHost.createSdDirectory(boardId, path);
  },
  async deleteSdPath(boardId, path, recursive) {
    return browserSimulatorRuntimeHost.deleteSdPath(boardId, path, recursive);
  },
  async exportSdImage(boardId) {
    return browserSimulatorRuntimeHost.exportSdImage(boardId);
  },
  async importSdImage(boardId, bytes) {
    return browserSimulatorRuntimeHost.importSdImage(boardId, bytes);
  },
  async startSim(boardId, firmwarePath, hostLocation) {
    return browserSimulatorRuntimeHost.startSim(boardId, firmwarePath, hostLocation);
  },
  async stopSim() {
    return browserSimulatorRuntimeHost.stopSim();
  },
  async fullRebootSim(boardId, firmwarePath, hostLocation) {
    return browserSimulatorRuntimeHost.fullRebootSim(boardId, firmwarePath, hostLocation);
  },
  async releaseButtons() {
    return browserSimulatorRuntimeHost.releaseButtons();
  },
  async injectButton(buttonId, pressed) {
    return browserSimulatorRuntimeHost.injectButton(buttonId, pressed);
  },
  async injectTouch(input) {
    return browserSimulatorRuntimeHost.injectTouch(input);
  },
  async injectPeripheralControl(payload) {
    return browserSimulatorRuntimeHost.injectPeripheralControl(payload);
  },
  async subscribeSerialLog(handler) {
    return browserSimulatorRuntimeHost.subscribeSerialLog(handler);
  },
  async subscribeSimulatorError(handler) {
    return browserSimulatorRuntimeHost.subscribeSimulatorError(handler);
  },
  async subscribeFramebuffer(handler) {
    return browserSimulatorRuntimeHost.subscribeFramebuffer(handler);
  },
  async subscribePeripheralControl(handler) {
    return browserSimulatorRuntimeHost.subscribePeripheralControl(handler);
  },
};
