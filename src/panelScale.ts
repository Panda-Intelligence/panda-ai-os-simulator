// Panel presentation geometry shared by the simulator UI and its tests (no DOM, no React).

/** Fluid layouts within this fraction of an integer device-pixel scale snap to it (exact pixels beat a 1:1.03 blur). */
const PANEL_SNAP_TOLERANCE = 0.12;

/**
 * CSS size that shows the panel at an exact integer device-pixel multiple when the available screen frame is
 * close to one; null when the frame is too far from any integer scale (then the canvas fills the frame).
 */
export function snappedPanelCssSize(
  frame: { width: number; height: number },
  board: { outputWidth: number; outputHeight: number },
  devicePixelRatio: number,
): { width: number; height: number } | null {
  if (frame.width <= 0 || frame.height <= 0 || devicePixelRatio <= 0) return null;
  const scale = Math.min((frame.width * devicePixelRatio) / board.outputWidth, (frame.height * devicePixelRatio) / board.outputHeight);
  const multiple = Math.round(scale);
  if (multiple < 1 || Math.abs(scale - multiple) > PANEL_SNAP_TOLERANCE) return null;
  const width = (multiple * board.outputWidth) / devicePixelRatio;
  const height = (multiple * board.outputHeight) / devicePixelRatio;
  if (width > frame.width + 0.5 || height > frame.height + 0.5) return null;
  return { width, height };
}
