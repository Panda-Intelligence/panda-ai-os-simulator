import { useEffect, useRef, useCallback, type CSSProperties, type PointerEvent as ReactPointerEvent } from "react";
import type { SimulatorFramebufferEvent, SimulatorHostBridge } from "../simulatorBridge";
import type { SimulatorBoard } from "../boards";

const TOUCH_DOWN = 1;
const TOUCH_MOVE = 2;
const TOUCH_UP = 3;
const MAX_TOUCHES = 2;

function decodeBase64ToUint8(b64: string): Uint8Array {
  const bin = atob(b64);
  const arr = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) arr[i] = bin.charCodeAt(i);
  return arr;
}

function rotate90CW(src: Uint8Array, srcW: number, srcH: number): Uint8Array {
  const dst = new Uint8Array(src.length);
  const dstW = srcH;
  // src(x, y) -> dst(srcH - 1 - y, x)
  // Or: dst(dx, dy) -> src(dy, srcH - 1 - dx)
  for (let dy = 0; dy < srcW; dy++) {
    for (let dx = 0; dx < dstW; dx++) {
      const sx = dy;
      const sy = srcH - 1 - dx;
      const srcIdx = (sy * srcW + sx) * 4;
      const dstIdx = (dy * dstW + dx) * 4;
      dst[dstIdx] = src[srcIdx];
      dst[dstIdx + 1] = src[srcIdx + 1];
      dst[dstIdx + 2] = src[srcIdx + 2];
      dst[dstIdx + 3] = src[srcIdx + 3];
    }
  }
  return dst;
}

function transformFramebuffer(bytes: Uint8Array, width: number, height: number, board: SimulatorBoard): Uint8Array | null {
  if (board.outputWidth === width && board.outputHeight === height) {
    return bytes;
  }
  if (board.outputWidth === height && board.outputHeight === width) {
    return rotate90CW(bytes, width, height);
  }
  return null;
}

/**
 * E-ink panel canvas. Subscribes to the host bridge framebuffer stream and
 * blits RGBA8888 pixel data sent by the QEMU SSD1677 virtual peripheral.
 * Mouse events on the canvas surface are converted to FT6336U panel
 * coordinates and sent through the host bridge touch injector.
 *
 * The native host IPC layer decodes the 1bpp wire format to RGBA before
 * emitting framebuffer payloads; this component just blits.
 */
type PanelCanvasProps = {
  ariaLabel: string;
  hostBridge: Pick<SimulatorHostBridge, "injectTouch" | "subscribeFramebuffer">;
  board: SimulatorBoard;
  displayScale: 0 | 1 | 2;
};

type PanelShellStyle = CSSProperties & {
  "--panel-canvas-width"?: string;
  "--panel-canvas-height"?: string;
};

export function PanelCanvas({ ariaLabel, hostBridge, board, displayScale }: PanelCanvasProps) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const trackingRef = useRef(new Map<number, { fingerId: number; x: number; y: number }>());
  const shellStyle: PanelShellStyle = {
    aspectRatio: `${board.outputWidth + 26} / ${board.outputHeight + 32}`,
    ...(displayScale > 0
      ? {
          "--panel-canvas-width": `${board.outputWidth * displayScale}px`,
          "--panel-canvas-height": `${board.outputHeight * displayScale}px`,
        }
      : {}),
  };
  const shellClassName = displayScale > 0 ? "panel-shell panel-shell--fixed-scale" : "panel-shell";

  // Subscribe to framebuffer events.
  useEffect(() => {
    let unlisten: (() => void) | null = null;
    let disposed = false;
    const handleFramebuffer = (payload: SimulatorFramebufferEvent) => {
      const canvas = canvasRef.current;
      if (!canvas) return;
      const ctx = canvas.getContext("2d", { willReadFrequently: true });
      if (!ctx) return;
      const { width, height, rgba_b64 } = payload;
      if (width !== board.framebufferWidth || height !== board.framebufferHeight) {
        // eslint-disable-next-line no-console
        console.warn(
          `framebuffer event with unexpected size ${width}x${height}, expected ${board.framebufferWidth}x${board.framebufferHeight}`,
        );
        return;
      }
      const bytes = decodeBase64ToUint8(rgba_b64);
      if (bytes.length !== width * height * 4) {
        // eslint-disable-next-line no-console
        console.warn(
          `framebuffer event payload ${bytes.length} bytes, expected ${width * height * 4}`,
        );
        return;
      }

      ctx.imageSmoothingEnabled = false;
      const outBytes = transformFramebuffer(bytes, width, height, board);
      if (!outBytes) {
        // eslint-disable-next-line no-console
        console.warn(
          `framebuffer transform unsupported for raw ${width}x${height} -> output ${board.outputWidth}x${board.outputHeight}`,
        );
        return;
      }

      const image = new ImageData(
        new Uint8ClampedArray(outBytes.buffer, outBytes.byteOffset, outBytes.byteLength),
        board.outputWidth,
        board.outputHeight,
      );
      ctx.putImageData(image, 0, 0);
    };

    (async () => {
      try {
        const nextUnlisten = await hostBridge.subscribeFramebuffer(handleFramebuffer);
        if (disposed) {
          nextUnlisten();
          return;
        }
        unlisten = nextUnlisten;
      } catch {
        unlisten = null;
      }
    })();
    return () => {
      disposed = true;
      if (unlisten) unlisten();
    };
  }, [board, hostBridge]);

  // Initialize canvas with all-white (matches panel power-on state).
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d", { willReadFrequently: true });
    if (!ctx) return;
    ctx.imageSmoothingEnabled = false;
    ctx.fillStyle = "#fff";
    ctx.fillRect(0, 0, board.outputWidth, board.outputHeight);
    ctx.strokeStyle = "#888";
    ctx.lineWidth = 1;
    ctx.strokeRect(0.5, 0.5, board.outputWidth - 1, board.outputHeight - 1);
  }, [board]);

  // Convert pointer event to firmware logical coordinates accounting for any
  // CSS scaling. The canvas's intrinsic size is the board's panel size but the
  // displayed size may be smaller on a constrained window.
  const eventToPanelXY = useCallback(
    (e: ReactPointerEvent<HTMLCanvasElement>): { x: number; y: number } | null => {
      const canvas = canvasRef.current;
      if (!canvas) return null;
      const rect = canvas.getBoundingClientRect();
      if (rect.width === 0 || rect.height === 0) return null;
      const scaleX = canvas.width / rect.width;
      const scaleY = canvas.height / rect.height;
      const px = Math.max(0, Math.min(canvas.width - 1, Math.round((e.clientX - rect.left) * scaleX)));
      const py = Math.max(0, Math.min(canvas.height - 1, Math.round((e.clientY - rect.top) * scaleY)));

      return { x: px, y: py };
    },
    [],
  );

  const sendTouch = useCallback(
    async (action: number, x: number, y: number, fingerId: number) => {
      try {
        await hostBridge.injectTouch({ action, x, y, fingerId });
      } catch (e) {
        // Inject failures during simulator-not-running are expected; ignore.
        // eslint-disable-next-line no-console
        console.debug("inject_touch failed:", e);
      }
    },
    [hostBridge],
  );

  const onPointerDown = (e: ReactPointerEvent<HTMLCanvasElement>) => {
    if (trackingRef.current.has(e.pointerId) || trackingRef.current.size >= MAX_TOUCHES) return;
    const xy = eventToPanelXY(e);
    if (!xy) return;
    const usedIds = new Set(Array.from(trackingRef.current.values(), (touch) => touch.fingerId));
    const fingerId = usedIds.has(0) ? 1 : 0;
    trackingRef.current.set(e.pointerId, { fingerId, ...xy });
    e.currentTarget.setPointerCapture(e.pointerId);
    e.preventDefault();
    void sendTouch(TOUCH_DOWN, xy.x, xy.y, fingerId);
  };
  const onPointerMove = (e: ReactPointerEvent<HTMLCanvasElement>) => {
    const tracked = trackingRef.current.get(e.pointerId);
    if (!tracked) return;
    const xy = eventToPanelXY(e);
    if (!xy || (xy.x === tracked.x && xy.y === tracked.y)) return;
    trackingRef.current.set(e.pointerId, { fingerId: tracked.fingerId, ...xy });
    e.preventDefault();
    void sendTouch(TOUCH_MOVE, xy.x, xy.y, tracked.fingerId);
  };
  const endPointer = (e: ReactPointerEvent<HTMLCanvasElement>) => {
    const tracked = trackingRef.current.get(e.pointerId);
    if (!tracked) return;
    trackingRef.current.delete(e.pointerId);
    const xy = eventToPanelXY(e) ?? tracked;
    e.preventDefault();
    void sendTouch(TOUCH_UP, xy.x, xy.y, tracked.fingerId);
  };

  return (
    <div className={shellClassName} aria-label={ariaLabel} style={shellStyle}>
      <canvas
        ref={canvasRef}
        width={board.outputWidth}
        height={board.outputHeight}
        className="panel-canvas"
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={endPointer}
        onPointerCancel={endPointer}
      />
    </div>
  );
}
