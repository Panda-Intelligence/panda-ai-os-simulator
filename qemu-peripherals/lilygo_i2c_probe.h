#ifndef HW_I2C_LILYGO_I2C_PROBE_H
#define HW_I2C_LILYGO_I2C_PROBE_H

#include "hw/i2c/i2c.h"

#define TYPE_LILYGO_I2C_PROBE "lilygo-i2c-probe"
#define LILYGO_I2C_PROBE_ADDRESS 0x6F
bool lilygo_i2c_probe_control(const uint8_t* payload, uint32_t len);

#endif
