#ifndef LILYGO_GNSS_UART_MODEL_STANDALONE
#include "qemu/osdep.h"
#include "hw/char/lilygo_gnss_uart.h"
#else
#include "lilygo_gnss_uart.h"
#endif

#include <string.h>

static const uint8_t kUbloxMonVerPoll[] = {0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34};
static const char kL76Poll[] = "$PCAS06,0*1B\r\n";
static const char kL76Identity[] = "$GPTXT,01,01,02,L76K GNSS FW 1.0*5C\r\n";
static const char kFixedNmea[] =
    "$GPGGA,120000.00,3113.8600,N,12128.9800,E,1,08,0.9,12.3,M,0.0,M,,*69\r\n"
    "$GPRMC,120000.00,A,3113.8600,N,12128.9800,E,0.50,90.0,140726,,,A*50\r\n"
    "$GPGSA,A,3,01,02,03,04,05,06,07,08,,,,,1.5,0.9,1.2*34\r\n"
    "$GPGSV,1,1,08,01,45,090,40,02,40,120,38,03,35,150,36,04,30,180,34*7F\r\n";
static const char kNoFixNmea[] =
    "$GPGGA,120000.00,,,,,0,00,99.9,,,,,,*5C\r\n"
    "$GPRMC,120000.00,V,,,,,,,140726,,,N*78\r\n";
static const char kMalformedNmea[] = "$GPGGA,120000.00,INVALID*00\r\n";

static void trace(LilygoGnssUartModel* model, LilygoGnssTraceEvent event, size_t bytes) {
  LilygoGnssTrace* item = &model->traces[model->trace_cursor];
  *item = (LilygoGnssTrace){.sequence = ++model->trace_sequence,
                            .time_ms = model->now_ms,
                            .event = event,
                            .fixture = model->fixture,
                            .bytes = (uint16_t)(bytes > UINT16_MAX ? UINT16_MAX : bytes),
                            .powered = model->powered};
  model->trace_cursor = (model->trace_cursor + 1u) % LILYGO_GNSS_TRACE_CAPACITY;
  if (model->trace_count < LILYGO_GNSS_TRACE_CAPACITY) model->trace_count++;
}

static void emit(LilygoGnssUartModel* model, const uint8_t* bytes, size_t length, LilygoGnssTraceEvent event) {
  if (!model->powered || model->emit == NULL || bytes == NULL || length == 0) {
    trace(model, LILYGO_GNSS_TRACE_SUPPRESS, 0);
    return;
  }
  if (length > LILYGO_GNSS_MAX_EMISSION) length = LILYGO_GNSS_MAX_EMISSION;
  model->emit(model->emit_opaque, bytes, length);
  model->emission_count++;
  trace(model, event, length);
}

static bool suffix_matches(const LilygoGnssUartModel* model, const uint8_t* expected, size_t length) {
  return model->probe_length >= length &&
         memcmp(model->probe_window + model->probe_length - length, expected, length) == 0;
}

static void emit_ublox_identity(LilygoGnssUartModel* model) {
  uint8_t response[48] = {0xB5, 0x62, 0x0A, 0x04, 40, 0};
  memcpy(response + 6, "EXT CORE 1.00", 13);
  memcpy(response + 36, "M10", 3);
  uint8_t checksum_a = 0;
  uint8_t checksum_b = 0;
  for (size_t index = 2; index < 46; ++index) {
    checksum_a = (uint8_t)(checksum_a + response[index]);
    checksum_b = (uint8_t)(checksum_b + checksum_a);
  }
  response[46] = checksum_a;
  response[47] = checksum_b;
  emit(model, response, sizeof(response), LILYGO_GNSS_TRACE_PROBE);
}

void lilygo_gnss_uart_init(LilygoGnssUartModel* model, LilygoGnssVariant variant, LilygoGnssEmitFn emit_fn,
                           void* opaque) {
  if (model == NULL) return;
  memset(model, 0, sizeof(*model));
  model->variant = variant;
  model->fixture = LILYGO_GNSS_FIXTURE_FIXED;
  model->emit = emit_fn;
  model->emit_opaque = opaque;
}

void lilygo_gnss_uart_set_power(LilygoGnssUartModel* model, bool powered, uint64_t now_ms) {
  if (model == NULL) return;
  model->now_ms = now_ms;
  if (model->powered == powered) return;
  model->powered = powered;
  model->probe_length = 0;
  model->next_emission_ms = powered ? now_ms : 0;
  model->stale_emitted = false;
  trace(model, LILYGO_GNSS_TRACE_POWER, 0);
}

void lilygo_gnss_uart_set_fixture(LilygoGnssUartModel* model, LilygoGnssFixture fixture, uint64_t now_ms) {
  if (model == NULL || fixture > LILYGO_GNSS_FIXTURE_STALE) return;
  model->now_ms = now_ms;
  const bool recovering = model->fixture != fixture && (model->fixture == LILYGO_GNSS_FIXTURE_SILENT ||
                                                        model->fixture == LILYGO_GNSS_FIXTURE_MALFORMED ||
                                                        model->fixture == LILYGO_GNSS_FIXTURE_STALE);
  model->fixture = fixture;
  model->next_emission_ms = now_ms;
  model->stale_emitted = false;
  if (recovering) trace(model, LILYGO_GNSS_TRACE_RECOVER, 0);
}

void lilygo_gnss_uart_receive_guest_bytes(LilygoGnssUartModel* model, const uint8_t* bytes, size_t length) {
  if (model == NULL || bytes == NULL || length == 0 || !model->powered) return;
  if (model->fixture == LILYGO_GNSS_FIXTURE_SILENT) {
    trace(model, LILYGO_GNSS_TRACE_SUPPRESS, 0);
    return;
  }
  for (size_t index = 0; index < length; ++index) {
    if (model->probe_length == sizeof(model->probe_window)) {
      memmove(model->probe_window, model->probe_window + 1, sizeof(model->probe_window) - 1);
      model->probe_length--;
    }
    model->probe_window[model->probe_length++] = bytes[index];
    if (model->variant == LILYGO_GNSS_VARIANT_UBLOX_M10 &&
        suffix_matches(model, kUbloxMonVerPoll, sizeof(kUbloxMonVerPoll))) {
      emit_ublox_identity(model);
      model->probe_length = 0;
    } else if (model->variant == LILYGO_GNSS_VARIANT_L76K &&
               suffix_matches(model, (const uint8_t*)kL76Poll, sizeof(kL76Poll) - 1)) {
      emit(model, (const uint8_t*)kL76Identity, sizeof(kL76Identity) - 1, LILYGO_GNSS_TRACE_PROBE);
      model->probe_length = 0;
    }
  }
}

void lilygo_gnss_uart_advance_time(LilygoGnssUartModel* model, uint64_t now_ms) {
  if (model == NULL) return;
  model->now_ms = now_ms;
  if (!model->powered || now_ms < model->next_emission_ms) return;
  model->next_emission_ms = now_ms + 1000;
  switch (model->fixture) {
    case LILYGO_GNSS_FIXTURE_FIXED:
      emit(model, (const uint8_t*)kFixedNmea, sizeof(kFixedNmea) - 1, LILYGO_GNSS_TRACE_EMIT);
      break;
    case LILYGO_GNSS_FIXTURE_NO_FIX:
      emit(model, (const uint8_t*)kNoFixNmea, sizeof(kNoFixNmea) - 1, LILYGO_GNSS_TRACE_EMIT);
      break;
    case LILYGO_GNSS_FIXTURE_MALFORMED:
      emit(model, (const uint8_t*)kMalformedNmea, sizeof(kMalformedNmea) - 1, LILYGO_GNSS_TRACE_EMIT);
      break;
    case LILYGO_GNSS_FIXTURE_STALE:
      if (!model->stale_emitted) {
        emit(model, (const uint8_t*)kFixedNmea, sizeof(kFixedNmea) - 1, LILYGO_GNSS_TRACE_EMIT);
        model->stale_emitted = true;
      } else {
        trace(model, LILYGO_GNSS_TRACE_SUPPRESS, 0);
      }
      break;
    case LILYGO_GNSS_FIXTURE_SILENT:
      trace(model, LILYGO_GNSS_TRACE_SUPPRESS, 0);
      break;
  }
}

size_t lilygo_gnss_uart_copy_traces(const LilygoGnssUartModel* model, LilygoGnssTrace* out, size_t capacity) {
  if (model == NULL || out == NULL || capacity == 0) return 0;
  size_t count = model->trace_count < capacity ? model->trace_count : capacity;
  size_t first = model->trace_count == LILYGO_GNSS_TRACE_CAPACITY ? model->trace_cursor : 0;
  if (count < model->trace_count) first = (first + model->trace_count - count) % LILYGO_GNSS_TRACE_CAPACITY;
  for (size_t index = 0; index < count; ++index) {
    out[index] = model->traces[(first + index) % LILYGO_GNSS_TRACE_CAPACITY];
  }
  return count;
}
