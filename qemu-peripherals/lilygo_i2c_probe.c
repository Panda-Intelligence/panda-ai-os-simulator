#include "qemu/osdep.h"
#include "hw/i2c/lilygo_i2c_probe.h"
#include "hw/misc/lilygo_peripheral_control.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define LILYGO_I2C_DEVICE_PROBE 1
#define LILYGO_I2C_FAULT_NACK 1
#define LILYGO_I2C_CONTROL_HEADER_SIZE 10
#define LILYGO_I2C_ACK_SIZE 12
#define LILYGO_I2C_TRACE_HEADER_SIZE 22
#define LILYGO_I2C_TRACE_DATA_MAX 16
#define LILYGO_I2C_FLAG_ONE_SHOT 0x01
#define LILYGO_I2C_TRACE_RESULT_ACK 0
#define LILYGO_I2C_TRACE_RESULT_NACK 1
#define LILYGO_I2C_TRACE_DIRECTION_WRITE 0
#define LILYGO_I2C_TRACE_DIRECTION_READ 1
#define LILYGO_I2C_CONTROL_VERSION LILYGO_PERIPHERAL_CONTROL_VERSION
#define LILYGO_I2C_CONTROL_MAX_PAYLOAD LILYGO_PERIPHERAL_CONTROL_MAX_PAYLOAD
#define LILYGO_I2C_MESSAGE_ACK LILYGO_PERIPHERAL_MESSAGE_ACK
#define LILYGO_I2C_MESSAGE_TRACE LILYGO_PERIPHERAL_MESSAGE_TRACE
#define LILYGO_I2C_ERROR_NONE LILYGO_PERIPHERAL_ERROR_NONE
#define LILYGO_I2C_ERROR_UNSUPPORTED_VERSION LILYGO_PERIPHERAL_ERROR_UNSUPPORTED_VERSION
#define LILYGO_I2C_ERROR_UNKNOWN_DEVICE LILYGO_PERIPHERAL_ERROR_UNKNOWN_DEVICE
#define LILYGO_I2C_ERROR_UNKNOWN_OPERATION LILYGO_PERIPHERAL_ERROR_UNKNOWN_OPERATION
#define LILYGO_I2C_ERROR_PAYLOAD_TOO_LARGE LILYGO_PERIPHERAL_ERROR_PAYLOAD_TOO_LARGE
#define LILYGO_I2C_ERROR_MALFORMED LILYGO_PERIPHERAL_ERROR_MALFORMED
#define LILYGO_I2C_ERROR_UNKNOWN_FIELD LILYGO_PERIPHERAL_ERROR_UNKNOWN_FIELD

typedef LilygoPeripheralControlError LilygoI2CControlError;

OBJECT_DECLARE_SIMPLE_TYPE(LilygoI2CProbeState, LILYGO_I2C_PROBE)

struct LilygoI2CProbeState {
  I2CSlave parent_obj;
  uint8_t registers[16];
  uint8_t reg_ptr;
  uint8_t trace_data[LILYGO_I2C_TRACE_DATA_MAX];
  uint8_t trace_len;
  uint8_t trace_direction;
  uint8_t trace_register;
  bool expecting_register;
  bool transaction_active;
  bool nack_latched;
  bool nack_once;
  uint32_t trace_sequence;
};

static LilygoI2CProbeState* g_lilygo_i2c_probe;

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

static void lilygo_i2c_probe_send_ack(uint32_t request_id, uint8_t device, bool accepted, LilygoI2CControlError error) {
  uint8_t ack[LILYGO_I2C_ACK_SIZE] = {0};
  ack[0] = LILYGO_I2C_CONTROL_VERSION;
  ack[1] = LILYGO_I2C_MESSAGE_ACK;
  ack[2] = device;
  ack[3] = accepted ? 1 : 0;
  write_le32(&ack[4], request_id);
  ack[8] = error;
  lilygo_peripheral_control_emit(ack, sizeof(ack));
  fprintf(stderr, "[LILYGO-I2C-ACK] request=%u accepted=%u error=%u\n", request_id, accepted ? 1 : 0, error);
}

static void lilygo_i2c_probe_send_trace(LilygoI2CProbeState* s, uint8_t result) {
  uint8_t trace[LILYGO_I2C_TRACE_HEADER_SIZE + LILYGO_I2C_TRACE_DATA_MAX] = {0};
  trace[0] = LILYGO_I2C_CONTROL_VERSION;
  trace[1] = LILYGO_I2C_MESSAGE_TRACE;
  trace[2] = LILYGO_I2C_DEVICE_PROBE;
  trace[3] = result;
  write_le32(&trace[4], ++s->trace_sequence);
  write_le64(&trace[8], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
  trace[16] = 0;
  trace[17] = LILYGO_I2C_PROBE_ADDRESS;
  trace[18] = s->trace_direction;
  trace[19] = s->trace_register;
  trace[20] = s->trace_len;
  memcpy(&trace[LILYGO_I2C_TRACE_HEADER_SIZE], s->trace_data, s->trace_len);
  lilygo_peripheral_control_emit(trace, LILYGO_I2C_TRACE_HEADER_SIZE + s->trace_len);
  fprintf(stderr, "[LILYGO-I2C-TRACE] sequence=%u bus=0 address=0x%02x direction=%s register=0x%02x bytes=%u data=",
          s->trace_sequence, LILYGO_I2C_PROBE_ADDRESS,
          s->trace_direction == LILYGO_I2C_TRACE_DIRECTION_READ ? "read" : "write", s->trace_register, s->trace_len);
  for (uint8_t i = 0; i < s->trace_len; ++i) fprintf(stderr, "%02x", s->trace_data[i]);
  fprintf(stderr, " result=%s\n", result == LILYGO_I2C_TRACE_RESULT_ACK ? "ack" : "nack");
}

static void lilygo_i2c_probe_reset(Object* obj, ResetType type) {
  LilygoI2CProbeState* s = LILYGO_I2C_PROBE(obj);
  (void)type;
  memset(s->registers, 0, sizeof(s->registers));
  s->registers[0] = 0xA5;
  s->registers[1] = 0x5A;
  s->reg_ptr = 0;
  s->trace_len = 0;
  s->trace_direction = LILYGO_I2C_TRACE_DIRECTION_WRITE;
  s->trace_register = 0;
  s->expecting_register = true;
  s->transaction_active = false;
  s->nack_latched = false;
  s->nack_once = false;
  s->trace_sequence = 0;
}

static int lilygo_i2c_probe_event(I2CSlave* i2c, enum i2c_event event) {
  LilygoI2CProbeState* s = LILYGO_I2C_PROBE(i2c);
  switch (event) {
    case I2C_START_SEND:
    case I2C_START_RECV:
      s->trace_len = 0;
      s->trace_direction = event == I2C_START_RECV ? LILYGO_I2C_TRACE_DIRECTION_READ : LILYGO_I2C_TRACE_DIRECTION_WRITE;
      s->trace_register = s->reg_ptr;
      s->expecting_register = event == I2C_START_SEND;
      if (s->nack_latched || s->nack_once) {
        s->nack_once = false;
        s->transaction_active = false;
        lilygo_i2c_probe_send_trace(s, LILYGO_I2C_TRACE_RESULT_NACK);
        return 1;
      }
      s->transaction_active = true;
      return 0;
    case I2C_FINISH:
      if (s->transaction_active) lilygo_i2c_probe_send_trace(s, LILYGO_I2C_TRACE_RESULT_ACK);
      s->transaction_active = false;
      return 0;
    case I2C_NACK:
    case I2C_START_SEND_ASYNC:
      return 0;
  }
  return 0;
}

static int lilygo_i2c_probe_send(I2CSlave* i2c, uint8_t data) {
  LilygoI2CProbeState* s = LILYGO_I2C_PROBE(i2c);
  if (s->trace_len < LILYGO_I2C_TRACE_DATA_MAX) s->trace_data[s->trace_len++] = data;
  if (s->expecting_register) {
    s->reg_ptr = data & 0x0F;
    s->trace_register = s->reg_ptr;
    s->expecting_register = false;
  } else {
    s->registers[s->reg_ptr] = data;
    s->reg_ptr = (s->reg_ptr + 1) & 0x0F;
  }
  return 0;
}

static uint8_t lilygo_i2c_probe_recv(I2CSlave* i2c) {
  LilygoI2CProbeState* s = LILYGO_I2C_PROBE(i2c);
  const uint8_t value = s->registers[s->reg_ptr];
  if (s->trace_len < LILYGO_I2C_TRACE_DATA_MAX) s->trace_data[s->trace_len++] = value;
  s->reg_ptr = (s->reg_ptr + 1) & 0x0F;
  return value;
}

bool lilygo_i2c_probe_control(const uint8_t* payload, uint32_t len) {
  uint32_t request_id = 0;
  uint8_t device = 0;
  if (payload != NULL) {
    request_id = len >= 8 ? read_le32(&payload[4]) : 0;
    device = len >= 3 ? payload[2] : 0;
  }
  if (len > LILYGO_I2C_CONTROL_MAX_PAYLOAD) {
    lilygo_i2c_probe_send_ack(request_id, device, false, LILYGO_I2C_ERROR_PAYLOAD_TOO_LARGE);
    return true;
  }
  if (payload == NULL || len < LILYGO_I2C_CONTROL_HEADER_SIZE) {
    lilygo_i2c_probe_send_ack(request_id, device, false, LILYGO_I2C_ERROR_MALFORMED);
    return true;
  }
  if (payload[0] != LILYGO_I2C_CONTROL_VERSION) {
    lilygo_i2c_probe_send_ack(request_id, device, false, LILYGO_I2C_ERROR_UNSUPPORTED_VERSION);
    return true;
  }
  if (device != LILYGO_I2C_DEVICE_PROBE) return false;
  if (g_lilygo_i2c_probe == NULL) {
    lilygo_i2c_probe_send_ack(request_id, device, false, LILYGO_I2C_ERROR_UNKNOWN_DEVICE);
    return true;
  }
  const uint8_t operation = payload[1];
  const uint8_t flags = payload[3];
  const uint8_t payload_len = payload[8];
  if (payload[9] != 0 || flags & ~LILYGO_I2C_FLAG_ONE_SHOT || len != LILYGO_I2C_CONTROL_HEADER_SIZE + payload_len) {
    lilygo_i2c_probe_send_ack(request_id, device, false, LILYGO_I2C_ERROR_UNKNOWN_FIELD);
    return true;
  }

  LilygoI2CProbeState* s = g_lilygo_i2c_probe;
  switch (operation) {
    case LILYGO_I2C_OPERATION_SET_STATE:
      if (payload_len != 2 || payload[10] >= sizeof(s->registers)) {
        lilygo_i2c_probe_send_ack(request_id, device, false, LILYGO_I2C_ERROR_MALFORMED);
        return true;
      }
      s->registers[payload[10]] = payload[11];
      break;
    case LILYGO_I2C_OPERATION_SET_FAULT:
      if (payload_len != 1 || payload[10] != LILYGO_I2C_FAULT_NACK) {
        lilygo_i2c_probe_send_ack(request_id, device, false, LILYGO_I2C_ERROR_MALFORMED);
        return true;
      }
      if (flags & LILYGO_I2C_FLAG_ONE_SHOT) {
        s->nack_once = true;
      } else {
        s->nack_latched = true;
      }
      break;
    case LILYGO_I2C_OPERATION_RESET:
      if (payload_len != 0 || flags != 0) {
        lilygo_i2c_probe_send_ack(request_id, device, false, LILYGO_I2C_ERROR_MALFORMED);
        return true;
      }
      lilygo_i2c_probe_reset(OBJECT(s), RESET_TYPE_COLD);
      break;
    default:
      lilygo_i2c_probe_send_ack(request_id, device, false, LILYGO_I2C_ERROR_UNKNOWN_OPERATION);
      return true;
  }
  lilygo_i2c_probe_send_ack(request_id, device, true, LILYGO_I2C_ERROR_NONE);
  return true;
}

static void lilygo_i2c_probe_realize(DeviceState* dev, Error** errp) {
  (void)errp;
  g_lilygo_i2c_probe = LILYGO_I2C_PROBE(dev);
  lilygo_i2c_probe_reset(OBJECT(dev), RESET_TYPE_COLD);
}

static const VMStateDescription vmstate_lilygo_i2c_probe = {
    .name = TYPE_LILYGO_I2C_PROBE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){
        VMSTATE_I2C_SLAVE(parent_obj, LilygoI2CProbeState), VMSTATE_UINT8_ARRAY(registers, LilygoI2CProbeState, 16),
        VMSTATE_UINT8(reg_ptr, LilygoI2CProbeState), VMSTATE_BOOL(expecting_register, LilygoI2CProbeState),
        VMSTATE_BOOL(nack_latched, LilygoI2CProbeState), VMSTATE_BOOL(nack_once, LilygoI2CProbeState),
        VMSTATE_UINT32(trace_sequence, LilygoI2CProbeState), VMSTATE_END_OF_LIST()}};

static void lilygo_i2c_probe_class_init(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);
  I2CSlaveClass* i2c = I2C_SLAVE_CLASS(klass);
  ResettableClass* rc = RESETTABLE_CLASS(klass);
  (void)data;
  dc->realize = lilygo_i2c_probe_realize;
  dc->vmsd = &vmstate_lilygo_i2c_probe;
  dc->desc = "LilyGo simulator I2C foundation probe";
  i2c->event = lilygo_i2c_probe_event;
  i2c->send = lilygo_i2c_probe_send;
  i2c->recv = lilygo_i2c_probe_recv;
  rc->phases.hold = lilygo_i2c_probe_reset;
}

static const TypeInfo lilygo_i2c_probe_info = {
    .name = TYPE_LILYGO_I2C_PROBE,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(LilygoI2CProbeState),
    .class_init = lilygo_i2c_probe_class_init,
};

static void lilygo_i2c_probe_register_types(void) { type_register_static(&lilygo_i2c_probe_info); }

type_init(lilygo_i2c_probe_register_types)
