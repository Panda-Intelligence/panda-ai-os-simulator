import type { SimulatorBoard } from "./boards";

export type PhysicalControlPlacement = {
  /** Stable hardware name; independent of keyMap and DOM order. */
  name: string;
  edge: "left" | "right";
  /** Portrait-front coordinates; body top=0, bottom=1. These are image-derived. */
  center: number;
  length: number;
  projection: number;
  reset?: boolean;
};
export type PhysicalControl = PhysicalControlPlacement & { buttonId?: number };

// Placement is digitized from the manufacturer side views (not dimensioned CAD
// button coordinates). Exact image URLs and uncertainty: docs/button-alignment.md.
export const PHYSICAL_CONTROL_LAYOUTS: Readonly<Record<string, readonly PhysicalControlPlacement[]>> = {
  m5papers3: [
    {name:"PWR", edge:"right", center:.825, length:.082, projection:.012},
  ],
  "lilygo-t5s3-pro": [
    {name:"RST", edge:"right", center:.590, length:.042, projection:.016, reset:true},
    {name:"PWR", edge:"right", center:.680, length:.042, projection:.016},
    {name:"BOOT", edge:"left", center:.575, length:.042, projection:.016},
    {name:"IO48", edge:"left", center:.665, length:.042, projection:.016},
  ],
};

export type PhysicalControlKind = "page" | "nav" | "confirm" | "utility" | "power";

export type PhysicalControlEdge =
  | "left"
  | "right"
  | "right-upper"
  | "right-lower"
  | "top-left"
  | "top-right"
  | "bottom-left"
  | "bottom-center"
  | "bottom-right";

export type RawControlDefinition = {
  id: string;
  label: string;
  shortLabel: string;
  key: string;
  edge: PhysicalControlEdge;
  kind: PhysicalControlKind;
};

export type RawControlProfile = {
  touchEnabled: boolean;
  controls: readonly RawControlDefinition[];
};

export const RAW_CONTROL_PROFILES: Readonly<Record<string, RawControlProfile>> = {
  "read-pico": {
    touchEnabled: false,
    controls: [
      { id: "page-up", label: "Page Up", shortLabel: "PgUp", key: "up", edge: "right-upper", kind: "page" },
      { id: "page-down", label: "Page Down", shortLabel: "PgDn", key: "down", edge: "right-lower", kind: "page" },
      { id: "power", label: "Power", shortLabel: "Power", key: "select", edge: "top-right", kind: "power" },
    ],
  },
  molink: {
    touchEnabled: false,
    controls: [
      { id: "nav-left", label: "Left", shortLabel: "Left", key: "left", edge: "bottom-left", kind: "nav" },
      { id: "nav-up", label: "Up", shortLabel: "Up", key: "up", edge: "right-upper", kind: "page" },
      { id: "nav-down", label: "Down", shortLabel: "Down", key: "down", edge: "right-lower", kind: "page" },
      { id: "nav-right", label: "Right", shortLabel: "Right", key: "right", edge: "bottom-right", kind: "nav" },
      { id: "nav-select", label: "Confirm", shortLabel: "OK", key: "select", edge: "bottom-center", kind: "confirm" },
      { id: "nav-back", label: "Back", shortLabel: "Back", key: "back", edge: "top-left", kind: "utility" },
    ],
  },
  x4pro: {
    touchEnabled: true,
    controls: [
      { id: "page-up", label: "Page Up", shortLabel: "PgUp", key: "up", edge: "right-upper", kind: "page" },
      { id: "page-down", label: "Page Down", shortLabel: "PgDn", key: "down", edge: "right-lower", kind: "page" },
      { id: "power", label: "Power", shortLabel: "Power", key: "select", edge: "top-right", kind: "power" },
    ],
  },
  "metalio-eink4": {
    touchEnabled: true,
    controls: [
      { id: "power", label: "Power", shortLabel: "Power", key: "select", edge: "top-right", kind: "power" },
    ],
  },
  "eego-a4": {
    touchEnabled: true,
    controls: [
      { id: "page-up", label: "Page Up", shortLabel: "PgUp", key: "up", edge: "right-upper", kind: "page" },
      { id: "page-down", label: "Page Down", shortLabel: "PgDn", key: "down", edge: "right-lower", kind: "page" },
      { id: "power", label: "Power", shortLabel: "Power", key: "select", edge: "top-right", kind: "power" },
    ],
  },
};

export function getPhysicalControls(board: Pick<SimulatorBoard,"id" | "keyMap">): PhysicalControl[] | null {
  const placements=PHYSICAL_CONTROL_LAYOUTS[board.id];
  if (!placements) return null; // Other boards retain their existing layout.
  return placements.map(placement=>{
    if (placement.reset) return {...placement}; // RST is reset, never a guessed GPIO id.
    const matches=board.keyMap.filter(key=>key.label.toUpperCase() === placement.name);
    if (matches.length !== 1) throw new Error(`physical_control_mapping_invalid:${board.id}:${placement.name}`);
    return {...placement,buttonId:matches[0].id};
  });
}
