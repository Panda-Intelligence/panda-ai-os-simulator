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

export const SIMULATOR_BOARD_PROFILES = SIMULATOR_BOARDS;
export const getSimulatorBoardProfile = getSimulatorBoard;

export function summarizeSimulatorBoard(board: SimulatorBoard): string {
  return `${board.outputWidth}x${board.outputHeight} - ${board.displayName} - ${board.qemu.displayType}/${board.qemu.touchType}`;
}
