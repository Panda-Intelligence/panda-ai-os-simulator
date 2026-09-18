#ifndef HW_I2C_LILYGO_POWER_RTC_H
#define HW_I2C_LILYGO_POWER_RTC_H

#include <stdbool.h>
#include <stdint.h>

#define TYPE_LILYGO_BQ25896 "lilygo-bq25896"
#define TYPE_LILYGO_PCF8563 "lilygo-pcf8563"

#define LILYGO_BQ25896_ADDRESS 0x6B
#define LILYGO_PCF8563_ADDRESS 0x51

#define LILYGO_BQ25896_REG_STATUS 0x0B
#define LILYGO_BQ25896_REG_FAULT 0x0C
#define LILYGO_PCF8563_REG_STATUS2 0x01
#define LILYGO_PCF8563_REG_SECONDS 0x02
#define LILYGO_PCF8563_REG_YEARS 0x08

#define LILYGO_POWER_RTC_TRACE_DATA_MAX 8

typedef enum LilygoPowerRtcResult {
  LILYGO_POWER_RTC_OK = 0,
  LILYGO_POWER_RTC_INVALID = 1,
  LILYGO_POWER_RTC_NACK = 2,
} LilygoPowerRtcResult;

typedef struct LilygoBq25896Model {
  uint8_t status;
  uint8_t current_fault;
  uint8_t latched_fault;
  uint8_t register_pointer;
  bool fault_latch_pending;
  bool expecting_pointer;
  bool nack_once;
  bool nack_latched;
} LilygoBq25896Model;

typedef struct LilygoPcf8563Model {
  uint8_t status2;
  uint8_t seconds;
  uint8_t minutes;
  uint8_t hours;
  uint8_t days;
  uint8_t weekdays;
  uint8_t months;
  uint8_t years;
  uint8_t register_pointer;
  uint64_t tick_base_ns;
  bool expecting_pointer;
  bool invalid_fixture;
  bool ticking;
  bool nack_once;
  bool nack_latched;
} LilygoPcf8563Model;

typedef struct LilygoPowerRtcTrace {
  uint32_t sequence;
  uint64_t virtual_time_ns;
  uint8_t address;
  uint8_t direction;
  uint8_t reg;
  uint8_t length;
  uint8_t result;
  uint8_t data[LILYGO_POWER_RTC_TRACE_DATA_MAX];
} LilygoPowerRtcTrace;

typedef void (*LilygoPowerRtcTraceSink)(const LilygoPowerRtcTrace* trace);

void lilygo_bq25896_model_reset(LilygoBq25896Model* model);
LilygoPowerRtcResult lilygo_bq25896_model_set_state(LilygoBq25896Model* model, uint8_t status, uint8_t latched_fault,
                                                    uint8_t current_fault);
uint8_t lilygo_bq25896_model_read(LilygoBq25896Model* model, uint8_t reg);
bool lilygo_bq25896_model_consume_nack(LilygoBq25896Model* model);
void lilygo_bq25896_model_set_nack(LilygoBq25896Model* model, bool one_shot);

void lilygo_pcf8563_model_reset(LilygoPcf8563Model* model, uint64_t now_ns);
LilygoPowerRtcResult lilygo_pcf8563_model_set_datetime(LilygoPcf8563Model* model, uint8_t year, uint8_t month,
                                                       uint8_t day, uint8_t weekday, uint8_t hour, uint8_t minute,
                                                       uint8_t second, bool oscillator_stopped, uint64_t now_ns);
LilygoPowerRtcResult lilygo_pcf8563_model_set_invalid(LilygoPcf8563Model* model, const uint8_t registers[8],
                                                      uint64_t now_ns);
void lilygo_pcf8563_model_set_ticking(LilygoPcf8563Model* model, bool ticking, uint64_t now_ns);
uint8_t lilygo_pcf8563_model_read(LilygoPcf8563Model* model, uint8_t reg, uint64_t now_ns);
bool lilygo_pcf8563_model_consume_nack(LilygoPcf8563Model* model);
void lilygo_pcf8563_model_set_nack(LilygoPcf8563Model* model, bool one_shot);

#ifndef LILYGO_POWER_RTC_MODEL_STANDALONE
typedef struct I2CBus I2CBus;
typedef struct I2CSlave I2CSlave;

I2CSlave* lilygo_bq25896_attach(I2CBus* lilygo_bus);
I2CSlave* lilygo_pcf8563_attach(I2CBus* lilygo_bus);
void lilygo_power_rtc_set_trace_sink(LilygoPowerRtcTraceSink sink);
uint8_t lilygo_bq25896_control(uint8_t operation, const uint8_t* payload, uint8_t payload_len);
uint8_t lilygo_pcf8563_control(uint8_t operation, const uint8_t* payload, uint8_t payload_len);
#endif

#endif
