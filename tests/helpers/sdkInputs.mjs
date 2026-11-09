import {existsSync} from "node:fs";
import {homedir} from "node:os";
import {join} from "node:path";

// Inputs that live outside the repo: the ESP-IDF checkout and the ROM image the Studio build ships.
// Without them a guard cannot run. Report that as a skip with the exact prerequisite, or fail loudly
// when PANDA_REQUIRE_SDK_INPUTS=1 (CI / release gates) so a missing input never reads as a pass.
export const idfPath = () =>
  process.env.IDF_PATH ||
  [join(homedir(), ".espressif/v6.0.1/esp-idf"), join(homedir(), "esp/esp-idf")].find(p => existsSync(p)) ||
  join(homedir(), ".espressif/v6.0.1/esp-idf");

export const romPath = () =>
  process.env.PANDA_ROM_SETJMP_BIN ||
  new URL("../../../panda-developers/public/studio-simulator/simulator-runtime/qemu/esp32s3_rev0_rom.bin", import.meta.url);

// Returns a skip reason (truthy) when any required file is missing, or false when the test can run.
export function missingInputs(label, files) {
  if (process.env.PANDA_REQUIRE_SDK_INPUTS === "1") return false;
  const missing = files.filter(f => !existsSync(f));
  return missing.length ? `${label}: missing ${missing.join(", ")} (set IDF_PATH or PANDA_ROM_SETJMP_BIN; PANDA_REQUIRE_SDK_INPUTS=1 turns this skip into a failure)` : false;
}
