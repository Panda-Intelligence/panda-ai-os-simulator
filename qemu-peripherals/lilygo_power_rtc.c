#ifndef LILYGO_POWER_RTC_MODEL_STANDALONE
#include "qemu/osdep.h"
#include "hw/i2c/lilygo_power_rtc.h"
#else
#include "lilygo_power_rtc.h"
#endif

#include <string.h>

#define NS_PER_SECOND 1000000000ULL

static uint8_t bcd_encode(uint8_t value) { return (uint8_t)(((value / 10u) << 4u) | (value % 10u)); }

static uint8_t bcd_decode(uint8_t value) { return (uint8_t)(((value >> 4u) * 10u) + (value & 0x0Fu)); }

static bool leap_year(uint8_t year) { return (year & 3u) == 0; }

static uint8_t days_in_month(uint8_t year, uint8_t month) {
  static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && leap_year(year)) return 29;
  return month >= 1 && month <= 12 ? days[month - 1] : 0;
}

void lilygo_bq25896_model_reset(LilygoBq25896Model* model) {
  memset(model, 0, sizeof(*model));
  model->expecting_pointer = true;
}

LilygoPowerRtcResult lilygo_bq25896_model_set_state(LilygoBq25896Model* model, uint8_t status, uint8_t latched_fault,
                                                    uint8_t current_fault) {
  if (model == NULL) return LILYGO_POWER_RTC_INVALID;
  model->status = status;
  model->latched_fault = latched_fault;
  model->current_fault = current_fault;
  model->fault_latch_pending = true;
  return LILYGO_POWER_RTC_OK;
}

uint8_t lilygo_bq25896_model_read(LilygoBq25896Model* model, uint8_t reg) {
  if (reg == LILYGO_BQ25896_REG_STATUS) return model->status;
  if (reg == LILYGO_BQ25896_REG_FAULT) {
    if (model->fault_latch_pending) {
      model->fault_latch_pending = false;
      return model->latched_fault;
    }
    return model->current_fault;
  }
  return 0;
}

bool lilygo_bq25896_model_consume_nack(LilygoBq25896Model* model) {
  const bool nack = model->nack_latched || model->nack_once;
  model->nack_once = false;
  return nack;
}

void lilygo_bq25896_model_set_nack(LilygoBq25896Model* model, bool one_shot) {
  if (one_shot)
    model->nack_once = true;
  else
    model->nack_latched = true;
}

void lilygo_pcf8563_model_reset(LilygoPcf8563Model* model, uint64_t now_ns) {
  memset(model, 0, sizeof(*model));
  model->seconds = bcd_encode(0);
  model->minutes = bcd_encode(0);
  model->hours = bcd_encode(0);
  model->days = bcd_encode(1);
  model->months = bcd_encode(1);
  model->years = bcd_encode(24);
  model->expecting_pointer = true;
  model->ticking = true;
  model->tick_base_ns = now_ns;
}

LilygoPowerRtcResult lilygo_pcf8563_model_set_datetime(LilygoPcf8563Model* model, uint8_t year, uint8_t month,
                                                       uint8_t day, uint8_t weekday, uint8_t hour, uint8_t minute,
                                                       uint8_t second, bool oscillator_stopped, uint64_t now_ns) {
  if (model == NULL || month < 1 || month > 12 || day < 1 || day > days_in_month(year, month) || weekday > 6 ||
      hour > 23 || minute > 59 || second > 59) {
    return LILYGO_POWER_RTC_INVALID;
  }
  model->seconds = (uint8_t)(bcd_encode(second) | (oscillator_stopped ? 0x80u : 0));
  model->minutes = bcd_encode(minute);
  model->hours = bcd_encode(hour);
  model->days = bcd_encode(day);
  model->weekdays = weekday;
  model->months = bcd_encode(month);
  model->years = bcd_encode(year);
  model->invalid_fixture = false;
  model->tick_base_ns = now_ns;
  return LILYGO_POWER_RTC_OK;
}

LilygoPowerRtcResult lilygo_pcf8563_model_set_invalid(LilygoPcf8563Model* model, const uint8_t registers[8],
                                                      uint64_t now_ns) {
  if (model == NULL || registers == NULL) return LILYGO_POWER_RTC_INVALID;
  model->status2 = registers[0];
  model->seconds = registers[1];
  model->minutes = registers[2];
  model->hours = registers[3];
  model->days = registers[4];
  model->weekdays = registers[5];
  model->months = registers[6];
  model->years = registers[7];
  model->invalid_fixture = true;
  model->tick_base_ns = now_ns;
  return LILYGO_POWER_RTC_OK;
}

static void pcf8563_add_seconds(LilygoPcf8563Model* model, uint64_t elapsed) {
  uint64_t seconds_of_day = bcd_decode(model->seconds & 0x7Fu) + 60u * bcd_decode(model->minutes & 0x7Fu) +
                            3600u * bcd_decode(model->hours & 0x3Fu) + elapsed;
  uint32_t elapsed_days = (uint32_t)((seconds_of_day / 86400u) % 36525u);
  seconds_of_day %= 86400u;
  uint8_t day = bcd_decode(model->days & 0x3Fu);
  uint8_t month = bcd_decode(model->months & 0x1Fu);
  uint8_t year = bcd_decode(model->years);
  const uint8_t vl = model->seconds & 0x80u;
  uint8_t century = model->months & 0x80u;
  model->weekdays = (uint8_t)((model->weekdays + elapsed_days) % 7u);
  while (elapsed_days-- > 0) {
    if (++day > days_in_month(year, month)) {
      day = 1;
      if (++month > 12) {
        month = 1;
        if (++year >= 100) {
          year = 0;
          century ^= 0x80u;
        }
      }
    }
  }
  model->seconds = (uint8_t)(vl | bcd_encode((uint8_t)(seconds_of_day % 60u)));
  model->minutes = bcd_encode((uint8_t)((seconds_of_day / 60u) % 60u));
  model->hours = bcd_encode((uint8_t)(seconds_of_day / 3600u));
  model->days = bcd_encode(day);
  model->months = (uint8_t)(century | bcd_encode(month));
  model->years = bcd_encode(year);
}

static void pcf8563_sync(LilygoPcf8563Model* model, uint64_t now_ns) {
  if (!model->ticking || model->invalid_fixture || now_ns <= model->tick_base_ns) return;
  const uint64_t elapsed = (now_ns - model->tick_base_ns) / NS_PER_SECOND;
  pcf8563_add_seconds(model, elapsed);
  model->tick_base_ns = now_ns - ((now_ns - model->tick_base_ns) % NS_PER_SECOND);
}

void lilygo_pcf8563_model_set_ticking(LilygoPcf8563Model* model, bool ticking, uint64_t now_ns) {
  if (model == NULL) return;
  pcf8563_sync(model, now_ns);
  model->ticking = ticking;
  model->tick_base_ns = now_ns;
}

uint8_t lilygo_pcf8563_model_read(LilygoPcf8563Model* model, uint8_t reg, uint64_t now_ns) {
  pcf8563_sync(model, now_ns);
  switch (reg) {
    case 0x01:
      return model->status2;
    case 0x02:
      return model->seconds;
    case 0x03:
      return model->minutes;
    case 0x04:
      return model->hours;
    case 0x05:
      return model->days;
    case 0x06:
      return model->weekdays;
    case 0x07:
      return model->months;
    case 0x08:
      return model->years;
    default:
      return 0;
  }
}

bool lilygo_pcf8563_model_consume_nack(LilygoPcf8563Model* model) {
  const bool nack = model->nack_latched || model->nack_once;
  model->nack_once = false;
  return nack;
}

void lilygo_pcf8563_model_set_nack(LilygoPcf8563Model* model, bool one_shot) {
  if (one_shot)
    model->nack_once = true;
  else
    model->nack_latched = true;
}

#ifndef LILYGO_POWER_RTC_MODEL_STANDALONE
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"

OBJECT_DECLARE_SIMPLE_TYPE(LilygoBq25896State, LILYGO_BQ25896)
OBJECT_DECLARE_SIMPLE_TYPE(LilygoPcf8563State, LILYGO_PCF8563)

typedef struct LilygoPowerRtcTransaction {
  uint8_t data[LILYGO_POWER_RTC_TRACE_DATA_MAX];
  uint8_t length;
  uint8_t direction;
  uint8_t reg;
  bool active;
} LilygoPowerRtcTransaction;

typedef struct LilygoBq25896State {
  I2CSlave parent_obj;
  LilygoBq25896Model model;
  LilygoPowerRtcTransaction transaction;
} LilygoBq25896State;

typedef struct LilygoPcf8563State {
  I2CSlave parent_obj;
  LilygoPcf8563Model model;
  LilygoPowerRtcTransaction transaction;
} LilygoPcf8563State;

static LilygoPowerRtcTraceSink g_trace_sink;
static uint32_t g_trace_sequence;
static LilygoBq25896State* g_bq25896;
static LilygoPcf8563State* g_pcf8563;

static void emit_trace(LilygoPowerRtcTransaction* transaction, uint8_t address, uint8_t result) {
  if (!transaction->active && result == LILYGO_POWER_RTC_OK) return;
  LilygoPowerRtcTrace trace = {.sequence = ++g_trace_sequence,
                               .virtual_time_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                               .address = address,
                               .direction = transaction->direction,
                               .reg = transaction->reg,
                               .length = transaction->length,
                               .result = result};
  memcpy(trace.data, transaction->data, transaction->length);
  if (g_trace_sink != NULL) g_trace_sink(&trace);
  fprintf(stderr,
          "[LILYGO-POWER-RTC-TRACE] sequence=%u address=0x%02x direction=%s register=0x%02x bytes=%u result=%s\n",
          trace.sequence, trace.address, trace.direction ? "read" : "write", trace.reg, trace.length,
          result == LILYGO_POWER_RTC_OK ? "ack" : "nack");
  transaction->active = false;
}

static int bq_event(I2CSlave* i2c, enum i2c_event event) {
  LilygoBq25896State* state = LILYGO_BQ25896(i2c);
  if (event == I2C_START_SEND || event == I2C_START_RECV) {
    emit_trace(&state->transaction, LILYGO_BQ25896_ADDRESS, LILYGO_POWER_RTC_OK);
    memset(&state->transaction, 0, sizeof(state->transaction));
    state->transaction.active = true;
    state->transaction.direction = event == I2C_START_RECV;
    state->transaction.reg = state->model.register_pointer;
    state->model.expecting_pointer = event == I2C_START_SEND;
    if (lilygo_bq25896_model_consume_nack(&state->model)) {
      emit_trace(&state->transaction, LILYGO_BQ25896_ADDRESS, LILYGO_POWER_RTC_NACK);
      return 1;
    }
  } else if (event == I2C_FINISH) {
    emit_trace(&state->transaction, LILYGO_BQ25896_ADDRESS, LILYGO_POWER_RTC_OK);
  }
  return 0;
}

static int bq_send(I2CSlave* i2c, uint8_t value) {
  LilygoBq25896State* state = LILYGO_BQ25896(i2c);
  if (state->transaction.length < LILYGO_POWER_RTC_TRACE_DATA_MAX)
    state->transaction.data[state->transaction.length++] = value;
  if (!state->model.expecting_pointer) return 1;
  state->model.register_pointer = value;
  state->transaction.reg = value;
  state->model.expecting_pointer = false;
  return 0;
}

static uint8_t bq_recv(I2CSlave* i2c) {
  LilygoBq25896State* state = LILYGO_BQ25896(i2c);
  uint8_t value = lilygo_bq25896_model_read(&state->model, state->model.register_pointer++);
  if (state->transaction.length < LILYGO_POWER_RTC_TRACE_DATA_MAX)
    state->transaction.data[state->transaction.length++] = value;
  return value;
}

static int pcf_event(I2CSlave* i2c, enum i2c_event event) {
  LilygoPcf8563State* state = LILYGO_PCF8563(i2c);
  if (event == I2C_START_SEND || event == I2C_START_RECV) {
    emit_trace(&state->transaction, LILYGO_PCF8563_ADDRESS, LILYGO_POWER_RTC_OK);
    memset(&state->transaction, 0, sizeof(state->transaction));
    state->transaction.active = true;
    state->transaction.direction = event == I2C_START_RECV;
    state->transaction.reg = state->model.register_pointer;
    state->model.expecting_pointer = event == I2C_START_SEND;
    if (lilygo_pcf8563_model_consume_nack(&state->model)) {
      emit_trace(&state->transaction, LILYGO_PCF8563_ADDRESS, LILYGO_POWER_RTC_NACK);
      return 1;
    }
  } else if (event == I2C_FINISH) {
    emit_trace(&state->transaction, LILYGO_PCF8563_ADDRESS, LILYGO_POWER_RTC_OK);
  }
  return 0;
}

static int pcf_send(I2CSlave* i2c, uint8_t value) {
  LilygoPcf8563State* state = LILYGO_PCF8563(i2c);
  if (state->transaction.length < LILYGO_POWER_RTC_TRACE_DATA_MAX)
    state->transaction.data[state->transaction.length++] = value;
  if (!state->model.expecting_pointer) return 1;
  state->model.register_pointer = value;
  state->transaction.reg = value;
  state->model.expecting_pointer = false;
  return 0;
}

static uint8_t pcf_recv(I2CSlave* i2c) {
  LilygoPcf8563State* state = LILYGO_PCF8563(i2c);
  uint8_t value =
      lilygo_pcf8563_model_read(&state->model, state->model.register_pointer++, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
  if (state->transaction.length < LILYGO_POWER_RTC_TRACE_DATA_MAX)
    state->transaction.data[state->transaction.length++] = value;
  return value;
}

static void bq_reset(Object* obj, ResetType type) {
  (void)type;
  LilygoBq25896State* state = LILYGO_BQ25896(obj);
  lilygo_bq25896_model_reset(&state->model);
  g_bq25896 = state;
}

static void pcf_reset(Object* obj, ResetType type) {
  (void)type;
  LilygoPcf8563State* state = LILYGO_PCF8563(obj);
  lilygo_pcf8563_model_reset(&state->model, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
  g_pcf8563 = state;
}

static void bq_realize(DeviceState* dev, Error** errp) {
  (void)errp;
  LilygoBq25896State* state = LILYGO_BQ25896(dev);
  lilygo_bq25896_model_reset(&state->model);
  g_bq25896 = state;
}

static void bq_unrealize(DeviceState* dev) {
  if (g_bq25896 == LILYGO_BQ25896(dev)) g_bq25896 = NULL;
}

static void pcf_realize(DeviceState* dev, Error** errp) {
  (void)errp;
  LilygoPcf8563State* state = LILYGO_PCF8563(dev);
  lilygo_pcf8563_model_reset(&state->model, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
  g_pcf8563 = state;
}

static void pcf_unrealize(DeviceState* dev) {
  if (g_pcf8563 == LILYGO_PCF8563(dev)) g_pcf8563 = NULL;
}

static void bq_class_init(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);
  I2CSlaveClass* i2c = I2C_SLAVE_CLASS(klass);
  ResettableClass* rc = RESETTABLE_CLASS(klass);
  (void)data;
  dc->realize = bq_realize;
  dc->unrealize = bq_unrealize;
  i2c->event = bq_event;
  i2c->send = bq_send;
  i2c->recv = bq_recv;
  rc->phases.hold = bq_reset;
  dc->desc = "LilyGo deterministic BQ25896 charger";
}

static void pcf_class_init(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);
  I2CSlaveClass* i2c = I2C_SLAVE_CLASS(klass);
  ResettableClass* rc = RESETTABLE_CLASS(klass);
  (void)data;
  dc->realize = pcf_realize;
  dc->unrealize = pcf_unrealize;
  i2c->event = pcf_event;
  i2c->send = pcf_send;
  i2c->recv = pcf_recv;
  rc->phases.hold = pcf_reset;
  dc->desc = "LilyGo deterministic PCF8563 RTC";
}

static const TypeInfo bq_info = {.name = TYPE_LILYGO_BQ25896,
                                 .parent = TYPE_I2C_SLAVE,
                                 .instance_size = sizeof(LilygoBq25896State),
                                 .class_init = bq_class_init};
static const TypeInfo pcf_info = {.name = TYPE_LILYGO_PCF8563,
                                  .parent = TYPE_I2C_SLAVE,
                                  .instance_size = sizeof(LilygoPcf8563State),
                                  .class_init = pcf_class_init};

static void lilygo_power_rtc_register_types(void) {
  type_register_static(&bq_info);
  type_register_static(&pcf_info);
}

type_init(lilygo_power_rtc_register_types)

    I2CSlave* lilygo_bq25896_attach(I2CBus* lilygo_bus) {
  return lilygo_bus == NULL ? NULL : i2c_slave_create_simple(lilygo_bus, TYPE_LILYGO_BQ25896, LILYGO_BQ25896_ADDRESS);
}

I2CSlave* lilygo_pcf8563_attach(I2CBus* lilygo_bus) {
  return lilygo_bus == NULL ? NULL : i2c_slave_create_simple(lilygo_bus, TYPE_LILYGO_PCF8563, LILYGO_PCF8563_ADDRESS);
}

void lilygo_power_rtc_set_trace_sink(LilygoPowerRtcTraceSink sink) { g_trace_sink = sink; }

uint8_t lilygo_bq25896_control(uint8_t operation, const uint8_t* payload, uint8_t payload_len) {
  if (g_bq25896 == NULL) return 8;
  LilygoBq25896Model* model = &g_bq25896->model;
  switch (operation) {
    case 1:
      if (payload == NULL || payload_len != 3) return 7;
      return lilygo_bq25896_model_set_state(model, payload[0], payload[1], payload[2]) == LILYGO_POWER_RTC_OK ? 0 : 7;
    case 2:
      if (payload == NULL || payload_len != 1 || payload[0] > 2) return 7;
      if (payload[0] == 0) {
        model->nack_once = false;
        model->nack_latched = false;
      } else {
        lilygo_bq25896_model_set_nack(model, payload[0] == 1);
      }
      return 0;
    case 3:
      if (payload_len != 0) return 7;
      lilygo_bq25896_model_reset(model);
      return 0;
    case 4:
      return payload_len == 0 ? 0 : 7;
    default:
      return 3;
  }
}

uint8_t lilygo_pcf8563_control(uint8_t operation, const uint8_t* payload, uint8_t payload_len) {
  if (g_pcf8563 == NULL) return 8;
  LilygoPcf8563Model* model = &g_pcf8563->model;
  const uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
  switch (operation) {
    case 1:
      if (payload == NULL || payload_len != 8 || payload[7] > 1) return 7;
      return lilygo_pcf8563_model_set_datetime(model, payload[0], payload[1], payload[2], payload[3], payload[4],
                                               payload[5], payload[6], payload[7] != 0, now_ns) == LILYGO_POWER_RTC_OK
                 ? 0
                 : 7;
    case 2:
      if (payload == NULL || payload_len == 0) return 7;
      if (payload[0] == 0 && payload_len == 1) {
        model->nack_once = false;
        model->nack_latched = false;
        model->invalid_fixture = false;
        return 0;
      }
      if ((payload[0] == 1 || payload[0] == 2) && payload_len == 1) {
        lilygo_pcf8563_model_set_nack(model, payload[0] == 1);
        return 0;
      }
      if (payload[0] == 3 && payload_len == 9) {
        return lilygo_pcf8563_model_set_invalid(model, &payload[1], now_ns) == LILYGO_POWER_RTC_OK ? 0 : 7;
      }
      return 7;
    case 3:
      if (payload_len != 0) return 7;
      lilygo_pcf8563_model_reset(model, now_ns);
      return 0;
    case 4:
      return payload_len == 0 ? 0 : 7;
    default:
      return 3;
  }
}
#endif
