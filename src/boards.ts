import registry from "../boards.json";

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
  keyMap: SimulatorBoardKey[];
};

type SimulatorBoardRegistry = {
  defaultBoard: string;
  boards: SimulatorBoard[];
};

export const SIMULATOR_BOARD_REGISTRY = registry as SimulatorBoardRegistry;

export const SIMULATOR_BOARDS = SIMULATOR_BOARD_REGISTRY.boards;

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
};

const BOARD_VISUALS: Record<string, SimulatorBoardVisual> = {
  mofei: { family: "panda", modelLabel: "PANDA · MOFEI", finish: "matte-black", buttonSide: "right", accent: "#202124" },
  s3r8: { family: "panda", modelLabel: "PANDA · S3R8", finish: "matte-black", buttonSide: "right", accent: "#202124" },
  s3wroom: { family: "panda", modelLabel: "ESP32-S3 · DEV", finish: "matte-black", buttonSide: "right", accent: "#303238" },
  s37uc: { family: "compact", modelLabel: "PANDA · S37UC", finish: "graphite", buttonSide: "right", accent: "#30343a" },
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
