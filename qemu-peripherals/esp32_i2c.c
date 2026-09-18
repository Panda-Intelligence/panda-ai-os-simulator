#include "qemu/osdep.h"
#include "hw/i2c/esp32_i2c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/error-report.h"
#include "qemu/fifo8.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void esp32_i2c_do_transaction(Esp32I2CState* s);
static void esp32_i2c_update_irq(Esp32I2CState* s);
static uint32_t esp32s3_i2c_completion_event;
#define ESP32S3_I2C_DIRECT_BUS_COUNT 2
static I2CBus* esp32s3_i2c_direct_buses[ESP32S3_I2C_DIRECT_BUS_COUNT];
static size_t esp32s3_i2c_direct_bus_count;

uint32_t esp32s3_i2c_consume_completion_event(void) {
  const uint32_t event = esp32s3_i2c_completion_event;
  esp32s3_i2c_completion_event = 0;
  return event;
}

int esp32s3_i2c_direct_transfer(uint16_t address, const uint8_t* write_data, size_t write_size, uint8_t* read_data,
                                size_t read_size) {
  if ((write_size > 0 && write_data == NULL) || (read_size > 0 && read_data == NULL)) {
    return -1;
  }
  if (write_size == 0 && read_size == 0) return 0;

  /* direct bridge 可能早于首个 MMIO transaction；逐个 controller 探测，选择真正 ACK 该地址的总线。 */
  I2CBus* bus = NULL;
  const int initial_direction = write_size > 0 ? 0 : 1;
  for (size_t i = 0; i < esp32s3_i2c_direct_bus_count; ++i) {
    if (i2c_start_transfer(esp32s3_i2c_direct_buses[i], address, initial_direction) == 0) {
      bus = esp32s3_i2c_direct_buses[i];
      break;
    }
  }
  if (bus == NULL) return -1;

  if (write_size > 0) {
    for (size_t i = 0; i < write_size; ++i) {
      if (i2c_send(bus, write_data[i]) != 0) {
        i2c_end_transfer(bus);
        return -1;
      }
    }
    i2c_end_transfer(bus);
  }
  if (read_size > 0) {
    if (write_size > 0 && i2c_start_transfer(bus, address, 1) != 0) return -1;
    for (size_t i = 0; i < read_size; ++i) read_data[i] = i2c_recv(bus);
    i2c_end_transfer(bus);
  }
  return 0;
}

static void esp32_i2c_reset_hold(Object* obj, ResetType type) {
  Esp32I2CState* s = Esp32_I2C(obj);
  (void)type;

  if (s->trans_ongoing) i2c_end_transfer(s->bus);
  fifo8_reset(&s->rx_fifo);
  fifo8_reset(&s->tx_fifo);
  s->trans_ongoing = false;
  if (s->esp32s3_compat) esp32s3_i2c_completion_event = 0;
  s->ctr_reg = 0;
  s->timeout_reg = 0;
  s->int_ena_reg = 0;
  s->int_raw_reg = 0;
  s->sda_hold_reg = 0;
  s->sda_sample_reg = 0;
  s->high_period_reg = 0;
  s->low_period_reg = 0;
  s->start_hold_reg = 0;
  s->rstart_setup_reg = 0;
  s->stop_hold_reg = 0;
  s->stop_setup_reg = 0;
  memset(s->cmd_reg, 0, sizeof(s->cmd_reg));
  esp32_i2c_update_irq(s);
}

static uint32_t esp32_i2c_get_status_reg(Esp32I2CState* s) {
  uint32_t res = 0;
  res = FIELD_DP32(res, I2C_STATUS, BUS_BUSY, s->trans_ongoing);
  res = FIELD_DP32(res, I2C_STATUS, RXFIFO_CNT, fifo8_num_used(&s->rx_fifo));
  res = FIELD_DP32(res, I2C_STATUS, TXFIFO_CNT, fifo8_num_used(&s->tx_fifo));
  return res;
}

static void esp32_i2c_update_irq(Esp32I2CState* s) { qemu_set_irq(s->irq, !!(s->int_raw_reg & s->int_ena_reg)); }

static uint64_t esp32_i2c_read(void* opaque, hwaddr addr, unsigned size) {
  Esp32I2CState* s = Esp32_I2C(opaque);
  (void)size;

  switch (addr) {
    case A_I2C_CTR:
      return s->ctr_reg;
    case A_I2C_STATUS:
      return esp32_i2c_get_status_reg(s);
    case A_I2C_FIFO_DATA:
      if (fifo8_num_used(&s->rx_fifo) == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "esp32_i2c: read I2C FIFO while it is empty\n");
        return 0xee;
      }
      return fifo8_pop(&s->rx_fifo);
    case A_I2C_INT_RAW:
      return s->int_raw_reg;
    case A_I2C_INT_ENA:
      return s->int_ena_reg;
    case A_I2C_INT_ST:
      return s->int_raw_reg & s->int_ena_reg;
    case A_I2C_CMD ...(A_I2C_CMD + ESP32_I2C_CMD_COUNT * 4 - 1):
      return s->cmd_reg[(addr - A_I2C_CMD) / 4];
    case A_I2C_TIMEOUT:
      return s->timeout_reg;
    case A_I2C_SDA_HOLD:
      return s->sda_hold_reg;
    case A_I2C_SDA_SAMPLE:
      return s->sda_sample_reg;
    case A_I2C_HIGH_PERIOD:
      return s->high_period_reg;
    case A_I2C_LOW_PERIOD:
      return s->low_period_reg;
    case A_I2C_START_HOLD:
      return s->start_hold_reg;
    case A_I2C_RSTART_SETUP:
      return s->rstart_setup_reg;
    case A_I2C_STOP_HOLD:
      return s->stop_hold_reg;
    case A_I2C_STOP_SETUP:
      return s->stop_setup_reg;
    default:
      return 0;
  }
}

static void esp32_i2c_write(void* opaque, hwaddr addr, uint64_t value, unsigned size) {
  Esp32I2CState* s = Esp32_I2C(opaque);
  (void)size;

  switch (addr) {
    case A_I2C_CTR: {
      const uint32_t start_mask = R_I2C_CTR_TRANS_START_MASK;
      const uint32_t master_mask = R_I2C_CTR_MS_MODE_MASK;
      if ((value & master_mask) == 0) qemu_log_mask(LOG_GUEST_ERROR, "esp32_i2c: slave mode not implemented\n");
      if (s->esp32s3_compat && (value & BIT(10))) {
        if (s->trans_ongoing) i2c_end_transfer(s->bus);
        fifo8_reset(&s->rx_fifo);
        fifo8_reset(&s->tx_fifo);
        s->trans_ongoing = false;
      }
      if (value & start_mask) {
        esp32_i2c_do_transaction(s);
      }
      s->ctr_reg = value & ~start_mask;
      break;
    }
    case A_I2C_FIFO_CONF:
      if (FIELD_EX32(value, I2C_FIFO_CONF, NONFIFO_EN)) {
        qemu_log_mask(LOG_GUEST_ERROR, "esp32_i2c: APB mode not implemented\n");
      }
      if (FIELD_EX32(value, I2C_FIFO_CONF, RX_FIFO_RST)) fifo8_reset(&s->rx_fifo);
      if (FIELD_EX32(value, I2C_FIFO_CONF, TX_FIFO_RST)) fifo8_reset(&s->tx_fifo);
      break;
    case A_I2C_FIFO_DATA:
      if (fifo8_num_free(&s->tx_fifo) == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "esp32_i2c: write to full I2C TX FIFO\n");
      } else {
        fifo8_push(&s->tx_fifo, value);
      }
      break;
    case A_I2C_INT_CLR:
      s->int_raw_reg &= ~value;
      esp32_i2c_update_irq(s);
      break;
    case A_I2C_INT_ENA:
      s->int_ena_reg = value;
      esp32_i2c_update_irq(s);
      break;
    case A_I2C_CMD ...(A_I2C_CMD + ESP32_I2C_CMD_COUNT * 4 - 1):
      s->cmd_reg[(addr - A_I2C_CMD) / 4] = value;
      break;
    case A_I2C_TIMEOUT:
      s->timeout_reg = value;
      break;
    case A_I2C_SDA_HOLD:
      s->sda_hold_reg = value;
      break;
    case A_I2C_SDA_SAMPLE:
      s->sda_sample_reg = value;
      break;
    case A_I2C_HIGH_PERIOD:
      s->high_period_reg = value;
      break;
    case A_I2C_LOW_PERIOD:
      s->low_period_reg = value;
      break;
    case A_I2C_START_HOLD:
      s->start_hold_reg = value;
      break;
    case A_I2C_RSTART_SETUP:
      s->rstart_setup_reg = value;
      break;
    case A_I2C_STOP_HOLD:
      s->stop_hold_reg = value;
      break;
    case A_I2C_STOP_SETUP:
      s->stop_setup_reg = value;
      break;
    default:
      break;
  }
}

static uint8_t esp32_i2c_pop_tx(Esp32I2CState* s) {
  if (fifo8_num_used(&s->tx_fifo) == 0) {
    qemu_log_mask(LOG_GUEST_ERROR, "esp32_i2c: transaction drained empty TX FIFO\n");
    return 0xff;
  }
  return fifo8_pop(&s->tx_fifo);
}

static void esp32_i2c_do_transaction(Esp32I2CState* s) {
  static uint32_t debug_transaction_count;
  const int command_count = s->esp32s3_compat ? ESP32S3_I2C_CMD_COUNT : ESP32_I2C_CMD_COUNT;
  const int restart_opcode = s->esp32s3_compat ? ESP32S3_I2C_OPCODE_RSTART : I2C_OPCODE_RSTART;
  const int write_opcode = s->esp32s3_compat ? ESP32S3_I2C_OPCODE_WRITE : I2C_OPCODE_WRITE;
  const int read_opcode = s->esp32s3_compat ? ESP32S3_I2C_OPCODE_READ : I2C_OPCODE_READ;
  const int stop_opcode = s->esp32s3_compat ? ESP32S3_I2C_OPCODE_STOP : I2C_OPCODE_STOP;
  const int end_opcode = s->esp32s3_compat ? ESP32S3_I2C_OPCODE_END : I2C_OPCODE_END;
  bool terminal = false;
  if (s->esp32s3_compat) esp32s3_i2c_completion_event = 0;
  const bool debug_transaction = g_getenv("MOFEI_SIM_DEBUG_I2C") != NULL && debug_transaction_count++ < 64;
  if (debug_transaction) {
    fprintf(stderr, "[ESP32S3-I2C-DIAG] start tx_fifo=%u command_count=%d\n", fifo8_num_used(&s->tx_fifo),
            command_count);
  }

  for (int i = 0; i < command_count && !terminal; ++i) {
    uint32_t cmd = s->cmd_reg[i];
    const int opcode = FIELD_EX32(cmd, I2C_CMD, OPCODE);
    if (opcode == restart_opcode) {
      if (s->trans_ongoing) {
        i2c_end_transfer(s->bus);
        s->trans_ongoing = false;
      }
    } else if (opcode == write_opcode) {
      size_t length = FIELD_EX32(cmd, I2C_CMD, BYTE_NUM);
      if (!s->trans_ongoing && length > 0) {
        const uint8_t address = esp32_i2c_pop_tx(s);
        if (debug_transaction) {
          fprintf(stderr, "[ESP32S3-I2C-DIAG] address=0x%02x direction=%s length=%zu\n", address >> 1,
                  (address & 1) != 0 ? "read" : "write", length);
        }
        if (i2c_start_transfer(s->bus, address >> 1, address & 1) != 0) {
          if (s->esp32s3_compat) esp32s3_i2c_completion_event = 2;
          if (s->esp32s3_compat) {
            fprintf(stderr, "[ESP32S3-I2C] address=0x%02x direction=%s result=nack\n", address >> 1,
                    (address & 1) != 0 ? "read" : "write");
          }
          if (FIELD_EX32(cmd, I2C_CMD, ACK_CHECK_EN)) s->int_raw_reg |= BIT(10);
          s->cmd_reg[i] |= BIT(31);
          terminal = true;
          break;
        }
        s->trans_ongoing = true;
        s->int_raw_reg &= ~BIT(10);
        --length;
      }
      while (length-- > 0) {
        if (i2c_send(s->bus, esp32_i2c_pop_tx(s)) != 0 && FIELD_EX32(cmd, I2C_CMD, ACK_CHECK_EN)) {
          if (s->esp32s3_compat) esp32s3_i2c_completion_event = 2;
          s->int_raw_reg |= BIT(10);
          i2c_end_transfer(s->bus);
          s->trans_ongoing = false;
          terminal = true;
          break;
        }
      }
    } else if (opcode == read_opcode) {
      size_t length = FIELD_EX32(cmd, I2C_CMD, BYTE_NUM);
      while (length-- > 0) {
        if (fifo8_num_free(&s->rx_fifo) == 0) {
          qemu_log_mask(LOG_GUEST_ERROR, "esp32_i2c: RX FIFO overflow\n");
          break;
        }
        fifo8_push(&s->rx_fifo, i2c_recv(s->bus));
      }
    } else if (opcode == stop_opcode) {
      if (s->trans_ongoing) i2c_end_transfer(s->bus);
      s->trans_ongoing = false;
      s->int_raw_reg |= BIT(7);
      if (s->esp32s3_compat && esp32s3_i2c_completion_event == 0) esp32s3_i2c_completion_event = 1;
      terminal = true;
    } else if (opcode == end_opcode) {
      s->int_raw_reg |= BIT(3);
      if (s->esp32s3_compat && esp32s3_i2c_completion_event == 0) esp32s3_i2c_completion_event = 1;
      terminal = true;
    } else {
      qemu_log_mask(LOG_GUEST_ERROR, "esp32_i2c: invalid command %d opcode %d\n", i, opcode);
      terminal = true;
    }
    s->cmd_reg[i] |= BIT(31);
  }
  if (debug_transaction) {
    fprintf(stderr, "[ESP32S3-I2C-DIAG] finish event=%u int_raw=0x%08x rx_fifo=%u\n", esp32s3_i2c_completion_event,
            s->int_raw_reg, fifo8_num_used(&s->rx_fifo));
  }
  esp32_i2c_update_irq(s);
}

static const MemoryRegionOps esp32_i2c_ops = {
    .read = esp32_i2c_read,
    .write = esp32_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const Property esp32_i2c_properties[] = {
    DEFINE_PROP_BOOL("esp32s3-compat", Esp32I2CState, esp32s3_compat, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_i2c_init(Object* obj) {
  Esp32I2CState* s = Esp32_I2C(obj);
  SysBusDevice* sbd = SYS_BUS_DEVICE(obj);

  memory_region_init_io(&s->iomem, obj, &esp32_i2c_ops, s, TYPE_ESP32_I2C, ESP32_I2C_MEM_SIZE);
  sysbus_init_mmio(sbd, &s->iomem);
  sysbus_init_irq(sbd, &s->irq);
  s->bus = i2c_init_bus(DEVICE(s), "i2c");
  if (esp32s3_i2c_direct_bus_count < ESP32S3_I2C_DIRECT_BUS_COUNT) {
    esp32s3_i2c_direct_buses[esp32s3_i2c_direct_bus_count++] = s->bus;
  }
  fifo8_create(&s->tx_fifo, ESP32_I2C_FIFO_LENGTH);
  fifo8_create(&s->rx_fifo, ESP32_I2C_FIFO_LENGTH);
}

static void esp32_i2c_class_init(ObjectClass* klass, void* data) {
  ResettableClass* rc = RESETTABLE_CLASS(klass);
  DeviceClass* dc = DEVICE_CLASS(klass);
  (void)data;
  rc->phases.hold = esp32_i2c_reset_hold;
  device_class_set_props(dc, esp32_i2c_properties);
}

static const TypeInfo esp32_i2c_type_info = {
    .name = TYPE_ESP32_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32I2CState),
    .instance_init = esp32_i2c_init,
    .class_init = esp32_i2c_class_init,
};

static void esp32_i2c_register_types(void) { type_register_static(&esp32_i2c_type_info); }

type_init(esp32_i2c_register_types)
