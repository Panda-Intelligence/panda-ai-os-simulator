#ifndef HW_SSI_LILYGO_SX1262_H
#define HW_SSI_LILYGO_SX1262_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LILYGO_SX1262_FIFO_SIZE 256
#define LILYGO_SX1262_REGISTER_SIZE 0x1000
#define LILYGO_SX1262_MAX_PACKET_SIZE 255
#define LILYGO_SX1262_CONTROL_DEVICE 5
#define LILYGO_SX1262_BROKER_MAX_PACKET_SIZE 27
#define LILYGO_SX1262_DIO1_GPIO 10
#define LILYGO_SX1262_BUSY_GPIO 47
#define TYPE_LILYGO_SX1262 "lilygo-sx1262"

typedef enum LilygoSx1262ControlState {
  LILYGO_SX1262_CONTROL_SET_ENDPOINT = 1,
  LILYGO_SX1262_CONTROL_SCHEDULE_RECEIVE = 2,
  LILYGO_SX1262_CONTROL_DELIVER_BROKER_PACKET = 3,
} LilygoSx1262ControlState;

typedef enum LilygoSx1262Mode {
  LILYGO_SX1262_MODE_STANDBY_RC,
  LILYGO_SX1262_MODE_STANDBY_XOSC,
  LILYGO_SX1262_MODE_FS,
  LILYGO_SX1262_MODE_TX,
  LILYGO_SX1262_MODE_RX,
  LILYGO_SX1262_MODE_SLEEP,
} LilygoSx1262Mode;

typedef enum LilygoSx1262Fault {
  LILYGO_SX1262_FAULT_NONE,
  LILYGO_SX1262_FAULT_MISSING_DEVICE,
  LILYGO_SX1262_FAULT_STUCK_BUSY,
  LILYGO_SX1262_FAULT_DROP_PACKET,
  LILYGO_SX1262_FAULT_CRC_ERROR,
  LILYGO_SX1262_FAULT_HEADER_ERROR,
  LILYGO_SX1262_FAULT_RX_TIMEOUT,
  LILYGO_SX1262_FAULT_AIR_DISCONNECTED,
} LilygoSx1262Fault;

typedef struct LilygoSx1262Envelope {
  uint64_t endpoint_id;
  uint32_t frequency_hz;
  uint8_t packet_type;
  uint8_t spreading_factor;
  uint8_t bandwidth;
  uint8_t coding_rate;
  uint16_t sync_word;
} LilygoSx1262Envelope;

typedef struct LilygoSx1262Trace {
  uint64_t sequence;
  uint64_t time_us;
  uint8_t opcode;
  uint8_t status;
  uint16_t transaction_length;
  LilygoSx1262Mode mode;
  uint16_t irq;
  bool busy;
  bool dio1;
} LilygoSx1262Trace;

typedef void (*LilygoSx1262TransmitFn)(void* opaque, const LilygoSx1262Envelope* envelope, const uint8_t* payload,
                                       size_t length, uint64_t time_us);
typedef void (*LilygoSx1262TraceFn)(void* opaque, const LilygoSx1262Trace* trace);

typedef struct LilygoSx1262State {
  uint8_t registers[LILYGO_SX1262_REGISTER_SIZE];
  uint8_t fifo[LILYGO_SX1262_FIFO_SIZE];
  uint8_t transaction[LILYGO_SX1262_FIFO_SIZE + 16];
  uint8_t pending_payload[LILYGO_SX1262_MAX_PACKET_SIZE];
  size_t transaction_length;
  size_t pending_length;
  uint64_t now_us;
  uint64_t busy_until_us;
  uint64_t operation_deadline_us;
  uint64_t pending_delivery_us;
  uint64_t trace_sequence;
  uint64_t power_ready_us;
  uint32_t rx_timeout_units;
  uint16_t irq;
  uint16_t irq_mask;
  uint16_t dio1_mask;
  uint16_t device_errors;
  uint16_t sync_word;
  uint8_t opcode;
  uint8_t tx_base;
  uint8_t rx_base;
  uint8_t rx_length;
  uint8_t rx_offset;
  uint8_t payload_length;
  uint8_t packet_type;
  uint8_t spreading_factor;
  uint8_t bandwidth;
  uint8_t coding_rate;
  uint8_t packet_rssi_raw;
  int8_t packet_snr_raw;
  int32_t mode;
  int32_t fault;
  LilygoSx1262Envelope envelope;
  LilygoSx1262TransmitFn transmit;
  LilygoSx1262TraceFn trace;
  void* transmit_opaque;
  void* trace_opaque;
  bool powered;
  bool reset_asserted;
  bool selected;
  bool command_valid;
  bool pending_receive;
} LilygoSx1262State;

void lilygo_sx1262_init(LilygoSx1262State* state);
void lilygo_sx1262_set_power(LilygoSx1262State* state, bool powered, uint64_t now_us, uint32_t settle_ms);
void lilygo_sx1262_set_reset(LilygoSx1262State* state, bool asserted, uint64_t now_us);
void lilygo_sx1262_set_fault(LilygoSx1262State* state, LilygoSx1262Fault fault);
void lilygo_sx1262_set_endpoint(LilygoSx1262State* state, uint64_t endpoint_id);
void lilygo_sx1262_set_transmit_hook(LilygoSx1262State* state, LilygoSx1262TransmitFn callback, void* opaque);
void lilygo_sx1262_set_trace_hook(LilygoSx1262State* state, LilygoSx1262TraceFn callback, void* opaque);
void lilygo_sx1262_advance_time(LilygoSx1262State* state, uint64_t now_us);
void lilygo_sx1262_select(LilygoSx1262State* state, bool selected);
uint8_t lilygo_sx1262_transfer(LilygoSx1262State* state, uint8_t value);
bool lilygo_sx1262_schedule_receive(LilygoSx1262State* state, const uint8_t* payload, size_t length, int16_t rssi_dbm,
                                    int8_t snr_db, uint64_t delivery_us);
bool lilygo_sx1262_envelopes_match(const LilygoSx1262Envelope* receiver, const LilygoSx1262Envelope* sender);
bool lilygo_sx1262_busy(const LilygoSx1262State* state);
bool lilygo_sx1262_dio1(const LilygoSx1262State* state);
uint16_t lilygo_sx1262_irq(const LilygoSx1262State* state);
LilygoSx1262Mode lilygo_sx1262_mode(const LilygoSx1262State* state);

#ifndef LILYGO_SX1262_MODEL_STANDALONE
void lilygo_sx1262_set_board_power(bool powered);
bool lilygo_sx1262_board_busy(void);
bool lilygo_sx1262_board_dio1(void);
uint8_t lilygo_sx1262_control(uint8_t operation, uint8_t flags, const uint8_t* payload, uint8_t len);
#endif

#endif
