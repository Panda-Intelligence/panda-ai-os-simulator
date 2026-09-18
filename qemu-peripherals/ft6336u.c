/*
 * FT6336U virtual capacitive touch controller for the Mofei simulator.
 *
 * Implements the Minimal Register Machine design from
 *   .trellis/tasks/05-04-mofei-simulator-bringup/research/ft6336u-i2c-modeling.md
 * Option A: only registers MofeiTouchDriver actually reads/writes
 * (0x00, 0x01, 0x02..0x0E, 0x80, 0x88, 0xA6, 0xA8) are modeled with state;
 * everything else returns 0 on read and is no-op on write.
 *
 * Touch/button events are injected by the SSD1677 model (which owns the
 * IPC chardev) via public ft6336u_inject_touch() / ft6336u_inject_button()
 * functions.  The FT6336U no longer has its own chardev — QEMU's CharBackend
 * does not allow multiple consumers on a single chardev.
 *
 * The Mofei firmware uses BIT-BANG software I2C (MOFEI_TOUCH_SOFT_I2C=1):
 * SCL=GPIO12, SDA=GPIO13. Therefore this peripheral does NOT attach to the
 * ESP32-S3 I2C controller. It attaches as a virtual I2C slave on a
 * BITBANG_I2C bus which is itself wired to the GPIO matrix lines for
 * GPIO12/GPIO13. See `hw/xtensa/esp32s3.c` integration notes in the
 * companion patch.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define TYPE_FT6336U "ft6336u-mofei"
OBJECT_DECLARE_SIMPLE_TYPE(FT6336UState, FT6336U)

/* Locked from .trellis/tasks/04-29-mofei-touch-support/prd.md and lib/hal/MofeiTouch.h */
#define FT6336U_DEFAULT_ADDR 0x2E

/* Hardware button ID mapping (matches Tauri ipc.rs encode_button_event) */
#define BUTTON_ID_KEY_LOCK 0 /* MOFEI_KEY_LOCK / GPIO 0 */
#define BUTTON_ID_KEY1 1     /* MOFEI_KEY1 / GPIO 1 */
#define BUTTON_ID_KEY2 2     /* MOFEI_KEY2 / GPIO 2 */
#define BUTTON_GPIO_COUNT 3
#define BUTTON_ID_COUNT 7

/* Touch action codes (used by ssd1677 inbound IPC dispatcher) */
#define TOUCH_DOWN 0x01
#define TOUCH_MOVE 0x02
#define TOUCH_UP 0x03

/* FT6336U register addresses (subset that MofeiTouchDriver actually touches) */
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

/* FT6336U event codes in P1_XH/P2_XH bits 7:6 */
#define EVT_PUT_DOWN 0x00
#define EVT_PUT_UP 0x01
#define EVT_CONTACT 0x02

#define MAX_POINTS 2
#define MAX_RAW_X 800
#define MAX_RAW_Y 480

struct FT6336UState {
  I2CSlave parent_obj;

  qemu_irq int_pin;                        /* INT GPIO line (active-low) */
  qemu_irq button_irqs[BUTTON_GPIO_COUNT]; /* Button GPIO output lines (active-low) */

  /* I2C state machine */
  uint8_t reg_ptr;         /* last register address received */
  bool expecting_reg_addr; /* true if next master-write byte is the register pointer */

  /* Register values for those that have meaningful state */
  uint8_t device_mode;  /* 0x00 */
  uint8_t gesture_id;   /* 0x01 */
  uint8_t thgroup;      /* 0x80 */
  uint8_t periodactive; /* 0x88 */
  uint8_t firmware_id;  /* 0xA6 — return canned 0x01 */
  uint8_t vendor_id;    /* 0xA8 — return FocalTech 0x11 */

  /* Touch frame buffer (11 bytes starting at reg 0x02) */
  uint8_t frame[11]; /* TD_STATUS (idx 0) + 2 × 5-byte point (idx 1..10) */

  /* Pending event from host — applied to frame on next master-read of TD_STATUS */
  uint16_t cur_x[MAX_POINTS];
  uint16_t cur_y[MAX_POINTS];
  uint8_t cur_evt[MAX_POINTS];
  uint8_t cur_id[MAX_POINTS];
  uint8_t cur_count;
  uint16_t active_x[MAX_POINTS];
  uint16_t active_y[MAX_POINTS];
  bool active[MAX_POINTS];
  bool have_pending;
};

/* Singleton — set in ft6336u_realize, used by public inject API. */
static FT6336UState* g_ft6336u;

extern void mofeiSimulatorTouchQueue(uint16_t x, uint16_t y, uint8_t action, uint8_t touch_id);
extern void mofeiSimulatorButtonStateWrite(uint8_t button_id, uint8_t pressed);

/* ---------------------- Frame buffer helpers ----------------------------- */

static void encode_frame(FT6336UState* s) {
  memset(s->frame, 0, sizeof(s->frame));
  s->frame[0] = s->cur_count & 0x0F; /* high nibble must be zero */
  for (int i = 0; i < MAX_POINTS && i < s->cur_count; i++) {
    uint16_t x = s->cur_x[i];
    uint16_t y = s->cur_y[i];
    if (x >= MAX_RAW_X) x = MAX_RAW_X - 1;
    if (y >= MAX_RAW_Y) y = MAX_RAW_Y - 1;

    uint8_t evt = s->cur_evt[i] & 0x03;
    uint8_t off = i == 0 ? 1 : 7; /* P1_XH/P2_XH relative to TD_STATUS. */
    s->frame[off + 0] = (uint8_t)((evt << 6) | ((x >> 8) & 0x0F));
    s->frame[off + 1] = (uint8_t)(x & 0xFF);
    s->frame[off + 2] = (uint8_t)(((s->cur_id[i] & 0x0F) << 4) | ((y >> 8) & 0x0F));
    s->frame[off + 3] = (uint8_t)(y & 0xFF);
    if (i == 0) {
      s->frame[off + 4] = 0x40; /* P1 WEIGHT canned; P2 weight is outside this read window. */
    }
  }
}

static void int_set(FT6336UState* s, bool active_low) {
  /* MofeiTouchDriver treats INT as active-low level: digitalRead(INT)==LOW
   * → data ready. Drive 0 when active, 1 when idle. */
  qemu_set_irq(s->int_pin, active_low ? 0 : 1);
}

static void apply_pending(FT6336UState* s) {
  if (!s->have_pending) {
    return;
  }
  encode_frame(s);
  s->have_pending = false;
  int_set(s, true); /* assert INT */
}

/* ----------------------- Public inject API -------------------------------- */
/* Called by the SSD1677 model's inbound IPC dispatcher (chr_read). */

void ft6336u_inject_touch(const uint8_t* payload, uint32_t len) {
  FT6336UState* s = g_ft6336u;

  /* Wire format (cf. ipc.rs TouchEvent::serialize):
   *   u8  action (0x01 DOWN, 0x02 MOVE, 0x03 UP)
   *   u16 x      (little-endian, firmware logical, 0..479)
   *   u16 y      (little-endian, firmware logical, 0..799)
   *   u8  finger_id (0 or 1)
   *
   * 6 bytes per event. Multiple events may be batched in one frame
   * payload. */
  const uint32_t per_event = 6;
  if (len == 0 || (len % per_event) != 0) {
    qemu_log_mask(LOG_GUEST_ERROR, "ft6336u: malformed touch payload len=%u\n", len);
    return;
  }

  /* Preserve contacts across independently delivered IPC events. */
  bool updated[MAX_POINTS] = {false, false};
  uint8_t evt[MAX_POINTS] = {EVT_CONTACT, EVT_CONTACT};

  for (uint32_t off = 0; off < len; off += per_event) {
    uint8_t action = payload[off + 0];
    uint16_t ex = (uint16_t)payload[off + 1] | ((uint16_t)payload[off + 2] << 8);
    uint16_t ey = (uint16_t)payload[off + 3] | ((uint16_t)payload[off + 4] << 8);
    uint8_t fid = payload[off + 5];

    if (fid >= MAX_POINTS) {
      continue;
    }
    updated[fid] = true;
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
    if (s) {
      s->active_x[fid] = ex;
      s->active_y[fid] = ey;
      s->active[fid] = action != TOUCH_UP;
    }
  }

  if (!s) {
    return;
  }

  s->cur_count = 0;
  for (int i = 0; i < MAX_POINTS; i++) {
    if (!s->active[i]) continue;
    s->cur_x[s->cur_count] = s->active_x[i];
    s->cur_y[s->cur_count] = s->active_y[i];
    s->cur_evt[s->cur_count] = updated[i] ? evt[i] : EVT_CONTACT;
    s->cur_id[s->cur_count] = i;
    s->cur_count++;
  }
  s->have_pending = true;
  apply_pending(s);
}

void ft6336u_inject_button(uint8_t button_id, uint8_t pressed) {
  FT6336UState* s = g_ft6336u;

  if (button_id >= BUTTON_ID_COUNT) {
    return;
  }

  /* Buttons are active-low with pull-up: pressed = LOW (0), released = HIGH (1).
   * The simulator firmware consumes g_mofeiSimButtonBits for logical HalGPIO
   * button ids. Only the first three ids have legacy discrete GPIO outputs. */
  int level = pressed ? 0 : 1;
  if (s && button_id < BUTTON_GPIO_COUNT) {
    qemu_set_irq(s->button_irqs[button_id], level);
  }

  /* Also write to firmware's g_mofeiSimButtonBits so readMofeiButtonState()
   * sees the injected state without going through GPIO reads (which are
   * intercepted by digitalRead → always-returns-1). */
  mofeiSimulatorButtonStateWrite(button_id, pressed);

  fprintf(stderr, "[FT6336U] button %u %s (GPIO level=%d)\n", button_id, pressed ? "pressed" : "released", level);
}

/* ----------------------- I²C slave callbacks ----------------------------- */

static int ft6336u_event(I2CSlave* i2c, enum i2c_event event) {
  FT6336UState* s = FT6336U(i2c);
  switch (event) {
    case I2C_START_SEND:
      s->expecting_reg_addr = true;
      break;
    case I2C_START_RECV:
      /* If the master is reading TD_STATUS, latch any pending event into
       * the frame buffer now. */
      if (s->reg_ptr == REG_TD_STATUS && s->have_pending) {
        apply_pending(s);
      }
      break;
    case I2C_FINISH:
      /* If the master read 11 bytes starting at TD_STATUS, the touch is
       * delivered — release INT until next event. */
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

static int ft6336u_send(I2CSlave* i2c, uint8_t data) {
  FT6336UState* s = FT6336U(i2c);
  if (s->expecting_reg_addr) {
    s->reg_ptr = data;
    s->expecting_reg_addr = false;
    return 0;
  }
  /* Subsequent bytes are register writes (single-byte register puts). */
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
      qemu_log_mask(LOG_UNIMP, "ft6336u: write to reg 0x%02X = 0x%02X (ignored)\n", s->reg_ptr, data);
      break;
  }
  s->reg_ptr++; /* auto-increment on multi-byte writes */
  return 0;
}

static uint8_t ft6336u_recv(I2CSlave* i2c) {
  FT6336UState* s = FT6336U(i2c);
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

static void ft6336u_realize(DeviceState* dev, Error** errp) {
  FT6336UState* s = FT6336U(dev);

  g_ft6336u = s;

  qdev_init_gpio_out_named(dev, &s->int_pin, "int", 1);
  qdev_init_gpio_out_named(dev, s->button_irqs, "button-out", BUTTON_GPIO_COUNT);

  s->reg_ptr = 0;
  s->expecting_reg_addr = true;
  s->device_mode = 0x00;
  s->gesture_id = 0x00;
  s->thgroup = 22;     /* matches FT6336_THGROUP_DEFAULT in firmware */
  s->periodactive = 4; /* matches FT6336_PERIODACTIVE_DEFAULT */
  s->firmware_id = 0x01;
  s->vendor_id = 0x11; /* FocalTech */
  s->cur_count = 0;
  memset(s->active, 0, sizeof(s->active));
  s->have_pending = false;
  memset(s->frame, 0, sizeof(s->frame));
  int_set(s, false); /* idle high */

  /* Button GPIO outputs start high (pull-up = not pressed) */
  for (int i = 0; i < BUTTON_GPIO_COUNT; i++) {
    qemu_set_irq(s->button_irqs[i], 1);
  }
}

static const VMStateDescription vmstate_ft6336u = {
    .name = TYPE_FT6336U,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){VMSTATE_I2C_SLAVE(parent_obj, FT6336UState), VMSTATE_UINT8(reg_ptr, FT6336UState),
                               VMSTATE_BOOL(expecting_reg_addr, FT6336UState), VMSTATE_UINT8(device_mode, FT6336UState),
                               VMSTATE_UINT8(gesture_id, FT6336UState), VMSTATE_UINT8(thgroup, FT6336UState),
                               VMSTATE_UINT8(periodactive, FT6336UState), VMSTATE_UINT8(firmware_id, FT6336UState),
                               VMSTATE_UINT8(vendor_id, FT6336UState), VMSTATE_UINT8_ARRAY(frame, FT6336UState, 11),
                               VMSTATE_END_OF_LIST()}};

static const Property ft6336u_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void ft6336u_class_init(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);
  I2CSlaveClass* k = I2C_SLAVE_CLASS(klass);

  k->event = ft6336u_event;
  k->send = ft6336u_send;
  k->recv = ft6336u_recv;
  dc->realize = ft6336u_realize;
  dc->vmsd = &vmstate_ft6336u;
  device_class_set_props(dc, ft6336u_properties);
  dc->desc = "FocalTech FT6336U capacitive touch controller (Mofei)";
}

static const TypeInfo ft6336u_info = {
    .name = TYPE_FT6336U,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(FT6336UState),
    .class_init = ft6336u_class_init,
};

static void ft6336u_register_types(void) { type_register_static(&ft6336u_info); }

type_init(ft6336u_register_types)
