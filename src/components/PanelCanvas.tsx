import { useEffect, useRef, useCallback, type CSSProperties, type PointerEvent as ReactPointerEvent } from "react";
import type { SimulatorFramebufferEvent, SimulatorHostBridge } from "../simulatorBridge";
import { getSimulatorBoardVisual, type SimulatorBoard } from "../boards";

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
  hostBridge: Pick<SimulatorHostBridge, "injectButton" | "injectTouch" | "subscribeFramebuffer">;
  board: SimulatorBoard;
  displayScale: 0 | 1 | 2;
  onReset?: () => void;
};

type PanelShellStyle = CSSProperties & {
  "--panel-canvas-width"?: string;
  "--panel-canvas-height"?: string;
  "--panel-accent"?: string;
  "--panel-thickness"?: string;
};

export function PanelCanvas({ ariaLabel, hostBridge, board, displayScale, onReset }: PanelCanvasProps) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const trackingRef = useRef(new Map<number, { fingerId: number; x: number; y: number }>());
  const visual = getSimulatorBoardVisual(board.id);
  const physicalAspect = visual.physicalSizeMm
    ? `${Math.min(visual.physicalSizeMm.width, visual.physicalSizeMm.height)} / ${Math.max(visual.physicalSizeMm.width, visual.physicalSizeMm.height)}`
    : `${board.outputWidth + 26} / ${board.outputHeight + 32}`;
  const shellStyle: PanelShellStyle = {
    aspectRatio: physicalAspect,
    "--panel-accent": visual.accent,
    ...(visual.physicalSizeMm
      ? { "--panel-thickness": `${visual.physicalSizeMm.thickness}px` }
      : {}),
    ...(displayScale > 0
      ? {
          "--panel-canvas-width": `${board.outputWidth * displayScale}px`,
          "--panel-canvas-height": `${board.outputHeight * displayScale}px`,
        }
      : {}),
  };
  const shellClassName = [
    "panel-shell",
    `panel-shell--${visual.family}`,
    `panel-shell--finish-${visual.finish}`,
    displayScale > 0 ? "panel-shell--fixed-scale" : "",
  ].filter(Boolean).join(" ");

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

  const setHardwareKey = (buttonId: number, pressed: boolean) => {
    void hostBridge.injectButton(buttonId, pressed).catch(() => false);
  };

  const physicalControls: Array<{ key: string; label: string; buttonId?: number; reset?: boolean }> =
    board.id === "lilygo-t5s3-pro"
      ? [
          { key: "rst", label: "RST", reset: true },
          ...["BOOT", "IO48", "PWR"].flatMap((label) => {
            const match = board.keyMap.find((key) => key.label.toUpperCase() === label);
            return match ? [{ key: `key-${match.id}`, label: match.label, buttonId: match.id }] : [];
          }),
        ]
      : board.keyMap.map((key) => ({
          key: `key-${key.id}`,
          label: key.label,
          buttonId: key.id,
        }));

  return (
    <div
      className={shellClassName}
      aria-label={ariaLabel}
      style={shellStyle}
      data-board={board.id}
      data-button-side={visual.buttonSide}
      data-physical-size={visual.physicalSizeMm
        ? `${visual.physicalSizeMm.width}×${visual.physicalSizeMm.height}×${visual.physicalSizeMm.thickness}mm`
        : undefined}
    >
      {visual.hangingEar ? <span className="panel-hanging-ear" aria-hidden="true" /> : null}
      {visual.family === "t5s3" ? <span className="panel-front-home-ring" aria-hidden="true" /> : null}
      <span className="panel-device-mark" aria-hidden="true">{visual.modelLabel}</span>
      <div className="panel-screen-frame">
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
      <div className="panel-physical-keys" aria-label="Physical device keys">
        {physicalControls.map((control, index) => (
          <button
            key={control.key}
            type="button"
            className={control.reset ? "panel-physical-key panel-physical-key--reset" : "panel-physical-key"}
            style={{ "--panel-key-index": index } as CSSProperties}
            aria-label={control.label}
            title={control.label}
            disabled={control.reset && !onReset}
            onPointerDown={(event) => {
              event.preventDefault();
              event.currentTarget.setPointerCapture(event.pointerId);
              if (control.reset) {
                onReset?.();
              } else if (control.buttonId !== undefined) {
                setHardwareKey(control.buttonId, true);
              }
            }}
            onPointerUp={(event) => {
              event.preventDefault();
              if (!control.reset && control.buttonId !== undefined) {
                setHardwareKey(control.buttonId, false);
              }
            }}
            onPointerCancel={() => {
              if (!control.reset && control.buttonId !== undefined) {
                setHardwareKey(control.buttonId, false);
              }
            }}
            onPointerLeave={() => {
              if (!control.reset && control.buttonId !== undefined) {
                setHardwareKey(control.buttonId, false);
              }
            }}
          >
            <span className="panel-physical-key__label">{control.label}</span>
          </button>
        ))}
      </div>
      {visual.ports?.length ? (
        <div className="panel-edge-ports" aria-hidden="true">
          {visual.ports.slice(0, 2).map((port) => (
            <span key={port} className="panel-edge-port" title={port} data-port={port}>
              <span>{port}</span>
            </span>
          ))}
        </div>
      ) : null}
      {visual.family === "t5s3" ? (
        <div className="panel-lilygo-side-legend" aria-hidden="true">
          <span>RST</span><span>BOOT</span><span>IO48</span><span>PWR</span>
        </div>
      ) : null}
    </div>
  );
}
