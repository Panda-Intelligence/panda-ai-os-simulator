/*
 * ESP32 GPIO emulation
 *
 * Copyright (c) 2019 Espressif Systems (Shanghai) Co. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32_gpio.h"

#define DR_REG_GPIO_BASE_OFFSET 0

#define GPIO_OUT_REG 0x04
#define GPIO_OUT_W1TS_REG 0x08
#define GPIO_OUT_W1TC_REG 0x0C
#define GPIO_OUT1_REG 0x10
#define GPIO_OUT1_W1TS_REG 0x14
#define GPIO_OUT1_W1TC_REG 0x18
#define GPIO_ENABLE_REG 0x20
#define GPIO_ENABLE_W1TS_REG 0x24
#define GPIO_ENABLE_W1TC_REG 0x28
#define GPIO_ENABLE1_REG 0x2C
#define GPIO_ENABLE1_W1TS_REG 0x30
#define GPIO_ENABLE1_W1TC_REG 0x34
#define GPIO_IN_REG 0x3C
#define GPIO_IN1_REG 0x40

static void esp32_gpio_update_output(Esp32GpioState* s) {
  uint64_t combined = (uint64_t)s->out1_reg << 32 | s->out_reg;
  for (int i = 0; i < ESP32_GPIO_PIN_COUNT; i++) {
    int level = (combined >> i) & 1;
    qemu_set_irq(s->out_irq[i], level);
  }
}

static void esp32_gpio_set_pin_input(void* opaque, int pin, int level) {
  Esp32GpioState* s = ESP32_GPIO(opaque);
  if (pin < 32) {
    if (level) {
      s->in_reg |= (1U << pin);
    } else {
      s->in_reg &= ~(1U << pin);
    }
  } else if (pin < ESP32_GPIO_PIN_COUNT) {
    int bit = pin - 32;
    if (level) {
      s->in1_reg |= (1U << bit);
    } else {
      s->in1_reg &= ~(1U << bit);
    }
  }
}

static uint64_t esp32_gpio_read(void* opaque, hwaddr addr, unsigned int size) {
  Esp32GpioState* s = ESP32_GPIO(opaque);
  uint64_t r = 0;
  switch (addr) {
    case A_GPIO_STRAP:
      r = s->strap_mode;
      break;
    case GPIO_OUT_REG:
      r = s->out_reg;
      break;
    case GPIO_OUT1_REG:
      r = s->out1_reg;
      break;
    case GPIO_IN_REG:
      r = s->in_reg;
      break;
    case GPIO_IN1_REG:
      r = s->in1_reg;
      break;
    case GPIO_ENABLE_REG:
      r = s->enable_reg;
      break;
    case GPIO_ENABLE1_REG:
      r = s->enable1_reg;
      break;
    default:
      break;
  }
  return r;
}

static void esp32_gpio_write(void* opaque, hwaddr addr, uint64_t value, unsigned int size) {
  Esp32GpioState* s = ESP32_GPIO(opaque);
  switch (addr) {
    case GPIO_OUT_REG:
      s->out_reg = value;
      esp32_gpio_update_output(s);
      break;
    case GPIO_OUT_W1TS_REG:
      s->out_reg |= value;
      esp32_gpio_update_output(s);
      break;
    case GPIO_OUT_W1TC_REG:
      s->out_reg &= ~value;
      esp32_gpio_update_output(s);
      break;
    case GPIO_OUT1_REG:
      s->out1_reg = value;
      esp32_gpio_update_output(s);
      break;
    case GPIO_OUT1_W1TS_REG:
      s->out1_reg |= value;
      esp32_gpio_update_output(s);
      break;
    case GPIO_OUT1_W1TC_REG:
      s->out1_reg &= ~value;
      esp32_gpio_update_output(s);
      break;
    case GPIO_ENABLE_REG:
      s->enable_reg = value;
      break;
    case GPIO_ENABLE_W1TS_REG:
      s->enable_reg |= value;
      break;
    case GPIO_ENABLE_W1TC_REG:
      s->enable_reg &= ~value;
      break;
    case GPIO_ENABLE1_REG:
      s->enable1_reg = value;
      break;
    case GPIO_ENABLE1_W1TS_REG:
      s->enable1_reg |= value;
      break;
    case GPIO_ENABLE1_W1TC_REG:
      s->enable1_reg &= ~value;
      break;
    default:
      break;
  }
}

static const MemoryRegionOps uart_ops = {
    .read = esp32_gpio_read,
    .write = esp32_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32_gpio_reset_hold(Object* obj, ResetType type) {
  Esp32GpioState* s = ESP32_GPIO(obj);
  s->out_reg = 0;
  s->out1_reg = 0;
  s->enable_reg = 0;
  s->enable1_reg = 0;
  s->in_reg = 0;
  s->in1_reg = 0;
}

static void esp32_gpio_realize(DeviceState* dev, Error** errp) {}

void esp32_gpio_init_common(Object* obj, const char* region_name, uint32_t strap_mode) {
  Esp32GpioState* s = ESP32_GPIO(obj);
  SysBusDevice* sbd = SYS_BUS_DEVICE(obj);

  object_property_set_int(obj, "strap_mode", strap_mode, &error_fatal);

  memory_region_init_io(&s->iomem, obj, &uart_ops, s, region_name, 0x1000);
  sysbus_init_mmio(sbd, &s->iomem);
  sysbus_init_irq(sbd, &s->irq);

  qdev_init_gpio_in(DEVICE(obj), esp32_gpio_set_pin_input, ESP32_GPIO_PIN_COUNT);
  qdev_init_gpio_out_named(DEVICE(obj), s->out_irq, "gpio-out", ESP32_GPIO_PIN_COUNT);
}

static void esp32_gpio_init(Object* obj) { esp32_gpio_init_common(obj, TYPE_ESP32_GPIO, ESP32_STRAP_MODE_FLASH_BOOT); }

static Property esp32_gpio_properties[] = {
    DEFINE_PROP_UINT32("strap_mode", Esp32GpioState, strap_mode, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32_gpio_class_init(ObjectClass* klass, void* data) {
  DeviceClass* dc = DEVICE_CLASS(klass);
  ResettableClass* rc = RESETTABLE_CLASS(klass);

  rc->phases.hold = esp32_gpio_reset_hold;
  dc->realize = esp32_gpio_realize;
  device_class_set_props(dc, esp32_gpio_properties);
}

static const TypeInfo esp32_gpio_info = {
    .name = TYPE_ESP32_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Esp32GpioState),
    .instance_init = esp32_gpio_init,
    .class_init = esp32_gpio_class_init,
    .class_size = sizeof(Esp32GpioClass),
};

static void esp32_gpio_register_types(void) { type_register_static(&esp32_gpio_info); }

type_init(esp32_gpio_register_types)
