#include "qemu/osdep.h"

#include "chardev/char.h"
#include "hw/char/lilygo_gnss_chardev.h"
#include "hw/char/lilygo_gnss_uart.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define TYPE_LILYGO_GNSS_CHARDEV "chardev-lilygo-gnss"

typedef struct LilygoGnssChardev {
  Chardev parent;
  LilygoGnssUartModel model;
  QEMUTimer timer;
  QEMUTimer probe_timer;
  uint8_t pending_probe[LILYGO_GNSS_MAX_EMISSION];
  size_t pending_probe_length;
} LilygoGnssChardev;

DECLARE_INSTANCE_CHECKER(LilygoGnssChardev, LILYGO_GNSS_CHARDEV, TYPE_LILYGO_GNSS_CHARDEV)

static LilygoGnssChardev* g_lilygo_gnss;

static uint64_t virtual_time_ms(void) { return qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL); }

static void gnss_emit(void* opaque, const uint8_t* bytes, size_t length) {
  LilygoGnssChardev* state = opaque;
  if (state == NULL || bytes == NULL || length == 0) return;
  // 探测回复优先于周期 NMEA 投递；否则 48B u-blox 回复和 81B NMEA 会超过 UART 的 128B FIFO。
  const bool ublox_probe = length >= 2 && bytes[0] == 0xB5 && bytes[1] == 0x62;
  const bool l76_probe = length >= 15 && memcmp(bytes, "$GPTXT,01,01,02", 15) == 0;
  if (ublox_probe || l76_probe) {
    state->pending_probe_length = MIN(length, sizeof(state->pending_probe));
    memcpy(state->pending_probe, bytes, state->pending_probe_length);
    timer_mod(&state->probe_timer, virtual_time_ms() + 1);
    return;
  }
  qemu_chr_be_write(CHARDEV(state), bytes, (int)MIN(length, (size_t)INT_MAX));
}

static void gnss_probe_timer(void* opaque) {
  LilygoGnssChardev* state = opaque;
  if (state == NULL || state->pending_probe_length == 0) return;
  const size_t length = state->pending_probe_length;
  state->pending_probe_length = 0;
  qemu_chr_be_write(CHARDEV(state), state->pending_probe, (int)length);
}

static int gnss_chr_write(Chardev* chr, const uint8_t* bytes, int length) {
  LilygoGnssChardev* state = LILYGO_GNSS_CHARDEV(chr);
  if (bytes == NULL || length < 0) return -1;
  lilygo_gnss_uart_receive_guest_bytes(&state->model, bytes, (size_t)length);
  // 优先将探测响应送入 UART FIFO，避免 48B u-blox 身份响应与 81B NMEA 同时入队时截断校验字节。
  if (state->model.probe_length == 0 && state->pending_probe_length == 0)
    lilygo_gnss_uart_advance_time(&state->model, virtual_time_ms());
  return length;
}

static void gnss_timer(void* opaque) {
  LilygoGnssChardev* state = opaque;
  const uint64_t now_ms = virtual_time_ms();
  if (state->pending_probe_length == 0) lilygo_gnss_uart_advance_time(&state->model, now_ms);
  timer_mod(&state->timer, now_ms + 1000);
}

static void gnss_init(Object* obj) {
  LilygoGnssChardev* state = LILYGO_GNSS_CHARDEV(obj);
  lilygo_gnss_uart_init(&state->model, LILYGO_GNSS_VARIANT_L76K, gnss_emit, state);
  timer_init_ms(&state->timer, QEMU_CLOCK_VIRTUAL, gnss_timer, state);
  timer_init_ms(&state->probe_timer, QEMU_CLOCK_VIRTUAL, gnss_probe_timer, state);
  timer_mod(&state->timer, virtual_time_ms() + 1000);
  g_lilygo_gnss = state;
}

static void gnss_finalize(Object* obj) {
  LilygoGnssChardev* state = LILYGO_GNSS_CHARDEV(obj);
  timer_del(&state->timer);
  timer_del(&state->probe_timer);
  if (g_lilygo_gnss == state) g_lilygo_gnss = NULL;
}

static void gnss_class_init(ObjectClass* klass, void* data) {
  ChardevClass* chardev = CHARDEV_CLASS(klass);
  (void)data;
  chardev->chr_write = gnss_chr_write;
}

static const TypeInfo gnss_info = {
    .name = TYPE_LILYGO_GNSS_CHARDEV,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(LilygoGnssChardev),
    .instance_init = gnss_init,
    .instance_finalize = gnss_finalize,
    .class_init = gnss_class_init,
};

static void gnss_register_types(void) { type_register_static(&gnss_info); }

type_init(gnss_register_types)

    Chardev* lilygo_gnss_chardev_create(void) {
  return qemu_chardev_new("lilygo-gnss-uart2", TYPE_LILYGO_GNSS_CHARDEV, NULL, NULL, &error_fatal);
}

void lilygo_gnss_chardev_set_board_power(bool powered) {
  if (g_lilygo_gnss != NULL) lilygo_gnss_uart_set_power(&g_lilygo_gnss->model, powered, virtual_time_ms());
}

uint8_t lilygo_gnss_chardev_control(uint8_t operation, const uint8_t* payload, uint8_t payload_len) {
  if (g_lilygo_gnss == NULL) return 8;
  LilygoGnssUartModel* model = &g_lilygo_gnss->model;
  const uint64_t now_ms = virtual_time_ms();
  switch (operation) {
    case 1:
      if (payload == NULL || payload_len != 3 || payload[0] > LILYGO_GNSS_VARIANT_UBLOX_M10 ||
          payload[1] > LILYGO_GNSS_FIXTURE_STALE || payload[2] > 1)
        return 7;
      {
        const LilygoGnssEmitFn emit = model->emit;
        void* opaque = model->emit_opaque;
        lilygo_gnss_uart_init(model, (LilygoGnssVariant)payload[0], emit, opaque);
        lilygo_gnss_uart_set_fixture(model, (LilygoGnssFixture)payload[1], now_ms);
        lilygo_gnss_uart_set_power(model, payload[2] != 0, now_ms);
      }
      return 0;
    case 2:
      if (payload == NULL || payload_len != 1 || payload[0] > LILYGO_GNSS_FIXTURE_STALE) return 7;
      lilygo_gnss_uart_set_fixture(model, (LilygoGnssFixture)payload[0], now_ms);
      return 0;
    case 3: {
      const LilygoGnssEmitFn emit = model->emit;
      void* opaque = model->emit_opaque;
      lilygo_gnss_uart_init(model, LILYGO_GNSS_VARIANT_L76K, emit, opaque);
    }
      return 0;
    case 4:
      return payload_len == 0 ? 0 : 7;
    default:
      return 3;
  }
}
