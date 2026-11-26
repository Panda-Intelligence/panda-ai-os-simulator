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
  const preserved = [...names.matchAll(/"(?:nvs_[^"]+|esp_partition_find|esp_partition_find_first|esp_partition_next|esp_partition_verify|esp_mmu_map_init)"/g)].length;
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
      assert(strcmp(want, "esp_mmu_map_init"));
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

function nativeLayout(source) {
  const start = source.indexOf("typedef struct MofeiNativeCacheRange {");
  const end = source.indexOf("\nstatic void esp32s3_machine_init(", start);
  assert.ok(start >= 0 && end > start);
  return source.slice(start, end);
}

test("native ELF cache layouts preserve load memory and heap while exposing the existing MMU window", () => {
  const directory = mkdtempSync(join(tmpdir(), "panda-native-layout-"));
  try {
    const source = join(directory, "layout.c");
    const binary = join(directory, "layout");
    const glib = spawnSync("pkg-config", ["--cflags", "--libs", "glib-2.0"], { encoding: "utf8" });
    assert.equal(glib.status, 0, glib.stderr);
    writeFileSync(source, `
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include "mofei-sim-addrs.h"
#define ELFMAG "\\177ELF"
#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2
#define EV_CURRENT 1
#define ET_EXEC 2
#define EM_XTENSA 94
#define PT_LOAD 1
#define ESP32S3_PAGE_SIZE 0x10000u
typedef void Object;
typedef struct MemoryRegion { struct MemoryRegion* backing; uint32_t offset, size; } MemoryRegion;
typedef struct Alias { MemoryRegion* container; MemoryRegion* region; uint32_t address; int priority; } Alias;
static Alias aliases[32];
static unsigned alias_count;
static void memory_region_init_alias(MemoryRegion* region, Object* owner, const char* name, MemoryRegion* backing, uint32_t offset, uint32_t size) {
  (void)owner; assert(name != NULL); *region = (MemoryRegion){backing, offset, size};
}
static void memory_region_add_subregion_overlap(MemoryRegion* container, uint32_t address, MemoryRegion* region, int priority) {
  assert(alias_count < 32); aliases[alias_count++] = (Alias){container, region, address, priority};
}
${nativeLayout(machine)}
static void put16(uint8_t* data, uint16_t value, bool be) {
  data[be ? 1 : 0] = value; data[be ? 0 : 1] = value >> 8;
}
static void put32(uint8_t* data, uint32_t value, bool be) {
  for (unsigned i = 0; i < 4; ++i) data[be ? 3-i : i] = value >> (i*8);
}
static void make_elf(uint8_t data[512], bool be) {
  memset(data, 0, 512); memcpy(data, ELFMAG, 4); data[4]=1; data[5]=be ? 2 : 1; data[6]=1;
  put16(data+16, ET_EXEC, be); put16(data+18, EM_XTENSA, be); put32(data+20, EV_CURRENT, be);
  put32(data+28,52,be); put16(data+40,52,be); put16(data+42,32,be); put16(data+44,4,be);
  const uint32_t vaddr[] = {0x3c000020,0x3c520020,0x42000020,0x3fc90000};
  const uint32_t filesz[] = {0,16,16,16};
  const uint32_t memsz[] = {0x66ffe0,0x14d5a2,0x51f582,32};
  for(unsigned i=0;i<4;++i) {
    uint8_t* ph=data+52+i*32; put32(ph,PT_LOAD,be); put32(ph+4,256,be); put32(ph+8,vaddr[i],be); put32(ph+12,vaddr[i],be);
    put32(ph+16,filesz[i],be); put32(ph+20,memsz[i],be);
  }
}
static void verify_layout(GArray* ranges, bool exact) {
  assert(ranges != NULL);
  if(exact) {
    assert(ranges->len==3);
    const MofeiNativeCacheRange* d=&g_array_index(ranges,MofeiNativeCacheRange,0);
    const MofeiNativeCacheRange* h=&g_array_index(ranges,MofeiNativeCacheRange,1);
    const MofeiNativeCacheRange* i=&g_array_index(ranges,MofeiNativeCacheRange,2);
    assert(d->start==0x3c000000 && d->end==0x3c670000 && !d->instruction);
    assert(h->start==MOFEI_SIM_ALLOC_HEAP_BASE && h->end==MOFEI_SIM_PSRAM_HEAP_LIMIT && !h->instruction);
    assert(i->start==0x42000000 && i->end==0x42520000 && i->instruction);
  }
  MemoryRegion system={0}, cpu={0}, instruction={0}, data={0};
  alias_count=0;
  mofei_install_native_cache_ranges(NULL,&system,&cpu,&instruction,&data,ranges);
  unsigned system_ranges=0,cpu_ranges=0,low_ranges=0,instruction_ranges=0;
  for(unsigned i=0;i<ranges->len;++i) if(g_array_index(ranges,MofeiNativeCacheRange,i).instruction) ++instruction_ranges;
  for(unsigned i=0;i<alias_count;++i) {
    const Alias* alias=&aliases[i];
    if(alias->container==&system) { assert(alias->priority==1); ++system_ranges; }
    else { assert(alias->container==&cpu); if(alias->priority==2) ++cpu_ranges; }
    if(alias->address>=MOFEI_SIM_DCACHE_BASE && alias->address<MOFEI_SIM_DCACHE_LIMIT) {
      assert(alias->region->backing==&data);
      assert(alias->region->offset==alias->address-MOFEI_SIM_DCACHE_BASE);
      assert(alias->address+alias->region->size<=MOFEI_SIM_PSRAM_HEAP_LIMIT);
    } else if(alias->address>=MOFEI_SIM_ICACHE_BASE) {
      assert(alias->region->backing==&instruction);
      assert(alias->region->offset==alias->address-MOFEI_SIM_ICACHE_BASE);
    } else {
      ++low_ranges;
      assert(alias->container==&cpu && alias->priority==1 && alias->region->backing==&instruction);
      assert(alias->region->offset==alias->address-0x02000000u);
      bool matched=false;
      for(unsigned i=0;i<ranges->len;++i) {
        const MofeiNativeCacheRange* range=&g_array_index(ranges,MofeiNativeCacheRange,i);
        if(range->instruction && alias->address==0x02000000u+range->start-MOFEI_SIM_ICACHE_BASE &&
           alias->region->offset==range->start-MOFEI_SIM_ICACHE_BASE && alias->region->size==range->end-range->start) matched=true;
      }
      assert(matched);
    }
    g_free(alias->region);
  }
  assert(system_ranges==ranges->len && cpu_ranges==ranges->len);
  assert(low_ranges==instruction_ranges);
}
int main(int argc, char** argv) {
  uint8_t bytes[512]; const char* error=NULL; GArray* ranges;
  for(unsigned endian=0;endian<2;++endian) {
    make_elf(bytes,endian);
    ranges=mofei_native_elf_cache_ranges(bytes,sizeof(bytes),"device",true,&error);
    verify_layout(ranges,true); g_array_free(ranges,true);
  }
  make_elf(bytes,false);
  assert(!mofei_native_elf_cache_ranges(bytes,sizeof(bytes),"device",false,&error));
  assert(strstr(error,"MTD"));
  assert(!mofei_native_elf_cache_ranges(bytes,sizeof(bytes),"TEST",true,&error));
  assert(strstr(error,"test heap"));
  assert(!mofei_native_elf_cache_ranges(NULL,0,"device",true,&error));
  for(unsigned scenario=0;scenario<11;++scenario) {
    make_elf(bytes,false);
    switch(scenario) {
      case 0: bytes[0]=0; break;
      case 1: bytes[4]=2; break;
      case 2: bytes[5]=0; break;
      case 3: put32(bytes+28,0xfffffff0,false); break;
      case 4: put16(bytes+42,1,false); break;
      case 5: put16(bytes+44,0,false); break;
      case 6: put32(bytes+52+16,513,false); break;
      case 7: put32(bytes+52+8,0xfffffff0,false); put32(bytes+52+12,0xfffffff0,false); put32(bytes+52+20,32,false); break;
      case 8: put32(bytes+52+8,0x3dfff000,false); put32(bytes+52+12,0x3dfff000,false); put32(bytes+52+20,0x2000,false); break;
      case 9: put32(bytes+52+8,MOFEI_SIM_ALLOC_HEAP_BASE,false); put32(bytes+52+12,MOFEI_SIM_ALLOC_HEAP_BASE,false); put32(bytes+52+20,64,false); break;
      case 10: put32(bytes+52+12,0x3c000024,false); break;
    }
    assert(!mofei_native_elf_cache_ranges(bytes,sizeof(bytes),"device",true,&error));
    assert(error!=NULL);
    if(scenario==6 || scenario==7) assert(strstr(error,"overflows"));
    if(scenario==8) assert(strstr(error,"crosses"));
    if(scenario==9) assert(strstr(error,"overlaps"));
    if(scenario==10) assert(strstr(error,"unsupported"));
  }
  if(argc>1) {
    gchar* actual=NULL; gsize length=0; assert(g_file_get_contents(argv[1],&actual,&length,NULL));
    ranges=mofei_native_elf_cache_ranges((uint8_t*)actual,length,"device",true,&error);
    verify_layout(ranges,false); g_array_free(ranges,true); g_free(actual);
  }
  return 0;
}
`);
    const command = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", join(root, "qemu-peripherals"), source, ...glib.stdout.trim().split(/\s+/), "-o", binary];
    const compile = spawnSync(process.env.CC || "cc", command, { encoding: "utf8" });
    assert.equal(compile.status, 0, compile.stderr);
    const args = process.env.NATIVE_NVS_TEST_GUEST_ELF ? [process.env.NATIVE_NVS_TEST_GUEST_ELF] : [];
    const run = spawnSync(binary, args, { encoding: "utf8" });
    assert.equal(run.status, 0, run.stderr);
    assert.equal(nativeLayout(mirrored), nativeLayout(machine));
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
