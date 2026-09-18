#include "qemu/osdep.h"
#include "hw/i2c/lilygo_bq27220.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define LILYGO_BQ27220_REGISTER_COUNT 256
#define LILYGO_BQ27220_TRACE_DATA_MAX 16

OBJECT_DECLARE_SIMPLE_TYPE(LilygoBq27220State, LILYGO_BQ27220)

struct LilygoBq27220State {
  I2CSlave parent_obj;
  uint8_t registers[LILYGO_BQ27220_REGISTER_COUNT];
  uint8_t reg_ptr;
  uint8_t trace_data[LILYGO_BQ27220_TRACE_DATA_MAX];
  uint8_t trace_len;
  uint8_t trace_direction;
  uint8_t trace_register;
  uint8_t trace_result;
  bool expecting_register;
  bool transaction_active;
  bool nack_latched;
  bool nack_once;
  uint32_t trace_sequence;
};

static LilygoBq27220State* g_lilygo_bq27220;
static LilygoBq27220TraceSink g_lilygo_bq27220_trace_sink;

static void lilygo_bq27220_set_register_u16(LilygoBq27220State* s, uint8_t reg, uint16_t value) {
  s->registers[reg] = (uint8_t)value;
  s->registers[(uint8_t)(reg + 1)] = (uint8_t)(value >> 8u);
}

static void lilygo_bq27220_emit_trace(LilygoBq27220State* s, uint8_t result) {
  LilygoBq27220Trace trace = {
      .sequence = ++s->trace_sequence,
      .virtual_time_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
      .bus = 0,
      .address = LILYGO_BQ27220_ADDRESS,
      .direction = s->trace_direction,
      .reg = s->trace_register,
      .length = s->trace_len,
      .result = result,
  };
  memcpy(trace.data, s->trace_data, s->trace_len);
  if (g_lilygo_bq27220_trace_sink != NULL) g_lilygo_bq27220_trace_sink(&trace);
  fprintf(stderr,
          "[LILYGO-BQ27220-TRACE] sequence=%u time_ns=%" PRIu64
          " bus=0 address=0x%02x direction=%s register=0x%02x bytes=%u data=",
          trace.sequence, trace.virtual_time_ns, trace.address,
          trace.direction == LILYGO_BQ27220_TRACE_READ ? "read" : "write", trace.reg, trace.length);
  for (uint8_t i = 0; i < trace.length; ++i) fprintf(stderr, "%02x", trace.data[i]);
  fprintf(stderr, " result=%s\n", result == LILYGO_BQ27220_TRACE_ACK ? "ack" : "nack");
}

static void lilygo_bq27220_finish_transaction(LilygoBq27220State* s) {
  if (!s->transaction_active) return;
  lilygo_bq27220_emit_trace(s, s->trace_result);
  s->transaction_active = false;
}

static void lilygo_bq27220_reset_state(LilygoBq27220State* s) {
  memset(s->registers, 0, sizeof(s->registers));
  lilygo_bq27220_set_register_u16(s, LILYGO_BQ27220_REG_VOLTAGE, LILYGO_BQ27220_RESET_VOLTAGE_MV);
  lilygo_bq27220_set_register_u16(s, LILYGO_BQ27220_REG_STATE_OF_CHARGE, LILYGO_BQ27220_RESET_STATE_OF_CHARGE);
  s->reg_ptr = 0;
  memset(s->trace_data, 0, sizeof(s->trace_data));
  s->trace_len = 0;
  s->trace_direction = LILYGO_BQ27220_TRACE_WRITE;
  s->trace_register = 0;
  s->trace_result = LILYGO_BQ27220_TRACE_ACK;
  s->expecting_register = true;
  s->transaction_active = false;
  s->nack_latched = false;
  s->nack_once = false;
  s->trace_sequence = 0;
}

static void lilygo_bq27220_reset_hold(Object* obj, ResetType type) {
  (void)type;
  lilygo_bq27220_reset_state(LILYGO_BQ27220(obj));
}

static int lilygo_bq27220_event(I2CSlave* i2c, enum i2c_event event) {
  LilygoBq27220State* s = LILYGO_BQ27220(i2c);
  switch (event) {
    case I2C_START_SEND:
    case I2C_START_RECV:
      /* A repeated start completes the pointer-write phase before beginning
       * the production driver's read phase, so both directions are traced. */
      lilygo_bq27220_finish_transaction(s);
      s->trace_len = 0;
      s->trace_direction = event == I2C_START_RECV ? LILYGO_BQ27220_TRACE_READ : LILYGO_BQ27220_TRACE_WRITE;
      s->trace_register = s->reg_ptr;
      s->trace_result = LILYGO_BQ27220_TRACE_ACK;
      s->expecting_register = event == I2C_START_SEND;
      if (s->nack_latched || s->nack_once) {
        s->nack_once = false;
        lilygo_bq27220_emit_trace(s, LILYGO_BQ27220_TRACE_NACK);
        return 1;
      }
      s->transaction_active = true;
      return 0;
    case I2C_FINISH:
      lilygo_bq27220_finish_transaction(s);
      return 0;
    case I2C_NACK:
      /* The controller NACKs the final byte of a normal read. Transport
       * failure injection is handled at transaction start above. */
      return 0;
    case I2C_START_SEND_ASYNC:
      return 0;
  }
  return 0;
}

static int lilygo_bq27220_send(I2CSlave* i2c, uint8_t data) {
  LilygoBq27220State* s = LILYGO_BQ27220(i2c);
  if (s->trace_len < LILYGO_BQ27220_TRACE_DATA_MAX) s->trace_data[s->trace_len++] = data;
  if (s->expecting_register) {
    s->reg_ptr = data;
    s->trace_register = data;
    s->expecting_register = false;
    return 0;
  }
  /* The production path is read-only. Do not let guest writes mutate injected
   * simulator state or accidentally model unobserved BQ27220 commands. */
  s->trace_result = LILYGO_BQ27220_TRACE_NACK;
  return 1;
}

static uint8_t lilygo_bq27220_recv(I2CSlave* i2c) {
  LilygoBq27220State* s = LILYGO_BQ27220(i2c);
  const uint8_t value = s->registers[s->reg_ptr];
  if (s->trace_len < LILYGO_BQ27220_TRACE_DATA_MAX) s->trace_data[s->trace_len++] = value;
  ++s->reg_ptr;
  return value;
}

LilygoBq27220ControlError lilygo_bq27220_control(uint8_t operation, uint8_t flags, const uint8_t* payload,
                                                 uint8_t payload_len) {
  LilygoBq27220State* s = g_lilygo_bq27220;
  if (s == NULL) return LILYGO_BQ27220_CONTROL_MALFORMED;
  if (s->transaction_active) return LILYGO_BQ27220_CONTROL_MALFORMED;

  switch (operation) {
    case LILYGO_BQ27220_CONTROL_SET_STATE: {
      if (flags != 0) return LILYGO_BQ27220_CONTROL_UNKNOWN_FIELD;
      if (payload == NULL || payload_len != 3) return LILYGO_BQ27220_CONTROL_MALFORMED;
      const uint16_t voltage_mv = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8u);
      const uint8_t state_of_charge = payload[2];
      if (state_of_charge > 100) return LILYGO_BQ27220_CONTROL_MALFORMED;
      /* Validation precedes both writes, keeping voltage/SOC updates atomic. */
      lilygo_bq27220_set_register_u16(s, LILYGO_BQ27220_REG_VOLTAGE, voltage_mv);
      lilygo_bq27220_set_register_u16(s, LILYGO_BQ27220_REG_STATE_OF_CHARGE, state_of_charge);
      return LILYGO_BQ27220_CONTROL_OK;
    }
    case LILYGO_BQ27220_CONTROL_SET_FAULT:
      if (flags & ~LILYGO_BQ27220_CONTROL_FLAG_ONE_SHOT) return LILYGO_BQ27220_CONTROL_UNKNOWN_FIELD;
      if (payload == NULL || payload_len != 1 || payload[0] != LILYGO_BQ27220_FAULT_NACK) {
        return LILYGO_BQ27220_CONTROL_MALFORMED;
      }
      if (flags & LILYGO_BQ27220_CONTROL_FLAG_ONE_SHOT) {
        s->nack_once = true;
      } else {
        s->nack_latched = true;
      }
      return LILYGO_BQ27220_CONTROL_OK;
    case LILYGO_BQ27220_CONTROL_RESET:
      if (flags != 0) return LILYGO_BQ27220_CONTROL_UNKNOWN_FIELD;
      if (payload_len != 0) return LILYGO_BQ27220_CONTROL_MALFORMED;
      lilygo_bq27220_reset_state(s);
      return LILYGO_BQ27220_CONTROL_OK;
    default:
      return LILYGO_BQ27220_CONTROL_UNKNOWN_OPERATION;
  }
}

I2CSlave* lilygo_bq27220_attach(I2CBus* lilygo_bus) {
  if (lilygo_bus == NULL) return NULL;
  return i2c_slave_create_simple(lilygo_bus, TYPE_LILYGO_BQ27220, LILYGO_BQ27220_ADDRESS);
}

void lilygo_bq27220_set_trace_sink(LilygoBq27220TraceSink sink) { g_lilygo_bq27220_trace_sink = sink; }

static void lilygo_bq27220_realize(DeviceState* dev, Error** errp) {
  (void)errp;
  g_lilygo_bq27220 = LILYGO_BQ27220(dev);
  lilygo_bq27220_reset_state(g_lilygo_bq27220);
}

static void lilygo_bq27220_unrealize(DeviceState* dev) {
  if (g_lilygo_bq27220 == LILYGO_BQ27220(dev)) g_lilygo_bq27220 = NULL;
}

static const VMStateDescription vmstate_lilygo_bq27220 = {
    .name = TYPE_LILYGO_BQ27220,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){
        VMSTATE_I2C_SLAVE(parent_obj, LilygoBq27220State),
        VMSTATE_UINT8_ARRAY(registers, LilygoBq27220State, LILYGO_BQ27220_REGISTER_COUNT),
        VMSTATE_UINT8(reg_ptr, LilygoBq27220State),
        VMSTATE_UINT8_ARRAY(trace_data, LilygoBq27220State, LILYGO_BQ27220_TRACE_DATA_MAX),
        VMSTATE_UINT8(trace_len, LilygoBq27220State),
        VMSTATE_UINT8(trace_direction, LilygoBq27220State),
        VMSTATE_UINT8(trace_register, LilygoBq27220State),
        VMSTATE_UINT8(trace_result, LilygoBq27220State),
        VMSTATE_BOOL(expecting_register, LilygoBq27220State),
        VMSTATE_BOOL(transaction_active, LilygoBq27220State),
        VMSTATE_BOOL(nack_latched, LilygoBq27220State),
        VMSTATE_BOOL(nack_once, LilygoBq27220State),
        VMSTATE_UINT32(trace_sequence, LilygoBq27220State),
        VMSTATE_END_OF_LIST(),
    }};

static void lilygo_bq27220_class_init(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);
  I2CSlaveClass* i2c = I2C_SLAVE_CLASS(klass);
  ResettableClass* rc = RESETTABLE_CLASS(klass);
  (void)data;
  dc->realize = lilygo_bq27220_realize;
  dc->unrealize = lilygo_bq27220_unrealize;
  dc->vmsd = &vmstate_lilygo_bq27220;
  dc->desc = "LilyGo deterministic BQ27220 fuel gauge";
  i2c->event = lilygo_bq27220_event;
  i2c->send = lilygo_bq27220_send;
  i2c->recv = lilygo_bq27220_recv;
  rc->phases.hold = lilygo_bq27220_reset_hold;
}

static const TypeInfo lilygo_bq27220_info = {
    .name = TYPE_LILYGO_BQ27220,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(LilygoBq27220State),
    .class_init = lilygo_bq27220_class_init,
};

static void lilygo_bq27220_register_types(void) { type_register_static(&lilygo_bq27220_info); }

type_init(lilygo_bq27220_register_types)
