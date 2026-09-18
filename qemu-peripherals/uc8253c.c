/*
 * UC8253C virtual e-ink panel for the s37uc simulator (logical 416x240
 * calibration, controller source/gate order 240x416).
 *
 * Mirrors the Hybrid FSM design of ssd1677_gdeq0426t82.c (faithful command +
 * data state machine; skip-and-consume for LUT / booster / VCOM / temperature),
 * adapted to the UltraChip UC8253C command set used by the s37uc 3.7" custom
 * panel. Source of truth for the command sequence:
 *   docs/hw/new/3.7寸UC8253C 客户常温5S GC DU例程.../main.c (+ waveinit.h LUTs).
 *
 * Like the SSD1677 model, the authoritative pixel data is read directly from
 * the guest framebuffer (Uc8253Display object) on Display Refresh, because the
 * QEMU SSI FSM cannot reliably reconstruct the bulk pixel stream. The command
 * FSM is modeled for BUSY timing + init correctness; the direct-guest-fb read
 * path produces the clean frame pushed to the host UI.
 *
 * Addressing difference vs SSD1677: UC8253C DIS_IMG transmits source bytes for
 * each gate line. The simulator's host-facing payload is still logical 416x240;
 * the source/gate order is only used for the SPI FSM fallback and init checks.
 *
 * IPC wire protocol to the host UI matches ssd1677: 8-byte header
 * (channel:u8, flags:u8, reserved:u16, payload_len:u32) + payload on a single
 * chardev. This peripheral owns the chardev and dispatches inbound touch/button
 * channels to the CHSC6440 model, mirroring the ssd1677 → ft6336u relationship.
 *
 * Integration status: committed against the espressif/qemu fork's hw/display/
 * layout; build wiring + SoC machine instantiation for the s37uc board are
 * applied by integrate-peripherals.sh / hand-merge (see INTEGRATION.md).
 * Compile-checked status: OWED — requires QEMU fork headers.
 */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "exec/address-spaces.h"
#include "exec/memattrs.h"
#include "exec/memory.h"
#include "hw/irq.h"
#include "hw/misc/lilygo_peripheral_control.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define TYPE_UC8253C "uc8253c-s37uc"
OBJECT_DECLARE_SIMPLE_TYPE(UC8253CState, UC8253C)

#define EPD_OUTPUT_WIDTH 416
#define EPD_OUTPUT_HEIGHT 240
#define EPD_SOURCE_PIXELS 240
#define EPD_GATE_PIXELS 416
#define EPD_FB_BYTES ((EPD_OUTPUT_WIDTH * EPD_OUTPUT_HEIGHT) / 8) /* 12480 */
#define EPD_OUTPUT_ROW_STRIDE_BYTES (EPD_OUTPUT_WIDTH / 8)        /* 52 */
#define EPD_SOURCE_ROW_STRIDE_BYTES (EPD_SOURCE_PIXELS / 8)       /* 30 */

/* IPC channel tags — keep in sync with apps/simulator/src-tauri/src/ipc.rs */
#define IPC_CHANNEL_FRAMEBUFFER 0x01
#define IPC_CHANNEL_TOUCH_EVENT 0x04
#define IPC_CHANNEL_BUTTON_EVENT 0x05
#define IPC_CHANNEL_CONTROL 0x06
#define IPC_CHANNEL_SYNTHETIC_TOUCH_EVENT 0x07
#define IPC_CHANNEL_DEBUG 0x08

/* Frame flags */
#define IPC_FLAG_FB_FULL 0x00
#define IPC_FLAG_FB_PARTIAL 0x01

/* Inbound IPC parser limits */
#define HEADER_LEN 8
#define MAX_INBOUND_PAYLOAD 64
#define MAX_DEBUG_INBOUND_PAYLOAD 4096
#define MAX_INBOUND_FRAME_PAYLOAD MAX_DEBUG_INBOUND_PAYLOAD

/* UC8253C commands we model (UltraChip PSR/PWR/LUT-from-register family). */
#define CMD_PANEL_SETTING 0x00             /* PSR — vendor sends 2 bytes */
#define CMD_POWER_SETTING 0x01             /* PWR — 5 bytes */
#define CMD_POWER_OFF 0x02                 /* no args */
#define CMD_POWER_ON 0x04                  /* no args */
#define CMD_BOOSTER_SOFT_START 0x06        /* 3 bytes */
#define CMD_DEEP_SLEEP 0x07                /* 1 byte (0xA5) */
#define CMD_DATA_START_TRANSMISSION_1 0x10 /* DTM1 (old) */
#define CMD_DISPLAY_REFRESH 0x12           /* no args — drives refresh */
#define CMD_DATA_START_TRANSMISSION_2 0x13 /* DTM2 (new image) */
#define CMD_AUTO_SEQUENCE 0x17             /* 1 byte (0xA5) PON+DRF+POF */
#define CMD_TEMP_MEASURE 0x05              /* Power ON measure — no args */
#define CMD_LUT_VCOM 0x20                  /* LUT registers (consume) */
#define CMD_LUT_WW 0x21
#define CMD_LUT_BW 0x22
#define CMD_LUT_WB 0x23
#define CMD_LUT_BB 0x24
#define CMD_PLL_CONTROL 0x30        /* 1 byte */
#define CMD_TEMP_SENSOR_CALIB 0x40  /* no args (read register) */
#define CMD_TEMP_SENSOR_SELECT 0x41 /* 1 byte */
#define CMD_VCOM_DATA_INTERVAL 0x50 /* 1 byte */
#define CMD_RESOLUTION_SETTING 0x61 /* 3 bytes (source_lo, gate_hi, gate_lo) */
#define CMD_VCOM_DC_SETTING 0x82    /* 1 byte */

/* Refresh delays (ms) — scaled from real ~5s/DU timings. */
#define REFRESH_MS_FULL 500
#define REFRESH_MS_FAST 200

typedef enum {
  PARSE_IDLE,            /* expecting next command byte */
  PARSE_COLLECTING_ARGS, /* mid-command, awaiting fixed arg count */
  PARSE_RAM_WRITE,       /* inside 0x10/0x13 image data — until next command */
  PARSE_LUT_CONSUME,     /* inside 0x20-0x24 — count down LUT byte budget */
} ParserMode;

struct UC8253CState {
  SSIPeripheral parent_obj;

  CharBackend chr; /* host IPC chardev (reads + writes) */

  /* Pin inputs (driven by guest via GPIO matrix → qemu_irq sinks here) */
  bool dc_high;   /* D/C# state — high = data, low = command */
  bool reset_low; /* RES# state — low = held in reset */

  /* Pin outputs */
  qemu_irq busy_out; /* BUSY pin to guest (UC8253C BUSY is active-low busy) */

  /* Parser state */
  uint8_t mode; /* ParserMode */
  uint8_t current_cmd;
  uint8_t arg_buf[8];
  uint8_t arg_index;
  uint8_t arg_expected;
  uint16_t lut_consume_remaining;

  /* Command-driven state */
  bool deep_sleep;
  bool busy_high;
  uint16_t configured_source_pixels;
  uint16_t configured_gate_pixels;

  /* Linear RAM write cursor into fb[] for 0x13 image data. */
  uint32_t ram_write_index;

  /* Framebuffer — single 12480-byte B/W buffer (DTM2 / new image). */
  uint8_t fb[EPD_FB_BYTES];

  /* Refresh-delay timer */
  QEMUTimer* busy_timer;

  /* Inbound IPC frame parser state (for touch/button dispatch) */
  uint8_t inbound_header[HEADER_LEN];
  uint8_t inbound_header_filled;
  uint8_t inbound_payload[MAX_INBOUND_FRAME_PAYLOAD];
  uint32_t inbound_payload_target;
  uint32_t inbound_payload_filled;
  uint32_t inbound_discard_remaining;
};

/* Singleton — set in uc8253c_realize, used by public API below. */
static UC8253CState* g_uc8253c;

/* Guest address of the framebuffer pointer / data, set externally. */
static uint32_t g_fb_guest_addr;
static uint32_t g_fb_data_guest_addr;

enum { MIN_DIRECT_CONTENT_BYTES = 64 };

typedef enum {
  SAMPLE_GUEST_FRAME_NONE,
  SAMPLE_GUEST_FRAME_WHITE,
  SAMPLE_GUEST_FRAME_CONTENT,
} SampleGuestFrameResult;

static uint32_t read_guest_u32_le(const uint8_t bytes[4]) {
  return ((uint32_t)bytes[0]) | ((uint32_t)bytes[1] << 8u) | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static bool g_direct_content_observed;

/* Forward declarations — public inject API from chsc6440.c */
extern void chsc6440_inject_touch(const uint8_t* payload, uint32_t len);
extern void chsc6440_inject_button(uint8_t button_id, uint8_t pressed);
extern void mofeiSimulatorTouchEventWrite(uint8_t type, uint16_t x, uint16_t y);
extern bool mofeiSimulatorPandaDebugWriteRequest(const uint8_t* payload, uint32_t len);
extern bool mofeiSimulatorPandaDebugPollResponse(void);

/* ---------------------------- Host IPC ----------------------------------- */

static void ipc_send_frame(UC8253CState* s, uint8_t channel, uint8_t flags, const uint8_t* payload, uint32_t len) {
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
    qemu_log_mask(LOG_GUEST_ERROR, "uc8253c: ipc header short write\n");
    return;
  }
  if (len > 0 && qemu_chr_fe_write_all(&s->chr, payload, len) != (int)len) {
    qemu_log_mask(LOG_GUEST_ERROR, "uc8253c: ipc payload short write\n");
  }
}

static void uc8253c_send_control(const uint8_t* payload, uint32_t len) {
  if (g_uc8253c != NULL && len <= LILYGO_I2C_CONTROL_MAX_PAYLOAD) {
    ipc_send_frame(g_uc8253c, IPC_CHANNEL_CONTROL, 0, payload, len);
  }
}

static void push_framebuffer(UC8253CState* s, bool partial) {
  ipc_send_frame(s, IPC_CHANNEL_FRAMEBUFFER, partial ? IPC_FLAG_FB_PARTIAL : IPC_FLAG_FB_FULL, s->fb, EPD_FB_BYTES);
  {
    static int push_count = 0;
    push_count++;
    if (push_count <= 5 || (push_count % 10) == 0) {
      int non_white = 0;
      for (int i = 0; i < EPD_FB_BYTES; i++) {
        if (s->fb[i] != 0xFF) non_white++;
      }
      fprintf(stderr, "[UC8253C] push_framebuffer #%d (partial=%d) non_white=%d/%d\n", push_count, partial, non_white,
              EPD_FB_BYTES);
    }
  }
}

/* ----------------------- Inbound IPC (chr_read) -------------------------- */
/* UC8253C owns the chardev and dispatches inbound channels to CHSC6440. */

static int uc8253c_chr_can_read(void* opaque) {
  (void)opaque;
  return MAX_INBOUND_FRAME_PAYLOAD + HEADER_LEN;
}

static uint32_t uc8253c_inbound_payload_cap(uint8_t channel) {
  return channel == IPC_CHANNEL_DEBUG ? MAX_DEBUG_INBOUND_PAYLOAD : MAX_INBOUND_PAYLOAD;
}

static void uc8253c_reset_inbound_frame(UC8253CState* s) {
  s->inbound_header_filled = 0;
  s->inbound_payload_target = 0;
  s->inbound_payload_filled = 0;
}

static void uc8253c_dispatch_inbound_frame(UC8253CState* s) {
  uint8_t channel = s->inbound_header[0];
  if (channel == IPC_CHANNEL_TOUCH_EVENT) {
    chsc6440_inject_touch(s->inbound_payload, s->inbound_payload_target);
  } else if (channel == IPC_CHANNEL_BUTTON_EVENT) {
    if (s->inbound_payload_target >= 2) {
      chsc6440_inject_button(s->inbound_payload[0], s->inbound_payload[1]);
    }
  } else if (channel == IPC_CHANNEL_CONTROL) {
    lilygo_peripheral_control_dispatch(s->inbound_payload, s->inbound_payload_target);
  } else if (channel == IPC_CHANNEL_SYNTHETIC_TOUCH_EVENT) {
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
  }
}

static void uc8253c_chr_read(void* opaque, const uint8_t* buf, int size) {
  UC8253CState* s = opaque;
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
        uint32_t cap = uc8253c_inbound_payload_cap(channel);
        if (s->inbound_payload_target > cap) {
          qemu_log_mask(LOG_GUEST_ERROR, "uc8253c: inbound channel 0x%02x payload %u exceeds cap %u\n", channel,
                        s->inbound_payload_target, cap);
          s->inbound_discard_remaining = s->inbound_payload_target;
          uc8253c_reset_inbound_frame(s);
          continue;
        }
        s->inbound_payload_filled = 0;
        if (s->inbound_payload_target == 0) {
          uc8253c_dispatch_inbound_frame(s);
          uc8253c_reset_inbound_frame(s);
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
        uc8253c_dispatch_inbound_frame(s);
        uc8253c_reset_inbound_frame(s);
      }
    }
  }
}

static void uc8253c_chr_event(void* opaque, QEMUChrEvent event) {
  UC8253CState* s = opaque;
  if (event == CHR_EVENT_OPENED) {
    uc8253c_reset_inbound_frame(s);
    s->inbound_discard_remaining = 0;
  }
}

/* ------------------------------ BUSY ------------------------------------- */
/* UC8253C BUSY is active-low (low = busy). Guest READBUSY waits for high. */

static void busy_set(UC8253CState* s, bool busy) {
  s->busy_high = !busy;
  qemu_set_irq(s->busy_out, busy ? 0 : 1);
}

static void busy_timer_expired(void* opaque) {
  UC8253CState* s = opaque;
  busy_set(s, false);
}

/* ----------------------- Command dispatch -------------------------------- */

static void reset_state(UC8253CState* s) {
  s->mode = PARSE_IDLE;
  s->current_cmd = 0;
  s->arg_index = 0;
  s->arg_expected = 0;
  s->lut_consume_remaining = 0;
  s->deep_sleep = false;
  s->ram_write_index = 0;
  s->configured_source_pixels = 0;
  s->configured_gate_pixels = 0;
  busy_set(s, false);
}

static SampleGuestFrameResult sample_guest_framebuffer(bool allow_low_content_frame);

static void trigger_refresh(UC8253CState* s) {
  /* Pull the authoritative frame from guest memory, then drive BUSY for the
   * refresh duration so the firmware's wait completes. */
  const SampleGuestFrameResult sample_result = sample_guest_framebuffer(true);
  if (sample_result == SAMPLE_GUEST_FRAME_CONTENT) {
    g_direct_content_observed = true;
    push_framebuffer(s, false);
  } else if (sample_result == SAMPLE_GUEST_FRAME_WHITE) {
    push_framebuffer(s, false);
  }
  busy_set(s, true);
  timer_mod(s->busy_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + REFRESH_MS_FULL);
}

static void execute_command_no_args(UC8253CState* s, uint8_t cmd) {
  switch (cmd) {
    case CMD_DISPLAY_REFRESH:
      trigger_refresh(s);
      break;
    case CMD_POWER_ON:
      busy_set(s, true);
      timer_mod(s->busy_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
      break;
    case CMD_POWER_OFF:
    case CMD_TEMP_MEASURE:
    case CMD_TEMP_SENSOR_CALIB:
      /* no-op / consume */
      break;
    default:
      qemu_log_mask(LOG_UNIMP, "uc8253c: unhandled no-arg cmd 0x%02X\n", cmd);
      break;
  }
}

static int command_arg_count(uint8_t cmd) {
  switch (cmd) {
    case CMD_PANEL_SETTING:
      return 2;
    case CMD_POWER_SETTING:
      return 5;
    case CMD_BOOSTER_SOFT_START:
      return 3;
    case CMD_DEEP_SLEEP:
      return 1;
    case CMD_AUTO_SEQUENCE:
      return 1;
    case CMD_PLL_CONTROL:
      return 1;
    case CMD_TEMP_SENSOR_SELECT:
      return 1;
    case CMD_VCOM_DATA_INTERVAL:
      return 1;
    case CMD_VCOM_DC_SETTING:
      return 1;
    case CMD_RESOLUTION_SETTING:
      return 3;
    default:
      return 0;
  }
}

static void apply_collected_args(UC8253CState* s) {
  switch (s->current_cmd) {
    case CMD_DEEP_SLEEP:
      if (s->arg_buf[0] == 0xA5) {
        s->deep_sleep = true;
      }
      break;
    case CMD_AUTO_SEQUENCE:
      /* 0xA5 = PON + DRF + POF auto sequence → behaves like a refresh. */
      if (s->arg_buf[0] == 0xA5) {
        trigger_refresh(s);
      }
      break;
    case CMD_PANEL_SETTING:
    case CMD_POWER_SETTING:
    case CMD_BOOSTER_SOFT_START:
    case CMD_PLL_CONTROL:
    case CMD_TEMP_SENSOR_SELECT:
    case CMD_VCOM_DATA_INTERVAL:
    case CMD_VCOM_DC_SETTING:
      /* Skip-and-consume per Hybrid FSM design (init params). */
      break;
    case CMD_RESOLUTION_SETTING:
      s->configured_source_pixels = s->arg_buf[0];
      s->configured_gate_pixels = ((uint16_t)s->arg_buf[1] << 8) | s->arg_buf[2];
      fprintf(stderr, "[UC8253C] resolution source=%u gate=%u\n", s->configured_source_pixels,
              s->configured_gate_pixels);
      break;
    default:
      qemu_log_mask(LOG_UNIMP, "uc8253c: unhandled cmd 0x%02X with %d args\n", s->current_cmd, s->arg_index);
      break;
  }
}

/* ----------------------- SSI transfer hook ------------------------------- */

static uint32_t uc8253c_transfer(SSIPeripheral* dev, uint32_t value) {
  UC8253CState* s = UC8253C(dev);
  uint8_t byte = value & 0xFF;

  if (s->reset_low) {
    return 0;
  }

  if (!s->dc_high) {
    /* Command byte. Aborts any in-progress data write. */
    s->current_cmd = byte;
    s->arg_index = 0;
    s->arg_expected = command_arg_count(byte);

    if (byte == CMD_DATA_START_TRANSMISSION_1 || byte == CMD_DATA_START_TRANSMISSION_2) {
      s->mode = PARSE_RAM_WRITE;
      s->ram_write_index = 0;
    } else if (byte >= CMD_LUT_VCOM && byte <= CMD_LUT_BB) {
      /* LUT registers: consume until the next command. Generous budget. */
      s->mode = PARSE_LUT_CONSUME;
      s->lut_consume_remaining = 256;
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
    case PARSE_RAM_WRITE:
      /* DTM2 (0x13) carries the image; DTM1 (0x10) is the previous frame.
       * Only DTM2 updates fb[]. The authoritative push uses direct-guest-fb,
       * but we keep fb[] in sync as a fallback. */
      if (s->current_cmd == CMD_DATA_START_TRANSMISSION_2 && s->ram_write_index < EPD_FB_BYTES) {
        const uint32_t gate = s->ram_write_index / EPD_SOURCE_ROW_STRIDE_BYTES;
        const uint32_t source_byte = s->ram_write_index % EPD_SOURCE_ROW_STRIDE_BYTES;
        if (gate < EPD_GATE_PIXELS) {
          for (uint32_t bit = 0; bit < 8; ++bit) {
            const uint32_t source = source_byte * 8 + bit;
            if (source >= EPD_SOURCE_PIXELS) {
              continue;
            }
            const uint32_t output_x = gate;
            const uint32_t output_y = source;
            const uint32_t output_byte = output_y * EPD_OUTPUT_ROW_STRIDE_BYTES + (output_x / 8);
            const uint8_t output_mask = (uint8_t)(0x80u >> (output_x & 7));
            if ((byte & (0x80u >> bit)) != 0) {
              s->fb[output_byte] |= output_mask;
            } else {
              s->fb[output_byte] &= (uint8_t)(~output_mask);
            }
          }
        }
      }
      s->ram_write_index++;
      break;
    case PARSE_LUT_CONSUME:
      if (s->lut_consume_remaining > 0) {
        s->lut_consume_remaining--;
      }
      break;
    case PARSE_IDLE:
    default:
      qemu_log_mask(LOG_GUEST_ERROR, "uc8253c: unexpected data 0x%02X in IDLE\n", byte);
      break;
  }
  return 0;
}

/* ----------------------- GPIO sinks (D/C#, RES#) ------------------------- */

static void dc_irq_handler(void* opaque, int n, int level) {
  UC8253CState* s = opaque;
  s->dc_high = (level != 0);
}

static void reset_irq_handler(void* opaque, int n, int level) {
  UC8253CState* s = opaque;
  bool was_low = s->reset_low;
  s->reset_low = (level == 0);
  if (was_low && !s->reset_low) {
    /* Rising edge — perform HW reset. */
    memset(s->fb, 0xFF, EPD_FB_BYTES);
    reset_state(s);
  }
}

/* ----------------------- Direct guest framebuffer ------------------------ */

static SampleGuestFrameResult sample_guest_framebuffer(bool allow_low_content_frame) {
  if (!g_uc8253c) {
    return SAMPLE_GUEST_FRAME_NONE;
  }
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

      int non_white = 0;
      for (int i = 0; i < EPD_FB_BYTES; i++) {
        if (guest_fb[i] != 0xFF) non_white++;
      }

      const bool has_content = non_white >= MIN_DIRECT_CONTENT_BYTES;
      if (has_content || allow_low_content_frame) {
        /* Direct-read path samples the firmware's simulator framebuffer, which
         * is already logical 416x240. */
        memcpy(g_uc8253c->fb, guest_fb, EPD_FB_BYTES);
        g_free(guest_fb);
        return has_content ? SAMPLE_GUEST_FRAME_CONTENT : SAMPLE_GUEST_FRAME_WHITE;
      }
      g_free(guest_fb);
    }
  }
  return SAMPLE_GUEST_FRAME_NONE;
}

/* ----------------------- Public API (exc_helper.c) ----------------------- */

void uc8253c_spi_byte(uint8_t byte) {
  if (!g_uc8253c) {
    return;
  }
  uc8253c_transfer(SSI_PERIPHERAL(g_uc8253c), byte);
}

void uc8253c_spi_bytes(const uint8_t* data, uint32_t len) {
  if (!g_uc8253c) {
    return;
  }
  for (uint32_t i = 0; i < len; i++) {
    uc8253c_transfer(SSI_PERIPHERAL(g_uc8253c), data[i]);
  }
}

void uc8253c_set_dc_pin(bool high) {
  if (g_uc8253c) {
    g_uc8253c->dc_high = high;
  }
}

void uc8253c_set_fb_guest_addr(uint32_t addr) { g_fb_guest_addr = addr; }
void uc8253c_set_fb_data_guest_addr(uint32_t addr) { g_fb_data_guest_addr = addr; }

void uc8253c_emit_debug_frame(const uint8_t* payload, uint32_t len) {
  if (!g_uc8253c) {
    return;
  }
  ipc_send_frame(g_uc8253c, IPC_CHANNEL_DEBUG, 0, payload, len);
}

void uc8253c_publish_injected_framebuffer(void) {
  if (!g_uc8253c) {
    return;
  }
  const SampleGuestFrameResult sample_result = sample_guest_framebuffer(true);
  if (sample_result == SAMPLE_GUEST_FRAME_CONTENT) {
    g_direct_content_observed = true;
    push_framebuffer(g_uc8253c, false);
  } else if (sample_result == SAMPLE_GUEST_FRAME_WHITE) {
    push_framebuffer(g_uc8253c, false);
  }
}

void uc8253c_inject_and_push_framebuffer(void) {
  if (!g_uc8253c) {
    return;
  }
  push_framebuffer(g_uc8253c, false);
}

/* ----------------------- QEMU plumbing ----------------------------------- */

static const VMStateDescription vmstate_uc8253c = {
    .name = TYPE_UC8253C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields =
        (VMStateField[]){VMSTATE_SSI_PERIPHERAL(parent_obj, UC8253CState), VMSTATE_BOOL(dc_high, UC8253CState),
                         VMSTATE_BOOL(reset_low, UC8253CState), VMSTATE_BOOL(busy_high, UC8253CState),
                         VMSTATE_UINT8(mode, UC8253CState), VMSTATE_UINT8(current_cmd, UC8253CState),
                         VMSTATE_UINT8_ARRAY(arg_buf, UC8253CState, 8), VMSTATE_UINT8(arg_index, UC8253CState),
                         VMSTATE_UINT8(arg_expected, UC8253CState), VMSTATE_UINT16(lut_consume_remaining, UC8253CState),
                         VMSTATE_BOOL(deep_sleep, UC8253CState), VMSTATE_UINT32(ram_write_index, UC8253CState),
                         VMSTATE_UINT8_ARRAY(fb, UC8253CState, EPD_FB_BYTES), VMSTATE_END_OF_LIST()}};

static const Property uc8253c_properties[] = {
    DEFINE_PROP_CHR("chardev", UC8253CState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void uc8253c_realize(SSIPeripheral* d, Error** errp) {
  UC8253CState* s = UC8253C(d);
  DeviceState* dev = DEVICE(d);

  g_uc8253c = s;
  lilygo_peripheral_control_set_ipc_sender(uc8253c_send_control);

  qdev_init_gpio_in_named(dev, dc_irq_handler, "dc", 1);
  qdev_init_gpio_in_named(dev, reset_irq_handler, "reset", 1);
  qdev_init_gpio_out_named(dev, &s->busy_out, "busy", 1);

  s->busy_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, busy_timer_expired, s);

  memset(s->fb, 0xFF, EPD_FB_BYTES);
  reset_state(s);

  s->inbound_header_filled = 0;
  s->inbound_payload_target = 0;
  s->inbound_payload_filled = 0;
  s->inbound_discard_remaining = 0;

  if (qemu_chr_fe_backend_connected(&s->chr)) {
    qemu_chr_fe_set_handlers(&s->chr, uc8253c_chr_can_read, uc8253c_chr_read, uc8253c_chr_event, NULL, s, NULL, true);
    fprintf(stderr, "[UC8253C] IPC chardev handlers registered (read+write)\n");
  }
}

static void uc8253c_class_init(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);
  SSIPeripheralClass* k = SSI_PERIPHERAL_CLASS(klass);

  k->realize = uc8253c_realize;
  k->transfer = uc8253c_transfer;
  dc->vmsd = &vmstate_uc8253c;
  device_class_set_props(dc, uc8253c_properties);
  dc->desc = "UltraChip UC8253C 3.7\" e-ink, logical 416x240 / source-gate 240x416 (s37uc)";
}

static const TypeInfo uc8253c_info = {
    .name = TYPE_UC8253C,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(UC8253CState),
    .class_init = uc8253c_class_init,
};

static void uc8253c_register_types(void) { type_register_static(&uc8253c_info); }

type_init(uc8253c_register_types)
