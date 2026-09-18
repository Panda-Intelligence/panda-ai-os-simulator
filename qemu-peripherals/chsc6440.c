/*
 * CHSC6440 virtual capacitive touch controller for the s37uc simulator.
 *
 * The ChipSemi CHSC6440 is register-compatible with the FocalTech FT6x36
 * family (see .trellis/tasks/06-06-new-board-uc8253c-port/research/
 * chsc6440-touch-register-map.md): touch count at reg 0x02, point-1 XH/XL/YH/YL
 * at 0x03..0x06 (12-bit, event in XH[7:6], id in YH[7:4]), point-2 at 0x09..0x0C.
 * This model therefore mirrors ft6336u.c (Minimal Register Machine, Option A)
 * with three board deltas:
 *   1. I2C slave address 0x40 (validated on s37uc hardware after GPIO42
 *      TOUCH_WAKE enable; M5 is 0x2E).
 *   2. Coordinate clamps for the 416x240 CHSC calibration plane.
 *   3. s37uc has two discrete side keys (SIDEKEY, SIDEKEY1).
 *
 * Touch/button events are injected by the UC8253C model (which owns the IPC
 * chardev) via chsc6440_inject_touch() / chsc6440_inject_button(), mirroring
 * the ssd1677 → ft6336u relationship. QEMU's CharBackend does not allow
 * multiple consumers on one chardev, so the display model is the sole owner.
 *
 * The s37uc firmware uses the same software-I2C touch path as Mofei; this
 * peripheral attaches as a virtual I2C slave on the BITBANG_I2C bus wired to
 * the IIC_SDA/IIC_SCL GPIO matrix lines. See hw/xtensa/esp32s3.c integration
 * notes in INTEGRATION.md for the s37uc pin map.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define TYPE_CHSC6440 "chsc6440-s37uc"
OBJECT_DECLARE_SIMPLE_TYPE(CHSC6440State, CHSC6440)

/* s37uc hardware responds with a valid CHSC frame at 0x40 after GPIO42
 * TOUCH_WAKE enable. FT6x36 defaults to 0x38 and the M5 CHSC variant defaults
 * to 0x2E. */
#define CHSC6440_DEFAULT_ADDR 0x40

/* Hardware button ID mapping (matches Tauri ipc.rs encode_button_event). s37uc
 * exposes two discrete side keys. */
#define BUTTON_ID_SIDEKEY 0  /* SIDEKEY */
#define BUTTON_ID_SIDEKEY1 1 /* SIDEKEY1 */
#define BUTTON_GPIO_COUNT 2
#define BUTTON_ID_COUNT 7

/* Touch action codes (used by uc8253c inbound IPC dispatcher) */
#define TOUCH_DOWN 0x01
#define TOUCH_MOVE 0x02
#define TOUCH_UP 0x03

/* FT6x36-compatible register addresses (subset the firmware actually touches) */
#define REG_DEVICE_MODE 0x00
#define REG_GESTURE_ID 0x01
#define REG_TD_STATUS 0x02
#define REG_P1_XH 0x03
#define REG_P1_XL 0x04
#define REG_P1_YH 0x05
#define REG_P1_YL 0x06
#define REG_P1_WEIGHT 0x07
#define REG_P1_MISC 0x08
#define REG_P2_XH 0x09
#define REG_P2_XL 0x0A
#define REG_P2_YH 0x0B
#define REG_P2_YL 0x0C
#define REG_THGROUP 0x80
#define REG_PERIODACTIVE 0x88
#define REG_FIRMWARE_ID 0xA6
#define REG_VENDOR_ID 0xA8

/* Event codes in P1_XH/P2_XH bits 7:6 */
#define EVT_PUT_DOWN 0x00
#define EVT_PUT_UP 0x01
#define EVT_CONTACT 0x02

#define MAX_POINTS 2
#define LOGICAL_WIDTH 240
#define LOGICAL_HEIGHT 416
#define MAX_RAW_X 416
#define MAX_RAW_Y 240

struct CHSC6440State {
  I2CSlave parent_obj;

  qemu_irq int_pin;                        /* INT GPIO line (active-low) */
  qemu_irq button_irqs[BUTTON_GPIO_COUNT]; /* Side-key GPIO output lines (active-low) */

  /* I2C state machine */
  uint8_t reg_ptr;         /* last register address received */
  bool expecting_reg_addr; /* true if next master-write byte is the register pointer */

  /* Register values for those that have meaningful state */
  uint8_t device_mode;  /* 0x00 */
  uint8_t gesture_id;   /* 0x01 */
  uint8_t thgroup;      /* 0x80 */
  uint8_t periodactive; /* 0x88 */
  uint8_t firmware_id;  /* 0xA6 — canned; s37uc firmware detects by ACK + sane frame */
  uint8_t vendor_id;    /* 0xA8 — canned */

  /* Touch frame buffer (11 bytes starting at reg 0x02) */
  uint8_t frame[11]; /* TD_STATUS (idx 0) + 2 × 5-byte point (idx 1..10) */

  /* Pending event from host — applied to frame on next master-read of TD_STATUS */
  uint16_t cur_x[MAX_POINTS];
  uint16_t cur_y[MAX_POINTS];
  uint8_t cur_evt[MAX_POINTS];
  uint8_t cur_count;
  bool have_pending;
};

/* Singleton — set in chsc6440_realize, used by public inject API. */
static CHSC6440State* g_chsc6440;

extern void mofeiSimulatorTouchQueue(uint16_t x, uint16_t y, uint8_t action, uint8_t touch_id);
extern void mofeiSimulatorButtonStateWrite(uint8_t button_id, uint8_t pressed);

/* ---------------------- Frame buffer helpers ----------------------------- */

static uint16_t scale_axis_endpoint(uint16_t value, uint16_t from_extent, uint16_t to_extent) {
  if (from_extent <= 1 || to_extent <= 1) {
    return 0;
  }
  if (value >= from_extent) {
    value = from_extent - 1;
  }
  return (uint16_t)(((uint32_t)value * (to_extent - 1)) / (from_extent - 1));
}

static void encode_frame(CHSC6440State* s) {
  memset(s->frame, 0, sizeof(s->frame));
  s->frame[0] = s->cur_count & 0x0F; /* high nibble must be zero */
  for (int i = 0; i < MAX_POINTS && i < s->cur_count; i++) {
    uint16_t x = s->cur_x[i];
    uint16_t y = s->cur_y[i];
    if (x >= MAX_RAW_X) x = MAX_RAW_X - 1;
    if (y >= MAX_RAW_Y) y = MAX_RAW_Y - 1;

    uint8_t evt = s->cur_evt[i] & 0x03;
    uint8_t off = 1 + i * 5; /* 1, 6 */
    s->frame[off + 0] = (uint8_t)((evt << 6) | ((x >> 8) & 0x0F));
    s->frame[off + 1] = (uint8_t)(x & 0xFF);
    s->frame[off + 2] = (uint8_t)(((i & 0x0F) << 4) | ((y >> 8) & 0x0F));
    s->frame[off + 3] = (uint8_t)(y & 0xFF);
    s->frame[off + 4] = 0x40; /* WEIGHT canned */
  }
}

static void int_set(CHSC6440State* s, bool active_low) {
  /* Firmware treats INT as active-low level: digitalRead(INT)==LOW → data
   * ready. Drive 0 when active, 1 when idle. */
  qemu_set_irq(s->int_pin, active_low ? 0 : 1);
}

static void apply_pending(CHSC6440State* s) {
  if (!s->have_pending) {
    return;
  }
  encode_frame(s);
  s->have_pending = false;
  int_set(s, true); /* assert INT */
}

/* ----------------------- Public inject API -------------------------------- */
/* Called by the UC8253C model's inbound IPC dispatcher (chr_read). */

void chsc6440_inject_touch(const uint8_t* payload, uint32_t len) {
  if (!g_chsc6440) {
    return;
  }
  CHSC6440State* s = g_chsc6440;

  /* Wire format (cf. ipc.rs TouchEvent::serialize):
   *   u8  action (0x01 DOWN, 0x02 MOVE, 0x03 UP)
   *   u16 x      (little-endian, firmware logical, 0..239)
   *   u16 y      (little-endian, firmware logical, 0..415)
   *   u8  finger_id (0 or 1)
   * 6 bytes per event; multiple events may be batched in one frame payload. */
  const uint32_t per_event = 6;
  if (len == 0 || (len % per_event) != 0) {
    qemu_log_mask(LOG_GUEST_ERROR, "chsc6440: malformed touch payload len=%u\n", len);
    return;
  }

  bool present[MAX_POINTS] = {false, false};
  uint16_t x[MAX_POINTS] = {0, 0};
  uint16_t y[MAX_POINTS] = {0, 0};
  uint8_t evt[MAX_POINTS] = {EVT_CONTACT, EVT_CONTACT};

  for (uint32_t off = 0; off < len; off += per_event) {
    uint8_t action = payload[off + 0];
    uint16_t ex = (uint16_t)payload[off + 1] | ((uint16_t)payload[off + 2] << 8);
    uint16_t ey = (uint16_t)payload[off + 3] | ((uint16_t)payload[off + 4] << 8);
    uint8_t fid = payload[off + 5];

    if (fid >= MAX_POINTS) {
      continue;
    }
    present[fid] = (action != TOUCH_UP);
    /* The simulator host injects Murphy OS logical portrait coordinates.
     * The virtual CHSC register frame exposes the controller calibration
     * plane: raw X follows logical Y, raw Y follows logical X. */
    x[fid] = scale_axis_endpoint(ey, LOGICAL_HEIGHT, MAX_RAW_X);
    y[fid] = scale_axis_endpoint(ex, LOGICAL_WIDTH, MAX_RAW_Y);
    switch (action) {
      case TOUCH_DOWN:
        evt[fid] = EVT_PUT_DOWN;
        break;
      case TOUCH_MOVE:
        evt[fid] = EVT_CONTACT;
        break;
      case TOUCH_UP:
        evt[fid] = EVT_PUT_UP;
        break;
      default:
        evt[fid] = EVT_CONTACT;
        break;
    }

    mofeiSimulatorTouchQueue(ex, ey, action, fid);
  }

  s->cur_count = 0;
  for (int i = 0; i < MAX_POINTS; i++) {
    if (present[i] || evt[i] == EVT_PUT_UP) {
      s->cur_x[s->cur_count] = x[i];
      s->cur_y[s->cur_count] = y[i];
      s->cur_evt[s->cur_count] = evt[i];
      s->cur_count++;
    }
  }
  s->have_pending = true;
  apply_pending(s);
}

void chsc6440_inject_button(uint8_t button_id, uint8_t pressed) {
  if (!g_chsc6440) {
    return;
  }
  CHSC6440State* s = g_chsc6440;

  if (button_id >= BUTTON_ID_COUNT) {
    return;
  }

  /* Buttons are active-low with pull-up: pressed = LOW (0), released = HIGH (1).
   * Only the discrete side keys have GPIO outputs; logical ids beyond that are
   * delivered via g_mofeiSimButtonBits only. */
  int level = pressed ? 0 : 1;
  if (button_id < BUTTON_GPIO_COUNT) {
    qemu_set_irq(s->button_irqs[button_id], level);
  }

  mofeiSimulatorButtonStateWrite(button_id, pressed);

  fprintf(stderr, "[CHSC6440] button %u %s (GPIO level=%d)\n", button_id, pressed ? "pressed" : "released", level);
}

/* ----------------------- I²C slave callbacks ----------------------------- */

static int chsc6440_event(I2CSlave* i2c, enum i2c_event event) {
  CHSC6440State* s = CHSC6440(i2c);
  switch (event) {
    case I2C_START_SEND:
      s->expecting_reg_addr = true;
      break;
    case I2C_START_RECV:
      if (s->reg_ptr == REG_TD_STATUS && s->have_pending) {
        apply_pending(s);
      }
      break;
    case I2C_FINISH:
      if (s->reg_ptr == REG_TD_STATUS) {
        int_set(s, false);
      }
      break;
    case I2C_NACK:
      break;
    default:
      break;
  }
  return 0;
}

static int chsc6440_send(I2CSlave* i2c, uint8_t data) {
  CHSC6440State* s = CHSC6440(i2c);
  if (s->expecting_reg_addr) {
    s->reg_ptr = data;
    s->expecting_reg_addr = false;
    return 0;
  }
  switch (s->reg_ptr) {
    case REG_DEVICE_MODE:
      s->device_mode = data;
      break;
    case REG_THGROUP:
      s->thgroup = data;
      break;
    case REG_PERIODACTIVE:
      s->periodactive = data;
      break;
    default:
      qemu_log_mask(LOG_UNIMP, "chsc6440: write to reg 0x%02X = 0x%02X (ignored)\n", s->reg_ptr, data);
      break;
  }
  s->reg_ptr++; /* auto-increment on multi-byte writes */
  return 0;
}

static uint8_t chsc6440_recv(I2CSlave* i2c) {
  CHSC6440State* s = CHSC6440(i2c);
  uint8_t value = 0x00;

  switch (s->reg_ptr) {
    case REG_DEVICE_MODE:
      value = s->device_mode;
      break;
    case REG_GESTURE_ID:
      value = s->gesture_id;
      break;
    case REG_TD_STATUS ... REG_P2_YL:
      value = s->frame[s->reg_ptr - REG_TD_STATUS];
      break;
    case REG_THGROUP:
      value = s->thgroup;
      break;
    case REG_PERIODACTIVE:
      value = s->periodactive;
      break;
    case REG_FIRMWARE_ID:
      value = s->firmware_id;
      break;
    case REG_VENDOR_ID:
      value = s->vendor_id;
      break;
    default:
      value = 0x00;
      break;
  }
  s->reg_ptr++; /* auto-increment for burst reads */
  return value;
}

/* ----------------------- QEMU plumbing ----------------------------------- */

static void chsc6440_realize(DeviceState* dev, Error** errp) {
  CHSC6440State* s = CHSC6440(dev);

  g_chsc6440 = s;

  qdev_init_gpio_out_named(dev, &s->int_pin, "int", 1);
  qdev_init_gpio_out_named(dev, s->button_irqs, "button-out", BUTTON_GPIO_COUNT);

  s->reg_ptr = 0;
  s->expecting_reg_addr = true;
  s->device_mode = 0x00;
  s->gesture_id = 0x00;
  s->thgroup = 22;
  s->periodactive = 4;
  s->firmware_id = 0x01;
  s->vendor_id = 0x11;
  s->cur_count = 0;
  s->have_pending = false;
  memset(s->frame, 0, sizeof(s->frame));
  int_set(s, false); /* idle high */

  for (int i = 0; i < BUTTON_GPIO_COUNT; i++) {
    qemu_set_irq(s->button_irqs[i], 1);
  }
}

static const VMStateDescription vmstate_chsc6440 = {
    .name = TYPE_CHSC6440,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){VMSTATE_I2C_SLAVE(parent_obj, CHSC6440State), VMSTATE_UINT8(reg_ptr, CHSC6440State),
                               VMSTATE_BOOL(expecting_reg_addr, CHSC6440State),
                               VMSTATE_UINT8(device_mode, CHSC6440State), VMSTATE_UINT8(gesture_id, CHSC6440State),
                               VMSTATE_UINT8(thgroup, CHSC6440State), VMSTATE_UINT8(periodactive, CHSC6440State),
                               VMSTATE_UINT8(firmware_id, CHSC6440State), VMSTATE_UINT8(vendor_id, CHSC6440State),
                               VMSTATE_UINT8_ARRAY(frame, CHSC6440State, 11), VMSTATE_END_OF_LIST()}};

static const Property chsc6440_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void chsc6440_class_init(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);
  I2CSlaveClass* k = I2C_SLAVE_CLASS(klass);

  k->event = chsc6440_event;
  k->send = chsc6440_send;
  k->recv = chsc6440_recv;
  dc->realize = chsc6440_realize;
  dc->vmsd = &vmstate_chsc6440;
  device_class_set_props(dc, chsc6440_properties);
  dc->desc = "ChipSemi CHSC6440 capacitive touch controller (s37uc)";
}

static const TypeInfo chsc6440_info = {
    .name = TYPE_CHSC6440,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(CHSC6440State),
    .class_init = chsc6440_class_init,
};

static void chsc6440_register_types(void) { type_register_static(&chsc6440_info); }

type_init(chsc6440_register_types)
