import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

function body(source, signature) {
  const start = source.indexOf(signature);
  assert.ok(start >= 0, signature);
  let end = source.indexOf("{", start) + 1;
  for (let depth = 1; depth; ++end) {
    if (source[end] === "{") ++depth;
    if (source[end] === "}") --depth;
  }
  return source.slice(start, end);
}

const control = readFileSync(new URL("../qemu-peripherals/lilygo_peripheral_control.c", import.meta.url), "utf8");
const sd = readFileSync(new URL("../qemu-peripherals/sd.c", import.meta.url), "utf8");
const header = fileURLToPath(new URL("../qemu-peripherals/lilygo_peripheral_control.h", import.meta.url));

// Compile the production dispatcher, SD handler, display dispatch and wire writer.
// Only unrelated device backends and the final chardev/Wasm byte sink are stubs.
// A NULL LilyGo SPI-SD backend models Mofei's normal SD/MMC configuration.
for (const [display, wasm] of [["ssd1677", false], ["uc8253c", false], ["ssd1677", true]]) {
  test(`transport QUERY fences completed ${display} output without SPI-SD (${wasm ? "Wasm" : "native"})`, (t) => {
    const displayFile = display === "ssd1677" ? "ssd1677_gdeq0426t82.c" : "uc8253c.c";
    const source = readFileSync(new URL(`../qemu-peripherals/${displayFile}`, import.meta.url), "utf8");
    const stateType = display === "ssd1677" ? "SSD1677State" : "UC8253CState";
    const definitions = [
      ...control.matchAll(/^#define LILYGO_PERIPHERAL_(?:DEVICE_|CONTROL_HEADER_SIZE|ACK_SIZE).*$/gm),
      ...sd.matchAll(/^#define LILYGO_SD_.*$/gm),
      ...source.matchAll(/^#define IPC_CHANNEL_.*$/gm),
    ].map((match) => match[0]).join("\n");
    const dir = mkdtempSync(join(tmpdir(), "panda-transport-barrier-"));
    try {
      writeFileSync(join(dir, "barrier.c"), `
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include ${JSON.stringify(header)}
#define BIT(n) (1u << (n))
#define DEVICE(sd) (sd)
#define LOG_GUEST_ERROR 1
#define EMSCRIPTEN_KEEPALIVE
#define g_malloc malloc
#define g_free free
#define LILYGO_BQ27220_CONTROL_DEVICE 4
#define LILYGO_SX1262_CONTROL_DEVICE 5
#define LILYGO_BQ27220_CONTROL_OK 0
typedef uint8_t LilygoBq27220ControlError;
${definitions}

static unsigned backend_calls;
static bool unexpected_bool(const uint8_t* payload, uint32_t len) {
  (void)payload; (void)len; ++backend_calls; return false;
}
static uint8_t unexpected_control(uint8_t operation, uint8_t flags, const uint8_t* payload, uint32_t len) {
  (void)operation; (void)flags; (void)payload; (void)len; ++backend_calls;
  return LILYGO_PERIPHERAL_ERROR_UNKNOWN_DEVICE;
}
static uint8_t unexpected_simple(uint8_t operation, const uint8_t* payload, uint32_t len) {
  return unexpected_control(operation, 0, payload, len);
}
#define lilygo_i2c_probe_control unexpected_bool
#define esp32s3_gpspi2_control unexpected_bool
#define lilygo_bq27220_control unexpected_control
#define lilygo_sx1262_control unexpected_control
#define lilygo_bq25896_control unexpected_simple
#define lilygo_pcf8563_control unexpected_simple
#define lilygo_gnss_chardev_control unexpected_simple
#define lilygo_tps65185_control unexpected_simple
bool esp32s3_gpspi2_transaction_active(void) { ++backend_calls; return false; }
static void send_bq27220_trace(const void* trace) { (void)trace; assert(false); }
static void send_power_rtc_trace(const void* trace) { (void)trace; assert(false); }
static void lilygo_bq27220_set_trace_sink(void (*sink)(const void*)) { (void)sink; }
static void lilygo_power_rtc_set_trace_sink(void (*sink)(const void*)) { (void)sink; }
static void (*g_lilygo_peripheral_ipc_sender)(const uint8_t*, uint32_t);
${body(control, "static uint32_t read_le32(")}
${body(control, "static void write_le32(")}
${body(control, "static void send_ack(")}
${body(control, "void lilygo_peripheral_control_set_ipc_sender(")}
${body(control, "void lilygo_peripheral_control_emit(")}
${body(control, "void lilygo_peripheral_control_dispatch(")}
${body(control, "void mofei_wasm_i2c_control(")}

typedef struct {
  bool lilygo_present, lilygo_writable, lilygo_read_fault, lilygo_read_fault_once;
  bool lilygo_write_fault, lilygo_write_fault_once, lilygo_corrupt_fault, lilygo_preserve_image;
} SDState;
static SDState* g_lilygo_sd;
static void lilygo_sd_restore_seed(SDState* sd) { (void)sd; assert(false); }
static void device_cold_reset(SDState* sd) { (void)sd; assert(false); }
${body(sd, "static uint32_t lilygo_sd_read_le32(")}
${body(sd, "static void lilygo_sd_write_le32(")}
${body(sd, "static void lilygo_sd_send_ack(")}
${body(sd, "static void lilygo_sd_send_snapshot(")}
${body(sd, "static void lilygo_sd_reset_faults(")}
${body(sd, "bool lilygo_sd_control(")}

static uint8_t wire[4096];
static size_t wire_len;
static bool interleave_ack;
static bool short_write;
static unsigned write_errors;
static uint8_t interleaved_query[10];
static void dispatch(const uint8_t* payload, uint32_t len);
static void append_bytes(const uint8_t* bytes, uint32_t len) {
  assert(wire_len + len <= sizeof(wire));
  memcpy(wire + wire_len, bytes, len); wire_len += len;
}
typedef struct {
  int chr;
  uint8_t inbound_header[8], inbound_payload[65];
  uint32_t inbound_payload_target;
} ${stateType};
static ${stateType} display_state;
static ${stateType}* g_${display} = &display_state;
static int qemu_chr_fe_write_all(int* chr, const uint8_t* payload, uint32_t len) {
  assert(chr == &display_state.chr);
  if (short_write) { append_bytes(payload, len - 1); return (int)len - 1; }
  append_bytes(payload, len);
  // The real chardev serializes each write call. Let another producer win at
  // the next call boundary: the old separate header/payload writes interleave.
  if (interleave_ack) {
    interleave_ack = false;
    dispatch(interleaved_query, sizeof(interleaved_query));
  }
  return (int)len;
}
#ifdef TRANSPORT_SPLIT_WRITE_NEGATIVE_CONTROL
static int split_write_all(int* chr, const uint8_t* payload, uint32_t len) {
  assert(len >= 8);
  int header_len = qemu_chr_fe_write_all(chr, payload, 8);
  return header_len + (len > 8 ? qemu_chr_fe_write_all(chr, payload + 8, len - 8) : 0);
}
#define qemu_chr_fe_write_all split_write_all
#endif
static void qemu_log_mask(int mask, const char* message) { (void)mask; (void)message; ++write_errors; }
static void mofei_wasm_ipc_send(uint32_t channel, uint32_t flags, const uint8_t* payload, uint32_t len) {
  uint8_t packet_header[8] = {(uint8_t)channel, (uint8_t)flags, 0, 0};
  write_le32(packet_header + 4, len); append_bytes(packet_header, sizeof(packet_header)); append_bytes(payload, len);
}
static void unexpected_input(const uint8_t* payload, uint32_t len) { (void)payload; (void)len; assert(false); }
static void unexpected_button(uint8_t button, uint8_t pressed) { (void)button; (void)pressed; assert(false); }
#define ssd1677_inject_touch unexpected_input
#define ssd1677_inject_button unexpected_button
#define chsc6440_inject_touch unexpected_input
#define chsc6440_inject_button unexpected_button
static void mofeiSimulatorTouchEventWrite(uint8_t type, uint16_t x, uint16_t y) {
  (void)type; (void)x; (void)y; assert(false);
}
static bool mofeiSimulatorPandaDebugPollResponse(void) { assert(false); return false; }
static bool mofeiSimulatorPandaDebugWriteRequest(const uint8_t* payload, uint32_t len) {
  (void)payload; (void)len; assert(false); return false;
}
static void mofeiSimulatorConsoleWrite(const uint8_t* payload, uint32_t len) { unexpected_input(payload, len); }
${body(source, `static void ipc_send_frame(${stateType}*`)}
${body(source, `static void ${display}_send_control(`)}
${body(source, `static void ${display}_dispatch_inbound_frame(`)}

static void dispatch(const uint8_t* payload, uint32_t len) {
  if (${wasm ? 1 : 0}) {
    mofei_wasm_i2c_control(payload, len);
  } else {
    assert(len <= sizeof(display_state.inbound_payload));
    display_state.inbound_header[0] = IPC_CHANNEL_CONTROL;
    memcpy(display_state.inbound_payload, payload, len);
    display_state.inbound_payload_target = len;
    ${display}_dispatch_inbound_frame(&display_state);
  }
}
static void expect_ack(const uint8_t* payload, uint32_t len, uint8_t device, uint32_t request_id, uint8_t error) {
  wire_len = 0;
  dispatch(payload, len);
  const uint8_t packet_header[8] = {IPC_CHANNEL_CONTROL, 0, 0, 0, 12, 0, 0, 0};
  uint8_t ack[12] = {1, 0x80, device, error == 0 ? 1 : 0, 0, 0, 0, 0, error, 0, 0, 0};
  write_le32(ack + 4, request_id);
  assert(wire_len == sizeof(packet_header) + sizeof(ack));
  assert(memcmp(wire, packet_header, sizeof(packet_header)) == 0);
  assert(memcmp(wire + sizeof(packet_header), ack, sizeof(ack)) == 0);
  assert(backend_calls == 0 && g_lilygo_sd == NULL);
}

int main(void) {
  assert(LILYGO_PERIPHERAL_DEVICE_TRANSPORT == 0);
  lilygo_peripheral_control_set_ipc_sender(${display}_send_control);
  uint8_t query[65] = {1, LILYGO_PERIPHERAL_OPERATION_QUERY, LILYGO_PERIPHERAL_DEVICE_TRANSPORT, 0};
  const uint32_t ids[] = {0, 1, 0x01234567, UINT32_MAX};
  for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
    write_le32(query + 4, ids[i]);
    expect_ack(query, 10, 0, ids[i], LILYGO_PERIPHERAL_ERROR_NONE);
  }
  const uint32_t id = 0x01234567;
  write_le32(query + 4, id);
  // Mofei has no registered LilyGo SPI-SD backend. Its real handler must still NAK.
  query[2] = LILYGO_SD_CONTROL_DEVICE;
  expect_ack(query, 10, 3, id, LILYGO_PERIPHERAL_ERROR_UNKNOWN_DEVICE);
  query[2] = 255;
  expect_ack(query, 10, 255, id, LILYGO_PERIPHERAL_ERROR_UNKNOWN_DEVICE);
  query[2] = 0;
  for (unsigned op = 0; op < 256; ++op) {
    if (op == LILYGO_PERIPHERAL_OPERATION_QUERY) continue;
    query[1] = (uint8_t)op;
    expect_ack(query, 10, 0, id, LILYGO_PERIPHERAL_ERROR_UNKNOWN_OPERATION);
  }
  query[1] = LILYGO_PERIPHERAL_OPERATION_QUERY;
  query[0] = 2;
  expect_ack(query, 10, 0, id, LILYGO_PERIPHERAL_ERROR_UNSUPPORTED_VERSION);
  query[0] = 1;
  for (unsigned index = 0; index < 3; ++index) {
    const unsigned offset = (unsigned[]){3, 8, 9}[index];
    query[offset] = 1;
    expect_ack(query, 10, 0, id, LILYGO_PERIPHERAL_ERROR_MALFORMED);
    query[offset] = 0;
  }
  expect_ack(query, 9, 0, id, LILYGO_PERIPHERAL_ERROR_MALFORMED);
  expect_ack(query, 11, 0, id, LILYGO_PERIPHERAL_ERROR_MALFORMED);
  expect_ack(query, 65, 0, id, LILYGO_PERIPHERAL_ERROR_PAYLOAD_TOO_LARGE);
  query[8] = 1;
  expect_ack(query, 11, 0, id, LILYGO_PERIPHERAL_ERROR_MALFORMED);
  query[8] = 0;
  // A completed frame and the barrier ACK use the very same production writer.
  wire_len = 0;
  const uint8_t pixels[4] = {0x55, 0xaa, 0x00, 0xff};
  ipc_send_frame(&display_state, IPC_CHANNEL_FRAMEBUFFER, 2, pixels, sizeof(pixels));
  dispatch(query, 10);
  assert(wire_len == 12 + 20);
  assert(wire[0] == IPC_CHANNEL_FRAMEBUFFER && wire[1] == 2 && read_le32(wire + 4) == 4);
  assert(memcmp(wire + 8, pixels, sizeof(pixels)) == 0);
  assert(wire[12] == IPC_CHANNEL_CONTROL && read_le32(wire + 16) == 12);
  assert(wire[22] == 0 && wire[23] == 1 && read_le32(wire + 24) == id && wire[28] == 0);
  assert(backend_calls == 0 && g_lilygo_sd == NULL);
  if (!${wasm ? 1 : 0}) {
    wire_len = 0;
    memcpy(interleaved_query, query, sizeof(interleaved_query));
    interleave_ack = true;
    ipc_send_frame(&display_state, IPC_CHANNEL_FRAMEBUFFER, 2, pixels, sizeof(pixels));
    assert(!interleave_ack && wire_len == 12 + 20);
    assert(memcmp(wire + 8, pixels, sizeof(pixels)) == 0);
    assert(wire[12] == IPC_CHANNEL_CONTROL && read_le32(wire + 16) == 12);
    assert(wire[22] == 0 && wire[23] == 1 && read_le32(wire + 24) == id && wire[28] == 0);
    assert(write_errors == 0);
    wire_len = 0;
    ipc_send_frame(&display_state, IPC_CHANNEL_FRAMEBUFFER, 2, pixels, UINT32_MAX);
    assert(wire_len == 0 && write_errors == 1);
    short_write = true;
    ipc_send_frame(&display_state, IPC_CHANNEL_FRAMEBUFFER, 2, pixels, sizeof(pixels));
    assert(wire_len == 11 && write_errors == 2);
  }
  return 0;
}
`);
      const compiler = process.env.CC || "cc";
      const flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", ...(wasm ? ["-D__EMSCRIPTEN__"] : [])];
      const compiled = spawnSync(compiler, [...flags, join(dir, "barrier.c"), "-o", join(dir, "barrier")], { encoding: "utf8" });
      assert.equal(compiled.status, 0, compiled.stdout + compiled.stderr);
      const result = spawnSync(join(dir, "barrier"), [], { encoding: "utf8" });
      assert.equal(result.status, 0, result.stdout + result.stderr);
      t.diagnostic(`production C contract exit=${result.status} signal=${result.signal ?? "none"}`);
      if (!wasm) {
        const split = spawnSync(compiler, [...flags, "-DTRANSPORT_SPLIT_WRITE_NEGATIVE_CONTROL", join(dir, "barrier.c"), "-o", join(dir, "split")], { encoding: "utf8" });
        assert.equal(split.status, 0, split.stdout + split.stderr);
        const corrupt = spawnSync(join(dir, "split"), [], { encoding: "utf8" });
        assert.ok(corrupt.status !== 0 || corrupt.signal, "old split writes unexpectedly preserved packet boundaries");
        assert.match(corrupt.stderr, /memcmp\(wire \+ 8, pixels, sizeof\(pixels\)\)/);
        t.diagnostic(`old split-write negative control exit=${corrupt.status} signal=${corrupt.signal ?? "none"}; rejected at framebuffer payload boundary`);
      }
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  });
}
