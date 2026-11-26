import test from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL("..", import.meta.url));
const machine = readFileSync(process.env.NATIVE_NVS_MACHINE_TEST_SOURCE || join(root, "qemu-peripherals/hw_xtensa_esp32s3.c"), "utf8");
const patch = readFileSync(join(root, "qemu-peripherals/esp32s3-soc-machine.patch"), "utf8");
const mirrored = patch.split("\n")
  .filter((line) => (line.startsWith("+") && !line.startsWith("+++")) || line.startsWith(" "))
  .map((line) => line.slice(1)).join("\n");

function dispatch(source) {
  const start = source.indexOf("        {\n          const bool native_nvs =", source.indexOf("return NULL."));
  const end = source.indexOf("\n\n        fprintf(", start);
  assert.ok(start >= 0 && end > start);
  return source.slice(start, end);
}

test("bootstrap patch retains the real native NVS dispatcher opt-in", () => {
  assert.equal(dispatch(mirrored), dispatch(machine));
});

test("the actual dispatcher preserves legacy refusals and runs native NVS only on explicit opt-in", () => {
  const directory = mkdtempSync(join(tmpdir(), "panda-native-nvs-"));
  try {
    const fields = [...dispatch(machine).matchAll(/a->(\w+_addr)\b/g)].map((match) => match[1]);
    const unique = [...new Set(fields)];
    const initialization = unique.map((field, index) => `a.${field} = ${index + 1};`).join("\n");
    const program = `
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "mofei-sim-addrs.h"
static int g_strcmp0(const char* lhs, const char* rhs) { return lhs ? strcmp(lhs, rhs) : -1; }
static void populate(MofeiSimAddrs* a) {
${dispatch(machine)}
}
int main(void) {
  MofeiSimAddrs a = {0};
  ${initialization}
  unsetenv("MOFEI_SIM_NATIVE_NVS");
  populate(&a);
  assert(a.call_null_count == 6 && a.call_fail_count == 18);
  setenv("MOFEI_SIM_NATIVE_NVS", "yes", 1);
  a.call_null_count = a.call_fail_count = 0;
  populate(&a);
  assert(a.call_null_count == 6 && a.call_fail_count == 18);
  setenv("MOFEI_SIM_NATIVE_NVS", "1", 1);
  a.call_null_count = a.call_fail_count = 0;
  populate(&a);
  assert(a.call_null_count == 2 && a.call_fail_count == 4);
  assert(a.call_null_addrs[0] == a.esp_ota_get_running_partition_addr);
  assert(a.call_null_addrs[1] == a.esp_ota_get_next_update_partition_addr);
  assert(a.call_fail_addrs[0] == a.esp_ota_get_app_partition_count_addr);
  assert(a.call_fail_addrs[1] == a.esp_ota_begin_addr);
  assert(a.call_fail_addrs[2] == a.esp_ota_end_addr);
  assert(a.call_fail_addrs[3] == a.esp_ota_write_addr);
  return 0;
}
`;
    const source = join(directory, "dispatch.c");
    const binary = join(directory, "dispatch");
    writeFileSync(source, program);
    const compile = spawnSync(process.env.CC || "cc", ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-I", join(root, "qemu-peripherals"), source, "-o", binary], { encoding: "utf8" });
    assert.equal(compile.status, 0, compile.stderr);
    const result = spawnSync(binary, [], { encoding: "utf8" });
    assert.equal(result.status, 0, result.stderr);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

function retwSelections(source) {
  return [...source.matchAll(/const char\* want = retw_names\[ni\];/g)].map((match) => {
    const start = match.index;
    const end = source.indexOf("int ", start + match[0].length);
    assert.ok(end > start);
    return source.slice(start, end);
  });
}

test("both ELF loader selection paths preserve native NVS function bodies", () => {
  const selections = retwSelections(machine);
  assert.equal(selections.length, 2);
  const names = machine.match(/static const char\* retw_names\[\] = \{([\s\S]*?)\n        \};/)[1];
  const preserved = [...names.matchAll(/"(?:nvs_[^"]+|esp_partition_find|esp_partition_find_first|esp_partition_next|esp_partition_verify)"/g)].length;
  const predicate = machine.match(/static bool mofei_native_nvs_keeps_function\(const char\* name\) \{[\s\S]*?\n\}/)?.[0] || "";
  const directory = mkdtempSync(join(tmpdir(), "panda-native-nvs-loader-"));
  try {
    const source = join(directory, "loader.c");
    const binary = join(directory, "loader");
    writeFileSync(source, `
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
${predicate ? "static int g_strcmp0(const char* lhs, const char* rhs) { return lhs ? strcmp(lhs, rhs) : -1; }" : ""}
${predicate}
static const char* retw_names[] = {${names}};
${selections.map((selection, index) => `
static void selection${index}(void) {
  unsigned selected = 0;
  for (unsigned ni = 0; ni < sizeof(retw_names) / sizeof(retw_names[0]); ++ni) {
    ${selection}
    if (getenv("MOFEI_SIM_NATIVE_NVS") && !strcmp(getenv("MOFEI_SIM_NATIVE_NVS"), "1")) {
      assert(strncmp(want, "nvs_", 4));
      assert(strcmp(want, "esp_partition_find"));
      assert(strcmp(want, "esp_partition_find_first"));
      assert(strcmp(want, "esp_partition_next"));
      assert(strcmp(want, "esp_partition_verify"));
    }
    ++selected;
  }
  assert(selected == sizeof(retw_names) / sizeof(retw_names[0]) -
      ((getenv("MOFEI_SIM_NATIVE_NVS") && !strcmp(getenv("MOFEI_SIM_NATIVE_NVS"), "1")) ? ${preserved} : 0));
}`).join("\n")}
int main(void) {
  unsetenv("MOFEI_SIM_NATIVE_NVS"); selection0(); selection1();
  setenv("MOFEI_SIM_NATIVE_NVS", "yes", 1); selection0(); selection1();
  setenv("MOFEI_SIM_NATIVE_NVS", "1", 1); selection0(); selection1();
  return 0;
}
`);
    const compile = spawnSync(process.env.CC || "cc", ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", source, "-o", binary], { encoding: "utf8" });
    assert.equal(compile.status, 0, compile.stderr);
    const run = spawnSync(binary, [], { encoding: "utf8" });
    assert.equal(run.status, 0, run.stderr);
    assert.deepEqual(retwSelections(mirrored), selections);
    assert.ok(mirrored.includes(predicate));
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

function cacheRomPatches(source) {
  const start = source.indexOf("        static const uint8_t retw3[]", source.indexOf("Patch ROM functions that crash"));
  const end = source.indexOf("\n      }\n", source.indexOf("rom_thunk_retw[]", start));
  assert.ok(start >= 0 && end > start);
  return source.slice(start, end);
}

test("cache ROM patches match the canonical source and bootstrap patch", () => {
  assert.equal(cacheRomPatches(mirrored), cacheRomPatches(machine));
});

test("the actual cache ROM loop covers instruction and data resume while preserving entry frames", () => {
  const directory = mkdtempSync(join(tmpdir(), "panda-native-cache-rom-"));
  try {
    const source = join(directory, "cache.c");
    const binary = join(directory, "cache");
    writeFileSync(source, `
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mofei-sim-addrs.h"
static unsigned selected;
static void mofei_add_retw_patch(MofeiSimAddrs* addrs, uint32_t address, const char* name) {
  assert(addrs != NULL && name != NULL);
  if (address == 0x400018ab) selected |= 1;
  if (address == 0x4004f3f7) selected |= 2;
  if (address == 0x400018c3) selected |= 4;
  if (address == 0x4004f483) selected |= 8;
}
static void apply(uint8_t* rom_data, size_t rom_len) {
  MofeiSimAddrs mofei_sim_addrs = {0};
  ${cacheRomPatches(machine)}
}
int main(void) {
  const uint8_t thunk[] = {0x91, 0xff, 0xff, 0xa0, 0x09, 0x00};
  const uint8_t entry[] = {0x36, 0x41, 0x00};
  const uint8_t stub[] = {0x36, 0x21, 0x00, 0x1d, 0xf0};
  const uint8_t retw[] = {0x90, 0x00, 0x00};
  uint8_t* rom = malloc(0x60000);
  assert(rom != NULL);
  memset(rom, 0xcc, 0x60000);
  const uint32_t target = 0x4004f3f4;
  memcpy(rom + 0x18a4, &target, sizeof(target));
  memcpy(rom + 0x18a8, thunk, sizeof(thunk));
  memcpy(rom + 0x18c0, thunk, sizeof(thunk));
  memcpy(rom + 0x4f3f4, entry, sizeof(entry));
  memcpy(rom + 0x4f480, entry, sizeof(entry));
  apply(rom, 0x60000);
  assert(selected == 15);
  assert(!memcmp(rom + 0x18a8, stub, sizeof(stub)));
  assert(!memcmp(rom + 0x18c0, stub, sizeof(stub)));
  assert(!memcmp(rom + 0x4f3f4, entry, sizeof(entry)));
  assert(!memcmp(rom + 0x4f3f7, retw, sizeof(retw)));
  assert(!memcmp(rom + 0x4f480, entry, sizeof(entry)));
  assert(!memcmp(rom + 0x4f483, retw, sizeof(retw)));
  uint32_t preserved_target = 0;
  memcpy(&preserved_target, rom + 0x18a4, sizeof(preserved_target));
  assert(preserved_target == target);
  free(rom);
  rom = malloc(0x100);
  assert(rom != NULL);
  memset(rom, 0xcc, 0x100);
  selected = 0;
  apply(rom, 0x100);
  assert(selected == 0);
  for (unsigned i = 0; i < 0x100; ++i) assert(rom[i] == 0xcc);
  free(rom);
  return 0;
}
`);
    const compile = spawnSync(process.env.CC || "cc", ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", join(root, "qemu-peripherals"), source, "-o", binary], { encoding: "utf8" });
    assert.equal(compile.status, 0, compile.stderr);
    const result = spawnSync(binary, [], { encoding: "utf8" });
    assert.equal(result.status, 0, result.stderr);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
