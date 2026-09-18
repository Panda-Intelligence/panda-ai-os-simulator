/*
 * GDEQ0426T82 / SSD1677 virtual e-ink panel for the Mofei simulator.
 *
 * Implements the Hybrid FSM design from
 *   .trellis/tasks/05-04-mofei-simulator-bringup/research/eink-spi-modeling.md
 * Approach C: faithful command + address-window state machine, skip-and-consume
 * for LUT bytes / softstart / border / temperature, single 800×480 1bpp
 * framebuffer pushed to the host UI on Master Activation (cmd 0x20).
 *
 * Wire protocol to the host UI: see
 *   .trellis/tasks/05-04-mofei-simulator-bringup/research/qemu-peripheral-ipc.md
 * 8-byte header (channel:u8, flags:u8, reserved:u16, payload_len:u32) +
 * payload, multiplexed on a single chardev backend. This peripheral writes
 * channel 0x01 (framebuffer).
 *
 * Integration status (PR2):
 *   - This file is committed against the espressif/qemu fork's hw/display/
 *     layout, but is NOT yet wired into the fork's Meson build. The build
 *     wiring lands when verification-pending.md item #4 (machine flag) is
 *     confirmed and we know the exact ESP32-S3 SSI bus surface we attach to.
 *   - Compile-checked status: NOT YET. This file will fail to build until
 *     QEMU headers are visible. See verification-pending.md.
 */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "exec/address-spaces.h"
#include "exec/memattrs.h"
#include "exec/memory.h"
#include "hw/i2c/lilygo_display_input.h"
#include "hw/irq.h"
#include "hw/misc/lilygo_peripheral_control.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "hw/xtensa/mofei-sim-addrs.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#define TYPE_SSD1677_GDEQ0426 "ssd1677-gdeq0426"
OBJECT_DECLARE_SIMPLE_TYPE(SSD1677State, SSD1677_GDEQ0426)

#define EPD_WIDTH 800
#define EPD_HEIGHT 480
#define EPD_FB_BYTES ((EPD_WIDTH * EPD_HEIGHT) / 8) /* 48000 */
#define EPD_ROW_STRIDE_BYTES (EPD_WIDTH / 8)        /* 100 */

#define MURPHY_LOGICAL_WIDTH 480
#define MURPHY_LOGICAL_HEIGHT 800
#define MURPHY_LOGICAL_ROW_STRIDE_BYTES (MURPHY_LOGICAL_WIDTH / 8) /* 60 */

/* IPC channel tags — keep in sync with apps/simulator/src-tauri/src/ipc.rs */
#define IPC_CHANNEL_FRAMEBUFFER 0x01
#define IPC_CHANNEL_TOUCH_EVENT 0x04
#define IPC_CHANNEL_BUTTON_EVENT 0x05
#define IPC_CHANNEL_CONTROL 0x06
#define IPC_CHANNEL_SYNTHETIC_TOUCH_EVENT 0x07
#define IPC_CHANNEL_DEBUG 0x08
#define IPC_CHANNEL_SERIAL_COMMAND 0x09

/* Frame flags */
#define IPC_FLAG_FB_FULL 0x00
#define IPC_FLAG_FB_PARTIAL 0x01

/* Inbound IPC parser limits */
#define HEADER_LEN 8
#define MAX_INBOUND_PAYLOAD 64
#define MAX_SERIAL_COMMAND_PAYLOAD 256
#define MAX_DEBUG_INBOUND_PAYLOAD 4096
#define MAX_INBOUND_FRAME_PAYLOAD MAX_DEBUG_INBOUND_PAYLOAD

/* SSD1677 commands we model. See research/eink-spi-modeling.md table for the
 * MUST/SHOULD/MAY classification. */
#define CMD_DRIVER_OUTPUT_CTL 0x01
#define CMD_GATE_VOLTAGE 0x03 /* MAY — consume args, no-op */
#define CMD_SOURCE_VOLTAGE 0x04
#define CMD_SOFTSTART 0x0C
#define CMD_DEEP_SLEEP 0x10
#define CMD_DATA_ENTRY_MODE 0x11
#define CMD_SW_RESET 0x12
#define CMD_TEMPERATURE_SENSOR 0x18
#define CMD_WRITE_TEMP 0x1A
#define CMD_MASTER_ACTIVATION 0x20
#define CMD_DISPLAY_UPDATE_CTL_1 0x21
#define CMD_DISPLAY_UPDATE_CTL_2 0x22
#define CMD_WRITE_RAM_BW 0x24
#define CMD_WRITE_RAM_RED 0x26
#define CMD_VCOM_CTL 0x2C
#define CMD_WRITE_LUT 0x32
#define CMD_BORDER_WAVEFORM 0x3C
#define CMD_RAM_X_START_END 0x44
#define CMD_RAM_Y_START_END 0x45
#define CMD_AUTO_WRITE_RED 0x46
#define CMD_AUTO_WRITE_BW 0x47
#define CMD_RAM_X_COUNTER 0x4E
#define CMD_RAM_Y_COUNTER 0x4F

/* Refresh delays (ms) — scaled down from real timings (3.5s/1.5s/0.42s).
 * Override with env var MOFEI_SIM_FAST_REFRESH=1 to skip entirely. */
#define REFRESH_MS_FULL 500
#define REFRESH_MS_FAST 200
#define REFRESH_MS_LOCK_TRANSITION 200
#define REFRESH_MS_PARTIAL 50

/* 锁屏唤醒使用单次全帧激活；这是模拟器中的命令级波形 profile。 */
#define DISPLAY_UPDATE_CTL_2_LOCK_TRANSITION 0xD7
#define DISPLAY_UPDATE_CTL_2_GRAY4_LOCK_TRANSITION 0xCF

typedef enum {
  PARSE_IDLE,            /* expecting next command byte */
  PARSE_COLLECTING_ARGS, /* mid-command, awaiting fixed arg count */
  PARSE_RAM_WRITE,       /* inside 0x24/0x26 RAM write — until next CS rise or new cmd */
  PARSE_LUT_CONSUME,     /* inside 0x32 — count down LUT byte budget */
} ParserMode;

struct SSD1677State {
  SSIPeripheral parent_obj;

  CharBackend chr; /* host IPC chardev (reads + writes) */

  /* Pin inputs (driven by guest via GPIO matrix → qemu_irq sinks here) */
  bool dc_high;   /* D/C# state — high = data, low = command */
  bool reset_low; /* RES# state — low = held in reset */

  /* Pin outputs */
  qemu_irq busy_out; /* BUSY pin to guest */

  /* Parser state */
  uint8_t mode; /* ParserMode, stored as uint8_t for VMSTATE_UINT8 */
  uint8_t current_cmd;
  uint8_t arg_buf[8];
  uint8_t arg_index;
  uint8_t arg_expected;
  uint16_t lut_consume_remaining;

  /* Command-driven state */
  bool deep_sleep;
  bool busy_high; /* mirror of busy_out for vmstate */

  /* Address window + counters */
  uint16_t ram_x_start, ram_x_end; /* in 8-pixel units */
  uint16_t ram_y_start, ram_y_end; /* in pixel rows */
  uint16_t ram_x_cur;
  uint16_t ram_y_cur;
  uint8_t data_entry_mode; /* command 0x11 byte */

  /* Display update control */
  uint8_t display_update_ctl_2; /* command 0x22 byte */
  uint32_t activation_count;

  /* Framebuffers — single 48KB B/W buffer; "red"/"old" buffer ignored for B/W panel */
  uint8_t fb[EPD_FB_BYTES];
  uint8_t fb_red[EPD_FB_BYTES]; /* secondary buffer for cmd 0x26 */

  /* Refresh-delay timer */
  QEMUTimer* busy_timer;
  QEMUTimer* console_timer;

  /* Inbound IPC frame parser state (for touch/button dispatch) */
  uint8_t inbound_header[HEADER_LEN];
  uint8_t inbound_header_filled;
  uint8_t inbound_payload[MAX_INBOUND_FRAME_PAYLOAD];
  uint32_t inbound_payload_target;
  uint32_t inbound_payload_filled;
  uint32_t inbound_discard_remaining;
};

/* Singleton — set in ssd1677_realize, used by public API below. */
static SSD1677State* g_ssd1677;

/* Guest address of the framebuffer, set by ssd1677_set_fb_guest_addr. */
static uint32_t g_fb_guest_addr;
static uint32_t g_fb_data_guest_addr;

static bool framebuffer_bit_is_white(const uint8_t* fb, int row_stride_bytes, int x, int y) {
  const int byte_idx = y * row_stride_bytes + (x / 8);
  const int bit = 7 - (x % 8);
  return ((fb[byte_idx] >> bit) & 1) != 0;
}

static void ssd1677_set_pixel(uint8_t* fb, int x, int y, bool white) {
  const int byte_idx = y * EPD_ROW_STRIDE_BYTES + (x / 8);
  const uint8_t mask = (uint8_t)(0x80u >> (x % 8));
  if (white) {
    fb[byte_idx] |= mask;
  } else {
    fb[byte_idx] &= (uint8_t)~mask;
  }
}

static void copy_murphy_logical_framebuffer_to_panel_ram(SSD1677State* s, const uint8_t* guest_fb) {
  memset(s->fb, 0xFF, EPD_FB_BYTES);
  for (int y = 0; y < MURPHY_LOGICAL_HEIGHT; ++y) {
    for (int x = 0; x < MURPHY_LOGICAL_WIDTH; ++x) {
      const bool white = framebuffer_bit_is_white(guest_fb, MURPHY_LOGICAL_ROW_STRIDE_BYTES, x, y);
      /* Store logical portrait (x,y) at panel RAM row x / column y. push_framebuffer()
       * flips rows, and the host rotates the 800x480 payload back to portrait. */
      ssd1677_set_pixel(s->fb, y, x, white);
    }
  }
}

/* Simulator touch state exported to the TCG helper. */
/* Periodic inject timer — re-reads guest framebuffer and pushes to IPC. */
static QEMUTimer* g_inject_timer;
static bool g_inject_timer_started;
static bool g_direct_content_observed;

enum { INJECT_TIMER_INTERVAL_MS = 250 };
enum { CONSOLE_TIMER_INTERVAL_MS = 10 };
enum { MIN_DIRECT_CONTENT_BYTES = 64 };

typedef enum {
  SAMPLE_GUEST_FRAME_NONE,
  SAMPLE_GUEST_FRAME_WHITE,
  SAMPLE_GUEST_FRAME_CONTENT,
} SampleGuestFrameResult;

static uint32_t read_guest_u32_le(const uint8_t bytes[4]) {
  return ((uint32_t)bytes[0]) | ((uint32_t)bytes[1] << 8u) | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

/* Forward declarations — public inject API from ft6336u.c */
extern void ft6336u_inject_touch(const uint8_t* payload, uint32_t len);
extern void ft6336u_inject_button(uint8_t button_id, uint8_t pressed);
extern void mofeiSimulatorTouchEventWrite(uint8_t type, uint16_t x, uint16_t y);
extern bool mofeiSimulatorConsoleWrite(const uint8_t* payload, uint32_t len);
extern bool mofeiSimulatorConsolePollResponse(void);
extern bool mofeiSimulatorPandaDebugWriteRequest(const uint8_t* payload, uint32_t len);
extern bool mofeiSimulatorPandaDebugPollResponse(void);
void ssd1677_publish_direct_framebuffer(const uint8_t* payload, uint32_t len);
#ifdef __EMSCRIPTEN__
extern void mofei_wasm_ipc_send(uint32_t channel, uint32_t flags, const uint8_t* payload, uint32_t len);
#endif

/* ---------------------------- Host IPC ----------------------------------- */

static void ipc_send_frame(SSD1677State* s, uint8_t channel, uint8_t flags, const uint8_t* payload, uint32_t len) {
#ifdef __EMSCRIPTEN__
  (void)s;
  mofei_wasm_ipc_send(channel, flags, payload, len);
  return;
#endif
  uint8_t header[8];
  header[0] = channel;
  header[1] = flags;
  header[2] = 0;
  header[3] = 0;
  header[4] = (uint8_t)(len & 0xFF);
  header[5] = (uint8_t)((len >> 8) & 0xFF);
  header[6] = (uint8_t)((len >> 16) & 0xFF);
  header[7] = (uint8_t)((len >> 24) & 0xFF);

  if (qemu_chr_fe_write_all(&s->chr, header, sizeof(header)) != sizeof(header)) {
    qemu_log_mask(LOG_GUEST_ERROR, "ssd1677: ipc header short write\n");
    return;
  }
  if (len > 0 && qemu_chr_fe_write_all(&s->chr, payload, len) != (int)len) {
    qemu_log_mask(LOG_GUEST_ERROR, "ssd1677: ipc payload short write\n");
  }
}

static void ssd1677_send_control(const uint8_t* payload, uint32_t len) {
  if (g_ssd1677 != NULL && len <= LILYGO_I2C_CONTROL_MAX_PAYLOAD) {
    ipc_send_frame(g_ssd1677, IPC_CHANNEL_CONTROL, 0, payload, len);
  }
}

static void push_framebuffer(SSD1677State* s, bool partial) {
  /* Y-flip: the firmware uses data_entry_mode=0x01 (Y-decrement), which writes
   * logical row 0 to SSD1677 RAM row (EPD_HEIGHT-1).  On real hardware, the
   * panel's gate drivers are physically reversed so gate (EPD_HEIGHT-1) drives
   * the top row.  The host PNG writer expects standard top-to-bottom order,
   * so we flip rows during push. */
  uint8_t flipped[EPD_FB_BYTES];
  for (int row = 0; row < EPD_HEIGHT; row++) {
    memcpy(&flipped[row * EPD_ROW_STRIDE_BYTES], &s->fb[(EPD_HEIGHT - 1 - row) * EPD_ROW_STRIDE_BYTES],
           EPD_ROW_STRIDE_BYTES);
  }
  ipc_send_frame(s, IPC_CHANNEL_FRAMEBUFFER, partial ? IPC_FLAG_FB_PARTIAL : IPC_FLAG_FB_FULL, flipped, EPD_FB_BYTES);
  {
    static int push_count = 0;
    push_count++;
    if (push_count <= 5 || (push_count % 10) == 0) {
      /* Check framebuffer content: sample across multiple rows to avoid
       * false all-white/all-black labels when only the top row is uniform. */
      int non_zero = 0;
      for (int i = 0; i < EPD_FB_BYTES; i++) {
        if (flipped[i] != 0) non_zero++;
      }
      int non_white = 0;
      for (int i = 0; i < EPD_FB_BYTES; i++) {
        if (flipped[i] != 0xFF) non_white++;
      }
      bool all_white = (non_white == 0);
      bool all_black = (non_zero == 0);
      bool has_content = (non_white >= MIN_DIRECT_CONTENT_BYTES) && !all_black;
      fprintf(stderr, "[SSD1677] push_framebuffer #%d (partial=%d) non_zero=%d/%d non_white=%d/%d%s%s%s\n", push_count,
              partial, non_zero, EPD_FB_BYTES, non_white, EPD_FB_BYTES, all_white ? " [ALL-WHITE]" : "",
              all_black ? " [ALL-BLACK]" : "", has_content ? " [HAS-CONTENT]" : "");
      /* Hex dump first 16 bytes of top and bottom rows (after flip). */
      if (push_count <= 3) {
        fprintf(stderr, "[SSD1677]   top-row (flipped): ");
        for (int i = 0; i < 16; i++) fprintf(stderr, "%02X ", flipped[i]);
        fprintf(stderr, "\n");
        fprintf(stderr, "[SSD1677]   bot-row (flipped): ");
        for (int i = 0; i < 16; i++) fprintf(stderr, "%02X ", flipped[(EPD_HEIGHT - 1) * EPD_ROW_STRIDE_BYTES + i]);
        fprintf(stderr, "\n");
        fprintf(stderr,
                "[SSD1677]   ram: mode=0x%02X dem=0x%02X x_cur=%u y_cur=%u x_start=%u x_end=%u y_start=%u y_end=%u\n",
                s->mode, s->data_entry_mode, s->ram_x_cur, s->ram_y_cur, s->ram_x_start, s->ram_x_end, s->ram_y_start,
                s->ram_y_end);
      }
    }
  }
}

/* ----------------------- Inbound IPC (chr_read) -------------------------- */
/* SSD1677 owns the chardev and dispatches inbound channels to the
 * appropriate peripheral (FT6336U for touch, button GPIO directly). */

static int ssd1677_chr_can_read(void* opaque) {
  (void)opaque;
  return MAX_INBOUND_FRAME_PAYLOAD + HEADER_LEN;
}

static uint32_t ssd1677_inbound_payload_cap(uint8_t channel) {
  if (channel == IPC_CHANNEL_DEBUG) return MAX_DEBUG_INBOUND_PAYLOAD;
  if (channel == IPC_CHANNEL_SERIAL_COMMAND) return MAX_SERIAL_COMMAND_PAYLOAD;
  return MAX_INBOUND_PAYLOAD;
}

static void ssd1677_reset_inbound_frame(SSD1677State* s) {
  s->inbound_header_filled = 0;
  s->inbound_payload_target = 0;
  s->inbound_payload_filled = 0;
}

static void ssd1677_inject_touch(const uint8_t* payload, uint32_t len) {
  if (!mofei_sim_board_is_lilygo_t5s3_pro()) {
    ft6336u_inject_touch(payload, len);
    return;
  }

  const uint32_t event_size = 6;
  if (len == 0 || len % event_size != 0) {
    qemu_log_mask(LOG_GUEST_ERROR, "lilygo-gt911: malformed touch payload len=%u\n", len);
    return;
  }
  for (uint32_t offset = 0; offset < len; offset += event_size) {
    const uint8_t action = payload[offset];
    const uint16_t x = (uint16_t)payload[offset + 1] | ((uint16_t)payload[offset + 2] << 8);
    const uint16_t y = (uint16_t)payload[offset + 3] | ((uint16_t)payload[offset + 4] << 8);
    const uint8_t finger_id = payload[offset + 5];
    if (!lilygo_gt911_inject_contact(action, x, y, finger_id)) {
      qemu_log_mask(LOG_GUEST_ERROR, "lilygo-gt911: rejected touch action=%u x=%u y=%u finger=%u\n", action, x, y,
                    finger_id);
    }
  }
}

static void ssd1677_inject_button(uint8_t button_id, uint8_t pressed) {
  if (mofei_sim_board_is_lilygo_t5s3_pro() && button_id == 1) {
    if (!lilygo_pca9535_inject_function(pressed != 0)) {
      qemu_log_mask(LOG_GUEST_ERROR, "lilygo-pca9535: function injection unavailable\n");
    }
    return;
  }
  ft6336u_inject_button(button_id, pressed);
}

static void ssd1677_dispatch_inbound_frame(SSD1677State* s) {
  uint8_t channel = s->inbound_header[0];
  if (channel == IPC_CHANNEL_TOUCH_EVENT) {
    ssd1677_inject_touch(s->inbound_payload, s->inbound_payload_target);
  } else if (channel == IPC_CHANNEL_BUTTON_EVENT) {
    /* Wire format: u8 button_id, u8 pressed */
    if (s->inbound_payload_target >= 2) {
      ssd1677_inject_button(s->inbound_payload[0], s->inbound_payload[1]);
    }
  } else if (channel == IPC_CHANNEL_CONTROL) {
    lilygo_peripheral_control_dispatch(s->inbound_payload, s->inbound_payload_target);
  } else if (channel == IPC_CHANNEL_SYNTHETIC_TOUCH_EVENT) {
    /* Wire format: u8 type, u16 x, u16 y */
    if (s->inbound_payload_target >= 5) {
      const uint8_t type = s->inbound_payload[0];
      const uint16_t x = (uint16_t)s->inbound_payload[1] | ((uint16_t)s->inbound_payload[2] << 8);
      const uint16_t y = (uint16_t)s->inbound_payload[3] | ((uint16_t)s->inbound_payload[4] << 8);
      mofeiSimulatorTouchEventWrite(type, x, y);
    }
  } else if (channel == IPC_CHANNEL_DEBUG) {
    if (s->inbound_payload_target == 0) {
      mofeiSimulatorPandaDebugPollResponse();
    } else if (mofeiSimulatorPandaDebugWriteRequest(s->inbound_payload, s->inbound_payload_target)) {
      mofeiSimulatorPandaDebugPollResponse();
    }
  } else if (channel == IPC_CHANNEL_SERIAL_COMMAND) {
    fprintf(stderr, "[MOFEI-SIM] console frame received len=%u\n", s->inbound_payload_target);
    mofeiSimulatorConsoleWrite(s->inbound_payload, s->inbound_payload_target);
  }
  /* Other channels silently ignored. */
}

static void ssd1677_chr_read(void* opaque, const uint8_t* buf, int size) {
  SSD1677State* s = opaque;
  int p = 0;
  while (p < size) {
    if (s->inbound_discard_remaining > 0) {
      uint32_t take = (uint32_t)(size - p);
      if (take > s->inbound_discard_remaining) take = s->inbound_discard_remaining;
      s->inbound_discard_remaining -= take;
      p += take;
      continue;
    }
    if (s->inbound_header_filled < HEADER_LEN) {
      uint32_t need = HEADER_LEN - s->inbound_header_filled;
      uint32_t take = (uint32_t)(size - p);
      if (take > need) take = need;
      memcpy(&s->inbound_header[s->inbound_header_filled], &buf[p], take);
      s->inbound_header_filled += take;
      p += take;
      if (s->inbound_header_filled == HEADER_LEN) {
        s->inbound_payload_target = (uint32_t)s->inbound_header[4] | ((uint32_t)s->inbound_header[5] << 8) |
                                    ((uint32_t)s->inbound_header[6] << 16) | ((uint32_t)s->inbound_header[7] << 24);
        uint8_t channel = s->inbound_header[0];
        uint32_t cap = ssd1677_inbound_payload_cap(channel);
        if (s->inbound_payload_target > cap) {
          qemu_log_mask(LOG_GUEST_ERROR, "ssd1677: inbound channel 0x%02x payload %u exceeds cap %u\n", channel,
                        s->inbound_payload_target, cap);
          s->inbound_discard_remaining = s->inbound_payload_target;
          ssd1677_reset_inbound_frame(s);
          continue;
        }
        s->inbound_payload_filled = 0;
        if (s->inbound_payload_target == 0) {
          ssd1677_dispatch_inbound_frame(s);
          ssd1677_reset_inbound_frame(s);
        }
      }
    } else {
      uint32_t need = s->inbound_payload_target - s->inbound_payload_filled;
      uint32_t take = (uint32_t)(size - p);
      if (take > need) take = need;
      memcpy(&s->inbound_payload[s->inbound_payload_filled], &buf[p], take);
      s->inbound_payload_filled += take;
      p += take;
      if (s->inbound_payload_filled == s->inbound_payload_target) {
        ssd1677_dispatch_inbound_frame(s);
        ssd1677_reset_inbound_frame(s);
      }
    }
  }
}

static void ssd1677_chr_event(void* opaque, QEMUChrEvent event) {
  SSD1677State* s = opaque;
  if (event == CHR_EVENT_OPENED) {
    ssd1677_reset_inbound_frame(s);
    s->inbound_discard_remaining = 0;
  }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
void mofei_wasm_inject_touch(uint8_t action, uint16_t x, uint16_t y, uint8_t finger_id) {
  uint8_t payload[6];
  payload[0] = action;
  payload[1] = (uint8_t)(x & 0xFF);
  payload[2] = (uint8_t)((x >> 8) & 0xFF);
  payload[3] = (uint8_t)(y & 0xFF);
  payload[4] = (uint8_t)((y >> 8) & 0xFF);
  payload[5] = finger_id;
  ssd1677_inject_touch(payload, sizeof(payload));
}

EMSCRIPTEN_KEEPALIVE
void mofei_wasm_inject_button(uint8_t button_id, uint8_t pressed) { ssd1677_inject_button(button_id, pressed); }

EMSCRIPTEN_KEEPALIVE
void mofei_wasm_release_buttons(void) {
  for (uint8_t button_id = 0; button_id < 7; button_id++) {
    ssd1677_inject_button(button_id, 0);
  }
}
#endif

/* ------------------------------ BUSY ------------------------------------- */

static void busy_set(SSD1677State* s, bool high) {
  s->busy_high = high;
  qemu_set_irq(s->busy_out, high ? 1 : 0);
}

static void busy_timer_expired(void* opaque) {
  SSD1677State* s = opaque;
  busy_set(s, false);
}

static int refresh_delay_ms(SSD1677State* s) {
  /* Crude classification from the command 0x22 byte. Patterns: full=0xF7,
   * fast=0xC7, lock-transition=0xD7/0xCF, partial=0xFF — vendor demos vary;
   * tune in PR2 once
   * we capture real firmware bytes. */
  uint8_t v = s->display_update_ctl_2;
  if (v == DISPLAY_UPDATE_CTL_2_LOCK_TRANSITION || v == DISPLAY_UPDATE_CTL_2_GRAY4_LOCK_TRANSITION) {
    return REFRESH_MS_LOCK_TRANSITION;
  } else if (v == 0xF7 || v == 0xC0) {
    return REFRESH_MS_FULL;
  } else if (v == 0xC7) {
    return REFRESH_MS_FAST;
  } else {
    return REFRESH_MS_PARTIAL;
  }
}

/* ---------------------- RAM-write helpers -------------------------------- */

static void advance_ram_pointer(SSD1677State* s) {
  /* SSD1677 data entry mode (command 0x11) bits:
   *   bit 0 (AM):  0 = X direction first, 1 = Y direction first
   *   bit 1 (ID0): 0 = Y decrement, 1 = Y increment
   *   bit 2 (ID1): 0 = X increment, 1 = X decrement
   *
   * Verified against GDEQ0426T82 firmware (data_entry_mode = 0x01
   * means X increment, Y decrement, Y-first) and GxEPD2 library
   * (data_entry_mode = 0x03 means X increment, Y increment, Y-first). */

  int x_inc = (s->data_entry_mode & 0x04) ? -1 : +1;
  int y_inc = (s->data_entry_mode & 0x02) ? +1 : -1;
  bool y_first = (s->data_entry_mode & 0x01) != 0;

  if (y_first) {
    /* Advance Y. Wrap when counter exits [ram_y_end .. ram_y_start]. */
    int new_y = (int)s->ram_y_cur + y_inc;
    if (new_y < (int)s->ram_y_end || new_y > (int)s->ram_y_start) {
      /* Wrapped — reset Y and advance X. */
      s->ram_y_cur = s->ram_y_start;
      int new_x = (int)s->ram_x_cur + x_inc;
      if (new_x < (int)s->ram_x_start || new_x > (int)s->ram_x_end) {
        s->ram_x_cur = s->ram_x_start;
      } else {
        s->ram_x_cur = (uint16_t)new_x;
      }
    } else {
      s->ram_y_cur = (uint16_t)new_y;
    }
  } else {
    /* Advance X. Wrap when counter exits [ram_x_start .. ram_x_end]. */
    int new_x = (int)s->ram_x_cur + x_inc;
    if (new_x < (int)s->ram_x_start || new_x > (int)s->ram_x_end) {
      /* Wrapped — reset X and advance Y. */
      s->ram_x_cur = s->ram_x_start;
      int new_y = (int)s->ram_y_cur + y_inc;
      if (new_y < (int)s->ram_y_end || new_y > (int)s->ram_y_start) {
        s->ram_y_cur = s->ram_y_start;
      } else {
        s->ram_y_cur = (uint16_t)new_y;
      }
    } else {
      s->ram_x_cur = (uint16_t)new_x;
    }
  }
}

static void ram_write_byte(SSD1677State* s, uint8_t* target, uint8_t value) {
  /* ram_x_cur is in 8-pixel units, ram_y_cur is in pixel rows. */
  uint32_t row = s->ram_y_cur;
  uint32_t col_byte = s->ram_x_cur;
  if (row < EPD_HEIGHT && col_byte < EPD_ROW_STRIDE_BYTES) {
    target[row * EPD_ROW_STRIDE_BYTES + col_byte] = value;
  } else {
    static int oor_log = 0;
    if (oor_log < 5) {
      fprintf(stderr, "[SSD1677] ram_write OOR: row=%u col=%u val=0x%02X (max %u×%u)\n", row, col_byte, value,
              EPD_HEIGHT, EPD_ROW_STRIDE_BYTES);
      oor_log++;
    }
  }
  advance_ram_pointer(s);
}

/* ----------------------- Command dispatch -------------------------------- */

static void reset_state(SSD1677State* s) {
  s->mode = PARSE_IDLE;
  s->current_cmd = 0;
  s->arg_index = 0;
  s->arg_expected = 0;
  s->lut_consume_remaining = 0;
  s->deep_sleep = false;
  s->ram_x_start = 0;
  s->ram_x_end = EPD_ROW_STRIDE_BYTES - 1;
  s->ram_y_start = 0;
  s->ram_y_end = EPD_HEIGHT - 1;
  s->ram_x_cur = 0;
  s->ram_y_cur = 0;
  s->data_entry_mode = 0x03; /* AM=1 (Y-first), Y increment, X increment */
  s->display_update_ctl_2 = 0;
  s->activation_count = 0;
  busy_set(s, false);
}

static void execute_command_no_args(SSD1677State* s, uint8_t cmd) {
  switch (cmd) {
    case CMD_SW_RESET:
      memset(s->fb, 0xFF, EPD_FB_BYTES);
      memset(s->fb_red, 0xFF, EPD_FB_BYTES);
      reset_state(s);
      /* Spec: brief BUSY high after reset */
      busy_set(s, true);
      timer_mod(s->busy_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
      break;
    case CMD_MASTER_ACTIVATION: {
      /* The SPI WRITE_RAM → ssd1677 internal fb[] path produces garbled
       * output because QEMU's SSI FSM cannot reliably reconstruct the
       * 48KB pixel stream.  The RETW inject path (via HELPER(mofei_inject))
       * copies the guest framebuffer directly and pushes a clean frame.
       * Suppress the SPI-triggered push to avoid a double-push race where
       * the garbled SPI frame overwrites a freshly-injected clean frame,
       * causing visible flicker.  Still honour the BUSY timing so the
       * firmware's refresh cycle completes normally. */
      s->activation_count++;
      const bool lock_transition = s->display_update_ctl_2 == DISPLAY_UPDATE_CTL_2_LOCK_TRANSITION ||
                                   s->display_update_ctl_2 == DISPLAY_UPDATE_CTL_2_GRAY4_LOCK_TRANSITION;
      const int wait_ms = refresh_delay_ms(s);
      fprintf(stderr,
              "[SSD1677] display_trace activation=%u profile=%s physical_activations=1 visible_flashes=1 "
              "update_control_2=0x%02X wait_ms=%d\n",
              s->activation_count, lock_transition ? "lock-transition" : "standard", s->display_update_ctl_2, wait_ms);
      busy_set(s, true);
      timer_mod(s->busy_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + wait_ms);
      break;
    }
    default:
      qemu_log_mask(LOG_UNIMP, "ssd1677: unhandled no-arg cmd 0x%02X\n", cmd);
      break;
  }
}

static int command_arg_count(uint8_t cmd) {
  switch (cmd) {
    case CMD_DRIVER_OUTPUT_CTL:
      return 3;
    case CMD_GATE_VOLTAGE:
      return 1;
    case CMD_SOURCE_VOLTAGE:
      return 3;
    case CMD_SOFTSTART:
      /* GDEQ0426T82 firmware sends 5 bytes (0x0C with 5 × sendData). */
      return 5;
    case CMD_DEEP_SLEEP:
      return 1;
    case CMD_DATA_ENTRY_MODE:
      return 1;
    case CMD_TEMPERATURE_SENSOR:
      return 1;
    case CMD_WRITE_TEMP:
      return 1;
    case CMD_DISPLAY_UPDATE_CTL_1:
      return 2;
    case CMD_DISPLAY_UPDATE_CTL_2:
      return 1;
    case CMD_VCOM_CTL:
      return 1;
    case CMD_BORDER_WAVEFORM:
      return 1;
    case CMD_RAM_X_START_END:
      /* MofeiDisplay::setRamArea sends 16-bit pixel values for X start/end
       * (4 bytes total: start_lo, start_hi, end_lo, end_hi), not 8-bit
       * 8-pixel-unit values (2 bytes). Accept 4 bytes and convert below. */
      return 4;
    case CMD_RAM_Y_START_END:
      return 4;
    case CMD_AUTO_WRITE_RED:
      return 1;
    case CMD_AUTO_WRITE_BW:
      return 1;
    case CMD_RAM_X_COUNTER:
      /* MofeiDisplay sends 16-bit pixel value (2 bytes) for X counter. */
      return 2;
    case CMD_RAM_Y_COUNTER:
      return 2;
    default:
      return 0;
  }
}

static void apply_collected_args(SSD1677State* s) {
  switch (s->current_cmd) {
    case CMD_DATA_ENTRY_MODE:
      s->data_entry_mode = s->arg_buf[0];
      break;
    case CMD_DISPLAY_UPDATE_CTL_2:
      s->display_update_ctl_2 = s->arg_buf[0];
      break;
    case CMD_DEEP_SLEEP:
      s->deep_sleep = true;
      break;
    case CMD_RAM_X_START_END:
      /* Firmware sends pixel values as 16-bit LE pairs (start_lo, start_hi,
       * end_lo, end_hi). Convert to 8-pixel units (divide by 8) for internal
       * RAM addressing. See MofeiDisplay::setRamArea in lib/hal/MofeiDisplay.cpp. */
      {
        uint16_t x_start_px = (uint16_t)s->arg_buf[0] | ((uint16_t)s->arg_buf[1] << 8);
        uint16_t x_end_px = (uint16_t)s->arg_buf[2] | ((uint16_t)s->arg_buf[3] << 8);
        s->ram_x_start = x_start_px / 8;
        s->ram_x_end = x_end_px / 8;
        fprintf(stderr, "[SSD1677] RAM_X_START_END: px %u..%u → col %u..%u\n", x_start_px, x_end_px, s->ram_x_start,
                s->ram_x_end);
      }
      break;
    case CMD_RAM_Y_START_END:
      s->ram_y_start = (uint16_t)s->arg_buf[0] | ((uint16_t)s->arg_buf[1] << 8);
      s->ram_y_end = (uint16_t)s->arg_buf[2] | ((uint16_t)s->arg_buf[3] << 8);
      fprintf(stderr, "[SSD1677] RAM_Y_START_END: row %u..%u\n", s->ram_y_start, s->ram_y_end);
      break;
    case CMD_RAM_X_COUNTER:
      /* Firmware sends 16-bit pixel value. Convert to 8-pixel unit. */
      {
        uint16_t x_px = (uint16_t)s->arg_buf[0] | ((uint16_t)s->arg_buf[1] << 8);
        s->ram_x_cur = x_px / 8;
      }
      break;
    case CMD_RAM_Y_COUNTER:
      s->ram_y_cur = (uint16_t)s->arg_buf[0] | ((uint16_t)s->arg_buf[1] << 8);
      break;
    case CMD_AUTO_WRITE_BW: {
      uint8_t pat = s->arg_buf[0];
      memset(s->fb, pat, EPD_FB_BYTES);
      break;
    }
    case CMD_AUTO_WRITE_RED: {
      uint8_t pat = s->arg_buf[0];
      memset(s->fb_red, pat, EPD_FB_BYTES);
      break;
    }
    case CMD_DRIVER_OUTPUT_CTL:
    case CMD_SOFTSTART:
    case CMD_TEMPERATURE_SENSOR:
    case CMD_WRITE_TEMP:
    case CMD_BORDER_WAVEFORM:
    case CMD_DISPLAY_UPDATE_CTL_1:
    case CMD_VCOM_CTL:
    case CMD_GATE_VOLTAGE:
    case CMD_SOURCE_VOLTAGE:
      /* Skip-and-consume per Hybrid FSM design. */
      break;
    default:
      qemu_log_mask(LOG_UNIMP, "ssd1677: unhandled cmd 0x%02X with %d args\n", s->current_cmd, s->arg_index);
      break;
  }
}

/* ----------------------- SSI transfer hook ------------------------------- */

static uint32_t ssd1677_transfer(SSIPeripheral* dev, uint32_t value) {
  SSD1677State* s = SSD1677_GDEQ0426(dev);
  uint8_t byte = value & 0xFF;

  if (s->reset_low) {
    return 0;
  }

  if (!s->dc_high) {
    /* Command byte. Aborts any in-progress RAM write. */
    s->current_cmd = byte;
    s->arg_index = 0;
    s->arg_expected = command_arg_count(byte);

    if (byte == CMD_WRITE_RAM_BW || byte == CMD_WRITE_RAM_RED) {
      s->mode = PARSE_RAM_WRITE;
    } else if (byte == CMD_WRITE_LUT) {
      /* SSD1677 LUT is up to 153 bytes. We accept a generous budget
       * and consume until next command; real firmwares are bounded. */
      s->mode = PARSE_LUT_CONSUME;
      s->lut_consume_remaining = 200;
    } else if (s->arg_expected > 0) {
      s->mode = PARSE_COLLECTING_ARGS;
    } else {
      s->mode = PARSE_IDLE;
      execute_command_no_args(s, byte);
    }
    return 0;
  }

  /* Data byte. */
  switch (s->mode) {
    case PARSE_COLLECTING_ARGS:
      if (s->arg_index < sizeof(s->arg_buf)) {
        s->arg_buf[s->arg_index++] = byte;
      }
      if (s->arg_index >= s->arg_expected) {
        apply_collected_args(s);
        s->mode = PARSE_IDLE;
      }
      break;
    case PARSE_RAM_WRITE: {
      uint8_t* target = (s->current_cmd == CMD_WRITE_RAM_BW) ? s->fb : s->fb_red;
      ram_write_byte(s, target, byte);
      break;
    }
    case PARSE_LUT_CONSUME:
      if (s->lut_consume_remaining > 0) {
        s->lut_consume_remaining--;
      }
      break;
    case PARSE_IDLE:
    default:
      /* Stray data byte before any command — log and ignore. */
      qemu_log_mask(LOG_GUEST_ERROR, "ssd1677: unexpected data 0x%02X in IDLE\n", byte);
      break;
  }
  return 0;
}

/* ----------------------- GPIO sinks (D/C#, RES#) ------------------------- */

static void dc_irq_handler(void* opaque, int n, int level) {
  SSD1677State* s = opaque;
  s->dc_high = (level != 0);
}

static void reset_irq_handler(void* opaque, int n, int level) {
  SSD1677State* s = opaque;
  bool was_low = s->reset_low;
  s->reset_low = (level == 0);
  if (was_low && !s->reset_low) {
    /* Rising edge — perform HW reset. */
    memset(s->fb, 0xFF, EPD_FB_BYTES);
    memset(s->fb_red, 0xFF, EPD_FB_BYTES);
    reset_state(s);
  }
}

/* ----------------------- Public API (exc_helper.c) ----------------------- */

static SampleGuestFrameResult sample_guest_framebuffer(bool allow_low_content_frame) {
  if (!g_ssd1677) {
    return SAMPLE_GUEST_FRAME_NONE;
  }

  /* Read the guest framebuffer directly from guest memory and push to IPC.
   *
   * The SPI bulk-transfer intercept correctly delivers individual init
   * commands through the GPSPI2 → SSD1677 FSM.  However, the 48KB bulk
   * framebuffer data arrives while the FSM may not be in DTM2 (0x24) data
   * mode, causing pixels to land at wrong positions.  Instead of relying
   * on the FSM path for the bulk data, we read the framebuffer directly
   * from guest memory and Y-reverse it to match the SSD1677's addressing.
   *
   * g_fb_guest_addr points to the uint8_t* frameBuffer pointer inside the
   * guest's MofeiDisplay object. The data is in logical row order (row 0
   * first), but the SSD1677 fb[] uses Y-decrement addressing (row 0 at
   * fb[479*100]). We reverse the rows during copy.  push_framebuffer then
   * Y-flips again for the output, producing the correct final image. */
  if (g_fb_guest_addr || g_fb_data_guest_addr) {
    extern bool mofei_read_guest_memory(uint32_t addr, uint8_t* dest, uint32_t len);

    uint32_t fb_ptr = g_fb_data_guest_addr;
    if (fb_ptr == 0) {
      uint8_t ptr_bytes[4] = {0};
      if (!mofei_read_guest_memory(g_fb_guest_addr, ptr_bytes, sizeof(ptr_bytes))) {
        return SAMPLE_GUEST_FRAME_NONE;
      }
      fb_ptr = read_guest_u32_le(ptr_bytes);
    }

    if (fb_ptr != 0) {
      uint8_t* guest_fb = g_malloc0(EPD_FB_BYTES);
      if (!mofei_read_guest_memory(fb_ptr, guest_fb, EPD_FB_BYTES)) {
        g_free(guest_fb);
        return SAMPLE_GUEST_FRAME_NONE;
      }

      int non_zero = 0;
      int non_white = 0;
      int first_non_white[6] = {-1, -1, -1, -1, -1, -1};
      uint8_t first_non_white_value[6] = {0};
      int first_non_white_count = 0;
      for (int i = 0; i < EPD_FB_BYTES; i++) {
        if (guest_fb[i] != 0) non_zero++;
        if (guest_fb[i] != 0xFF) {
          if (first_non_white_count < 6) {
            first_non_white[first_non_white_count] = i;
            first_non_white_value[first_non_white_count] = guest_fb[i];
            first_non_white_count++;
          }
          non_white++;
        }
      }
      static int inject_log = 0;
      inject_log++;
      if (inject_log <= 12) {
        fprintf(stderr, "[SSD1677] inject_timer #%d: fb_ptr=0x%08x non_zero=%d/%d non_white=%d/%d first=", inject_log,
                fb_ptr, non_zero, EPD_FB_BYTES, non_white, EPD_FB_BYTES);
        if (first_non_white_count == 0) {
          fprintf(stderr, "none");
        }
        for (int i = 0; i < first_non_white_count; ++i) {
          fprintf(stderr, "%s%d:0x%02x", i == 0 ? "" : ",", first_non_white[i], first_non_white_value[i]);
        }
        fprintf(stderr, "\n");
      }

      const bool has_content = non_white >= MIN_DIRECT_CONTENT_BYTES;
      const bool is_white = non_white == 0;
      if (has_content || allow_low_content_frame) {
        for (int row = 0; row < EPD_HEIGHT; row++) {
          if (g_fb_data_guest_addr != 0) {
            copy_murphy_logical_framebuffer_to_panel_ram(g_ssd1677, guest_fb);
            break;
          }
          memcpy(&g_ssd1677->fb[(EPD_HEIGHT - 1 - row) * EPD_ROW_STRIDE_BYTES], &guest_fb[row * EPD_ROW_STRIDE_BYTES],
                 EPD_ROW_STRIDE_BYTES);
        }
        g_free(guest_fb);
        return has_content ? SAMPLE_GUEST_FRAME_CONTENT : SAMPLE_GUEST_FRAME_WHITE;
      }

      g_free(guest_fb);
    }
  }

  return SAMPLE_GUEST_FRAME_NONE;
}

static void inject_timer_cb(void* opaque) {
  /* Periodic timer is permanently disabled.  Frames are only pushed
   * synchronously via the RETW inject path, one push per displayBuffer(). */
  (void)opaque;
  return;
  if (!g_ssd1677) {
    return;
  }

  const SampleGuestFrameResult sample_result = sample_guest_framebuffer(false);

  /* Once real activity content has been observed, do not let a timer tick that
   * lands between firmware clearScreen() and the following draw publish a full
   * white frame. Synchronous publish hooks after Activity render push the
   * completed frame immediately, so skipping these transient white samples
   * avoids the touch-triggered white-screen loop without adding a busy poll. */
  if (sample_result == SAMPLE_GUEST_FRAME_CONTENT) {
    g_direct_content_observed = true;
    push_framebuffer(g_ssd1677, false);
  }

  if (!g_direct_content_observed) {
    timer_mod(g_inject_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + INJECT_TIMER_INTERVAL_MS);
  }
}

static void console_timer_cb(void* opaque) {
  SSD1677State* s = opaque;
  mofeiSimulatorConsolePollResponse();
  timer_mod(s->console_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + CONSOLE_TIMER_INTERVAL_MS);
}

void ssd1677_spi_byte(uint8_t byte) {
  if (!g_ssd1677) {
    return;
  }
  ssd1677_transfer(SSI_PERIPHERAL(g_ssd1677), byte);
}

void ssd1677_spi_bytes(const uint8_t* data, uint32_t len) {
  if (!g_ssd1677) {
    return;
  }
  for (uint32_t i = 0; i < len; i++) {
    ssd1677_transfer(SSI_PERIPHERAL(g_ssd1677), data[i]);
  }
}

void mofei_set_dc_pin(bool high) {
  if (g_ssd1677) {
    g_ssd1677->dc_high = high;
  }
}

void ssd1677_set_fb_guest_addr(uint32_t addr) { g_fb_guest_addr = addr; }
void ssd1677_set_fb_data_guest_addr(uint32_t addr) { g_fb_data_guest_addr = addr; }

void ssd1677_emit_debug_frame(const uint8_t* payload, uint32_t len) {
  if (!g_ssd1677) {
    return;
  }
  ipc_send_frame(g_ssd1677, IPC_CHANNEL_DEBUG, 0, payload, len);
}

void ssd1677_publish_direct_framebuffer(const uint8_t* payload, uint32_t len) {
  if (!g_ssd1677 || !payload || len == 0) {
    return;
  }
  ipc_send_frame(g_ssd1677, IPC_CHANNEL_FRAMEBUFFER, IPC_FLAG_FB_FULL, payload, len);
}

void ssd1677_inject_real_framebuffer(const uint8_t* data, uint32_t len) {
  if (!g_ssd1677) {
    return;
  }
  if (len > EPD_FB_BYTES) {
    len = EPD_FB_BYTES;
  }
  memcpy(g_ssd1677->fb, data, len);
}

void ssd1677_start_inject_timer(void) {
  /* Periodic timer is permanently disabled.  Frames are only pushed
   * synchronously via the RETW inject path. */
  return;
  if (!g_inject_timer) {
    g_inject_timer = timer_new_ms(QEMU_CLOCK_REALTIME, inject_timer_cb, NULL);
  }
  if (g_inject_timer_started) {
    return;
  }
  g_inject_timer_started = true;
  timer_mod(g_inject_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + INJECT_TIMER_INTERVAL_MS);
}

void ssd1677_publish_injected_framebuffer(void) {
  if (!g_ssd1677) {
    return;
  }
  const SampleGuestFrameResult sample_result = sample_guest_framebuffer(true);
  if (sample_result == SAMPLE_GUEST_FRAME_CONTENT) {
    g_direct_content_observed = true;
    push_framebuffer(g_ssd1677, false);
  } else if (sample_result == SAMPLE_GUEST_FRAME_WHITE) {
    push_framebuffer(g_ssd1677, false);
  }
  static int publish_hex_dump = 0;
  if (publish_hex_dump < 3) {
    uint32_t fb_ptr = g_fb_data_guest_addr;
    if (fb_ptr != 0) {
      extern bool mofei_read_guest_memory(uint32_t addr, uint8_t* dest, uint32_t len);
      uint8_t peek[56];
      if (!mofei_read_guest_memory(fb_ptr, peek, sizeof(peek))) {
        ++publish_hex_dump;
        return;
      }
      fprintf(stderr, "[SSD1677] publish hook fb=%08x result=%d bytes:", fb_ptr, (int)sample_result);
      for (int i = 0; i < (int)sizeof(peek); ++i) {
        if (i % 20 == 0) fprintf(stderr, "\n  ");
        fprintf(stderr, "%02x ", peek[i]);
      }
      fprintf(stderr, "\n");
    }
    ++publish_hex_dump;
  }
}

void ssd1677_inject_and_push_framebuffer(void) {
  if (!g_ssd1677) {
    return;
  }
  push_framebuffer(g_ssd1677, false);
}

/* ----------------------- QEMU plumbing ----------------------------------- */

static const VMStateDescription vmstate_ssd1677 = {
    .name = TYPE_SSD1677_GDEQ0426,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){VMSTATE_SSI_PERIPHERAL(parent_obj, SSD1677State),
                               VMSTATE_BOOL(dc_high, SSD1677State),
                               VMSTATE_BOOL(reset_low, SSD1677State),
                               VMSTATE_BOOL(busy_high, SSD1677State),
                               VMSTATE_UINT8(mode, SSD1677State),
                               VMSTATE_UINT8(current_cmd, SSD1677State),
                               VMSTATE_UINT8_ARRAY(arg_buf, SSD1677State, 8),
                               VMSTATE_UINT8(arg_index, SSD1677State),
                               VMSTATE_UINT8(arg_expected, SSD1677State),
                               VMSTATE_UINT16(lut_consume_remaining, SSD1677State),
                               VMSTATE_BOOL(deep_sleep, SSD1677State),
                               VMSTATE_UINT16(ram_x_start, SSD1677State),
                               VMSTATE_UINT16(ram_x_end, SSD1677State),
                               VMSTATE_UINT16(ram_y_start, SSD1677State),
                               VMSTATE_UINT16(ram_y_end, SSD1677State),
                               VMSTATE_UINT16(ram_x_cur, SSD1677State),
                               VMSTATE_UINT16(ram_y_cur, SSD1677State),
                               VMSTATE_UINT8(data_entry_mode, SSD1677State),
                               VMSTATE_UINT8(display_update_ctl_2, SSD1677State),
                               VMSTATE_UINT32(activation_count, SSD1677State),
                               VMSTATE_UINT8_ARRAY(fb, SSD1677State, EPD_FB_BYTES),
                               VMSTATE_UINT8_ARRAY(fb_red, SSD1677State, EPD_FB_BYTES),
                               VMSTATE_END_OF_LIST()}};

static const Property ssd1677_properties[] = {
    DEFINE_PROP_CHR("chardev", SSD1677State, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void ssd1677_realize(SSIPeripheral* d, Error** errp) {
  SSD1677State* s = SSD1677_GDEQ0426(d);
  DeviceState* dev = DEVICE(d);

  g_ssd1677 = s;
  lilygo_peripheral_control_set_ipc_sender(ssd1677_send_control);

  qdev_init_gpio_in_named(dev, dc_irq_handler, "dc", 1);
  qdev_init_gpio_in_named(dev, reset_irq_handler, "reset", 1);
  qdev_init_gpio_out_named(dev, &s->busy_out, "busy", 1);

  s->busy_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, busy_timer_expired, s);
  s->console_timer = timer_new_ms(QEMU_CLOCK_REALTIME, console_timer_cb, s);
  timer_mod(s->console_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + CONSOLE_TIMER_INTERVAL_MS);

  memset(s->fb, 0xFF, EPD_FB_BYTES);
  memset(s->fb_red, 0xFF, EPD_FB_BYTES);
  reset_state(s);

  /* Register inbound IPC handlers so SSD1677 can read touch/button events
   * from the chardev and dispatch them to FT6336U / GPIO. */
  s->inbound_header_filled = 0;
  s->inbound_payload_target = 0;
  s->inbound_payload_filled = 0;
  s->inbound_discard_remaining = 0;

  if (qemu_chr_fe_backend_connected(&s->chr)) {
    qemu_chr_fe_set_handlers(&s->chr, ssd1677_chr_can_read, ssd1677_chr_read, ssd1677_chr_event, NULL, s, NULL, true);
    fprintf(stderr, "[SSD1677] IPC chardev handlers registered (read+write)\n");
  }
}

static void ssd1677_class_init(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);
  SSIPeripheralClass* k = SSI_PERIPHERAL_CLASS(klass);

  k->realize = ssd1677_realize;
  k->transfer = ssd1677_transfer;
  dc->vmsd = &vmstate_ssd1677;
  device_class_set_props(dc, ssd1677_properties);
  dc->desc = "GoodDisplay GDEQ0426T82-T01C 4.26\" e-ink, SSD1677 driver";
}

static const TypeInfo ssd1677_info = {
    .name = TYPE_SSD1677_GDEQ0426,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(SSD1677State),
    .class_init = ssd1677_class_init,
};

static void ssd1677_register_types(void) { type_register_static(&ssd1677_info); }

type_init(ssd1677_register_types)
