#ifndef HW_I2C_LILYGO_BQ27220_H
#define HW_I2C_LILYGO_BQ27220_H

#include <stdbool.h>
#include <stdint.h>

#include "hw/i2c/i2c.h"

#define TYPE_LILYGO_BQ27220 "lilygo-bq27220"
#define LILYGO_BQ27220_ADDRESS 0x55
#define LILYGO_BQ27220_CONTROL_DEVICE 4

#define LILYGO_BQ27220_REG_VOLTAGE 0x08
#define LILYGO_BQ27220_REG_STATE_OF_CHARGE 0x2C
#define LILYGO_BQ27220_RESET_VOLTAGE_MV 3700
#define LILYGO_BQ27220_RESET_STATE_OF_CHARGE 50

#define LILYGO_BQ27220_CONTROL_SET_STATE 1
#define LILYGO_BQ27220_CONTROL_SET_FAULT 2
#define LILYGO_BQ27220_CONTROL_RESET 3
#define LILYGO_BQ27220_CONTROL_FLAG_ONE_SHOT 0x01
#define LILYGO_BQ27220_FAULT_NACK 1

typedef enum LilygoBq27220ControlError {
  LILYGO_BQ27220_CONTROL_OK = 0,
  LILYGO_BQ27220_CONTROL_UNKNOWN_OPERATION = 3,
  LILYGO_BQ27220_CONTROL_MALFORMED = 5,
  LILYGO_BQ27220_CONTROL_UNKNOWN_FIELD = 6,
} LilygoBq27220ControlError;

typedef enum LilygoBq27220TraceDirection {
  LILYGO_BQ27220_TRACE_WRITE = 0,
  LILYGO_BQ27220_TRACE_READ = 1,
} LilygoBq27220TraceDirection;

typedef enum LilygoBq27220TraceResult {
  LILYGO_BQ27220_TRACE_ACK = 0,
  LILYGO_BQ27220_TRACE_NACK = 1,
} LilygoBq27220TraceResult;

typedef struct LilygoBq27220Trace {
  uint32_t sequence;
  uint64_t virtual_time_ns;
  uint8_t bus;
  uint8_t address;
  uint8_t direction;
  uint8_t reg;
  uint8_t length;
  uint8_t data[16];
  uint8_t result;
} LilygoBq27220Trace;

typedef void (*LilygoBq27220TraceSink)(const LilygoBq27220Trace* trace);

/* The machine passes only its LilyGo-gated main bus. A null/non-LilyGo bus
 * leaves the model absent, preserving all other board battery paths. */
I2CSlave* lilygo_bq27220_attach(I2CBus* lilygo_bus);

/* The shared control router validates the common envelope, then forwards the
 * decoded operation, flags, and model payload through this narrow hook. */
LilygoBq27220ControlError lilygo_bq27220_control(uint8_t operation, uint8_t flags, const uint8_t* payload,
                                                 uint8_t payload_len);
void lilygo_bq27220_set_trace_sink(LilygoBq27220TraceSink sink);

#endif
