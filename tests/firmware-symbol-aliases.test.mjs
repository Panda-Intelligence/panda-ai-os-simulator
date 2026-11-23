import test from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const readSource = (file) =>
  readFileSync(new URL("../qemu-peripherals/" + file, import.meta.url), "utf8");
const machine = readSource("hw_xtensa_esp32s3.c");
const mirrored = readSource("esp32s3-soc-machine.patch")
  .split("\n")
  .filter((line) => line.startsWith("+") && !line.startsWith("+++"))
  .map((line) => line.slice(1))
  .join("\n");

function resolver(source) {
  const start = source.indexOf("static uint32_t mofei_resolve_elf_symbol_alias(");
  const end = source.indexOf("\n}\n", start);
  assert.ok(start >= 0 && end > start);
  return source.slice(start, end + 2);
}

function aliasTable(source) {
  const table = source.match(/\bpanda_syms\[\] = \{([\s\S]+?)\n        \};/);
  assert.ok(table);
  return [...table[1].matchAll(/\{"([^"]+)",\s*"([^"]+)",\s*&a->(\w+)\}/g)]
    .map((match) => match.slice(1));
}

test("QEMU uses identical current and legacy symbol pairs in the canonical SoC and bootstrap patch", () => {
  const pairs = aliasTable(machine);
  assert.deepEqual(aliasTable(mirrored), pairs);
  assert.equal(resolver(mirrored), resolver(machine));
  assert.ok(pairs.some(([name, legacy, field]) =>
    name === "pandaSimulatorMain" && legacy === "murphySimulatorMain" && field === "pandaSimulatorMain_addr"));
  assert.ok(pairs.some(([name, legacy]) =>
    name === "_ZN7pandaos11DeviceShell18startSimulatorLoopERKNS0_7ContextE" &&
    legacy === "_ZN8murphyos11DeviceShell18startSimulatorLoopERKNS0_7ContextE"));
  assert.ok(pairs.some(([name, legacy]) =>
    name === "_ZZL18runPandaDeviceMainvE5shell" && legacy === "_ZZL19runMurphyDeviceMainvE5shell"));
});

test("the real QEMU alias resolver prefers current ELF symbols and reports the matched name", () => {
  const dir = mkdtempSync(join(tmpdir(), "panda-firmware-symbols-"));
  try {
    const fixture = join(dir, "fixture.c");
    const binary = join(dir, "fixture");
    writeFileSync(fixture, `
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef size_t gsize;
static uint32_t current_address, legacy_address;
static unsigned lookups;

static uint32_t mofei_resolve_elf_symbol(const uint8_t* elf_data, gsize elf_len, const char* name) {
  (void)elf_data;
  (void)elf_len;
  ++lookups;
  if (strcmp(name, "pandaSimulatorMain") == 0) return current_address;
  if (strcmp(name, "murphySimulatorMain") == 0) return legacy_address;
  return 0;
}

${resolver(machine)}

int main(void) {
  const char* name = "pandaSimulatorMain";
  current_address = 0x42001000u;
  legacy_address = 0x42002000u;
  assert(mofei_resolve_elf_symbol_alias(NULL, 0, &name, "murphySimulatorMain") == current_address);
  assert(lookups == 1 && strcmp(name, "pandaSimulatorMain") == 0);

  current_address = 0;
  lookups = 0;
  assert(mofei_resolve_elf_symbol_alias(NULL, 0, &name, "murphySimulatorMain") == legacy_address);
  assert(lookups == 2 && strcmp(name, "murphySimulatorMain") == 0);

  legacy_address = 0;
  lookups = 0;
  name = "pandaSimulatorMain";
  assert(mofei_resolve_elf_symbol_alias(NULL, 0, &name, "murphySimulatorMain") == 0);
  assert(lookups == 2 && strcmp(name, "pandaSimulatorMain") == 0);
}
`);
    const compiled = spawnSync("cc", ["-std=c11", "-Wall", "-Wextra", "-Werror", fixture, "-o", binary], {
      encoding: "utf8",
    });
    assert.equal(compiled.status, 0, compiled.stderr);
    const run = spawnSync(binary, [], { encoding: "utf8", timeout: 5000 });
    assert.equal(run.status, 0, run.stderr);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
