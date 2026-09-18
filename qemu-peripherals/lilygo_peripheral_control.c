#include "qemu/osdep.h"
#include "hw/char/lilygo_gnss_chardev.h"
#include "hw/i2c/lilygo_bq27220.h"
#include "hw/i2c/lilygo_display_input.h"
#include "hw/i2c/lilygo_i2c_probe.h"
#include "hw/i2c/lilygo_power_rtc.h"
#include "hw/misc/lilygo_peripheral_control.h"
#include "hw/ssi/lilygo_sx1262.h"

#define LILYGO_PERIPHERAL_DEVICE_I2C_PROBE 1
#define LILYGO_PERIPHERAL_DEVICE_GPSPI2 2
#define LILYGO_PERIPHERAL_DEVICE_SD 3
#define LILYGO_PERIPHERAL_CONTROL_HEADER_SIZE 10
#define LILYGO_PERIPHERAL_ACK_SIZE 12
#define LILYGO_PERIPHERAL_TRACE_HEADER_SIZE 22
#define LILYGO_PERIPHERAL_TRACE_DATA_MAX 16

extern bool esp32s3_gpspi2_control(const uint8_t* payload, uint32_t len);
extern bool lilygo_sd_control(const uint8_t* payload, uint32_t len);

static void (*g_lilygo_peripheral_ipc_sender)(const uint8_t* payload, uint32_t len);

static uint32_t read_le32(const uint8_t* bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static void write_le32(uint8_t* bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8u);
  bytes[2] = (uint8_t)(value >> 16u);
  bytes[3] = (uint8_t)(value >> 24u);
}

static void write_le64(uint8_t* bytes, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) bytes[i] = (uint8_t)(value >> (i * 8u));
}

static void send_bq27220_trace(const LilygoBq27220Trace* source) {
  uint8_t trace[LILYGO_PERIPHERAL_TRACE_HEADER_SIZE + LILYGO_PERIPHERAL_TRACE_DATA_MAX] = {0};
  const uint8_t length = MIN(source->length, LILYGO_PERIPHERAL_TRACE_DATA_MAX);
  trace[0] = LILYGO_PERIPHERAL_CONTROL_VERSION;
  trace[1] = LILYGO_PERIPHERAL_MESSAGE_TRACE;
  trace[2] = LILYGO_BQ27220_CONTROL_DEVICE;
  trace[3] = source->result;
  write_le32(&trace[4], source->sequence);
  write_le64(&trace[8], source->virtual_time_ns);
  trace[16] = source->bus;
  trace[17] = source->address;
  trace[18] = source->direction;
  trace[19] = source->reg;
  trace[20] = length;
  memcpy(&trace[LILYGO_PERIPHERAL_TRACE_HEADER_SIZE], source->data, length);
  lilygo_peripheral_control_emit(trace, LILYGO_PERIPHERAL_TRACE_HEADER_SIZE + length);
}

static void send_power_rtc_trace(const LilygoPowerRtcTrace* source) {
  uint8_t trace[LILYGO_PERIPHERAL_TRACE_HEADER_SIZE + LILYGO_PERIPHERAL_TRACE_DATA_MAX] = {0};
  const uint8_t length = MIN(source->length, LILYGO_PERIPHERAL_TRACE_DATA_MAX);
  trace[0] = LILYGO_PERIPHERAL_CONTROL_VERSION;
  trace[1] = LILYGO_PERIPHERAL_MESSAGE_TRACE;
  trace[2] = source->address == LILYGO_BQ25896_ADDRESS ? LILYGO_BQ25896_CONTROL_DEVICE : LILYGO_PCF8563_CONTROL_DEVICE;
  trace[3] = source->result;
  write_le32(&trace[4], source->sequence);
  write_le64(&trace[8], source->virtual_time_ns);
  trace[16] = 0;
  trace[17] = source->address;
  trace[18] = source->direction;
  trace[19] = source->reg;
  trace[20] = length;
  memcpy(&trace[LILYGO_PERIPHERAL_TRACE_HEADER_SIZE], source->data, length);
  lilygo_peripheral_control_emit(trace, LILYGO_PERIPHERAL_TRACE_HEADER_SIZE + length);
}

static void send_ack(uint32_t request_id, uint8_t device, bool accepted, LilygoPeripheralControlError error) {
  uint8_t ack[LILYGO_PERIPHERAL_ACK_SIZE] = {0};
  ack[0] = LILYGO_PERIPHERAL_CONTROL_VERSION;
  ack[1] = LILYGO_PERIPHERAL_MESSAGE_ACK;
  ack[2] = device;
  ack[3] = accepted ? 1 : 0;
  write_le32(&ack[4], request_id);
  ack[8] = error;
  lilygo_peripheral_control_emit(ack, sizeof(ack));
  fprintf(stderr, "[LILYGO-CONTROL-ACK] request=%u device=%u accepted=%u error=%u\n", request_id, device,
          accepted ? 1 : 0, error);
}

void lilygo_peripheral_control_set_ipc_sender(void (*sender)(const uint8_t* payload, uint32_t len)) {
  g_lilygo_peripheral_ipc_sender = sender;
  lilygo_bq27220_set_trace_sink(sender != NULL ? send_bq27220_trace : NULL);
  lilygo_power_rtc_set_trace_sink(sender != NULL ? send_power_rtc_trace : NULL);
}

void lilygo_peripheral_control_emit(const uint8_t* payload, uint32_t len) {
  if (g_lilygo_peripheral_ipc_sender != NULL) g_lilygo_peripheral_ipc_sender(payload, len);
}

void lilygo_peripheral_control_dispatch(const uint8_t* payload, uint32_t len) {
  const uint32_t request_id = payload != NULL && len >= 8 ? read_le32(&payload[4]) : 0;
  const uint8_t device = payload != NULL && len >= 3 ? payload[2] : 0;
  if (len > LILYGO_PERIPHERAL_CONTROL_MAX_PAYLOAD) {
    send_ack(request_id, device, false, LILYGO_PERIPHERAL_ERROR_PAYLOAD_TOO_LARGE);
    return;
  }
  if (payload == NULL || len < LILYGO_PERIPHERAL_CONTROL_HEADER_SIZE) {
    send_ack(request_id, device, false, LILYGO_PERIPHERAL_ERROR_MALFORMED);
    return;
  }
  if (payload[0] != LILYGO_PERIPHERAL_CONTROL_VERSION) {
    send_ack(request_id, device, false, LILYGO_PERIPHERAL_ERROR_UNSUPPORTED_VERSION);
    return;
  }

  if (device == LILYGO_PERIPHERAL_DEVICE_I2C_PROBE && lilygo_i2c_probe_control(payload, len)) return;
  if (device == LILYGO_PERIPHERAL_DEVICE_GPSPI2 && esp32s3_gpspi2_control(payload, len)) return;
  if (device == LILYGO_PERIPHERAL_DEVICE_SD && lilygo_sd_control(payload, len)) return;
  if (device == LILYGO_BQ27220_CONTROL_DEVICE) {
    const uint8_t payload_len = payload[8];
    if (payload[9] != 0 || len != LILYGO_PERIPHERAL_CONTROL_HEADER_SIZE + payload_len) {
      send_ack(request_id, device, false, LILYGO_PERIPHERAL_ERROR_UNKNOWN_FIELD);
      return;
    }
    const LilygoBq27220ControlError error =
        lilygo_bq27220_control(payload[1], payload[3], &payload[LILYGO_PERIPHERAL_CONTROL_HEADER_SIZE], payload_len);
    send_ack(request_id, device, error == LILYGO_BQ27220_CONTROL_OK, (LilygoPeripheralControlError)error);
    return;
  }
  if (device == LILYGO_SX1262_CONTROL_DEVICE) {
    const uint8_t payload_len = payload[8];
    if (payload[9] != 0 || len != LILYGO_PERIPHERAL_CONTROL_HEADER_SIZE + payload_len) {
      send_ack(request_id, device, false, LILYGO_PERIPHERAL_ERROR_UNKNOWN_FIELD);
      return;
    }
    const uint8_t error =
        lilygo_sx1262_control(payload[1], payload[3], &payload[LILYGO_PERIPHERAL_CONTROL_HEADER_SIZE], payload_len);
    send_ack(request_id, device, error == LILYGO_PERIPHERAL_ERROR_NONE, (LilygoPeripheralControlError)error);
    return;
  }
  if (device == LILYGO_BQ25896_CONTROL_DEVICE || device == LILYGO_PCF8563_CONTROL_DEVICE ||
      device == LILYGO_GNSS_CONTROL_DEVICE || device == LILYGO_TPS651851_CONTROL_DEVICE) {
    const uint8_t payload_len = payload[8];
    if (payload[9] != 0 || len != LILYGO_PERIPHERAL_CONTROL_HEADER_SIZE + payload_len) {
      send_ack(request_id, device, false, LILYGO_PERIPHERAL_ERROR_UNKNOWN_FIELD);
      return;
    }
    uint8_t error = LILYGO_PERIPHERAL_ERROR_UNKNOWN_DEVICE;
    if (device == LILYGO_BQ25896_CONTROL_DEVICE)
      error = lilygo_bq25896_control(payload[1], &payload[LILYGO_PERIPHERAL_CONTROL_HEADER_SIZE], payload_len);
    else if (device == LILYGO_PCF8563_CONTROL_DEVICE)
      error = lilygo_pcf8563_control(payload[1], &payload[LILYGO_PERIPHERAL_CONTROL_HEADER_SIZE], payload_len);
    else if (device == LILYGO_GNSS_CONTROL_DEVICE)
      error = lilygo_gnss_chardev_control(payload[1], &payload[LILYGO_PERIPHERAL_CONTROL_HEADER_SIZE], payload_len);
    else if (device == LILYGO_TPS651851_CONTROL_DEVICE)
      error = lilygo_tps65185_control(payload[1], &payload[LILYGO_PERIPHERAL_CONTROL_HEADER_SIZE], payload_len);
    send_ack(request_id, device, error == LILYGO_PERIPHERAL_ERROR_NONE, (LilygoPeripheralControlError)error);
    return;
  }
  send_ack(request_id, device, false, LILYGO_PERIPHERAL_ERROR_UNKNOWN_DEVICE);
}

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
EMSCRIPTEN_KEEPALIVE
void mofei_wasm_i2c_control(const uint8_t* payload, uint32_t len) { lilygo_peripheral_control_dispatch(payload, len); }
#endif
