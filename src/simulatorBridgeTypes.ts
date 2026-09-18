export type SimulatorHostLocation = {
  name: string;
  timezone: string;
  latitude: number;
  longitude: number;
};

export type SimulatorFramebufferEvent = {
  kind: "full" | "partial";
  width: number;
  height: number;
  rgba_b64: string;
};

export type SimulatorTouchInput = {
  action: number;
  x: number;
  y: number;
  fingerId: number;
};

export type SimulatorUnlisten = () => void;

export type SimulatorSdEntry = {
  name: string;
  path: string;
  kind: "file" | "directory";
  size: number;
  attributes?: number;
};

export type SimulatorSdDirectory = {
  path: string;
  entries: SimulatorSdEntry[];
};

export type SimulatorSdFile = {
  path: string;
  bytes: ArrayBuffer;
};

export type SimulatorSdImage = {
  path: string;
  byteLength: number;
  bytes: ArrayBuffer;
  templateFingerprint: string | null;
  storedTemplateFingerprint: string | null;
  templateConflict: boolean;
};

export type SimulatorHostBridge = {
  chooseFirmware(boardId: string, filterName: string): Promise<string | undefined>;
  readSdRootPath(): Promise<string>;
  listSdDirectory(boardId: string, path: string): Promise<SimulatorSdDirectory>;
  readSdFile(boardId: string, path: string): Promise<SimulatorSdFile>;
  writeSdFile(boardId: string, path: string, bytes: Uint8Array): Promise<void>;
  createSdDirectory(boardId: string, path: string): Promise<void>;
  deleteSdPath(boardId: string, path: string, recursive?: boolean): Promise<void>;
  exportSdImage?: (boardId: string) => Promise<SimulatorSdImage>;
  importSdImage?: (boardId: string, bytes: Uint8Array) => Promise<void>;
  startSim(boardId: string, firmwarePath: string, hostLocation: SimulatorHostLocation): Promise<string>;
  stopSim(): Promise<string>;
  fullRebootSim(boardId: string, firmwarePath: string, hostLocation: SimulatorHostLocation): Promise<string>;
  releaseButtons(): Promise<boolean>;
  injectButton(buttonId: number, pressed: boolean): Promise<boolean>;
  injectTouch(input: SimulatorTouchInput): Promise<boolean>;
  injectPeripheralControl(payload: number[]): Promise<boolean>;
  subscribeSerialLog(handler: (payload: string) => void): Promise<SimulatorUnlisten>;
  subscribeSimulatorError(handler: (payload: string) => void): Promise<SimulatorUnlisten>;
  subscribeFramebuffer(handler: (payload: SimulatorFramebufferEvent) => void): Promise<SimulatorUnlisten>;
  subscribePeripheralControl(handler: (payload: number[]) => void): Promise<SimulatorUnlisten>;
};
