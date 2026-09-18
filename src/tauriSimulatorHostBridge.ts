import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { open } from "@tauri-apps/plugin-dialog";

import type { SimulatorFramebufferEvent, SimulatorHostBridge } from "./simulatorBridgeTypes";

const browserSdUnsupported = () => new Error("browser_sd_card_api_unavailable_in_tauri");
const PERIPHERAL_CONTROL_MAX_PAYLOAD = 64;

export const tauriSimulatorHostBridge: SimulatorHostBridge = {
  async chooseFirmware(_boardId, filterName) {
    const selected = await open({
      multiple: false,
      filters: [
        {
          name: filterName,
          extensions: ["elf", "bin"],
        },
      ],
    });
    if (selected === null) {
      return undefined;
    }
    return Array.isArray(selected) ? selected[0] : selected;
  },

  async readSdRootPath() {
    return await invoke<string>("simulator_sd_root_path");
  },

  async listSdDirectory(_boardId, _path) {
    throw browserSdUnsupported();
  },

  async readSdFile(_boardId, _path) {
    throw browserSdUnsupported();
  },

  async writeSdFile(_boardId, _path, _bytes) {
    throw browserSdUnsupported();
  },

  async createSdDirectory(_boardId, _path) {
    throw browserSdUnsupported();
  },

  async deleteSdPath(_boardId, _path, _recursive) {
    throw browserSdUnsupported();
  },

  async startSim(boardId, firmwarePath, hostLocation) {
    return await invoke<string>("start_sim", { boardId, firmwarePath, hostLocation });
  },

  async stopSim() {
    return await invoke<string>("stop_sim");
  },

  async fullRebootSim(boardId, firmwarePath, hostLocation) {
    return await invoke<string>("full_reboot_sim", { boardId, firmwarePath, hostLocation });
  },

  async releaseButtons() {
    return await invoke<boolean>("release_buttons");
  },

  async injectButton(buttonId, pressed) {
    return await invoke<boolean>("inject_button", { buttonId, pressed });
  },

  async injectTouch(input) {
    return await invoke<boolean>("inject_touch", input);
  },

  async injectPeripheralControl(payload) {
    return await invoke<boolean>("inject_peripheral_control", { payload });
  },

  async subscribeSerialLog(handler) {
    return await listen<string>("serial-log", (event) => {
      handler(event.payload);
    });
  },

  async subscribeSimulatorError(handler) {
    return await listen<string>("simulator-error", (event) => {
      handler(event.payload);
    });
  },

  async subscribeFramebuffer(handler) {
    return await listen<SimulatorFramebufferEvent>("framebuffer", (event) => {
      handler(event.payload);
    });
  },

  async subscribePeripheralControl(handler) {
    return await listen<number[]>("peripheral-control", (event) => {
      if (isPeripheralControlPayload(event.payload)) {
        handler(event.payload);
      }
    });
  },
};

function isPeripheralControlPayload(payload: unknown): payload is number[] {
  return Array.isArray(payload)
    && payload.length <= PERIPHERAL_CONTROL_MAX_PAYLOAD
    && payload.every((byte) => Number.isInteger(byte) && byte >= 0 && byte <= 0xff);
}
