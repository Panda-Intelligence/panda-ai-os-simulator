#include "qemu/osdep.h"

#ifdef LILYGO_SX1262_MODEL_STANDALONE
#include "lilygo_sx1262.h"
#else
#include "hw/ssi/lilygo_sx1262.h"
#endif

#define SX1262_CMD_NOP 0x00
#define SX1262_CMD_CLEAR_IRQ_STATUS 0x02
#define SX1262_CMD_CLEAR_DEVICE_ERRORS 0x07
#define SX1262_CMD_SET_DIO_IRQ_PARAMS 0x08
#define SX1262_CMD_WRITE_REGISTER 0x0D
#define SX1262_CMD_WRITE_BUFFER 0x0E
#define SX1262_CMD_GET_STATS 0x10
#define SX1262_CMD_GET_PACKET_TYPE 0x11
#define SX1262_CMD_GET_IRQ_STATUS 0x12
#define SX1262_CMD_GET_RX_BUFFER_STATUS 0x13
#define SX1262_CMD_GET_PACKET_STATUS 0x14
#define SX1262_CMD_GET_RSSI_INST 0x15
#define SX1262_CMD_GET_DEVICE_ERRORS 0x17
#define SX1262_CMD_READ_REGISTER 0x1D
#define SX1262_CMD_READ_BUFFER 0x1E
#define SX1262_CMD_SET_STANDBY 0x80
#define SX1262_CMD_SET_RX 0x82
#define SX1262_CMD_SET_TX 0x83
#define SX1262_CMD_SET_SLEEP 0x84
#define SX1262_CMD_SET_RF_FREQUENCY 0x86
#define SX1262_CMD_SET_CAD_PARAMS 0x88
#define SX1262_CMD_SET_DIO3_AS_TCXO_CTRL 0x97
#define SX1262_CMD_CALIBRATE_IMAGE 0x98
#define SX1262_CMD_SET_DIO2_AS_RF_SWITCH_CTRL 0x9D
#define SX1262_CMD_STOP_TIMER_ON_PREAMBLE 0x9F
#define SX1262_CMD_SET_LORA_SYMB_NUM_TIMEOUT 0xA0
#define SX1262_CMD_GET_STATUS 0xC0
#define SX1262_CMD_SET_FS 0xC1
#define SX1262_CMD_CALIBRATE 0x89
#define SX1262_CMD_SET_PACKET_TYPE 0x8A
#define SX1262_CMD_SET_MODULATION_PARAMS 0x8B
#define SX1262_CMD_SET_PACKET_PARAMS 0x8C
#define SX1262_CMD_SET_TX_PARAMS 0x8E
#define SX1262_CMD_SET_BUFFER_BASE_ADDRESS 0x8F
#define SX1262_CMD_SET_RX_TX_FALLBACK_MODE 0x93
#define SX1262_CMD_SET_REGULATOR_MODE 0x96
#define SX1262_CMD_SET_PA_CONFIG 0x95

#define SX1262_IRQ_TX_DONE 0x0001
#define SX1262_IRQ_RX_DONE 0x0002
#define SX1262_IRQ_HEADER_VALID 0x0010
#define SX1262_IRQ_HEADER_ERROR 0x0020
#define SX1262_IRQ_CRC_ERROR 0x0040
#define SX1262_IRQ_TIMEOUT 0x0200

#define SX1262_REG_VERSION_STRING 0x0320
#define SX1262_REG_LORA_SYNC_WORD_MSB 0x0740

#define SX1262_STATUS_DATA_AVAILABLE 0x04
#define SX1262_STATUS_CMD_INVALID 0x08
#define SX1262_STATUS_TX_DONE 0x0C
#define SX1262_STATUS_CMD_OK 0x02

#define SX1262_COMMAND_BUSY_US 1
#define SX1262_TX_DURATION_US 1000
#define SX1262_BOARD_GUEST_SETTLE_MS 1500

static uint16_t sx1262_be16(const uint8_t* value) { return ((uint16_t)value[0] << 8) | value[1]; }

static uint32_t sx1262_be24(const uint8_t* value) {
  return ((uint32_t)value[0] << 16) | ((uint32_t)value[1] << 8) | value[2];
}

static uint32_t sx1262_be32(const uint8_t* value) {
  return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) | ((uint32_t)value[2] << 8) | value[3];
}

static uint8_t sx1262_mode_status(const LilygoSx1262State* state) {
  switch (state->mode) {
    case LILYGO_SX1262_MODE_STANDBY_XOSC:
      return 0x30;
    case LILYGO_SX1262_MODE_FS:
      return 0x40;
    case LILYGO_SX1262_MODE_RX:
      return 0x50;
    case LILYGO_SX1262_MODE_TX:
      return 0x60;
    case LILYGO_SX1262_MODE_STANDBY_RC:
    case LILYGO_SX1262_MODE_SLEEP:
    default:
      return 0x20;
  }
}

static bool sx1262_available(const LilygoSx1262State* state) {
  return state->powered && !state->reset_asserted && state->now_us >= state->power_ready_us &&
         state->fault != LILYGO_SX1262_FAULT_MISSING_DEVICE;
}

bool lilygo_sx1262_busy(const LilygoSx1262State* state) {
  return state->fault == LILYGO_SX1262_FAULT_STUCK_BUSY || state->now_us < state->busy_until_us;
}

bool lilygo_sx1262_dio1(const LilygoSx1262State* state) { return (state->irq & state->dio1_mask) != 0; }

uint16_t lilygo_sx1262_irq(const LilygoSx1262State* state) { return state->irq; }

LilygoSx1262Mode lilygo_sx1262_mode(const LilygoSx1262State* state) { return state->mode; }

static uint8_t sx1262_status(const LilygoSx1262State* state) {
  uint8_t command = SX1262_STATUS_CMD_OK;
  if (!state->command_valid) {
    command = SX1262_STATUS_CMD_INVALID;
  } else if (state->irq & SX1262_IRQ_RX_DONE) {
    command = SX1262_STATUS_DATA_AVAILABLE;
  } else if (state->irq & SX1262_IRQ_TX_DONE) {
    command = SX1262_STATUS_TX_DONE;
  }
  return sx1262_mode_status(state) | command;
}

static void sx1262_emit_trace(LilygoSx1262State* state) {
  if (state->trace == NULL) return;
  LilygoSx1262Trace trace = {
      .sequence = ++state->trace_sequence,
      .time_us = state->now_us,
      .opcode = state->opcode,
      .status = sx1262_status(state),
      .transaction_length = state->transaction_length > UINT16_MAX ? UINT16_MAX : state->transaction_length,
      .mode = state->mode,
      .irq = state->irq,
      .busy = lilygo_sx1262_busy(state),
      .dio1 = lilygo_sx1262_dio1(state),
  };
  state->trace(state->trace_opaque, &trace);
}

static void sx1262_reset_protocol(LilygoSx1262State* state) {
  const uint64_t endpoint_id = state->envelope.endpoint_id;
  memset(state->registers, 0, sizeof(state->registers));
  memset(state->fifo, 0, sizeof(state->fifo));
  memset(&state->envelope, 0, sizeof(state->envelope));
  state->envelope.endpoint_id = endpoint_id;
  memcpy(&state->registers[SX1262_REG_VERSION_STRING], "SX1261", 6);
  state->registers[SX1262_REG_LORA_SYNC_WORD_MSB] = 0x14;
  state->registers[SX1262_REG_LORA_SYNC_WORD_MSB + 1] = 0x24;
  state->mode = LILYGO_SX1262_MODE_STANDBY_RC;
  state->fault = LILYGO_SX1262_FAULT_NONE;
  state->irq = 0;
  state->irq_mask = 0;
  state->dio1_mask = 0;
  state->device_errors = 0;
  state->tx_base = 0;
  state->rx_base = 0;
  state->rx_length = 0;
  state->rx_offset = 0;
  state->payload_length = 0;
  state->packet_type = 0x01;
  state->spreading_factor = 0;
  state->bandwidth = 0;
  state->coding_rate = 0;
  state->sync_word = 0x1424;
  state->envelope.packet_type = state->packet_type;
  state->envelope.sync_word = state->sync_word;
  state->packet_rssi_raw = 160;
  state->packet_snr_raw = 0;
  state->busy_until_us = 0;
  state->operation_deadline_us = 0;
  state->pending_receive = false;
  state->pending_length = 0;
  state->selected = false;
  state->transaction_length = 0;
}

void lilygo_sx1262_init(LilygoSx1262State* state) {
  memset(state, 0, sizeof(*state));
  sx1262_reset_protocol(state);
  state->powered = false;
}

void lilygo_sx1262_set_power(LilygoSx1262State* state, bool powered, uint64_t now_us, uint32_t settle_ms) {
  state->now_us = now_us;
  state->powered = powered;
  state->power_ready_us = powered ? now_us + (uint64_t)settle_ms * 1000 : 0;
  sx1262_reset_protocol(state);
}

void lilygo_sx1262_set_reset(LilygoSx1262State* state, bool asserted, uint64_t now_us) {
  state->now_us = now_us;
  if (state->reset_asserted == asserted) return;
  state->reset_asserted = asserted;
  sx1262_reset_protocol(state);
  state->reset_asserted = asserted;
  if (!asserted && state->powered) state->busy_until_us = now_us + SX1262_COMMAND_BUSY_US;
}

void lilygo_sx1262_set_fault(LilygoSx1262State* state, LilygoSx1262Fault fault) { state->fault = fault; }

void lilygo_sx1262_set_endpoint(LilygoSx1262State* state, uint64_t endpoint_id) {
  state->envelope.endpoint_id = endpoint_id;
}

void lilygo_sx1262_set_transmit_hook(LilygoSx1262State* state, LilygoSx1262TransmitFn callback, void* opaque) {
  state->transmit = callback;
  state->transmit_opaque = opaque;
}

void lilygo_sx1262_set_trace_hook(LilygoSx1262State* state, LilygoSx1262TraceFn callback, void* opaque) {
  state->trace = callback;
  state->trace_opaque = opaque;
}

static void sx1262_deliver_receive(LilygoSx1262State* state) {
  size_t length = state->pending_length;
  for (size_t index = 0; index < length; ++index) {
    state->fifo[(uint8_t)(state->rx_base + index)] = state->pending_payload[index];
  }
  state->rx_offset = state->rx_base;
  state->rx_length = (uint8_t)length;
  state->mode = LILYGO_SX1262_MODE_STANDBY_RC;
  state->pending_receive = false;
  state->operation_deadline_us = 0;
  if (state->fault == LILYGO_SX1262_FAULT_CRC_ERROR) {
    state->irq |= SX1262_IRQ_CRC_ERROR;
  } else if (state->fault == LILYGO_SX1262_FAULT_HEADER_ERROR) {
    state->irq |= SX1262_IRQ_HEADER_ERROR;
  } else {
    state->irq |= SX1262_IRQ_RX_DONE | SX1262_IRQ_HEADER_VALID;
  }
  sx1262_emit_trace(state);
}

void lilygo_sx1262_advance_time(LilygoSx1262State* state, uint64_t now_us) {
  state->now_us = now_us;
  if (state->mode == LILYGO_SX1262_MODE_TX && state->operation_deadline_us != 0 &&
      now_us >= state->operation_deadline_us) {
    state->mode = LILYGO_SX1262_MODE_STANDBY_RC;
    state->operation_deadline_us = 0;
    state->irq |= SX1262_IRQ_TX_DONE;
    if (state->transmit != NULL && state->fault != LILYGO_SX1262_FAULT_DROP_PACKET &&
        state->fault != LILYGO_SX1262_FAULT_AIR_DISCONNECTED) {
      size_t length = state->payload_length;
      state->transmit(state->transmit_opaque, &state->envelope, &state->fifo[state->tx_base], length, now_us);
    }
    sx1262_emit_trace(state);
  }
  if (state->mode == LILYGO_SX1262_MODE_RX && state->pending_receive && now_us >= state->pending_delivery_us &&
      state->fault != LILYGO_SX1262_FAULT_RX_TIMEOUT && state->fault != LILYGO_SX1262_FAULT_DROP_PACKET &&
      state->fault != LILYGO_SX1262_FAULT_AIR_DISCONNECTED) {
    sx1262_deliver_receive(state);
  }
  if (state->mode == LILYGO_SX1262_MODE_RX && state->operation_deadline_us != 0 &&
      now_us >= state->operation_deadline_us) {
    state->mode = LILYGO_SX1262_MODE_STANDBY_RC;
    state->operation_deadline_us = 0;
    state->pending_receive = false;
    state->irq |= SX1262_IRQ_TIMEOUT;
    sx1262_emit_trace(state);
  }
}

bool lilygo_sx1262_schedule_receive(LilygoSx1262State* state, const uint8_t* payload, size_t length, int16_t rssi_dbm,
                                    int8_t snr_db, uint64_t delivery_us) {
  if (payload == NULL || length == 0 || length > LILYGO_SX1262_MAX_PACKET_SIZE) return false;
  memcpy(state->pending_payload, payload, length);
  state->pending_length = length;
  state->pending_delivery_us = delivery_us;
  state->packet_rssi_raw = (uint8_t)(rssi_dbm <= -127 ? 254 : (rssi_dbm >= 0 ? 0 : -2 * rssi_dbm));
  int16_t snr_raw = (int16_t)snr_db * 4;
  state->packet_snr_raw = (int8_t)(snr_raw < -128 ? -128 : (snr_raw > 127 ? 127 : snr_raw));
  state->pending_receive = true;
  return true;
}

bool lilygo_sx1262_envelopes_match(const LilygoSx1262Envelope* receiver, const LilygoSx1262Envelope* sender) {
  if (receiver == NULL || sender == NULL || receiver->endpoint_id == sender->endpoint_id) return false;
  return receiver->frequency_hz == sender->frequency_hz && receiver->packet_type == sender->packet_type &&
         receiver->spreading_factor == sender->spreading_factor && receiver->bandwidth == sender->bandwidth &&
         receiver->coding_rate == sender->coding_rate && receiver->sync_word == sender->sync_word;
}

static bool sx1262_known_command(uint8_t opcode) {
  switch (opcode) {
    case SX1262_CMD_NOP:
    case SX1262_CMD_SET_SLEEP:
    case SX1262_CMD_SET_STANDBY:
    case SX1262_CMD_SET_FS:
    case SX1262_CMD_SET_TX:
    case SX1262_CMD_SET_RX:
    case SX1262_CMD_STOP_TIMER_ON_PREAMBLE:
    case SX1262_CMD_SET_REGULATOR_MODE:
    case SX1262_CMD_CALIBRATE:
    case SX1262_CMD_CALIBRATE_IMAGE:
    case SX1262_CMD_SET_PA_CONFIG:
    case SX1262_CMD_SET_RX_TX_FALLBACK_MODE:
    case SX1262_CMD_WRITE_REGISTER:
    case SX1262_CMD_READ_REGISTER:
    case SX1262_CMD_WRITE_BUFFER:
    case SX1262_CMD_READ_BUFFER:
    case SX1262_CMD_SET_DIO_IRQ_PARAMS:
    case SX1262_CMD_GET_IRQ_STATUS:
    case SX1262_CMD_CLEAR_IRQ_STATUS:
    case SX1262_CMD_SET_DIO2_AS_RF_SWITCH_CTRL:
    case SX1262_CMD_SET_DIO3_AS_TCXO_CTRL:
    case SX1262_CMD_SET_RF_FREQUENCY:
    case SX1262_CMD_SET_CAD_PARAMS:
    case SX1262_CMD_SET_PACKET_TYPE:
    case SX1262_CMD_GET_PACKET_TYPE:
    case SX1262_CMD_SET_TX_PARAMS:
    case SX1262_CMD_SET_MODULATION_PARAMS:
    case SX1262_CMD_SET_PACKET_PARAMS:
    case SX1262_CMD_SET_BUFFER_BASE_ADDRESS:
    case SX1262_CMD_SET_LORA_SYMB_NUM_TIMEOUT:
    case SX1262_CMD_GET_STATUS:
    case SX1262_CMD_GET_RSSI_INST:
    case SX1262_CMD_GET_RX_BUFFER_STATUS:
    case SX1262_CMD_GET_PACKET_STATUS:
    case SX1262_CMD_GET_DEVICE_ERRORS:
    case SX1262_CMD_CLEAR_DEVICE_ERRORS:
    case SX1262_CMD_GET_STATS:
      return true;
    default:
      return false;
  }
}

static uint8_t sx1262_read_response(const LilygoSx1262State* state, size_t position) {
  if (position == 1) return sx1262_status(state);
  switch (state->opcode) {
    case SX1262_CMD_READ_REGISTER:
      if (position >= 4 && state->transaction_length >= 3) {
        uint16_t address = sx1262_be16(&state->transaction[1]);
        address = (uint16_t)(address + position - 4);
        return address < LILYGO_SX1262_REGISTER_SIZE ? state->registers[address] : 0;
      }
      break;
    case SX1262_CMD_READ_BUFFER:
      if (position >= 3 && state->transaction_length >= 2)
        return state->fifo[(uint8_t)(state->transaction[1] + position - 3)];
      break;
    case SX1262_CMD_GET_IRQ_STATUS:
      if (position == 2) return state->irq >> 8;
      if (position == 3) return state->irq & 0xFF;
      break;
    case SX1262_CMD_GET_RX_BUFFER_STATUS:
      if (position == 2) return state->rx_length;
      if (position == 3) return state->rx_offset;
      break;
    case SX1262_CMD_GET_PACKET_STATUS:
      if (position == 2) return state->packet_rssi_raw;
      if (position == 3) return (uint8_t)state->packet_snr_raw;
      if (position == 4) return state->packet_rssi_raw;
      break;
    case SX1262_CMD_GET_PACKET_TYPE:
      if (position == 2) return state->packet_type;
      break;
    case SX1262_CMD_GET_DEVICE_ERRORS:
      if (position == 2) return state->device_errors >> 8;
      if (position == 3) return state->device_errors & 0xFF;
      break;
    case SX1262_CMD_GET_RSSI_INST:
      if (position == 2) return state->packet_rssi_raw;
      break;
    default:
      break;
  }
  return 0;
}

static void sx1262_apply_command(LilygoSx1262State* state) {
  const uint8_t* data = &state->transaction[1];
  size_t length = state->transaction_length > 0 ? state->transaction_length - 1 : 0;
  state->busy_until_us = state->now_us + SX1262_COMMAND_BUSY_US;
  switch (state->opcode) {
    case SX1262_CMD_SET_SLEEP:
      state->mode = LILYGO_SX1262_MODE_SLEEP;
      break;
    case SX1262_CMD_SET_STANDBY:
      if (length >= 1) state->mode = data[0] == 1 ? LILYGO_SX1262_MODE_STANDBY_XOSC : LILYGO_SX1262_MODE_STANDBY_RC;
      break;
    case SX1262_CMD_SET_FS:
      state->mode = LILYGO_SX1262_MODE_FS;
      break;
    case SX1262_CMD_SET_TX:
      if (length >= 3) {
        state->mode = LILYGO_SX1262_MODE_TX;
        state->irq &= (uint16_t)~(SX1262_IRQ_TX_DONE | SX1262_IRQ_TIMEOUT);
        state->operation_deadline_us = state->now_us + SX1262_TX_DURATION_US;
      }
      break;
    case SX1262_CMD_SET_RX:
      if (length >= 3) {
        state->mode = LILYGO_SX1262_MODE_RX;
        state->irq &= (uint16_t)~(SX1262_IRQ_RX_DONE | SX1262_IRQ_TIMEOUT | SX1262_IRQ_CRC_ERROR |
                                  SX1262_IRQ_HEADER_ERROR | SX1262_IRQ_HEADER_VALID);
        state->rx_timeout_units = sx1262_be24(data);
        state->operation_deadline_us = state->rx_timeout_units == 0 || state->rx_timeout_units == 0xFFFFFF
                                           ? 0
                                           : state->now_us + (uint64_t)state->rx_timeout_units * 15625 / 1000;
      }
      break;
    case SX1262_CMD_WRITE_REGISTER:
      if (length >= 3) {
        uint16_t address = sx1262_be16(data);
        for (size_t index = 2; index < length && address + index - 2 < LILYGO_SX1262_REGISTER_SIZE; ++index) {
          state->registers[address + index - 2] = data[index];
        }
        state->sync_word = ((uint16_t)state->registers[SX1262_REG_LORA_SYNC_WORD_MSB] << 8) |
                           state->registers[SX1262_REG_LORA_SYNC_WORD_MSB + 1];
        state->envelope.sync_word = state->sync_word;
      }
      break;
    case SX1262_CMD_WRITE_BUFFER:
      if (length >= 2) {
        for (size_t index = 1; index < length; ++index) state->fifo[(uint8_t)(data[0] + index - 1)] = data[index];
      }
      break;
    case SX1262_CMD_SET_DIO_IRQ_PARAMS:
      if (length >= 4) {
        state->irq_mask = sx1262_be16(data);
        state->dio1_mask = sx1262_be16(&data[2]);
      }
      break;
    case SX1262_CMD_CLEAR_IRQ_STATUS:
      if (length >= 2) state->irq &= (uint16_t)~sx1262_be16(data);
      break;
    case SX1262_CMD_SET_RF_FREQUENCY:
      if (length >= 4) state->envelope.frequency_hz = (uint32_t)(((uint64_t)sx1262_be32(data) * 32000000) >> 25);
      break;
    case SX1262_CMD_SET_PACKET_TYPE:
      if (length >= 1) state->packet_type = state->envelope.packet_type = data[0];
      break;
    case SX1262_CMD_SET_MODULATION_PARAMS:
      if (length >= 3 && state->packet_type == 0x01) {
        state->spreading_factor = state->envelope.spreading_factor = data[0];
        state->bandwidth = state->envelope.bandwidth = data[1];
        state->coding_rate = state->envelope.coding_rate = data[2];
      }
      break;
    case SX1262_CMD_SET_PACKET_PARAMS:
      if (length >= 4 && state->packet_type == 0x01) state->payload_length = data[3];
      break;
    case SX1262_CMD_SET_BUFFER_BASE_ADDRESS:
      if (length >= 2) {
        state->tx_base = data[0];
        state->rx_base = data[1];
      }
      break;
    case SX1262_CMD_CLEAR_DEVICE_ERRORS:
      state->device_errors = 0;
      break;
    default:
      break;
  }
  sx1262_emit_trace(state);
}

void lilygo_sx1262_select(LilygoSx1262State* state, bool selected) {
  if (selected) {
    state->selected = true;
    state->transaction_length = 0;
    state->opcode = SX1262_CMD_NOP;
    state->command_valid = true;
    return;
  }
  if (!state->selected) return;
  if (sx1262_available(state) && state->transaction_length > 0) {
    if (state->command_valid) {
      sx1262_apply_command(state);
    } else {
      sx1262_emit_trace(state);
    }
  }
  state->selected = false;
  state->transaction_length = 0;
}

uint8_t lilygo_sx1262_transfer(LilygoSx1262State* state, uint8_t value) {
  if (!state->selected || !sx1262_available(state) || lilygo_sx1262_busy(state)) return 0xFF;
  size_t position = state->transaction_length;
  if (position < sizeof(state->transaction)) state->transaction[position] = value;
  state->transaction_length++;
  if (position == 0) {
    state->opcode = value;
    state->command_valid = sx1262_known_command(value);
    if (state->mode == LILYGO_SX1262_MODE_SLEEP && value == SX1262_CMD_NOP) state->mode = LILYGO_SX1262_MODE_STANDBY_RC;
    return 0;
  }
  return sx1262_read_response(state, position);
}

#ifndef LILYGO_SX1262_MODEL_STANDALONE

#include "hw/irq.h"
#include "hw/misc/lilygo_peripheral_control.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"

OBJECT_DECLARE_SIMPLE_TYPE(LilygoSx1262Device, LILYGO_SX1262)

typedef struct LilygoSx1262Device {
  SSIPeripheral parent_obj;
  LilygoSx1262State model;
  QEMUTimer* timer;
  qemu_irq busy_out;
  qemu_irq dio1_out;
  uint32_t printed_traces;
  uint32_t printed_transfer_failures;
  uint32_t printed_state_traces;
  uint32_t transmitted_events;
} LilygoSx1262Device;

static LilygoSx1262Device* g_lilygo_sx1262;

static uint64_t sx1262_virtual_us(void) { return qemu_clock_get_us(QEMU_CLOCK_VIRTUAL); }

static uint16_t sx1262_read_le16(const uint8_t* bytes) { return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u); }

static uint32_t sx1262_read_le32(const uint8_t* bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static uint64_t sx1262_read_le64(const uint8_t* bytes) {
  uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) value |= (uint64_t)bytes[index] << (index * 8u);
  return value;
}

static void sx1262_write_le16(uint8_t* bytes, uint16_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8u);
}

static void sx1262_write_le32(uint8_t* bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8u);
  bytes[2] = (uint8_t)(value >> 16u);
  bytes[3] = (uint8_t)(value >> 24u);
}

static void sx1262_write_le64(uint8_t* bytes, uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) bytes[index] = (uint8_t)(value >> (index * 8u));
}

static void sx1262_device_transmit(void* opaque, const LilygoSx1262Envelope* envelope, const uint8_t* payload,
                                   size_t length, uint64_t time_us) {
  LilygoSx1262Device* device = opaque;
  const uint8_t bounded_length = MIN(length, LILYGO_SX1262_BROKER_MAX_PACKET_SIZE);
  uint8_t event[35 + LILYGO_SX1262_BROKER_MAX_PACKET_SIZE] = {0};
  event[0] = LILYGO_PERIPHERAL_CONTROL_VERSION;
  event[1] = LILYGO_PERIPHERAL_MESSAGE_RADIO_TX;
  event[2] = LILYGO_SX1262_CONTROL_DEVICE;
  event[3] = length > bounded_length ? 1 : 0;
  sx1262_write_le32(&event[4], ++device->transmitted_events);
  sx1262_write_le64(&event[8], envelope->endpoint_id);
  sx1262_write_le64(&event[16], time_us);
  sx1262_write_le32(&event[24], envelope->frequency_hz);
  event[28] = envelope->packet_type;
  event[29] = envelope->spreading_factor;
  event[30] = envelope->bandwidth;
  event[31] = envelope->coding_rate;
  sx1262_write_le16(&event[32], envelope->sync_word);
  event[34] = bounded_length;
  memcpy(&event[35], payload, bounded_length);
  lilygo_peripheral_control_emit(event, 35 + bounded_length);
  fprintf(stderr, "[LILYGO-SX1262-TX] sequence=%u endpoint=%" PRIu64 " frequency_hz=%u bytes=%u truncated=%u\n",
          device->transmitted_events, envelope->endpoint_id, envelope->frequency_hz, bounded_length,
          length > bounded_length ? 1 : 0);
}

static void sx1262_device_trace(void* opaque, const LilygoSx1262Trace* trace) {
  LilygoSx1262Device* device = opaque;
  if ((trace->opcode == SX1262_CMD_SET_TX || trace->irq != 0) && device->printed_state_traces < 32) {
    device->printed_state_traces++;
    fprintf(stderr,
            "[LILYGO-SX1262] state seq=%" PRIu64
            " opcode=0x%02x bytes=%u status=0x%02x mode=%d irq=0x%04x dio1_mask=0x%04x busy=%u dio1=%u now_us=%" PRIu64
            " deadline_us=%" PRIu64 "\n",
            trace->sequence, trace->opcode, trace->transaction_length, trace->status, trace->mode, trace->irq,
            device->model.dio1_mask, trace->busy ? 1 : 0, trace->dio1 ? 1 : 0, trace->time_us,
            device->model.operation_deadline_us);
  }
  if (device->printed_traces >= 64) return;
  device->printed_traces++;
  fprintf(stderr, "[LILYGO-SX1262] spi seq=%" PRIu64 " opcode=0x%02x bytes=%u status=0x%02x mode=%d irq=0x%04x\n",
          trace->sequence, trace->opcode, trace->transaction_length, trace->status, trace->mode, trace->irq);
}

static void sx1262_device_update_outputs(LilygoSx1262Device* device) {
  qemu_set_irq(device->busy_out, lilygo_sx1262_busy(&device->model));
  qemu_set_irq(device->dio1_out, lilygo_sx1262_dio1(&device->model));
}

static void sx1262_device_schedule(LilygoSx1262Device* device) {
  uint64_t deadline = 0;
  if (device->model.busy_until_us > device->model.now_us) deadline = device->model.busy_until_us;
  if (device->model.operation_deadline_us > device->model.now_us &&
      (deadline == 0 || device->model.operation_deadline_us < deadline)) {
    deadline = device->model.operation_deadline_us;
  }
  if (device->model.pending_receive && device->model.pending_delivery_us > device->model.now_us &&
      (deadline == 0 || device->model.pending_delivery_us < deadline)) {
    deadline = device->model.pending_delivery_us;
  }
  if (deadline != 0) timer_mod(device->timer, deadline);
}

static void sx1262_device_sync(LilygoSx1262Device* device) {
  lilygo_sx1262_advance_time(&device->model, sx1262_virtual_us());
  sx1262_device_update_outputs(device);
  sx1262_device_schedule(device);
}

static bool sx1262_control_schedule(LilygoSx1262Device* device, const uint8_t* payload, uint8_t len,
                                    bool require_envelope_match) {
  const uint8_t prefix = require_envelope_match ? 27 : 9;
  if (len < prefix) return false;

  LilygoSx1262Envelope sender = {0};
  uint32_t delay_us;
  int16_t rssi_dbm;
  int8_t snr_db;
  uint8_t packet_length;
  const uint8_t* packet;
  if (require_envelope_match) {
    sender.endpoint_id = sx1262_read_le64(&payload[1]);
    sender.frequency_hz = sx1262_read_le32(&payload[9]);
    sender.packet_type = payload[13];
    sender.spreading_factor = payload[14];
    sender.bandwidth = payload[15];
    sender.coding_rate = payload[16];
    sender.sync_word = sx1262_read_le16(&payload[17]);
    delay_us = sx1262_read_le32(&payload[19]);
    rssi_dbm = (int16_t)sx1262_read_le16(&payload[23]);
    snr_db = (int8_t)payload[25];
    packet_length = payload[26];
    packet = &payload[27];
    if (!lilygo_sx1262_envelopes_match(&device->model.envelope, &sender)) return false;
  } else {
    delay_us = sx1262_read_le32(&payload[1]);
    rssi_dbm = (int16_t)sx1262_read_le16(&payload[5]);
    snr_db = (int8_t)payload[7];
    packet_length = payload[8];
    packet = &payload[9];
  }
  if (packet_length == 0 || len != prefix + packet_length || rssi_dbm < -127 || rssi_dbm > 0 || snr_db < -32 ||
      snr_db > 31) {
    return false;
  }
  return lilygo_sx1262_schedule_receive(&device->model, packet, packet_length, rssi_dbm, snr_db,
                                        device->model.now_us + delay_us);
}

uint8_t lilygo_sx1262_control(uint8_t operation, uint8_t flags, const uint8_t* payload, uint8_t len) {
  if (g_lilygo_sx1262 == NULL) return LILYGO_PERIPHERAL_ERROR_UNKNOWN_DEVICE;
  if (flags != 0) return LILYGO_PERIPHERAL_ERROR_UNKNOWN_FIELD;
  sx1262_device_sync(g_lilygo_sx1262);

  switch (operation) {
    case LILYGO_PERIPHERAL_OPERATION_SET_STATE:
      if (payload == NULL || len == 0) return LILYGO_PERIPHERAL_ERROR_MALFORMED;
      if (payload[0] == LILYGO_SX1262_CONTROL_SET_ENDPOINT) {
        if (len != 9 || sx1262_read_le64(&payload[1]) == 0) return LILYGO_PERIPHERAL_ERROR_INVALID_VALUE;
        lilygo_sx1262_set_endpoint(&g_lilygo_sx1262->model, sx1262_read_le64(&payload[1]));
      } else if (payload[0] == LILYGO_SX1262_CONTROL_SCHEDULE_RECEIVE) {
        if (!sx1262_control_schedule(g_lilygo_sx1262, payload, len, false)) {
          return LILYGO_PERIPHERAL_ERROR_INVALID_VALUE;
        }
      } else if (payload[0] == LILYGO_SX1262_CONTROL_DELIVER_BROKER_PACKET) {
        if (g_lilygo_sx1262->model.mode != LILYGO_SX1262_MODE_RX) {
          return LILYGO_PERIPHERAL_ERROR_NOT_READY;
        }
        if (!sx1262_control_schedule(g_lilygo_sx1262, payload, len, true)) {
          return LILYGO_PERIPHERAL_ERROR_NOT_READY;
        }
      } else {
        return LILYGO_PERIPHERAL_ERROR_UNKNOWN_FIELD;
      }
      break;
    case LILYGO_PERIPHERAL_OPERATION_SET_FAULT:
      if (payload == NULL || len != 1 || payload[0] > LILYGO_SX1262_FAULT_AIR_DISCONNECTED) {
        return LILYGO_PERIPHERAL_ERROR_INVALID_VALUE;
      }
      lilygo_sx1262_set_fault(&g_lilygo_sx1262->model, (LilygoSx1262Fault)payload[0]);
      break;
    case LILYGO_PERIPHERAL_OPERATION_RESET:
      if (len != 0) return LILYGO_PERIPHERAL_ERROR_UNKNOWN_FIELD;
      lilygo_sx1262_set_reset(&g_lilygo_sx1262->model, true, g_lilygo_sx1262->model.now_us);
      lilygo_sx1262_set_reset(&g_lilygo_sx1262->model, false, g_lilygo_sx1262->model.now_us);
      break;
    default:
      return LILYGO_PERIPHERAL_ERROR_UNKNOWN_OPERATION;
  }

  sx1262_device_update_outputs(g_lilygo_sx1262);
  sx1262_device_schedule(g_lilygo_sx1262);
  return LILYGO_PERIPHERAL_ERROR_NONE;
}

static void sx1262_device_timer(void* opaque) { sx1262_device_sync(opaque); }

static uint32_t sx1262_device_transfer(SSIPeripheral* peripheral, uint32_t value) {
  LilygoSx1262Device* device = LILYGO_SX1262(peripheral);
  sx1262_device_sync(device);
  const uint8_t response = lilygo_sx1262_transfer(&device->model, (uint8_t)value);
  if (response == 0xFF && device->printed_transfer_failures < 32) {
    device->printed_transfer_failures++;
    fprintf(stderr,
            "[LILYGO-SX1262] transfer-rejected tx=0x%02x selected=%u powered=%u reset=%u ready=%u busy=%u "
            "opcode=0x%02x position=%zu now_us=%" PRIu64 " ready_us=%" PRIu64 "\n",
            (uint8_t)value, device->model.selected ? 1 : 0, device->model.powered ? 1 : 0,
            device->model.reset_asserted ? 1 : 0, device->model.now_us >= device->model.power_ready_us ? 1 : 0,
            lilygo_sx1262_busy(&device->model) ? 1 : 0, device->model.opcode, device->model.transaction_length,
            device->model.now_us, device->model.power_ready_us);
  }
  sx1262_device_update_outputs(device);
  return response;
}

static int sx1262_device_set_cs(SSIPeripheral* peripheral, bool level) {
  LilygoSx1262Device* device = LILYGO_SX1262(peripheral);
  sx1262_device_sync(device);
  lilygo_sx1262_select(&device->model, !level);
  sx1262_device_update_outputs(device);
  sx1262_device_schedule(device);
  return 0;
}

static void sx1262_device_reset_gpio(void* opaque, int line, int level) {
  LilygoSx1262Device* device = opaque;
  (void)line;
  lilygo_sx1262_set_reset(&device->model, level == 0, sx1262_virtual_us());
  sx1262_device_update_outputs(device);
  sx1262_device_schedule(device);
}

void lilygo_sx1262_set_board_power(bool powered) {
  if (g_lilygo_sx1262 == NULL) return;
  /* Production firmware owns the 1500 ms PCA9535 rail settle. Do not apply it
   * twice in the peripheral adapter; standalone model tests still exercise
   * explicit pre-ready rejection with a non-zero settle value. */
  lilygo_sx1262_set_power(&g_lilygo_sx1262->model, powered, sx1262_virtual_us(), 0);
  if (powered) g_lilygo_sx1262->printed_transfer_failures = 0;
  sx1262_device_update_outputs(g_lilygo_sx1262);
  sx1262_device_schedule(g_lilygo_sx1262);
  fprintf(stderr,
          "[LILYGO-SX1262] rail=%s guest_settle_ms=%u model_settle_ms=0 now_us=%" PRIu64 " ready_us=%" PRIu64 "\n",
          powered ? "on" : "off", SX1262_BOARD_GUEST_SETTLE_MS, g_lilygo_sx1262->model.now_us,
          g_lilygo_sx1262->model.power_ready_us);
}

bool lilygo_sx1262_board_busy(void) {
  if (g_lilygo_sx1262 == NULL) return false;
  sx1262_device_sync(g_lilygo_sx1262);
  return lilygo_sx1262_busy(&g_lilygo_sx1262->model);
}

bool lilygo_sx1262_board_dio1(void) {
  if (g_lilygo_sx1262 == NULL) return false;
  sx1262_device_sync(g_lilygo_sx1262);
  return lilygo_sx1262_dio1(&g_lilygo_sx1262->model);
}

static void sx1262_device_reset(Object* object, ResetType type) {
  LilygoSx1262Device* device = LILYGO_SX1262(object);
  (void)type;
  lilygo_sx1262_init(&device->model);
  device->printed_traces = 0;
  device->printed_transfer_failures = 0;
  device->printed_state_traces = 0;
  device->transmitted_events = 0;
  lilygo_sx1262_set_trace_hook(&device->model, sx1262_device_trace, device);
  lilygo_sx1262_set_transmit_hook(&device->model, sx1262_device_transmit, device);
  sx1262_device_update_outputs(device);
}

static void sx1262_device_realize(SSIPeripheral* peripheral, Error** errp) {
  LilygoSx1262Device* device = LILYGO_SX1262(peripheral);
  (void)errp;
  g_lilygo_sx1262 = device;
  device->timer = timer_new_us(QEMU_CLOCK_VIRTUAL, sx1262_device_timer, device);
  qdev_init_gpio_in_named(DEVICE(device), sx1262_device_reset_gpio, "reset", 1);
  qdev_init_gpio_out_named(DEVICE(device), &device->busy_out, "busy", 1);
  qdev_init_gpio_out_named(DEVICE(device), &device->dio1_out, "dio1", 1);
  sx1262_device_reset(OBJECT(device), RESET_TYPE_COLD);
}

static const VMStateDescription vmstate_lilygo_sx1262 = {
    .name = TYPE_LILYGO_SX1262,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]){
        VMSTATE_SSI_PERIPHERAL(parent_obj, LilygoSx1262Device),
        VMSTATE_UINT8_ARRAY(model.registers, LilygoSx1262Device, LILYGO_SX1262_REGISTER_SIZE),
        VMSTATE_UINT8_ARRAY(model.fifo, LilygoSx1262Device, LILYGO_SX1262_FIFO_SIZE),
        VMSTATE_UINT64(model.now_us, LilygoSx1262Device), VMSTATE_UINT64(model.busy_until_us, LilygoSx1262Device),
        VMSTATE_UINT64(model.operation_deadline_us, LilygoSx1262Device), VMSTATE_UINT16(model.irq, LilygoSx1262Device),
        VMSTATE_UINT16(model.irq_mask, LilygoSx1262Device), VMSTATE_UINT16(model.dio1_mask, LilygoSx1262Device),
        VMSTATE_INT32(model.mode, LilygoSx1262Device), VMSTATE_INT32(model.fault, LilygoSx1262Device),
        VMSTATE_BOOL(model.powered, LilygoSx1262Device), VMSTATE_BOOL(model.reset_asserted, LilygoSx1262Device),
        VMSTATE_END_OF_LIST()}};

static void sx1262_device_class_init(ObjectClass* klass, void* data) {
  DeviceClass* device = DEVICE_CLASS(klass);
  SSIPeripheralClass* ssi = SSI_PERIPHERAL_CLASS(klass);
  ResettableClass* reset = RESETTABLE_CLASS(klass);
  (void)data;
  ssi->realize = sx1262_device_realize;
  ssi->transfer = sx1262_device_transfer;
  ssi->set_cs = sx1262_device_set_cs;
  ssi->cs_polarity = SSI_CS_LOW;
  device->vmsd = &vmstate_lilygo_sx1262;
  device->desc = "LilyGo T5S3 Pro SX1262 functional radio model";
  reset->phases.hold = sx1262_device_reset;
}

static const TypeInfo sx1262_device_info = {.name = TYPE_LILYGO_SX1262,
                                            .parent = TYPE_SSI_PERIPHERAL,
                                            .instance_size = sizeof(LilygoSx1262Device),
                                            .class_init = sx1262_device_class_init};

static void sx1262_device_register_types(void) { type_register_static(&sx1262_device_info); }

type_init(sx1262_device_register_types)

#endif
