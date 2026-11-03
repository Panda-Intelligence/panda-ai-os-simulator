import registry from "../boards.json";

export type BoardId =
  | "mofei"
  | "s3r8"
  | "s3wroom"
  | "s37uc"
  | "m5papers3"
  | "lilygo-t5s3-pro"
  | "read-pico"
  | "molink"
  | "x4pro"
  | "metalio-eink4"
  | "eego-a4";

export type SimulatorBoardKey = {
  id: number;
  label: string;
  aliases?: string[];
};

export type SimulatorBoard = {
  id: string;
  displayName: string;
  rawWidth: number;
  rawHeight: number;
  framebufferWidth: number;
  framebufferHeight: number;
  framebufferFormat: "mono1" | "gray16";
  outputWidth: number;
  outputHeight: number;
  touchI2cAddr: string;
  qemu: {
    displayType: string;
    touchType: string;
  };
  capabilities?: string[];
  keyMap: SimulatorBoardKey[];
};

type SimulatorBoardRegistry = {
  defaultBoard: string;
  boards: SimulatorBoard[];
};

export const SIMULATOR_BOARD_REGISTRY = registry as SimulatorBoardRegistry;

export const SIMULATOR_BOARDS = SIMULATOR_BOARD_REGISTRY.boards;
export const BOARD_LIST = SIMULATOR_BOARDS;

export const DEFAULT_SIMULATOR_BOARD =
  SIMULATOR_BOARDS.find((board) => board.id === SIMULATOR_BOARD_REGISTRY.defaultBoard) ?? SIMULATOR_BOARDS[0];

export const DEFAULT_SIMULATOR_BOARD_ID = DEFAULT_SIMULATOR_BOARD.id;

export function getSimulatorBoard(boardId: string): SimulatorBoard {
  const board = SIMULATOR_BOARDS.find((candidate) => candidate.id === boardId);
  if (!board) {
    const supported = SIMULATOR_BOARDS.map((candidate) => candidate.id).join(", ");
    throw new Error(`unsupported simulator board '${boardId}'; supported boards: ${supported}`);
  }
  return board;
}


export type SimulatorBoardVisual = {
  family: "panda" | "compact" | "papers3" | "t5s3";
  modelLabel: string;
  finish: "matte-black" | "graphite" | "paper-white" | "industrial-black";
  buttonSide: "left" | "right";
  accent: string;
  physicalSizeMm?: { width: number; height: number; thickness: number };
  screenInches?: number;
  ports?: string[];
  mountingHoles?: number;
  hangingEar?: boolean;
  officialReference?: string;
  shellWidth?: number;
  shellPaddingTop?: number;
  shellPaddingSides?: number;
  shellPaddingBottom?: number;
  bezelRadius?: number;
  screenRadius?: number;
  shellAccent?:
    | "mofei"
    | "s3r8"
    | "s3wroom"
    | "s37uc"
    | "m5papers3"
    | "lilygo-t5s3-pro"
    | "read-pico"
    | "molink"
    | "x4pro"
    | "metalio"
    | "eego-a4";
  chassisLabel?: string;
  controlCluster?: "none" | "side-pair" | "bottom-strip";
  powerControl?: "top-edge" | "side-edge";
};

export type BoardVisualProfile = SimulatorBoardVisual;

const BOARD_VISUALS: Record<string, SimulatorBoardVisual> = {
  mofei: { family: "panda", modelLabel: "PANDA · MOFEI", finish: "matte-black", buttonSide: "right", accent: "#202124", shellAccent: "mofei" },
  s3r8: { family: "panda", modelLabel: "PANDA · S3R8", finish: "matte-black", buttonSide: "right", accent: "#202124", shellAccent: "s3r8" },
  s3wroom: { family: "panda", modelLabel: "ESP32-S3 · DEV", finish: "matte-black", buttonSide: "right", accent: "#303238", shellAccent: "s3wroom" },
  s37uc: { family: "compact", modelLabel: "PANDA · S37UC", finish: "graphite", buttonSide: "right", accent: "#30343a", shellAccent: "s37uc" },
  m5papers3: {
    family: "papers3",
    modelLabel: "M5STACK · PAPERS3",
    finish: "paper-white",
    buttonSide: "right",
    accent: "#f2f2f0",
    physicalSizeMm: { width: 121.5, height: 67.7, thickness: 7.7 },
    screenInches: 4.7,
    ports: ["USB-C", "microSD"],
    hangingEar: true,
    officialReference: "https://docs.m5stack.com/en/core/PaperS3",
    shellAccent: "m5papers3",
  },
  "lilygo-t5s3-pro": {
    family: "t5s3",
    modelLabel: "LILYGO · T5-4.7 S3 PRO",
    finish: "industrial-black",
    buttonSide: "right",
    accent: "#ec6b2d",
    physicalSizeMm: { width: 129, height: 69, thickness: 11 },
    screenInches: 4.7,
    ports: ["USB-C", "TF", "QWIIC ×2"],
    mountingHoles: 2,
    officialReference: "https://wiki.lilygo.cc/products/t5-series/t5-e-paper-s3-pro/",
    shellAccent: "lilygo-t5s3-pro",
  },
  "read-pico": {
    family: "panda",
    modelLabel: "READ PICO · ED052TC4",
    finish: "graphite",
    buttonSide: "right",
    accent: "#2b3038",
    shellWidth: 456,
    shellPaddingTop: 28,
    shellPaddingSides: 24,
    shellPaddingBottom: 44,
    bezelRadius: 28,
    screenRadius: 12,
    shellAccent: "read-pico",
    chassisLabel: 'Read Pico · 5.2" Parallel Paper',
    controlCluster: "side-pair",
    powerControl: "top-edge",
  },
  molink: {
    family: "panda",
    modelLabel: "MOLINK · GDEQ0426",
    finish: "matte-black",
    buttonSide: "right",
    accent: "#252830",
    shellWidth: 620,
    shellPaddingTop: 24,
    shellPaddingSides: 28,
    shellPaddingBottom: 40,
    bezelRadius: 24,
    screenRadius: 10,
    shellAccent: "molink",
    chassisLabel: 'MoLink · 4.26" Landscape E-Ink',
    controlCluster: "bottom-strip",
    powerControl: "side-edge",
  },
  x4pro: {
    family: "panda",
    modelLabel: "X4PRO · GDEQ0426",
    finish: "industrial-black",
    buttonSide: "right",
    accent: "#2d323b",
    shellWidth: 620,
    shellPaddingTop: 24,
    shellPaddingSides: 28,
    shellPaddingBottom: 36,
    bezelRadius: 24,
    screenRadius: 10,
    shellAccent: "x4pro",
    chassisLabel: 'X4Pro · 4.26" Touch + Keys',
    controlCluster: "side-pair",
    powerControl: "top-edge",
  },
  "metalio-eink4": {
    family: "panda",
    modelLabel: "METALIO · E-INK 4",
    finish: "graphite",
    buttonSide: "right",
    accent: "#3a3f47",
    shellWidth: 452,
    shellPaddingTop: 26,
    shellPaddingSides: 22,
    shellPaddingBottom: 36,
    bezelRadius: 26,
    screenRadius: 12,
    shellAccent: "metalio",
    chassisLabel: "MetalIO E-Ink 4 · CST816S Touch",
    controlCluster: "none",
    powerControl: "top-edge",
  },
  "eego-a4": {
    family: "panda",
    modelLabel: "EEGO · A4",
    finish: "matte-black",
    buttonSide: "right",
    accent: "#282c34",
    shellWidth: 464,
    shellPaddingTop: 28,
    shellPaddingSides: 24,
    shellPaddingBottom: 42,
    bezelRadius: 26,
    screenRadius: 12,
    shellAccent: "eego-a4",
    chassisLabel: 'EEGO A4 · 3.97" Touch + Warm Light',
    controlCluster: "side-pair",
    powerControl: "top-edge",
  },
};

export function getSimulatorBoardVisual(boardId: string): SimulatorBoardVisual {
  return BOARD_VISUALS[boardId] ?? {
    family: "panda", modelLabel: boardId.toUpperCase(), finish: "matte-black",
    buttonSide: "right", accent: "#202124",
  };
}

export function shortSimulatorBoardName(board: SimulatorBoard): string {
  return board.displayName.split(" (", 1)[0];
}

export const SIMULATOR_BOARD_PROFILES = SIMULATOR_BOARDS;
export const getSimulatorBoardProfile = getSimulatorBoard;

export function summarizeSimulatorBoard(board: SimulatorBoard): string {
  return `${board.outputWidth}x${board.outputHeight} - ${board.displayName} - ${board.qemu.displayType}/${board.qemu.touchType}`;
}
