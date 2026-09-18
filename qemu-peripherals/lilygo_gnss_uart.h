#ifndef HW_CHAR_LILYGO_GNSS_UART_H
#define HW_CHAR_LILYGO_GNSS_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LILYGO_GNSS_MAX_EMISSION 384
#define LILYGO_GNSS_TRACE_CAPACITY 32

typedef enum LilygoGnssVariant {
  LILYGO_GNSS_VARIANT_L76K = 0,
  LILYGO_GNSS_VARIANT_UBLOX_M10 = 1,
} LilygoGnssVariant;

typedef enum LilygoGnssFixture {
  LILYGO_GNSS_FIXTURE_FIXED = 0,
  LILYGO_GNSS_FIXTURE_NO_FIX = 1,
  LILYGO_GNSS_FIXTURE_SILENT = 2,
  LILYGO_GNSS_FIXTURE_MALFORMED = 3,
  LILYGO_GNSS_FIXTURE_STALE = 4,
} LilygoGnssFixture;

typedef enum LilygoGnssTraceEvent {
  LILYGO_GNSS_TRACE_POWER = 0,
  LILYGO_GNSS_TRACE_PROBE = 1,
  LILYGO_GNSS_TRACE_EMIT = 2,
  LILYGO_GNSS_TRACE_SUPPRESS = 3,
  LILYGO_GNSS_TRACE_RECOVER = 4,
} LilygoGnssTraceEvent;

typedef struct LilygoGnssTrace {
  uint64_t sequence;
  uint64_t time_ms;
  LilygoGnssTraceEvent event;
  LilygoGnssFixture fixture;
  uint16_t bytes;
  bool powered;
} LilygoGnssTrace;

typedef void (*LilygoGnssEmitFn)(void* opaque, const uint8_t* bytes, size_t length);

typedef struct LilygoGnssUartModel {
  LilygoGnssVariant variant;
  LilygoGnssFixture fixture;
  LilygoGnssEmitFn emit;
  void* emit_opaque;
  uint64_t now_ms;
  uint64_t next_emission_ms;
  uint64_t trace_sequence;
  uint32_t emission_count;
  uint8_t probe_window[16];
  size_t probe_length;
  LilygoGnssTrace traces[LILYGO_GNSS_TRACE_CAPACITY];
  size_t trace_count;
  size_t trace_cursor;
  bool powered;
  bool stale_emitted;
} LilygoGnssUartModel;

void lilygo_gnss_uart_init(LilygoGnssUartModel* model, LilygoGnssVariant variant, LilygoGnssEmitFn emit, void* opaque);
void lilygo_gnss_uart_set_power(LilygoGnssUartModel* model, bool powered, uint64_t now_ms);
void lilygo_gnss_uart_set_fixture(LilygoGnssUartModel* model, LilygoGnssFixture fixture, uint64_t now_ms);
void lilygo_gnss_uart_receive_guest_bytes(LilygoGnssUartModel* model, const uint8_t* bytes, size_t length);
void lilygo_gnss_uart_advance_time(LilygoGnssUartModel* model, uint64_t now_ms);
size_t lilygo_gnss_uart_copy_traces(const LilygoGnssUartModel* model, LilygoGnssTrace* out, size_t capacity);

#endif
