#ifndef LILYGO_DISPLAY_INPUT_MODEL_STANDALONE
#include "qemu/osdep.h"
#include "hw/i2c/lilygo_display_input.h"
#else
#include "lilygo_display_input.h"
#endif

#include <string.h>

static uint8_t gt911_config_checksum(const LilygoGt911Model* model) {
  uint8_t sum = 0;
  for (size_t i = 0; i < LILYGO_GT911_CONFIG_SIZE; ++i) sum = (uint8_t)(sum + model->config[i]);
  return (uint8_t)(~sum + 1u);
}

void lilygo_gt911_model_reset(LilygoGt911Model* model) {
  memset(model, 0, sizeof(*model));
  model->product_id[0] = '9';
  model->product_id[1] = '1';
  model->product_id[2] = '1';
  model->config[0] = 0x61;
  model->config[1] = (uint8_t)LILYGO_GT911_WIDTH;
  model->config[2] = (uint8_t)(LILYGO_GT911_WIDTH >> 8u);
  model->config[3] = (uint8_t)LILYGO_GT911_HEIGHT;
  model->config[4] = (uint8_t)(LILYGO_GT911_HEIGHT >> 8u);
  model->config[6] = 0x02;
  model->config_checksum = gt911_config_checksum(model);
  model->config_fresh = 1;
}

LilygoModelResult lilygo_gt911_model_inject(LilygoGt911Model* model, uint8_t action, uint16_t x, uint16_t y,
                                            uint8_t finger_id) {
  if (model == NULL || finger_id != 0 || action < LILYGO_GT911_ACTION_DOWN || action > LILYGO_GT911_ACTION_UP ||
      x >= LILYGO_GT911_WIDTH || y >= LILYGO_GT911_HEIGHT || model->sleeping) {
    return LILYGO_MODEL_INVALID;
  }
  if (action != LILYGO_GT911_ACTION_UP) {
    memset(model->contact, 0, sizeof(model->contact));
    model->contact[0] = finger_id;
    model->contact[1] = (uint8_t)x;
    model->contact[2] = (uint8_t)(x >> 8u);
    model->contact[3] = (uint8_t)y;
    model->contact[4] = (uint8_t)(y >> 8u);
    model->contact[5] = 1;
    model->contact_active = true;
    model->release_pending = false;
    model->status = 0x81;
  } else if ((model->status & 0x80u) != 0) {
    // 固件尚未确认当前触点报告时，先保留坐标，待确认后再交付抬起帧。
    model->contact_active = false;
    model->release_pending = true;
    return LILYGO_MODEL_OK;
  } else {
    model->contact_active = false;
    model->status = 0x80;
  }
  ++model->contact_generation;
  model->int_low = (model->config[6] & 0x03u) == 0x02u;
  return LILYGO_MODEL_OK;
}

uint8_t lilygo_gt911_model_read(const LilygoGt911Model* model, uint16_t reg) {
  if (reg >= LILYGO_GT911_CONFIG_BASE && reg < LILYGO_GT911_CONFIG_BASE + LILYGO_GT911_CONFIG_SIZE) {
    return model->config[reg - LILYGO_GT911_CONFIG_BASE];
  }
  if (reg >= LILYGO_GT911_PRODUCT_ID_REGISTER && reg < LILYGO_GT911_PRODUCT_ID_REGISTER + sizeof(model->product_id)) {
    return model->product_id[reg - LILYGO_GT911_PRODUCT_ID_REGISTER];
  }
  if (reg == LILYGO_GT911_CONFIG_CHECKSUM_REGISTER) return model->config_checksum;
  if (reg == LILYGO_GT911_CONFIG_FRESH_REGISTER) return model->config_fresh;
  if (reg == LILYGO_GT911_STATUS_REGISTER) return model->status;
  if (reg >= LILYGO_GT911_CONTACT_REGISTER && reg < LILYGO_GT911_CONTACT_REGISTER + sizeof(model->contact)) {
    return model->contact[reg - LILYGO_GT911_CONTACT_REGISTER];
  }
  return 0;
}

LilygoModelResult lilygo_gt911_model_write(LilygoGt911Model* model, uint16_t reg, uint8_t value) {
  if (reg >= LILYGO_GT911_CONFIG_BASE && reg < LILYGO_GT911_CONFIG_BASE + LILYGO_GT911_CONFIG_SIZE) {
    model->config[reg - LILYGO_GT911_CONFIG_BASE] = value;
    model->config_checksum = gt911_config_checksum(model);
    return LILYGO_MODEL_OK;
  }
  if (reg == LILYGO_GT911_CONFIG_CHECKSUM_REGISTER) {
    model->config_checksum = value;
    return LILYGO_MODEL_OK;
  }
  if (reg == LILYGO_GT911_CONFIG_FRESH_REGISTER) {
    model->config_fresh = value;
    return LILYGO_MODEL_OK;
  }
  if (reg == LILYGO_GT911_STATUS_REGISTER) {
    if ((value & 0x80u) == 0) {
      if (model->release_pending) {
        model->release_pending = false;
        model->status = 0x80;
        ++model->contact_generation;
        model->int_low = (model->config[6] & 0x03u) == 0x02u;
      } else if (model->contact_active) {
        // GT911 确认帧不会取消仍按住的触点；下一次轮询必须继续看到同一 contact，
        // 否则滑动会退化成一连串无关的点击。
        model->status = 0x81;
        model->int_low = (model->config[6] & 0x03u) == 0x02u;
      } else {
        model->status = 0;
        model->int_low = false;
      }
    }
    return LILYGO_MODEL_OK;
  }
  if (reg == LILYGO_GT911_COMMAND_REGISTER) {
    model->sleeping = value == 0x05;
    if (model->sleeping) {
      model->contact_active = false;
      model->release_pending = false;
      model->status = 0;
      model->int_low = false;
    }
    return LILYGO_MODEL_OK;
  }
  return LILYGO_MODEL_INVALID;
}

bool lilygo_gt911_model_consume_nack(LilygoGt911Model* model) {
  const bool nack = model->nack_latched || model->nack_once;
  model->nack_once = false;
  return nack;
}

void lilygo_gt911_model_set_nack(LilygoGt911Model* model, bool one_shot) {
  if (one_shot) {
    model->nack_once = true;
  } else {
    model->nack_latched = true;
  }
}

void lilygo_pca9535_model_reset(LilygoPca9535Model* model) {
  memset(model, 0, sizeof(*model));
  model->external[0] = 0xFF;
  model->external[1] = 0xFF;
  model->output[0] = 0xFF;
  model->output[1] = 0xFF;
  model->config[0] = 0xFF;
  model->config[1] = 0xFF;
  model->expecting_pointer = true;
}

uint8_t lilygo_pca9535_model_read(const LilygoPca9535Model* model, uint8_t reg) {
  const uint8_t port = reg & 1u;
  switch (reg) {
    case 0:
    case 1:
      return (uint8_t)(((model->external[port] & model->config[port]) |
                        (model->output[port] & (uint8_t)~model->config[port])) ^
                       model->polarity[port]);
    case 2:
    case 3:
      return model->output[port];
    case 4:
    case 5:
      return model->polarity[port];
    case 6:
    case 7:
      return model->config[port];
    default:
      return 0;
  }
}

LilygoModelResult lilygo_pca9535_model_write(LilygoPca9535Model* model, uint8_t reg, uint8_t value) {
  const uint8_t port = reg & 1u;
  switch (reg) {
    case 2:
    case 3:
      model->output[port] = value;
      return LILYGO_MODEL_OK;
    case 4:
    case 5:
      model->polarity[port] = value;
      return LILYGO_MODEL_OK;
    case 6:
    case 7:
      model->config[port] = value;
      return LILYGO_MODEL_OK;
    default:
      return LILYGO_MODEL_INVALID;
  }
}

LilygoModelResult lilygo_pca9535_model_set_external(LilygoPca9535Model* model, uint8_t port, uint8_t bit, bool high) {
  if (model == NULL || port >= 2 || bit >= 8) return LILYGO_MODEL_INVALID;
  const uint8_t mask = (uint8_t)(1u << bit);
  model->external[port] = high ? (uint8_t)(model->external[port] | mask) : (uint8_t)(model->external[port] & ~mask);
  return LILYGO_MODEL_OK;
}

LilygoModelResult lilygo_pca9535_model_set_function_pressed(LilygoPca9535Model* model, bool pressed) {
  if (model == NULL) return LILYGO_MODEL_INVALID;
  if (pressed) {
    model->function_press_unread = true;
    model->function_release_pending = false;
    return lilygo_pca9535_model_set_external(model, LILYGO_PCA9535_FUNCTION_PORT, LILYGO_PCA9535_FUNCTION_BIT, false);
  }
  if (model->function_press_unread) {
    model->function_release_pending = true;
    return LILYGO_MODEL_OK;
  }
  return lilygo_pca9535_model_set_external(model, LILYGO_PCA9535_FUNCTION_PORT, LILYGO_PCA9535_FUNCTION_BIT, true);
}

void lilygo_pca9535_model_ack_function_read(LilygoPca9535Model* model) {
  if (model == NULL || !model->function_press_unread) return;
  model->function_press_unread = false;
  if (model->function_release_pending) {
    model->function_release_pending = false;
    (void)lilygo_pca9535_model_set_external(model, LILYGO_PCA9535_FUNCTION_PORT, LILYGO_PCA9535_FUNCTION_BIT, true);
  }
}

bool lilygo_pca9535_model_output_high(const LilygoPca9535Model* model, uint8_t port, uint8_t bit) {
  if (port >= 2 || bit >= 8 || (model->config[port] & (1u << bit)) != 0) return false;
  return (model->output[port] & (1u << bit)) != 0;
}

bool lilygo_pca9535_model_consume_nack(LilygoPca9535Model* model) {
  const bool nack = model->nack_latched || model->nack_once;
  model->nack_once = false;
  return nack;
}

void lilygo_pca9535_model_set_nack(LilygoPca9535Model* model, bool one_shot) {
  if (one_shot) {
    model->nack_once = true;
  } else {
    model->nack_latched = true;
  }
}

static bool tps_control_sequence_active(const LilygoPca9535Model* pca) {
  const uint8_t mask = 0x3Bu;
  return (pca->config[1] & mask) == 0 && (pca->output[1] & mask) == mask;
}

static void tps_drive_pca_inputs(LilygoPca9535Model* pca, bool rail_good, bool fault) {
  (void)lilygo_pca9535_model_set_external(pca, 1, LILYGO_PCA9535_POWER_GOOD_BIT, rail_good);
  (void)lilygo_pca9535_model_set_external(pca, 1, LILYGO_PCA9535_INTERRUPT_BIT, !fault);
}

void lilygo_tps65185_model_reset(LilygoTps65185Model* model, LilygoPca9535Model* pca) {
  memset(model, 0, sizeof(*model));
  model->expecting_pointer = true;
  model->power_good = 0;
  if (pca != NULL) tps_drive_pca_inputs(pca, false, false);
}

void lilygo_tps65185_model_sync(LilygoTps65185Model* model, LilygoPca9535Model* pca, uint64_t now_ns) {
  const bool sequence = pca != NULL && tps_control_sequence_active(pca);
  const bool enabled = (model->enable & LILYGO_TPS65185_ENABLE_MASK) == LILYGO_TPS65185_ENABLE_MASK;
  if (!sequence || !enabled || model->hold_not_ready) {
    model->ready = false;
    model->ready_pending = false;
    model->power_good = 0;
  } else if (!model->ready && !model->ready_pending) {
    model->ready_pending = true;
    model->ready_deadline_ns = now_ns + LILYGO_TPS65185_READY_DELAY_NS;
  } else if (model->ready_pending && now_ns >= model->ready_deadline_ns) {
    model->ready = true;
    model->ready_pending = false;
    model->power_good = LILYGO_TPS65185_POWER_GOOD_MASK;
  }
  /* FastEPD waits for the discrete PCA P1.6 rail-good input before it writes
   * TPS ENABLE. TPS register 0x0F still settles only after ENABLE. */
  if (pca != NULL) tps_drive_pca_inputs(pca, sequence && !model->hold_not_ready, model->hold_not_ready);
}

uint8_t lilygo_tps65185_model_read(LilygoTps65185Model* model, LilygoPca9535Model* pca, uint8_t reg, uint64_t now_ns) {
  lilygo_tps65185_model_sync(model, pca, now_ns);
  uint8_t value = 0;
  switch (reg) {
    case LILYGO_TPS65185_ENABLE_REGISTER:
      value = model->enable;
      break;
    case LILYGO_TPS65185_VCOM_LOW_REGISTER:
      value = model->vcom_low;
      break;
    case LILYGO_TPS65185_VCOM_HIGH_REGISTER:
      value = model->vcom_high;
      break;
    case LILYGO_TPS65185_POWER_GOOD_REGISTER:
      value = model->power_good;
      break;
    default:
      break;
  }
  model->last_observed_register = reg;
  model->last_observed_value = value;
  ++model->observational_read_sequence;
  return value;
}

LilygoModelResult lilygo_tps65185_model_write(LilygoTps65185Model* model, LilygoPca9535Model* pca, uint8_t reg,
                                              uint8_t value, uint64_t now_ns) {
  switch (reg) {
    case LILYGO_TPS65185_ENABLE_REGISTER:
      model->enable = value;
      break;
    case LILYGO_TPS65185_VCOM_LOW_REGISTER:
      model->vcom_low = value;
      break;
    case LILYGO_TPS65185_VCOM_HIGH_REGISTER:
      model->vcom_high = value;
      break;
    default:
      return LILYGO_MODEL_INVALID;
  }
  lilygo_tps65185_model_sync(model, pca, now_ns);
  return LILYGO_MODEL_OK;
}

void lilygo_tps65185_model_set_hold_not_ready(LilygoTps65185Model* model, LilygoPca9535Model* pca, bool hold,
                                              uint64_t now_ns) {
  model->hold_not_ready = hold;
  lilygo_tps65185_model_sync(model, pca, now_ns);
}

bool lilygo_tps65185_model_consume_nack(LilygoTps65185Model* model) {
  const bool nack = model->nack_latched || model->nack_once;
  model->nack_once = false;
  return nack;
}

void lilygo_tps65185_model_set_nack(LilygoTps65185Model* model, bool one_shot) {
  if (one_shot) {
    model->nack_once = true;
  } else {
    model->nack_latched = true;
  }
}

uint32_t lilygo_tps65185_model_observational_read_sequence(const LilygoTps65185Model* model) {
  return model == NULL ? 0 : model->observational_read_sequence;
}

uint32_t lilygo_frontlight_duty_for_brightness(uint8_t brightness) {
  const uint32_t level = brightness;
  return (level * level * 1023u) / (255u * 255u);
}

void lilygo_frontlight_model_reset(LilygoFrontlightModel* model) { memset(model, 0, sizeof(*model)); }

LilygoModelResult lilygo_frontlight_model_configure(LilygoFrontlightModel* model, uint32_t frequency_hz,
                                                    uint8_t resolution_bits, uint8_t channel, uint8_t gpio) {
  if (model == NULL || frequency_hz == 0 || resolution_bits == 0 || resolution_bits > 20) return LILYGO_MODEL_INVALID;
  model->frequency_hz = frequency_hz;
  model->resolution_bits = resolution_bits;
  model->channel = channel;
  model->gpio = gpio;
  model->configured = true;
  ++model->sequence;
  return LILYGO_MODEL_OK;
}

LilygoModelResult lilygo_frontlight_model_set_brightness(LilygoFrontlightModel* model, uint8_t brightness) {
  if (model == NULL || !model->configured) return LILYGO_MODEL_INVALID;
  if (model->requested_brightness == brightness) return LILYGO_MODEL_OK;
  model->requested_brightness = brightness;
  model->duty = lilygo_frontlight_duty_for_brightness(brightness);
  ++model->sequence;
  return LILYGO_MODEL_OK;
}

#ifndef LILYGO_DISPLAY_INPUT_MODEL_STANDALONE

#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"

OBJECT_DECLARE_SIMPLE_TYPE(LilygoGt911State, LILYGO_GT911)
OBJECT_DECLARE_SIMPLE_TYPE(LilygoPca9535State, LILYGO_PCA9535)
OBJECT_DECLARE_SIMPLE_TYPE(LilygoTps65185State, LILYGO_TPS65185)

typedef struct LilygoGt911State {
  I2CSlave parent_obj;
  LilygoGt911Model model;
  qemu_irq int_out;
  uint32_t last_logged_generation;
  uint8_t last_logged_status;
  bool expecting_pointer;
} LilygoGt911State;

typedef struct LilygoPca9535State {
  I2CSlave parent_obj;
  LilygoPca9535Model model;
  LilygoPca9535Model logged_model;
  uint8_t last_function_input;
  bool function_input_known;
} LilygoPca9535State;

typedef struct LilygoTps65185State {
  I2CSlave parent_obj;
  LilygoTps65185Model model;
} LilygoTps65185State;

static LilygoGt911State* g_gt911;
static LilygoPca9535State* g_pca9535;
static LilygoTps65185State* g_tps65185;
static LilygoFrontlightModel g_frontlight;
static void (*g_radio_power_sink)(bool powered);
static void (*g_gnss_power_sink)(bool powered);

static void gt911_update_irq(LilygoGt911State* state) { qemu_set_irq(state->int_out, state->model.int_low ? 0 : 1); }

static int gt911_event(I2CSlave* i2c, enum i2c_event event) {
  LilygoGt911State* state = LILYGO_GT911(i2c);
  if (event == I2C_START_SEND) {
    state->expecting_pointer = true;
    state->model.pointer_bytes = 0;
  }
  if ((event == I2C_START_SEND || event == I2C_START_RECV) && lilygo_gt911_model_consume_nack(&state->model)) return 1;
  if (event == I2C_FINISH && (state->model.contact_generation != state->last_logged_generation ||
                              state->model.status != state->last_logged_status)) {
    const uint16_t x = (uint16_t)state->model.contact[1] | ((uint16_t)state->model.contact[2] << 8u);
    const uint16_t y = (uint16_t)state->model.contact[3] | ((uint16_t)state->model.contact[4] << 8u);
    fprintf(stderr, "[LILYGO-GT911-TRACE] register=0x%04x status=0x%02x x=%u y=%u generation=%u result=ack\n",
            state->model.register_pointer, state->model.status, x, y, state->model.contact_generation);
    state->last_logged_generation = state->model.contact_generation;
    state->last_logged_status = state->model.status;
  }
  return 0;
}

static int gt911_send(I2CSlave* i2c, uint8_t value) {
  LilygoGt911State* state = LILYGO_GT911(i2c);
  if (state->expecting_pointer) {
    if (state->model.pointer_bytes++ == 0) {
      state->model.register_pointer = (uint16_t)value << 8u;
    } else {
      state->model.register_pointer |= value;
      state->expecting_pointer = false;
    }
    return 0;
  }
  const LilygoModelResult result = lilygo_gt911_model_write(&state->model, state->model.register_pointer++, value);
  gt911_update_irq(state);
  return result == LILYGO_MODEL_OK ? 0 : 1;
}

static uint8_t gt911_recv(I2CSlave* i2c) {
  LilygoGt911State* state = LILYGO_GT911(i2c);
  return lilygo_gt911_model_read(&state->model, state->model.register_pointer++);
}

static void gt911_reset(Object* obj, ResetType type) {
  LilygoGt911State* state = LILYGO_GT911(obj);
  (void)type;
  lilygo_gt911_model_reset(&state->model);
  state->last_logged_generation = 0;
  state->last_logged_status = 0;
  state->expecting_pointer = true;
  gt911_update_irq(state);
}

static void gt911_gpio_reset(void* opaque, int line, int level) {
  (void)line;
  if (level == 0) gt911_reset(OBJECT(opaque), RESET_TYPE_COLD);
}

static void gt911_realize(DeviceState* dev, Error** errp) {
  LilygoGt911State* state = LILYGO_GT911(dev);
  (void)errp;
  g_gt911 = state;
  qdev_init_gpio_out_named(dev, &state->int_out, "int", 1);
  qdev_init_gpio_in_named(dev, gt911_gpio_reset, "reset", 1);
  gt911_reset(OBJECT(dev), RESET_TYPE_COLD);
}

static int pca9535_event(I2CSlave* i2c, enum i2c_event event) {
  LilygoPca9535State* state = LILYGO_PCA9535(i2c);
  if (event == I2C_START_SEND) state->model.expecting_pointer = true;
  if ((event == I2C_START_SEND || event == I2C_START_RECV) && lilygo_pca9535_model_consume_nack(&state->model))
    return 1;
  if (event == I2C_FINISH && memcmp(&state->model.external, &state->logged_model.external,
                                    sizeof(state->model.external) + sizeof(state->model.output) +
                                        sizeof(state->model.polarity) + sizeof(state->model.config)) != 0) {
    fprintf(stderr, "[LILYGO-PCA9535-TRACE] register=0x%02x output=%02x%02x config=%02x%02x result=ack\n",
            state->model.register_pointer, state->model.output[1], state->model.output[0], state->model.config[1],
            state->model.config[0]);
    memcpy(&state->logged_model.external, &state->model.external,
           sizeof(state->model.external) + sizeof(state->model.output) + sizeof(state->model.polarity) +
               sizeof(state->model.config));
  }
  return 0;
}

static int pca9535_send(I2CSlave* i2c, uint8_t value) {
  LilygoPca9535State* state = LILYGO_PCA9535(i2c);
  if (state->model.expecting_pointer) {
    state->model.register_pointer = value & 0x07u;
    state->model.expecting_pointer = false;
    return 0;
  }
  const bool peripheral_rail_was_powered =
      lilygo_pca9535_model_output_high(&state->model, LILYGO_PCA9535_RADIO_PORT, LILYGO_PCA9535_RADIO_BIT);
  const LilygoModelResult result = lilygo_pca9535_model_write(&state->model, state->model.register_pointer, value);
  state->model.register_pointer = (uint8_t)((state->model.register_pointer + 1u) & 0x07u);
  if (g_tps65185 != NULL) {
    lilygo_tps65185_model_sync(&g_tps65185->model, &state->model, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
  }
  const bool peripheral_rail_is_powered =
      lilygo_pca9535_model_output_high(&state->model, LILYGO_PCA9535_RADIO_PORT, LILYGO_PCA9535_RADIO_BIT);
  if (g_radio_power_sink != NULL && peripheral_rail_was_powered != peripheral_rail_is_powered)
    g_radio_power_sink(peripheral_rail_is_powered);
  if (g_gnss_power_sink != NULL && peripheral_rail_was_powered != peripheral_rail_is_powered)
    g_gnss_power_sink(peripheral_rail_is_powered);
  return result == LILYGO_MODEL_OK ? 0 : 1;
}

static uint8_t pca9535_recv(I2CSlave* i2c) {
  LilygoPca9535State* state = LILYGO_PCA9535(i2c);
  if (g_tps65185 != NULL) {
    lilygo_tps65185_model_sync(&g_tps65185->model, &state->model, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
  }
  const uint8_t register_pointer = state->model.register_pointer;
  const uint8_t value = lilygo_pca9535_model_read(&state->model, register_pointer);
  if (register_pointer == LILYGO_PCA9535_FUNCTION_PORT &&
      (!state->function_input_known || value != state->last_function_input)) {
    const bool pressed = (value & (1u << LILYGO_PCA9535_FUNCTION_BIT)) == 0;
    fprintf(stderr, "[LILYGO-PCA9535-FUNCTION-TRACE] source=firmware-read register=0x%02x input=0x%02x pressed=%u\n",
            register_pointer, value, pressed ? 1u : 0u);
    state->last_function_input = value;
    state->function_input_known = true;
  }
  if (register_pointer == LILYGO_PCA9535_FUNCTION_PORT) {
    lilygo_pca9535_model_ack_function_read(&state->model);
  }
  state->model.register_pointer = (uint8_t)((state->model.register_pointer + 1u) & 0x07u);
  return value;
}

static void pca9535_reset(Object* obj, ResetType type) {
  LilygoPca9535State* state = LILYGO_PCA9535(obj);
  (void)type;
  lilygo_pca9535_model_reset(&state->model);
  memset(&state->logged_model, 0, sizeof(state->logged_model));
  state->last_function_input = 0;
  state->function_input_known = false;
}

static void pca9535_realize(DeviceState* dev, Error** errp) {
  (void)errp;
  g_pca9535 = LILYGO_PCA9535(dev);
  pca9535_reset(OBJECT(dev), RESET_TYPE_COLD);
}

static int tps65185_event(I2CSlave* i2c, enum i2c_event event) {
  LilygoTps65185State* state = LILYGO_TPS65185(i2c);
  if (event == I2C_START_SEND) state->model.expecting_pointer = true;
  if ((event == I2C_START_SEND || event == I2C_START_RECV) && lilygo_tps65185_model_consume_nack(&state->model))
    return 1;
  return 0;
}

static int tps65185_send(I2CSlave* i2c, uint8_t value) {
  LilygoTps65185State* state = LILYGO_TPS65185(i2c);
  if (state->model.expecting_pointer) {
    state->model.register_pointer = value;
    state->model.expecting_pointer = false;
    return 0;
  }
  LilygoPca9535Model* pca = g_pca9535 != NULL ? &g_pca9535->model : NULL;
  const LilygoModelResult result = lilygo_tps65185_model_write(&state->model, pca, state->model.register_pointer++,
                                                               value, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
  return result == LILYGO_MODEL_OK ? 0 : 1;
}

static uint8_t tps65185_recv(I2CSlave* i2c) {
  LilygoTps65185State* state = LILYGO_TPS65185(i2c);
  LilygoPca9535Model* pca = g_pca9535 != NULL ? &g_pca9535->model : NULL;
  const uint8_t reg = state->model.register_pointer++;
  const uint8_t value = lilygo_tps65185_model_read(&state->model, pca, reg, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
  if (reg == LILYGO_TPS65185_POWER_GOOD_REGISTER) {
    fprintf(stderr, "[LILYGO-TPS65185-OBSERVE] sequence=%u register=0x%02x value=0x%02x expected=0x%02x\n",
            state->model.observational_read_sequence, reg, value, LILYGO_TPS65185_POWER_GOOD_MASK);
  }
  return value;
}

static void tps65185_reset(Object* obj, ResetType type) {
  LilygoTps65185State* state = LILYGO_TPS65185(obj);
  (void)type;
  lilygo_tps65185_model_reset(&state->model, g_pca9535 != NULL ? &g_pca9535->model : NULL);
}

static void tps65185_realize(DeviceState* dev, Error** errp) {
  (void)errp;
  g_tps65185 = LILYGO_TPS65185(dev);
  tps65185_reset(OBJECT(dev), RESET_TYPE_COLD);
}

bool lilygo_gt911_inject_contact(uint8_t action, uint16_t x, uint16_t y, uint8_t finger_id) {
  if (g_gt911 == NULL) return false;
  const bool accepted = lilygo_gt911_model_inject(&g_gt911->model, action, x, y, finger_id) == LILYGO_MODEL_OK;
  gt911_update_irq(g_gt911);
  fprintf(stderr, "[LILYGO-GT911-INJECT] action=%u x=%u y=%u finger=%u generation=%u result=%s\n", action, x, y,
          finger_id, g_gt911->model.contact_generation, accepted ? "accepted" : "rejected");
  return accepted;
}

bool lilygo_pca9535_set_external_input(uint8_t port, uint8_t bit, bool high) {
  return g_pca9535 != NULL && lilygo_pca9535_model_set_external(&g_pca9535->model, port, bit, high) == LILYGO_MODEL_OK;
}

bool lilygo_pca9535_inject_function(bool pressed) {
  return g_pca9535 != NULL && lilygo_pca9535_model_set_function_pressed(&g_pca9535->model, pressed) == LILYGO_MODEL_OK;
}

void lilygo_pca9535_release_external_inputs(void) {
  if (g_pca9535 != NULL) {
    g_pca9535->model.external[0] = 0xFF;
    g_pca9535->model.external[1] = 0xFF;
  }
}

void lilygo_pca9535_set_radio_power_sink(void (*sink)(bool powered)) {
  g_radio_power_sink = sink;
  if (sink != NULL && g_pca9535 != NULL) {
    sink(lilygo_pca9535_model_output_high(&g_pca9535->model, LILYGO_PCA9535_RADIO_PORT, LILYGO_PCA9535_RADIO_BIT));
  }
}

void lilygo_pca9535_set_gnss_power_sink(void (*sink)(bool powered)) {
  g_gnss_power_sink = sink;
  if (sink != NULL && g_pca9535 != NULL) {
    sink(lilygo_pca9535_model_output_high(&g_pca9535->model, LILYGO_PCA9535_GNSS_PORT, LILYGO_PCA9535_GNSS_BIT));
  }
}

void lilygo_tps65185_set_hold_not_ready(bool hold) {
  if (g_tps65185 != NULL) {
    lilygo_tps65185_model_set_hold_not_ready(&g_tps65185->model, g_pca9535 != NULL ? &g_pca9535->model : NULL, hold,
                                             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
  }
}

uint8_t lilygo_tps65185_control(uint8_t operation, const uint8_t* payload, uint8_t payload_len) {
  if (g_tps65185 == NULL) return 8;
  LilygoTps65185Model* model = &g_tps65185->model;
  LilygoPca9535Model* pca = g_pca9535 != NULL ? &g_pca9535->model : NULL;
  switch (operation) {
    case 1:
      if (payload == NULL || payload_len != 1 || payload[0] > 1) return 7;
      lilygo_tps65185_model_set_hold_not_ready(model, pca, payload[0] != 0, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
      return 0;
    case 2:
      if (payload == NULL || payload_len != 1 || payload[0] > 2) return 7;
      if (payload[0] == 0) {
        model->nack_once = false;
        model->nack_latched = false;
      } else {
        lilygo_tps65185_model_set_nack(model, payload[0] == 1);
      }
      return 0;
    case 3:
      if (payload_len != 0) return 7;
      lilygo_tps65185_model_reset(model, pca);
      return 0;
    case 4:
      return payload_len == 0 ? 0 : 7;
    default:
      return 3;
  }
}

void lilygo_frontlight_observe_config(uint32_t frequency_hz, uint8_t resolution_bits, uint8_t channel, uint8_t gpio) {
  (void)lilygo_frontlight_model_configure(&g_frontlight, frequency_hz, resolution_bits, channel, gpio);
}

void lilygo_frontlight_observe_brightness(uint8_t brightness) {
  (void)lilygo_frontlight_model_set_brightness(&g_frontlight, brightness);
}

const LilygoFrontlightModel* lilygo_frontlight_observer_state(void) { return &g_frontlight; }

static void gt911_class_init(ObjectClass* klass, void* data) {
  DeviceClass* device = DEVICE_CLASS(klass);
  I2CSlaveClass* i2c = I2C_SLAVE_CLASS(klass);
  ResettableClass* reset = RESETTABLE_CLASS(klass);
  (void)data;
  device->realize = gt911_realize;
  i2c->event = gt911_event;
  i2c->send = gt911_send;
  i2c->recv = gt911_recv;
  reset->phases.hold = gt911_reset;
}

static void pca9535_class_init(ObjectClass* klass, void* data) {
  DeviceClass* device = DEVICE_CLASS(klass);
  I2CSlaveClass* i2c = I2C_SLAVE_CLASS(klass);
  ResettableClass* reset = RESETTABLE_CLASS(klass);
  (void)data;
  device->realize = pca9535_realize;
  i2c->event = pca9535_event;
  i2c->send = pca9535_send;
  i2c->recv = pca9535_recv;
  reset->phases.hold = pca9535_reset;
}

static void tps65185_class_init(ObjectClass* klass, void* data) {
  DeviceClass* device = DEVICE_CLASS(klass);
  I2CSlaveClass* i2c = I2C_SLAVE_CLASS(klass);
  ResettableClass* reset = RESETTABLE_CLASS(klass);
  (void)data;
  device->realize = tps65185_realize;
  i2c->event = tps65185_event;
  i2c->send = tps65185_send;
  i2c->recv = tps65185_recv;
  reset->phases.hold = tps65185_reset;
}

static const TypeInfo gt911_info = {.name = TYPE_LILYGO_GT911,
                                    .parent = TYPE_I2C_SLAVE,
                                    .instance_size = sizeof(LilygoGt911State),
                                    .class_init = gt911_class_init};
static const TypeInfo pca9535_info = {.name = TYPE_LILYGO_PCA9535,
                                      .parent = TYPE_I2C_SLAVE,
                                      .instance_size = sizeof(LilygoPca9535State),
                                      .class_init = pca9535_class_init};
static const TypeInfo tps65185_info = {.name = TYPE_LILYGO_TPS65185,
                                       .parent = TYPE_I2C_SLAVE,
                                       .instance_size = sizeof(LilygoTps65185State),
                                       .class_init = tps65185_class_init};

static void lilygo_display_input_register_types(void) {
  type_register_static(&gt911_info);
  type_register_static(&pca9535_info);
  type_register_static(&tps65185_info);
  lilygo_frontlight_model_reset(&g_frontlight);
}

type_init(lilygo_display_input_register_types)

#endif
