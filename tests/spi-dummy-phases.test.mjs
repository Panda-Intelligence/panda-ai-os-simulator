import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, writeFileSync, mkdtempSync, mkdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";

const root = resolve(import.meta.dirname, "..");
const source = readFileSync(join(root, "qemu-peripherals/esp32s3_spi.c"), "utf8");
const pinnedControllerBlob = "90250e7e254ce13e23b7640a26522893bcd4b64e";

function gitBlobHash(text) {
  const bytes = Buffer.from(text);
  return createHash("sha1").update(`blob ${bytes.length}\0`).update(bytes).digest("hex");
}

function actualFunction(name) {
  const start = source.search(new RegExp(`static (?:inline )?void ${name}\\(`));
  const end = source.indexOf("\nstatic ", start + 1);
  assert.ok(start >= 0 && end > start, name);
  return source.slice(start, end);
}

test("SPI bootstrap patch round-trips the canonical controller", () => {
  assert.doesNotMatch(source, /see remaining:|rtk proxy|\.\.\. \(\+\d+ lines\)/);
  assert.match(source, /static void esp32s3_spi_write\(/);
  assert.match(source, /static void esp32s3_spi_realize\(/);
  assert.match(source, /type_init\(esp32s3_spi_register_types\)\s*$/);
  const directory = mkdtempSync(join(tmpdir(), "panda-spi-mirror-"));
  try {
    mkdirSync(join(directory, "hw/ssi"), { recursive: true });
    const path = join(directory, "hw/ssi/esp32s3_spi.c");
    writeFileSync(path, source);
    const patch = readFileSync(join(root, "qemu-peripherals/esp32s3-spi-dummy.patch"), "utf8");
    for (const reverse of [true, false]) {
      const result = spawnSync("patch", ["--batch", "-p1", ...(reverse ? ["-R"] : [])], {
        cwd: directory, input: patch, encoding: "utf8",
      });
      assert.equal(result.status, 0, result.stderr + result.stdout);
      if (reverse) {
        const pristine = readFileSync(path, "utf8");
        assert.equal(gitBlobHash(pristine), pinnedControllerBlob);
        assert.match(pristine, /\*len = \(dummy_count \+ 7\) \/ 8;/);
        assert.equal(source.slice(0, source.indexOf("static inline void esp32s3_spi_get_dummy(")), pristine.slice(0, pristine.indexOf("static inline void esp32s3_spi_get_dummy(")));
        assert.equal(source.slice(source.indexOf("static void esp32s3_spi_begin_transaction(")), pristine.slice(pristine.indexOf("static void esp32s3_spi_begin_transaction(")));
      }
    }
    assert.equal(readFileSync(path, "utf8"), source);
    const integration = readFileSync(join(root, "scripts/integrate-peripherals.sh"), "utf8");
    assert.match(integration, /cp -f "\$\{SRC_DIR\}\/esp32s3_spi.c" "\$\{SPI1_DST\}"/);
    assert.match(integration, /check_patch_against_clean_head "\$\{SRC_DIR\}\/esp32s3-spi-dummy.patch" hw\/ssi\/esp32s3_spi.c/);
    const wasm = readFileSync(join(root, "scripts/build-qemu-wasm.sh"), "utf8");
    assert.match(wasm, /copy_if_present "\$\{src\}\/esp32s3_spi.c" "\$\{QEMU_WASM_SRC_DIR\}\/hw\/ssi\/esp32s3_spi.c"/);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("actual SPI transactions preserve dummy phases, first and last read bytes, and register bounds", () => {
  const directory = mkdtempSync(join(tmpdir(), "panda-spi-dummy-"));
  try {
    let helper = actualFunction("esp32s3_spi_get_dummy");
    if (process.env.PANDA_SPI_DUMMY_MUTANT === "1") {
      helper = helper.replace("(cycles * lanes + 7) / 8", "(FIELD_EX32(s->mem_user1, SPI_MEM_USER1, USR_DUMMY_CYCLELEN) + 7) / 8 + 0 * cycles + 0 * lanes");
    }
    const c = `
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define MAX(a,b) ((a)>(b)?(a):(b))
#define FIELD_EX32(value,reg,field) (((value)>>reg##_##field##_SHIFT)&reg##_##field##_MASK)
#define SPI_MEM_MISC_CS0_DIS_SHIFT 0
#define SPI_MEM_MISC_CS0_DIS_MASK 1
#define SPI_MEM_MISC_CS1_DIS_SHIFT 1
#define SPI_MEM_MISC_CS1_DIS_MASK 1
#define SPI_MEM_ADDR_USR_ADDR_VALUE_SHIFT 0
#define SPI_MEM_ADDR_USR_ADDR_VALUE_MASK UINT32_MAX
#define SPI_MEM_USER1_USR_ADDR_BITLEN_SHIFT 26
#define SPI_MEM_USER1_USR_ADDR_BITLEN_MASK 63
#define SPI_MEM_USER1_USR_DUMMY_CYCLELEN_SHIFT 0
#define SPI_MEM_USER1_USR_DUMMY_CYCLELEN_MASK 63
#define SPI_MEM_USER2_USR_COMMAND_VALUE_SHIFT 0
#define SPI_MEM_USER2_USR_COMMAND_VALUE_MASK 65535
#define SPI_MEM_USER2_USR_COMMAND_BITLEN_SHIFT 28
#define SPI_MEM_USER2_USR_COMMAND_BITLEN_MASK 15
#define SPI_MEM_MISO_DLEN_USR_MISO_DBITLEN_SHIFT 0
#define SPI_MEM_MISO_DLEN_USR_MISO_DBITLEN_MASK 1023
#define SPI_MEM_MOSI_DLEN_USR_MOSI_DBITLEN_SHIFT 0
#define SPI_MEM_MOSI_DLEN_USR_MOSI_DBITLEN_MASK 1023
#define SPI_MEM_CTRL_FREAD_QIO_SHIFT 24
#define SPI_MEM_CTRL_FREAD_QIO_MASK 1
#define SPI_MEM_CTRL_FREAD_DIO_SHIFT 23
#define SPI_MEM_CTRL_FREAD_DIO_MASK 1
#define R_SPI_MEM_USER_USR_MISO_MASK (1u<<28)
#define R_SPI_MEM_USER_USR_MOSI_MASK (1u<<27)
#define R_SPI_MEM_USER_USR_ADDR_MASK (1u<<30)
#define R_SPI_MEM_USER_USR_DUMMY_MASK (1u<<29)
#define CMD_PP 2
typedef struct ESP32S3SpiState {
  uint32_t mem_ctrl,mem_user1,mem_user2,mem_user,mem_addr,mem_miso_len,mem_mosi_len,misc;
  int cs_gpio[3]; void *spi,*xts_aes;
  uint32_t pre_guard,data_reg[16],post_guard;
} ESP32S3SpiState;
typedef struct ESP32S3XtsAesClass {
  bool (*is_manual_enc_enabled)(void*);
  bool (*is_ciphertext_spi_visible)(void*);
  void (*read_ciphertext)(void*,void*,uint32_t*,uint32_t*,uint32_t*);
} ESP32S3XtsAesClass;
static ESP32S3XtsAesClass xts_class={0};
#define ESP32S3_XTS_AES_GET_CLASS(value) (&xts_class)
static uint32_t bswap32(uint32_t value) {return __builtin_bswap32(value);}
static unsigned wire_dummy,wire_phase,wire_reads,wire_transfers;
static uint32_t wire_command;
static bool selected;
static uint8_t payload[64];
static uint32_t ssi_transfer(void* bus,uint32_t value) {
  (void)bus; assert(selected); ++wire_transfers;
  unsigned phase=wire_phase++;
  if(phase==0) {assert(value==wire_command);return 0;}
  if(phase<4) {const uint8_t address[]={0,0x90,0};assert(value==address[phase-1]);return 0;}
  if(phase<4+wire_dummy) return 0;
  assert(value==0); assert(wire_reads<sizeof(payload));
  return payload[wire_reads++];
}
static void qemu_set_irq(int pin,int level) {
  if(pin!=0) return;
  if(level==0) {selected=true;wire_phase=wire_reads=wire_transfers=0;}
  else selected=false;
}
${source.slice(source.indexOf("typedef struct ESP32S3SpiTransaction {"), source.indexOf("\n\nstatic uint64_t esp32s3_spi_read"))}
${actualFunction("esp32s3_spi_txrx_buffer")}
${actualFunction("esp32s3_spi_dummy_cycles")}
${actualFunction("esp32s3_spi_cs_set")}
${actualFunction("esp32s3_spi_perform_transaction")}
${actualFunction("esp32s3_spi_get_addr")}
${helper}
${actualFunction("esp32s3_spi_begin_transaction")}
static void transaction(uint32_t ctrl,unsigned raw,unsigned expected_dummy,bool enabled,unsigned read_bytes) {
  ESP32S3SpiState state={0};
  state.mem_ctrl=ctrl;state.mem_user1=(23u<<26)|raw;
  wire_command=!enabled?0x03:(ctrl&(1u<<24))?0xeb:(ctrl&(1u<<23))?0xbb:(ctrl&(1u<<20))?0x6b:(ctrl&(1u<<14))?0x3b:0x0b;
  state.mem_user2=(7u<<28)|wire_command;
  state.mem_user=(1u<<31)|R_SPI_MEM_USER_USR_ADDR_MASK|R_SPI_MEM_USER_USR_MISO_MASK;
  if(enabled) state.mem_user|=R_SPI_MEM_USER_USR_DUMMY_MASK;
  state.mem_addr=0x9000;state.mem_miso_len=read_bytes*8-1;state.misc=2;
  state.cs_gpio[0]=0;state.cs_gpio[1]=1;
  state.pre_guard=0x12345678;state.post_guard=0x87654321;
  memset(state.data_reg,0xcc,sizeof(state.data_reg));
  wire_dummy=expected_dummy;
  esp32s3_spi_begin_transaction(&state);
  assert(wire_transfers==1+3+expected_dummy+read_bytes);
  assert(wire_reads==read_bytes);
  assert(memcmp(state.data_reg,payload,read_bytes)==0);
  assert(((uint8_t*)state.data_reg)[0]==payload[0]);
  assert(((uint8_t*)state.data_reg)[read_bytes-1]==payload[read_bytes-1]);
  for(unsigned i=read_bytes;i<sizeof(state.data_reg);++i) assert(((uint8_t*)state.data_reg)[i]==0xcc);
  assert(state.pre_guard==0x12345678 && state.post_guard==0x87654321);
}
int main(void) {
  for(unsigned i=0;i<sizeof(payload);++i) payload[i]=(uint8_t)(0xa5^i);
  transaction(1u<<24,5,3,true,32);
  transaction(1u<<23,3,1,true,32);
  transaction(0,7,1,true,32);
  transaction(1u<<20,7,1,true,32);
  transaction(1u<<14,7,1,true,32);
  transaction(0,8,2,true,32);
  for(unsigned raw=0;raw<64;++raw) {
    const uint32_t control[]={0,1u<<14,1u<<20,1u<<23,1u<<24};
    const unsigned lanes[]={1,1,1,2,4};
    for(unsigned mode=0;mode<5;++mode) {
      unsigned expected=((raw+1)*lanes[mode]+7)/8;
      transaction(control[mode],raw,expected,true,64);
      transaction(control[mode],raw,0,false,64);
    }
  }
  return 0;
}
`;
    const path = join(directory, "phases.c");
    const binary = join(directory, "phases");
    writeFileSync(path, c);
    const compile = spawnSync(process.env.CC || "cc", ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", path, "-o", binary], { encoding: "utf8" });
    assert.equal(compile.status, 0, compile.stderr);
    const result = spawnSync(binary, [], { encoding: "utf8" });
    assert.equal(result.status, 0, result.stderr);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
