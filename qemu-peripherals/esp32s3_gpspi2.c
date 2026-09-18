/*
 * ESP32-S3 GPSPI2 (SPI2 / FSPI / HSPI) minimal QEMU model for the
 * Mofei simulator.
 *
 * Models the GPSPI2 register and GDMA behavior exercised by ESP-IDF's SPI
 * master and SDSPI drivers. CPU/FIFO and DMA transfers are full duplex: SSI
 * return bytes are written back to W0-W15 or the configured GDMA IN channel.
 *
 * Two transfer modes are supported:
 *   1. FIFO mode: data in W0-W15, byte count from MOSI_DLEN (≤ 64 bytes)
 *   2. DMA mode: reuse the SoC's generic ESP GDMA implementation and its
 *      descriptor/status/interrupt behavior for SPI2 OUT/IN channels.
 *
 * Mofei/S37UC retain their single always-selected display target. LilyGo
 * enables GPIO-routed CS12/CS46 selection. Both targets reset inactive-high;
 * overlapping selection fails explicitly and never combines responses.
 *
 * Register reference: ESP32-S3 TRM §7, ESP-IDF soc/spi_struct.h.
 * MMIO base: 0x60024000 (DR_REG_SPI2_BASE in esp32s3_reg.h).
 */

#include "qemu/osdep.h"
#include "exec/address-spaces.h"
#include "hw/dma/esp_gdma.h"
#include "hw/irq.h"
#include "hw/misc/lilygo_peripheral_control.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "hw/ssi/ssi.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define TYPE_ESP32S3_GPSPI2 "esp32s3-gpspi2"
OBJECT_DECLARE_SIMPLE_TYPE(ESP32S3GPSPI2State, ESP32S3_GPSPI2)

/* Register offsets — ESP32-S3 SPI2 peripheral (see TRM §7 / spi_reg.h) */
#define GPSPI2_CMD 0x000
#define GPSPI2_ADDR 0x004
#define GPSPI2_CTRL 0x008
#define GPSPI2_CLOCK 0x00C
#define GPSPI2_USER 0x010
#define GPSPI2_USER1 0x014
#define GPSPI2_USER2 0x018
#define GPSPI2_MS_DLEN 0x01C
#define GPSPI2_MISC 0x020
#define GPSPI2_DIN_MODE 0x024
#define GPSPI2_DIN_NUM 0x028
#define GPSPI2_DOUT_MODE 0x02C
#define GPSPI2_DMA_CONF 0x030
#define GPSPI2_DMA_INT_ENA 0x034
#define GPSPI2_DMA_INT_CLR 0x038
#define GPSPI2_DMA_INT_RAW 0x03C
#define GPSPI2_DMA_INT_ST 0x040
#define GPSPI2_DMA_INT_SET 0x044
#define GPSPI2_DATA_BUF(n) (0x098 + (n) * 4) /* W0-W15: 0x098..0x0D4 */

#define GPSPI2_IO_SIZE 0x400 /* covers all registers through W15 at 0x0D4 */

/* Bit masks */
#define GPSPI2_CMD_USR BIT(24)
#define GPSPI2_CMD_UPDATE BIT(23)

#define GPSPI2_USER_USR_MOSI BIT(27)
#define GPSPI2_USER_USR_MISO BIT(28)
#define GPSPI2_USER_USR_DUMMY BIT(29)
#define GPSPI2_USER_USR_ADDR BIT(30)
#define GPSPI2_USER_USR_COMMAND BIT(31)

#define GPSPI2_DMA_CONF_DMA_TX_ENA BIT(28)
#define GPSPI2_DMA_CONF_DMA_RX_ENA BIT(27)

#define GPSPI2_MISC_CS0_DIS BIT(0)
#define GPSPI2_MISC_CS1_DIS BIT(1)

#define GPSPI2_DMA_INT_TRANS_DONE BIT(12)
#define GPSPI2_DMA_INT_RX_OVERFLOW BIT(17)
#define GPSPI2_DMA_INT_TX_UNDERFLOW BIT(18)

#define GPSPI2_FIFO_WORDS 16
#define GPSPI2_FIFO_BYTES (GPSPI2_FIFO_WORDS * 4) /* 64 */
#define GPSPI2_MAX_TRANSFER_BYTES (BIT(15))

#define GPSPI2_TARGET_COUNT 2
#define GPSPI2_TARGET_SD 0
#define GPSPI2_TARGET_RADIO 1

#define GPSPI2_CONTROL_VERSION 1
#define GPSPI2_CONTROL_DEVICE 2
#define GPSPI2_CONTROL_HEADER_SIZE 10
#define GPSPI2_CONTROL_MAX_PAYLOAD 64
#define GPSPI2_CONTROL_OPERATION_SET_STATE 1
#define GPSPI2_CONTROL_OPERATION_SET_FAULT 2
#define GPSPI2_CONTROL_OPERATION_RESET 3
#define GPSPI2_CONTROL_FLAG_ONE_SHOT BIT(0)
#define GPSPI2_CONTROL_FAULT_CONFLICT 1
#define GPSPI2_CONTROL_FAULT_SD_ABSENT 2
#define GPSPI2_CONTROL_FAULT_SD_READ 3
#define GPSPI2_CONTROL_FAULT_SD_WRITE 4
#define GPSPI2_CONTROL_FAULT_SD_CORRUPT 5
#define GPSPI2_CONTROL_MESSAGE_ACK 0x80
#define GPSPI2_CONTROL_MESSAGE_TRACE 0x82
#define GPSPI2_CONTROL_ACK_SIZE 12
#define GPSPI2_CONTROL_TRACE_HEADER_SIZE 34
#define GPSPI2_CONTROL_TRACE_SAMPLE_BYTES 8
#define GPSPI2_CONTROL_RESULT_OK 0
#define GPSPI2_CONTROL_RESULT_CONFLICT 1
#define GPSPI2_CONTROL_RESULT_TRANSFER_ERROR 2
#define GPSPI2_CONTROL_RESULT_RESET_ABORT 3
#define GPSPI2_CONTROL_ERROR_NONE 0
#define GPSPI2_CONTROL_ERROR_UNSUPPORTED_VERSION 1
#define GPSPI2_CONTROL_ERROR_UNKNOWN_DEVICE 2
#define GPSPI2_CONTROL_ERROR_UNKNOWN_OPERATION 3
#define GPSPI2_CONTROL_ERROR_PAYLOAD_TOO_LARGE 4
#define GPSPI2_CONTROL_ERROR_MALFORMED 5
#define GPSPI2_CONTROL_ERROR_UNKNOWN_FIELD 6
#define GPSPI2_CONTROL_ERROR_BUSY 7

struct ESP32S3GPSPI2State {
  SysBusDevice parent_obj;

  MemoryRegion iomem;
  SSIBus* spi_bus;
  qemu_irq irq;
  qemu_irq target_select[GPSPI2_TARGET_COUNT];
  ESPGdmaState* gdma;

  bool route_chip_selects;
  bool chip_select[GPSPI2_TARGET_COUNT];
  bool conflict_fault_latched;
  bool conflict_fault_once;
  int8_t active_target;
  uint64_t trace_sequence;
  uint32_t transaction_tx_bytes;
  uint32_t transaction_rx_bytes;
  uint8_t transaction_tx_sample[GPSPI2_CONTROL_TRACE_SAMPLE_BYTES];
  uint8_t transaction_rx_sample[GPSPI2_CONTROL_TRACE_SAMPLE_BYTES];
  bool transaction_dma;
  bool transaction_error;
  uint8_t transaction_fault;
  uint8_t transaction_command;
  uint32_t transaction_block_argument;

  uint32_t cmd;
  uint32_t addr_reg;
  uint32_t ctrl;
  uint32_t clock_reg;
  uint32_t user;
  uint32_t user1;
  uint32_t user2;
  uint32_t ms_dlen;
  uint32_t misc;
  uint32_t dma_conf;
  uint32_t dma_int_ena;
  uint32_t dma_int_raw;
  uint32_t w[GPSPI2_FIFO_WORDS];
};

static ESP32S3GPSPI2State* g_gpspi2;

static void gpspi2_finish_transaction(ESP32S3GPSPI2State* s, uint8_t result);
static void gpspi2_reset_transaction(ESP32S3GPSPI2State* s);
static uint8_t gpspi2_transfer_byte(ESP32S3GPSPI2State* s, uint8_t tx, bool* conflict);
static void gpspi2_record_transfer(ESP32S3GPSPI2State* s, bool dma, uint32_t tx_bytes, uint32_t rx_bytes,
                                   const uint8_t* tx, const uint8_t* rx, bool ok);
bool esp32s3_gpspi2_radio_selected(void);
int esp32s3_gpspi2_direct_radio_transfer(const uint8_t* tx, uint8_t* rx, size_t len);

static void gpspi2_abort_endpoints(ESP32S3GPSPI2State* s) {
  for (int i = 0; i < GPSPI2_TARGET_COUNT; ++i) qemu_set_irq(s->target_select[i], 1);
  if (s->spi_bus != NULL) bus_cold_reset(BUS(s->spi_bus));
}

static uint32_t gpspi2_read_le32(const uint8_t* bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) | ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static void gpspi2_write_le16(uint8_t* bytes, uint16_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8u);
}

static void gpspi2_write_le32(uint8_t* bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8u);
  bytes[2] = (uint8_t)(value >> 16u);
  bytes[3] = (uint8_t)(value >> 24u);
}

static void gpspi2_write_le64(uint8_t* bytes, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) bytes[i] = (uint8_t)(value >> (i * 8u));
}

static void gpspi2_send_control_ack(uint32_t request_id, bool accepted, uint8_t error) {
  uint8_t ack[GPSPI2_CONTROL_ACK_SIZE] = {0};
  ack[0] = GPSPI2_CONTROL_VERSION;
  ack[1] = GPSPI2_CONTROL_MESSAGE_ACK;
  ack[2] = GPSPI2_CONTROL_DEVICE;
  ack[3] = accepted ? 1 : 0;
  gpspi2_write_le32(&ack[4], request_id);
  ack[8] = error;
  lilygo_peripheral_control_emit(ack, sizeof(ack));
  fprintf(stderr, "[LILYGO-SPI-ACK] request=%u accepted=%u error=%u\n", request_id, accepted ? 1 : 0, error);
}

bool esp32s3_gpspi2_control(const uint8_t* payload, uint32_t len) {
  if (payload == NULL || len < 3 || payload[2] != GPSPI2_CONTROL_DEVICE) {
    return false;
  }
  const uint32_t request_id = len >= 8 ? gpspi2_read_le32(&payload[4]) : 0;
  if (len > GPSPI2_CONTROL_MAX_PAYLOAD) {
    gpspi2_send_control_ack(request_id, false, GPSPI2_CONTROL_ERROR_PAYLOAD_TOO_LARGE);
    return true;
  }
  if (len < GPSPI2_CONTROL_HEADER_SIZE) {
    gpspi2_send_control_ack(request_id, false, GPSPI2_CONTROL_ERROR_MALFORMED);
    return true;
  }
  if (payload[0] != GPSPI2_CONTROL_VERSION) {
    gpspi2_send_control_ack(request_id, false, GPSPI2_CONTROL_ERROR_UNSUPPORTED_VERSION);
    return true;
  }
  if (g_gpspi2 == NULL || !g_gpspi2->route_chip_selects) {
    gpspi2_send_control_ack(request_id, false, GPSPI2_CONTROL_ERROR_UNKNOWN_DEVICE);
    return true;
  }
  const uint8_t operation = payload[1];
  const uint8_t flags = payload[3];
  const uint8_t payload_len = payload[8];
  if (payload[9] != 0 || flags & ~GPSPI2_CONTROL_FLAG_ONE_SHOT || len != GPSPI2_CONTROL_HEADER_SIZE + payload_len) {
    gpspi2_send_control_ack(request_id, false, GPSPI2_CONTROL_ERROR_UNKNOWN_FIELD);
    return true;
  }

  switch (operation) {
    case GPSPI2_CONTROL_OPERATION_SET_STATE:
      /* CS12/CS46 are production firmware outputs, never host control state. */
      gpspi2_send_control_ack(request_id, false, GPSPI2_CONTROL_ERROR_UNKNOWN_OPERATION);
      return true;
    case GPSPI2_CONTROL_OPERATION_SET_FAULT:
      if (payload_len != 1 || payload[10] != GPSPI2_CONTROL_FAULT_CONFLICT) {
        gpspi2_send_control_ack(request_id, false, GPSPI2_CONTROL_ERROR_MALFORMED);
        return true;
      }
      if (g_gpspi2->active_target != -1 || !g_gpspi2->chip_select[GPSPI2_TARGET_SD] ||
          !g_gpspi2->chip_select[GPSPI2_TARGET_RADIO]) {
        gpspi2_send_control_ack(request_id, false, GPSPI2_CONTROL_ERROR_BUSY);
        return true;
      }
      if (flags & GPSPI2_CONTROL_FLAG_ONE_SHOT) {
        g_gpspi2->conflict_fault_once = true;
      } else {
        g_gpspi2->conflict_fault_latched = true;
      }
      break;
    case GPSPI2_CONTROL_OPERATION_RESET:
      if (payload_len != 0 || flags != 0) {
        gpspi2_send_control_ack(request_id, false, GPSPI2_CONTROL_ERROR_MALFORMED);
        return true;
      }
      gpspi2_finish_transaction(g_gpspi2, GPSPI2_CONTROL_RESULT_RESET_ABORT);
      gpspi2_abort_endpoints(g_gpspi2);
      device_cold_reset(DEVICE(g_gpspi2));
      break;
    default:
      gpspi2_send_control_ack(request_id, false, GPSPI2_CONTROL_ERROR_UNKNOWN_OPERATION);
      return true;
  }
  gpspi2_send_control_ack(request_id, true, GPSPI2_CONTROL_ERROR_NONE);
  return true;
}

bool esp32s3_gpspi2_transaction_active(void) {
  return g_gpspi2 != NULL && g_gpspi2->route_chip_selects && g_gpspi2->active_target != -1;
}

/* ---- Transfer execution ------------------------------------------------- */

static uint32_t gpspi2_transfer_bytes(const ESP32S3GPSPI2State* s) {
  if ((s->user & (GPSPI2_USER_USR_MOSI | GPSPI2_USER_USR_MISO)) == 0) {
    return 0;
  }
  const uint32_t total_bits = (s->ms_dlen & 0x3FFFF) + 1;
  return (total_bits + 7) / 8;
}

static uint8_t gpspi2_fifo_get_byte(const ESP32S3GPSPI2State* s, uint32_t index) {
  const uint32_t word = s->w[index / 4];
  return (uint8_t)(word >> ((index % 4) * 8));
}

static void gpspi2_fifo_set_byte(ESP32S3GPSPI2State* s, uint32_t index, uint8_t value) {
  const uint32_t shift = (index % 4) * 8;
  const uint32_t mask = 0xFFu << shift;
  s->w[index / 4] = (s->w[index / 4] & ~mask) | ((uint32_t)value << shift);
}

static int gpspi2_gpio_target(const ESP32S3GPSPI2State* s) {
  const bool sd_selected = !s->chip_select[GPSPI2_TARGET_SD];
  const bool radio_selected = !s->chip_select[GPSPI2_TARGET_RADIO];
  if (sd_selected && radio_selected) {
    return -2;
  }
  if (sd_selected) {
    return GPSPI2_TARGET_SD;
  }
  if (radio_selected) {
    return GPSPI2_TARGET_RADIO;
  }
  return -1;
}

static int gpspi2_configured_target(const ESP32S3GPSPI2State* s) {
  const int gpio_target = gpspi2_gpio_target(s);
  if (gpio_target != -1) {
    return gpio_target;
  }

  const bool cs0_enabled = (s->misc & GPSPI2_MISC_CS0_DIS) == 0;
  const bool cs1_enabled = (s->misc & GPSPI2_MISC_CS1_DIS) == 0;
  if (cs0_enabled && cs1_enabled) {
    return -2;
  }
  if (cs0_enabled) {
    return GPSPI2_TARGET_SD;
  }
  if (cs1_enabled) {
    return GPSPI2_TARGET_RADIO;
  }
  return -1;
}

static bool gpspi2_begin_hardware_transaction(ESP32S3GPSPI2State* s) {
  if (!s->route_chip_selects || s->active_target != -1 || gpspi2_gpio_target(s) != -1) {
    return false;
  }

  const int target = gpspi2_configured_target(s);
  if (target == -1) {
    return false;
  }
  gpspi2_reset_transaction(s);
  if (s->conflict_fault_latched || s->conflict_fault_once) {
    s->active_target = -2;
    s->transaction_fault = GPSPI2_CONTROL_FAULT_CONFLICT;
    s->conflict_fault_once = false;
  } else {
    s->active_target = target;
  }

  if (s->active_target >= 0) {
    qemu_set_irq(s->target_select[s->active_target], 0);
  } else if (s->active_target == -2) {
    gpspi2_abort_endpoints(s);
  }
  return true;
}

static void gpspi2_finish_hardware_transaction(ESP32S3GPSPI2State* s) {
  if (s->active_target >= 0) {
    qemu_set_irq(s->target_select[s->active_target], 1);
  }
  gpspi2_finish_transaction(s, GPSPI2_CONTROL_RESULT_OK);
}

static int gpspi2_selected_target(const ESP32S3GPSPI2State* s) {
  return s->active_target != -1 ? s->active_target : gpspi2_configured_target(s);
}

static const char* gpspi2_target_name(int target) {
  switch (target) {
    case GPSPI2_TARGET_SD:
      return "sd";
    case GPSPI2_TARGET_RADIO:
      return "radio";
    case -2:
      return "conflict";
    default:
      return "none";
  }
}

static uint8_t gpspi2_transfer_byte(ESP32S3GPSPI2State* s, uint8_t tx, bool* conflict) {
  if (s->route_chip_selects) {
    const int target = gpspi2_selected_target(s);
    if (target == -2) {
      *conflict = true;
      return 0xFF;
    }
    if (target < 0) {
      return 0xFF;
    }
  }
  return (uint8_t)ssi_transfer(s->spi_bus, tx);
}

bool esp32s3_gpspi2_radio_selected(void) {
  return g_gpspi2 != NULL && g_gpspi2->route_chip_selects && g_gpspi2->active_target == GPSPI2_TARGET_RADIO;
}

int esp32s3_gpspi2_direct_radio_transfer(const uint8_t* tx, uint8_t* rx, size_t len) {
  ESP32S3GPSPI2State* s = g_gpspi2;
  if (!esp32s3_gpspi2_radio_selected() || len == 0 || len > GPSPI2_MAX_TRANSFER_BYTES) return -1;

  bool conflict = false;
  uint8_t tx_sample[GPSPI2_CONTROL_TRACE_SAMPLE_BYTES] = {0};
  uint8_t rx_sample[GPSPI2_CONTROL_TRACE_SAMPLE_BYTES] = {0};
  for (size_t i = 0; i < len; ++i) {
    const uint8_t tx_byte = tx != NULL ? tx[i] : 0xFF;
    const uint8_t rx_byte = gpspi2_transfer_byte(s, tx_byte, &conflict);
    if (rx != NULL) rx[i] = rx_byte;
    if (i < GPSPI2_CONTROL_TRACE_SAMPLE_BYTES) {
      tx_sample[i] = tx_byte;
      rx_sample[i] = rx_byte;
    }
    if (conflict) break;
  }
  gpspi2_record_transfer(s, false, tx != NULL ? len : 0, rx != NULL ? len : 0, tx_sample, rx_sample, !conflict);
  return conflict ? -1 : 0;
}

static void gpspi2_reset_transaction(ESP32S3GPSPI2State* s) {
  s->transaction_tx_bytes = 0;
  s->transaction_rx_bytes = 0;
  memset(s->transaction_tx_sample, 0, sizeof(s->transaction_tx_sample));
  memset(s->transaction_rx_sample, 0, sizeof(s->transaction_rx_sample));
  s->transaction_dma = false;
  s->transaction_error = false;
  s->transaction_fault = 0;
  s->transaction_command = 0xFF;
  s->transaction_block_argument = 0;
}

void esp32s3_gpspi2_note_sd_result(uint8_t command, uint32_t block_argument, uint8_t fault) {
  if (g_gpspi2 == NULL || g_gpspi2->active_target != GPSPI2_TARGET_SD) return;
  g_gpspi2->transaction_command = command;
  g_gpspi2->transaction_block_argument = block_argument;
  if (fault != 0) {
    g_gpspi2->transaction_fault = fault;
    g_gpspi2->transaction_error = true;
  }
}

static void gpspi2_record_transfer(ESP32S3GPSPI2State* s, bool dma, uint32_t tx_bytes, uint32_t rx_bytes,
                                   const uint8_t* tx, const uint8_t* rx, bool ok) {
  if (!s->route_chip_selects) return;
  const uint32_t tx_sample_offset = MIN(s->transaction_tx_bytes, GPSPI2_CONTROL_TRACE_SAMPLE_BYTES);
  const uint32_t rx_sample_offset = MIN(s->transaction_rx_bytes, GPSPI2_CONTROL_TRACE_SAMPLE_BYTES);
  const uint32_t tx_sample_len = MIN(tx_bytes, GPSPI2_CONTROL_TRACE_SAMPLE_BYTES - tx_sample_offset);
  const uint32_t rx_sample_len = MIN(rx_bytes, GPSPI2_CONTROL_TRACE_SAMPLE_BYTES - rx_sample_offset);
  if (tx != NULL && tx_sample_len != 0) memcpy(&s->transaction_tx_sample[tx_sample_offset], tx, tx_sample_len);
  if (rx != NULL && rx_sample_len != 0) memcpy(&s->transaction_rx_sample[rx_sample_offset], rx, rx_sample_len);
  s->transaction_tx_bytes += tx_bytes;
  s->transaction_rx_bytes += rx_bytes;
  s->transaction_dma |= dma;
  s->transaction_error |= !ok;
}

static void gpspi2_trace_transaction(ESP32S3GPSPI2State* s, uint8_t result) {
  if (!s->route_chip_selects) {
    return;
  }
  const int target = s->active_target;
  const uint32_t bytes = MAX(s->transaction_tx_bytes, s->transaction_rx_bytes);
  const uint8_t sample_len = MIN(bytes, GPSPI2_CONTROL_TRACE_SAMPLE_BYTES);
  uint8_t trace[GPSPI2_CONTROL_TRACE_HEADER_SIZE + GPSPI2_CONTROL_TRACE_SAMPLE_BYTES * 2] = {0};
  trace[0] = GPSPI2_CONTROL_VERSION;
  trace[1] = GPSPI2_CONTROL_MESSAGE_TRACE;
  trace[2] = GPSPI2_CONTROL_DEVICE;
  trace[3] = result;
  const uint64_t sequence = ++s->trace_sequence;
  gpspi2_write_le64(&trace[4], sequence);
  gpspi2_write_le64(&trace[12], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
  trace[20] = target >= 0 ? (uint8_t)target : 0xFF;
  trace[21] = target == GPSPI2_TARGET_SD ? 12 : target == GPSPI2_TARGET_RADIO ? 46 : 0xFF;
  trace[22] = s->transaction_dma ? 1 : 0;
  trace[23] = sample_len;
  gpspi2_write_le16(&trace[24], MIN(s->transaction_tx_bytes, UINT16_MAX));
  gpspi2_write_le16(&trace[26], MIN(s->transaction_rx_bytes, UINT16_MAX));
  trace[28] = s->transaction_fault;
  trace[29] = s->transaction_command;
  gpspi2_write_le32(&trace[30], s->transaction_block_argument);
  memcpy(&trace[GPSPI2_CONTROL_TRACE_HEADER_SIZE], s->transaction_tx_sample, sample_len);
  memcpy(&trace[GPSPI2_CONTROL_TRACE_HEADER_SIZE + GPSPI2_CONTROL_TRACE_SAMPLE_BYTES], s->transaction_rx_sample,
         sample_len);
  lilygo_peripheral_control_emit(trace, sizeof(trace));
  fprintf(stderr, "[LILYGO-SPI] seq=%" PRIu64 " owner=%s bytes=%u command=%u block=%u fault=%u result=%s\n", sequence,
          gpspi2_target_name(target), bytes, s->transaction_command, s->transaction_block_argument,
          s->transaction_fault, result == GPSPI2_CONTROL_RESULT_OK ? "ok" : "error");
  fflush(stderr);
}

static void gpspi2_finish_transaction(ESP32S3GPSPI2State* s, uint8_t result) {
  if (s->active_target == -1) return;
  if (s->active_target == -2 && result == GPSPI2_CONTROL_RESULT_OK) result = GPSPI2_CONTROL_RESULT_CONFLICT;
  if (s->transaction_error && result == GPSPI2_CONTROL_RESULT_OK) result = GPSPI2_CONTROL_RESULT_TRANSFER_ERROR;
  gpspi2_trace_transaction(s, result);
  s->active_target = -1;
  gpspi2_reset_transaction(s);
}

static void gpspi2_update_irq(ESP32S3GPSPI2State* s) { qemu_set_irq(s->irq, (s->dma_int_raw & s->dma_int_ena) != 0); }

static bool gpspi2_transfer_fifo(ESP32S3GPSPI2State* s, uint32_t total_bytes) {
  if (total_bytes == 0 || total_bytes > GPSPI2_FIFO_BYTES) {
    s->transaction_error = true;
    return false;
  }

  const bool transmit = (s->user & GPSPI2_USER_USR_MOSI) != 0;
  const bool receive = (s->user & GPSPI2_USER_USR_MISO) != 0;
  bool conflict = false;
  uint8_t tx_sample[GPSPI2_CONTROL_TRACE_SAMPLE_BYTES] = {0};
  uint8_t rx_sample[GPSPI2_CONTROL_TRACE_SAMPLE_BYTES] = {0};
  for (uint32_t i = 0; i < total_bytes; i++) {
    const uint8_t tx = transmit ? gpspi2_fifo_get_byte(s, i) : 0xFF;
    const uint8_t rx = gpspi2_transfer_byte(s, tx, &conflict);
    if (i < GPSPI2_CONTROL_TRACE_SAMPLE_BYTES) {
      tx_sample[i] = tx;
      rx_sample[i] = rx;
    }
    if (receive) {
      gpspi2_fifo_set_byte(s, i, rx);
    }
  }
  gpspi2_record_transfer(s, false, transmit ? total_bytes : 0, receive ? total_bytes : 0, tx_sample, rx_sample,
                         !conflict);
  return !conflict;
}

static bool gpspi2_transfer_dma(ESP32S3GPSPI2State* s, uint32_t total_bytes) {
  if (s->gdma == NULL || total_bytes == 0 || total_bytes > GPSPI2_MAX_TRANSFER_BYTES) {
    s->transaction_error = true;
    return false;
  }

  const bool transmit = (s->user & GPSPI2_USER_USR_MOSI) != 0;
  const bool receive = (s->user & GPSPI2_USER_USR_MISO) != 0;
  uint8_t* data = g_malloc0(total_bytes);
  bool ok = true;
  bool conflict = false;
  uint8_t tx_sample[GPSPI2_CONTROL_TRACE_SAMPLE_BYTES] = {0};
  uint8_t rx_sample[GPSPI2_CONTROL_TRACE_SAMPLE_BYTES] = {0};
  if (transmit && (s->dma_conf & GPSPI2_DMA_CONF_DMA_TX_ENA) != 0) {
    uint32_t channel = 0;
    ok = esp_gdma_get_channel_periph(s->gdma, GDMA_SPI2, ESP_GDMA_OUT_IDX, &channel) &&
         esp_gdma_read_channel(s->gdma, channel, data, total_bytes);
  } else if (transmit) {
    ok = total_bytes <= GPSPI2_FIFO_BYTES;
    for (uint32_t i = 0; ok && i < total_bytes; ++i) {
      data[i] = gpspi2_fifo_get_byte(s, i);
    }
  } else {
    memset(data, 0xFF, total_bytes);
  }

  memcpy(tx_sample, data, MIN(total_bytes, GPSPI2_CONTROL_TRACE_SAMPLE_BYTES));
  for (uint32_t i = 0; ok && i < total_bytes; ++i) {
    data[i] = gpspi2_transfer_byte(s, data[i], &conflict);
    ok = !conflict;
  }
  memcpy(rx_sample, data, MIN(total_bytes, GPSPI2_CONTROL_TRACE_SAMPLE_BYTES));

  if (ok && receive && (s->dma_conf & GPSPI2_DMA_CONF_DMA_RX_ENA) != 0) {
    uint32_t channel = 0;
    ok = esp_gdma_get_channel_periph(s->gdma, GDMA_SPI2, ESP_GDMA_IN_IDX, &channel) &&
         esp_gdma_write_channel(s->gdma, channel, data, total_bytes);
  } else if (ok && receive) {
    ok = total_bytes <= GPSPI2_FIFO_BYTES;
    for (uint32_t i = 0; ok && i < total_bytes; ++i) {
      gpspi2_fifo_set_byte(s, i, data[i]);
    }
  }

  gpspi2_record_transfer(s, true, transmit ? total_bytes : 0, receive ? total_bytes : 0, tx_sample, rx_sample, ok);
  g_free(data);
  return ok;
}

static void execute_usr_transfer(ESP32S3GPSPI2State* s) {
  const uint32_t total_bytes = gpspi2_transfer_bytes(s);
  if (total_bytes == 0) {
    s->cmd &= ~GPSPI2_CMD_USR;
    return;
  }

  const bool hardware_transaction = gpspi2_begin_hardware_transaction(s);
  bool ok = false;
  if (s->dma_conf & (GPSPI2_DMA_CONF_DMA_TX_ENA | GPSPI2_DMA_CONF_DMA_RX_ENA)) {
    ok = gpspi2_transfer_dma(s, total_bytes);
  } else {
    ok = gpspi2_transfer_fifo(s, total_bytes);
  }

  s->dma_int_raw |= ok ? GPSPI2_DMA_INT_TRANS_DONE : GPSPI2_DMA_INT_RX_OVERFLOW | GPSPI2_DMA_INT_TX_UNDERFLOW;
  s->cmd &= ~GPSPI2_CMD_USR;
  gpspi2_update_irq(s);
  if (hardware_transaction) {
    gpspi2_finish_hardware_transaction(s);
  }
}

/* ---- MMIO read/write ---------------------------------------------------- */

static uint64_t gpspi2_read(void* opaque, hwaddr offset, unsigned size) {
  ESP32S3GPSPI2State* s = ESP32S3_GPSPI2(opaque);

  switch (offset) {
    case GPSPI2_CMD:
      return s->cmd;
    case GPSPI2_ADDR:
      return s->addr_reg;
    case GPSPI2_CTRL:
      return s->ctrl;
    case GPSPI2_CLOCK:
      return s->clock_reg;
    case GPSPI2_USER:
      return s->user;
    case GPSPI2_USER1:
      return s->user1;
    case GPSPI2_USER2:
      return s->user2;
    case GPSPI2_MS_DLEN:
      return s->ms_dlen;
    case GPSPI2_MISC:
      return s->misc;
    case GPSPI2_DMA_CONF:
      return s->dma_conf;
    case GPSPI2_DMA_INT_ENA:
      return s->dma_int_ena;
    case GPSPI2_DMA_INT_RAW:
      return s->dma_int_raw;
    case GPSPI2_DMA_INT_ST:
      return s->dma_int_raw & s->dma_int_ena;
    default:
      if (offset >= GPSPI2_DATA_BUF(0) && offset <= GPSPI2_DATA_BUF(15)) {
        uint32_t idx = (offset - GPSPI2_DATA_BUF(0)) / 4;
        return s->w[idx];
      }
      qemu_log_mask(LOG_UNIMP, "gpspi2: unhandled read at offset 0x%03X\n", (uint32_t)offset);
      return 0;
  }
}

static void gpspi2_write(void* opaque, hwaddr offset, uint64_t value, unsigned size) {
  ESP32S3GPSPI2State* s = ESP32S3_GPSPI2(opaque);

  /* Handle sub-word writes to the data buffer (W0-W15) region.
   * Guest firmware may write individual bytes via s8i instructions. */
  if (offset >= GPSPI2_DATA_BUF(0) && offset <= GPSPI2_DATA_BUF(15) + 3) {
    uint32_t word_off = offset - GPSPI2_DATA_BUF(0);
    uint32_t idx = word_off / 4;
    uint32_t byte_off = word_off % 4;
    if (idx < GPSPI2_FIFO_WORDS) {
      if (size == 1) {
        s->w[idx] = (s->w[idx] & ~(0xFFu << (byte_off * 8))) | ((uint32_t)(value & 0xFF) << (byte_off * 8));
      } else if (size == 2) {
        s->w[idx] = (s->w[idx] & ~(0xFFFFu << (byte_off * 8))) | ((uint32_t)(value & 0xFFFF) << (byte_off * 8));
      } else {
        s->w[idx] = (uint32_t)value;
      }
      return;
    }
  }

  /* All other registers: only handle word-aligned, 4-byte accesses */
  if (size != 4 || (offset & 3) != 0) {
    qemu_log_mask(LOG_UNIMP, "gpspi2: sub-word write at offset 0x%03X = 0x%08X (size=%u)\n", (uint32_t)offset,
                  (uint32_t)value, size);
    return;
  }

  switch (offset) {
    case GPSPI2_CMD:
      s->cmd = (uint32_t)value;
      if (value & GPSPI2_CMD_UPDATE) {
        s->cmd &= ~GPSPI2_CMD_UPDATE;
      }
      if (value & GPSPI2_CMD_USR) {
        execute_usr_transfer(s);
      }
      break;
    case GPSPI2_ADDR:
      s->addr_reg = (uint32_t)value;
      break;
    case GPSPI2_CTRL:
      s->ctrl = (uint32_t)value;
      break;
    case GPSPI2_CLOCK:
      s->clock_reg = (uint32_t)value;
      break;
    case GPSPI2_USER:
      s->user = (uint32_t)value;
      break;
    case GPSPI2_USER1:
      s->user1 = (uint32_t)value;
      break;
    case GPSPI2_USER2:
      s->user2 = (uint32_t)value;
      break;
    case GPSPI2_MS_DLEN:
      s->ms_dlen = (uint32_t)value;
      break;
    case GPSPI2_MISC:
      s->misc = (uint32_t)value;
      break;
    case GPSPI2_DMA_CONF:
      s->dma_conf = (uint32_t)value;
      break;
    case GPSPI2_DMA_INT_ENA:
      s->dma_int_ena = (uint32_t)value;
      gpspi2_update_irq(s);
      break;
    case GPSPI2_DMA_INT_CLR:
      s->dma_int_raw &= ~((uint32_t)value);
      gpspi2_update_irq(s);
      break;
    case GPSPI2_DMA_INT_SET:
      s->dma_int_raw |= (uint32_t)value;
      gpspi2_update_irq(s);
      break;
    default:
      qemu_log_mask(LOG_UNIMP, "gpspi2: unhandled write at offset 0x%03X = 0x%08X\n", (uint32_t)offset,
                    (uint32_t)value);
      break;
  }
}

static const MemoryRegionOps gpspi2_ops = {
    .read = gpspi2_read,
    .write = gpspi2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 1,
            .max_access_size = 4,
        },
};

/* ---- QEMU device plumbing ----------------------------------------------- */

static void gpspi2_realize(DeviceState* dev, Error** errp) {
  ESP32S3GPSPI2State* s = ESP32S3_GPSPI2(dev);

  g_gpspi2 = s;
  s->spi_bus = ssi_create_bus(dev, "spi");

  memory_region_init_io(&s->iomem, OBJECT(dev), &gpspi2_ops, s, TYPE_ESP32S3_GPSPI2, GPSPI2_IO_SIZE);
  sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
  sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void gpspi2_set_chip_select(void* opaque, int target, int level) {
  ESP32S3GPSPI2State* s = ESP32S3_GPSPI2(opaque);
  if (target < 0 || target >= GPSPI2_TARGET_COUNT) {
    return;
  }
  const bool was_high = s->chip_select[target];
  const bool is_high = level != 0;
  if (was_high == is_high) return;

  s->chip_select[target] = is_high;
  if (!is_high) {
    if (s->active_target == -1) {
      gpspi2_reset_transaction(s);
      if (s->conflict_fault_latched || s->conflict_fault_once) {
        s->active_target = -2;
        s->transaction_fault = GPSPI2_CONTROL_FAULT_CONFLICT;
        s->conflict_fault_once = false;
      } else {
        s->active_target = target;
        qemu_set_irq(s->target_select[target], 0);
        return;
      }
    } else if (s->active_target != target) {
      s->active_target = -2;
    }
    /* A conflict never leaves either endpoint selected. */
    gpspi2_abort_endpoints(s);
    return;
  }

  qemu_set_irq(s->target_select[target], 1);
  if (s->chip_select[GPSPI2_TARGET_SD] && s->chip_select[GPSPI2_TARGET_RADIO]) {
    gpspi2_finish_transaction(s, GPSPI2_CONTROL_RESULT_OK);
  }
}

static void gpspi2_init(Object* obj) {
  DeviceState* dev = DEVICE(obj);
  ESP32S3GPSPI2State* s = ESP32S3_GPSPI2(obj);
  s->active_target = -1;
  for (int i = 0; i < GPSPI2_TARGET_COUNT; ++i) s->chip_select[i] = true;
  qdev_init_gpio_in_named(dev, gpspi2_set_chip_select, "chip-select", GPSPI2_TARGET_COUNT);
  qdev_init_gpio_out_named(dev, s->target_select, "target-select", GPSPI2_TARGET_COUNT);
}

static void gpspi2_reset_hold(Object* obj, ResetType type) {
  ESP32S3GPSPI2State* s = ESP32S3_GPSPI2(obj);
  if (s->route_chip_selects && s->active_target != -1) {
    gpspi2_finish_transaction(s, GPSPI2_CONTROL_RESULT_RESET_ABORT);
    gpspi2_abort_endpoints(s);
  }
  memset(s->w, 0, sizeof(s->w));
  s->cmd = 0;
  s->addr_reg = 0;
  s->ctrl = 0;
  s->clock_reg = 0;
  s->user = 0;
  s->user1 = 0;
  s->user2 = 0;
  s->ms_dlen = 0;
  s->misc = 0;
  s->dma_conf = 0;
  s->dma_int_ena = 0;
  s->dma_int_raw = 0;
  s->conflict_fault_latched = false;
  s->conflict_fault_once = false;
  s->active_target = -1;
  s->trace_sequence = 0;
  gpspi2_reset_transaction(s);
  for (int i = 0; i < GPSPI2_TARGET_COUNT; ++i) {
    s->chip_select[i] = true;
    qemu_set_irq(s->target_select[i], 1);
  }
  gpspi2_update_irq(s);
}

static int gpspi2_post_load(void* opaque, int version_id) {
  ESP32S3GPSPI2State* s = ESP32S3_GPSPI2(opaque);
  (void)version_id;
  for (int i = 0; i < GPSPI2_TARGET_COUNT; ++i) qemu_set_irq(s->target_select[i], 1);
  if (s->active_target >= 0) qemu_set_irq(s->target_select[s->active_target], 0);
  gpspi2_update_irq(s);
  return 0;
}

static const VMStateDescription vmstate_gpspi2 = {
    .name = TYPE_ESP32S3_GPSPI2,
    .version_id = 3,
    .minimum_version_id = 2,
    .post_load = gpspi2_post_load,
    .fields = (VMStateField[]){
        VMSTATE_UINT32(cmd, ESP32S3GPSPI2State),
        VMSTATE_UINT32(addr_reg, ESP32S3GPSPI2State),
        VMSTATE_UINT32(ctrl, ESP32S3GPSPI2State),
        VMSTATE_UINT32(clock_reg, ESP32S3GPSPI2State),
        VMSTATE_UINT32(user, ESP32S3GPSPI2State),
        VMSTATE_UINT32(user1, ESP32S3GPSPI2State),
        VMSTATE_UINT32(user2, ESP32S3GPSPI2State),
        VMSTATE_UINT32(ms_dlen, ESP32S3GPSPI2State),
        VMSTATE_UINT32(misc, ESP32S3GPSPI2State),
        VMSTATE_UINT32(dma_conf, ESP32S3GPSPI2State),
        VMSTATE_UINT32(dma_int_ena, ESP32S3GPSPI2State),
        VMSTATE_UINT32(dma_int_raw, ESP32S3GPSPI2State),
        VMSTATE_BOOL(route_chip_selects, ESP32S3GPSPI2State),
        VMSTATE_BOOL_ARRAY(chip_select, ESP32S3GPSPI2State, GPSPI2_TARGET_COUNT),
        VMSTATE_BOOL(conflict_fault_latched, ESP32S3GPSPI2State),
        VMSTATE_BOOL(conflict_fault_once, ESP32S3GPSPI2State),
        VMSTATE_INT8(active_target, ESP32S3GPSPI2State),
        VMSTATE_UINT64(trace_sequence, ESP32S3GPSPI2State),
        VMSTATE_UINT32(transaction_tx_bytes, ESP32S3GPSPI2State),
        VMSTATE_UINT32(transaction_rx_bytes, ESP32S3GPSPI2State),
        VMSTATE_UINT8_ARRAY(transaction_tx_sample, ESP32S3GPSPI2State, GPSPI2_CONTROL_TRACE_SAMPLE_BYTES),
        VMSTATE_UINT8_ARRAY(transaction_rx_sample, ESP32S3GPSPI2State, GPSPI2_CONTROL_TRACE_SAMPLE_BYTES),
        VMSTATE_BOOL(transaction_dma, ESP32S3GPSPI2State),
        VMSTATE_BOOL(transaction_error, ESP32S3GPSPI2State),
        VMSTATE_UINT8(transaction_fault, ESP32S3GPSPI2State),
        VMSTATE_UINT8_V(transaction_command, ESP32S3GPSPI2State, 3),
        VMSTATE_UINT32_V(transaction_block_argument, ESP32S3GPSPI2State, 3),
        VMSTATE_UINT32_ARRAY(w, ESP32S3GPSPI2State, GPSPI2_FIFO_WORDS),
        VMSTATE_END_OF_LIST()}};

static Property gpspi2_properties[] = {
    DEFINE_PROP_LINK("gdma", ESP32S3GPSPI2State, gdma, TYPE_ESP_GDMA, ESPGdmaState*),
    DEFINE_PROP_BOOL("route-chip-selects", ESP32S3GPSPI2State, route_chip_selects, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void gpspi2_class_init(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);
  ResettableClass* rc = RESETTABLE_CLASS(klass);
  dc->realize = gpspi2_realize;
  rc->phases.hold = gpspi2_reset_hold;
  device_class_set_props(dc, gpspi2_properties);
  dc->vmsd = &vmstate_gpspi2;
  dc->desc = "ESP32-S3 GPSPI2 (SPI2) minimal controller for Mofei simulator";
}

static const TypeInfo gpspi2_info = {
    .name = TYPE_ESP32S3_GPSPI2,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3GPSPI2State),
    .instance_init = gpspi2_init,
    .class_init = gpspi2_class_init,
};

static void gpspi2_register_types(void) { type_register_static(&gpspi2_info); }

type_init(gpspi2_register_types)
