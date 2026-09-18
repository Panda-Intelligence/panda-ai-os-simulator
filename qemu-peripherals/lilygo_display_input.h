#ifndef HW_I2C_LILYGO_DISPLAY_INPUT_H
#define HW_I2C_LILYGO_DISPLAY_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TYPE_LILYGO_GT911 "lilygo-gt911"
#define TYPE_LILYGO_PCA9535 "lilygo-pca9535"
#define TYPE_LILYGO_TPS65185 "lilygo-tps65185"
#define TYPE_LILYGO_FRONTLIGHT_OBSERVER "lilygo-frontlight-observer"

#define LILYGO_GT911_ADDRESS 0x5D
#define LILYGO_PCA9535_ADDRESS 0x20
#define LILYGO_TPS65185_ADDRESS 0x68

#define LILYGO_GT911_WIDTH 540
#define LILYGO_GT911_HEIGHT 960
#define LILYGO_GT911_CONFIG_BASE 0x8047
#define LILYGO_GT911_CONFIG_SIZE 184
#define LILYGO_GT911_COMMAND_REGISTER 0x8040
#define LILYGO_GT911_PRODUCT_ID_REGISTER 0x8140
#define LILYGO_GT911_STATUS_REGISTER 0x814E
#define LILYGO_GT911_CONTACT_REGISTER 0x814F
#define LILYGO_GT911_CONFIG_CHECKSUM_REGISTER 0x80FF
#define LILYGO_GT911_CONFIG_FRESH_REGISTER 0x8100

#define LILYGO_GT911_ACTION_DOWN 1
#define LILYGO_GT911_ACTION_MOVE 2
#define LILYGO_GT911_ACTION_UP 3

#define LILYGO_PCA9535_FUNCTION_PORT 1
#define LILYGO_PCA9535_FUNCTION_BIT 2
#define LILYGO_PCA9535_RADIO_PORT 0
#define LILYGO_PCA9535_RADIO_BIT 0
#define LILYGO_PCA9535_GNSS_PORT LILYGO_PCA9535_RADIO_PORT
#define LILYGO_PCA9535_GNSS_BIT LILYGO_PCA9535_RADIO_BIT
#define LILYGO_PCA9535_POWER_GOOD_BIT 6
#define LILYGO_PCA9535_INTERRUPT_BIT 7

#define LILYGO_TPS65185_ENABLE_REGISTER 0x01
#define LILYGO_TPS65185_VCOM_LOW_REGISTER 0x03
#define LILYGO_TPS65185_VCOM_HIGH_REGISTER 0x04
#define LILYGO_TPS65185_POWER_GOOD_REGISTER 0x0F
#define LILYGO_TPS65185_ENABLE_MASK 0x3F
#define LILYGO_TPS65185_POWER_GOOD_MASK 0xFA
#define LILYGO_TPS65185_READY_DELAY_NS 3000000ULL

#define LILYGO_FRONTLIGHT_GPIO 11
#define LILYGO_FRONTLIGHT_CHANNEL 0
#define LILYGO_FRONTLIGHT_FREQUENCY_HZ 5000
#define LILYGO_FRONTLIGHT_RESOLUTION_BITS 10

typedef enum LilygoModelResult {
  LILYGO_MODEL_OK = 0,
  LILYGO_MODEL_INVALID = 1,
  LILYGO_MODEL_NACK = 2,
} LilygoModelResult;

typedef struct LilygoGt911Model {
  uint8_t config[LILYGO_GT911_CONFIG_SIZE];
  uint8_t product_id[4];
  uint8_t contact[8];
  uint16_t register_pointer;
  uint32_t contact_generation;
  uint8_t status;
  uint8_t config_checksum;
  uint8_t config_fresh;
  uint8_t pointer_bytes;
  bool int_low;
  bool sleeping;
  bool contact_active;
  bool release_pending;
  bool nack_once;
  bool nack_latched;
} LilygoGt911Model;

typedef struct LilygoPca9535Model {
  uint8_t external[2];
  uint8_t output[2];
  uint8_t polarity[2];
  uint8_t config[2];
  uint8_t register_pointer;
  bool expecting_pointer;
  bool function_press_unread;
  bool function_release_pending;
  bool nack_once;
  bool nack_latched;
} LilygoPca9535Model;

typedef struct LilygoTps65185Model {
  uint8_t enable;
  uint8_t vcom_low;
  uint8_t vcom_high;
  uint8_t power_good;
  uint8_t register_pointer;
  uint64_t ready_deadline_ns;
  uint32_t observational_read_sequence;
  uint8_t last_observed_register;
  uint8_t last_observed_value;
  bool expecting_pointer;
  bool ready_pending;
  bool ready;
  bool hold_not_ready;
  bool nack_once;
  bool nack_latched;
} LilygoTps65185Model;

typedef struct LilygoFrontlightModel {
  uint32_t frequency_hz;
  uint32_t duty;
  uint32_t sequence;
  uint8_t resolution_bits;
  uint8_t channel;
  uint8_t gpio;
  uint8_t requested_brightness;
  bool configured;
} LilygoFrontlightModel;

void lilygo_gt911_model_reset(LilygoGt911Model* model);
LilygoModelResult lilygo_gt911_model_inject(LilygoGt911Model* model, uint8_t action, uint16_t x, uint16_t y,
                                            uint8_t finger_id);
uint8_t lilygo_gt911_model_read(const LilygoGt911Model* model, uint16_t reg);
LilygoModelResult lilygo_gt911_model_write(LilygoGt911Model* model, uint16_t reg, uint8_t value);
bool lilygo_gt911_model_consume_nack(LilygoGt911Model* model);
void lilygo_gt911_model_set_nack(LilygoGt911Model* model, bool one_shot);

void lilygo_pca9535_model_reset(LilygoPca9535Model* model);
uint8_t lilygo_pca9535_model_read(const LilygoPca9535Model* model, uint8_t reg);
LilygoModelResult lilygo_pca9535_model_write(LilygoPca9535Model* model, uint8_t reg, uint8_t value);
LilygoModelResult lilygo_pca9535_model_set_external(LilygoPca9535Model* model, uint8_t port, uint8_t bit, bool high);
LilygoModelResult lilygo_pca9535_model_set_function_pressed(LilygoPca9535Model* model, bool pressed);
void lilygo_pca9535_model_ack_function_read(LilygoPca9535Model* model);
bool lilygo_pca9535_model_output_high(const LilygoPca9535Model* model, uint8_t port, uint8_t bit);
bool lilygo_pca9535_model_consume_nack(LilygoPca9535Model* model);
void lilygo_pca9535_model_set_nack(LilygoPca9535Model* model, bool one_shot);

void lilygo_tps65185_model_reset(LilygoTps65185Model* model, LilygoPca9535Model* pca);
uint8_t lilygo_tps65185_model_read(LilygoTps65185Model* model, LilygoPca9535Model* pca, uint8_t reg, uint64_t now_ns);
LilygoModelResult lilygo_tps65185_model_write(LilygoTps65185Model* model, LilygoPca9535Model* pca, uint8_t reg,
                                              uint8_t value, uint64_t now_ns);
void lilygo_tps65185_model_sync(LilygoTps65185Model* model, LilygoPca9535Model* pca, uint64_t now_ns);
void lilygo_tps65185_model_set_hold_not_ready(LilygoTps65185Model* model, LilygoPca9535Model* pca, bool hold,
                                              uint64_t now_ns);
bool lilygo_tps65185_model_consume_nack(LilygoTps65185Model* model);
void lilygo_tps65185_model_set_nack(LilygoTps65185Model* model, bool one_shot);
uint32_t lilygo_tps65185_model_observational_read_sequence(const LilygoTps65185Model* model);

void lilygo_frontlight_model_reset(LilygoFrontlightModel* model);
LilygoModelResult lilygo_frontlight_model_configure(LilygoFrontlightModel* model, uint32_t frequency_hz,
                                                    uint8_t resolution_bits, uint8_t channel, uint8_t gpio);
LilygoModelResult lilygo_frontlight_model_set_brightness(LilygoFrontlightModel* model, uint8_t brightness);
uint32_t lilygo_frontlight_duty_for_brightness(uint8_t brightness);

#ifndef LILYGO_DISPLAY_INPUT_MODEL_STANDALONE
bool lilygo_gt911_inject_contact(uint8_t action, uint16_t x, uint16_t y, uint8_t finger_id);
bool lilygo_pca9535_set_external_input(uint8_t port, uint8_t bit, bool high);
bool lilygo_pca9535_inject_function(bool pressed);
void lilygo_pca9535_release_external_inputs(void);
void lilygo_pca9535_set_radio_power_sink(void (*sink)(bool powered));
void lilygo_pca9535_set_gnss_power_sink(void (*sink)(bool powered));
void lilygo_tps65185_set_hold_not_ready(bool hold);
uint8_t lilygo_tps65185_control(uint8_t operation, const uint8_t* payload, uint8_t payload_len);
void lilygo_frontlight_observe_config(uint32_t frequency_hz, uint8_t resolution_bits, uint8_t channel, uint8_t gpio);
void lilygo_frontlight_observe_brightness(uint8_t brightness);
const LilygoFrontlightModel* lilygo_frontlight_observer_state(void);
#endif

#endif
