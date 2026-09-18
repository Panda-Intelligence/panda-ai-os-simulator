#ifndef HW_CHAR_LILYGO_GNSS_CHARDEV_H
#define HW_CHAR_LILYGO_GNSS_CHARDEV_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Chardev Chardev;

Chardev* lilygo_gnss_chardev_create(void);
void lilygo_gnss_chardev_set_board_power(bool powered);
uint8_t lilygo_gnss_chardev_control(uint8_t operation, const uint8_t* payload, uint8_t payload_len);

#endif
