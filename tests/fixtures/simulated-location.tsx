import { createRoot } from "react-dom/client";
import { SimulatorDevicePane } from "../../src/components/SimulatorDevicePane";
import type { SimulatorHostBridge, SimulatorHostLocation } from "../../src/simulatorBridgeTypes";
import "../../src/panda-ide.css";
import "../../src/App.css";

type Call = { method: string; boardId?: string; firmwarePath?: string; location?: SimulatorHostLocation };
const probe = { calls: [] as Call[], cancelPicker: false, failNextStart: false };
(window as unknown as { __locationProbe: typeof probe }).__locationProbe = probe;
const browserMode = new URLSearchParams(location.search).get("host") === "browser";
const bridge: SimulatorHostBridge = {
  chooseFirmware: async boardId => probe.cancelPicker ? undefined : "/test/" + boardId + ".bin",
  readSdRootPath: async () => "test virtual SD",
  listSdDirectory: async (_boardId,path) => ({path,entries:[]}),
  readSdFile: async (_boardId,path) => ({path,bytes:new ArrayBuffer(0)}),
  writeSdFile: async () => {}, createSdDirectory: async () => {}, deleteSdPath: async () => {},
  startSim: async (boardId,firmwarePath,location) => {
    probe.calls.push({method:"start",boardId,firmwarePath,location:{...location}});
    if (probe.failNextStart) { probe.failNextStart=false; throw new Error("Test start failure"); }
    return "started";
  },
  stopSim: async () => { probe.calls.push({method:"stop"}); return "stopped"; },
  fullRebootSim: async (boardId,firmwarePath,location) => {
    probe.calls.push({method:"reboot",boardId,firmwarePath,location:{...location}});return "started";
  },
  releaseButtons: async () => true,
  injectButton: async () => true, injectTouch: async () => true, injectPeripheralControl: async () => true,
  subscribeSerialLog: async () => () => {}, subscribeSimulatorError: async () => () => {},
  subscribeFramebuffer: async () => () => {}, subscribePeripheralControl: async () => () => {},
};
if (browserMode) bridge.listFirmwareOptions = async boardId => [{
  id:boardId,boardId,label:"Test firmware",path:"/test/"+boardId+".bin",bundled:true,
}];
createRoot(document.getElementById("root")!).render(<SimulatorDevicePane hostBridge={bridge} />);
