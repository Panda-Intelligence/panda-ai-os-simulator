import type { SimulatorHostLocation } from "./simulatorBridgeTypes";

// Deliberate test location. Never consult browser permission, IP or host timezone.
export const SIMULATED_LONDON_LOCATION: Readonly<SimulatorHostLocation> = Object.freeze({
  name: "London",
  timezone: "Europe%2FLondon",
  latitude: 51.5074,
  longitude: -0.1278,
});

export function createSimulatedLocation(): SimulatorHostLocation {
  return { ...SIMULATED_LONDON_LOCATION };
}

// Hidden and FAT-visible aliases consumed by the simulator firmware contract.
export const SIMULATED_LOCATION_PATHS = Object.freeze([
  "/.mofei/simulator_location.json",
  "/mofei/simulator_location.json",
]);

// Boot wall clock (UTC seconds, timezone-free) read by PANDA_SIMULATOR firmware so the status bar shows the
// time like a device instead of "--:--"; QEMU has no NTP, RTC battery or GNSS source for the guest.
export const SIMULATED_CLOCK_PATHS = Object.freeze([
  "/.mofei/simulator_clock.txt",
  "/mofei/simulator_clock.txt",
]);

export function simulatedClockPayload(nowUnixSeconds = Math.floor(Date.now() / 1000)): Uint8Array {
  return new TextEncoder().encode(`${nowUnixSeconds}\n`);
}

export async function prepareSimulatedLocationImage<T>(
  sdCard: { bytes: Uint8Array; templateConflict?: boolean } | null,
  edit: (bytes: Uint8Array, action: (fs: T) => void) => Promise<unknown>,
  write: (fs: T, path: string, bytes: Uint8Array) => void,
): Promise<boolean> {
  if (!sdCard) return false;
  if (sdCard.templateConflict) throw new Error("browser_sd_card_template_conflict");
  // Publish the edited in-memory image only after both alias writes succeed.
  // A failed boot preparation must not modify the cached or persisted user SD.
  const candidate = sdCard.bytes.slice();
  const payload = new TextEncoder().encode(JSON.stringify(createSimulatedLocation()) + "\n");
  const clock = simulatedClockPayload();
  await edit(candidate, (fs) => {
    for (const path of SIMULATED_LOCATION_PATHS) write(fs, path, payload);
    for (const path of SIMULATED_CLOCK_PATHS) write(fs, path, clock);
  });
  sdCard.bytes = candidate;
  return true;
}
