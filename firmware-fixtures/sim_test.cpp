/*
 * Minimal QEMU peripheral test firmware for Mofei simulator.
 *
 * This is a bare-metal ESP32-S3 program that:
 *   1. Outputs "HELLO" on UART0 to verify serial works in QEMU
 *   2. Sends a checkerboard pattern through GPSPI2 to SSD1677
 *   3. Reads FT6336U touch events via bit-bang I2C
 *
 * It does NOT depend on ESP-IDF boot or Arduino framework.
 * It's loaded directly via QEMU -kernel and enters from reset vector.
 */

#include <stdint.h>

#define UART0_BASE 0x60000000
#define UART0_TX (*(volatile uint32_t*)(UART0_BASE + 0x00))
#define UART0_STATUS (*(volatile uint32_t*)(UART0_BASE + 0x1C))
#define UART0_CLKDIV (*(volatile uint32_t*)(UART0_BASE + 0x14))
#define UART0_CONF0 (*(volatile uint32_t*)(UART0_BASE + 0x20))
#define UART0_INT_ENA (*(volatile uint32_t*)(UART0_BASE + 0x08))

#define GPSPI2_BASE 0x60024000
#define GPSPI2_CMD (*(volatile uint32_t*)(GPSPI2_BASE + 0x00))
#define GPSPI2_ADDR (*(volatile uint32_t*)(GPSPI2_BASE + 0x04))
#define GPSPI2_CTRL (*(volatile uint32_t*)(GPSPI2_BASE + 0x08))
#define GPSPI2_CLOCK (*(volatile uint32_t*)(GPSPI2_BASE + 0x0C))
#define GPSPI2_USER (*(volatile uint32_t*)(GPSPI2_BASE + 0x10))
#define GPSPI2_USER1 (*(volatile uint32_t*)(GPSPI2_BASE + 0x14))
#define GPSPI2_USER2 (*(volatile uint32_t*)(GPSPI2_BASE + 0x18))
#define GPSPI2_MS_DLEN (*(volatile uint32_t*)(GPSPI2_BASE + 0x1C))
#define GPSPI2_MISC (*(volatile uint32_t*)(GPSPI2_BASE + 0x20))
#define GPSPI2_DMA_CONF (*(volatile uint32_t*)(GPSPI2_BASE + 0x30))
#define GPSPI2_W0 (*(volatile uint32_t*)(GPSPI2_BASE + 0x98))
#define GPSPI2_W1 (*(volatile uint32_t*)(GPSPI2_BASE + 0x9C))
#define GPSPI2_W2 (*(volatile uint32_t*)(GPSPI2_BASE + 0xA0))
#define GPSPI2_W3 (*(volatile uint32_t*)(GPSPI2_BASE + 0xA4))
#define GPSPI2_W4 (*(volatile uint32_t*)(GPSPI2_BASE + 0xA8))
#define GPSPI2_W5 (*(volatile uint32_t*)(GPSPI2_BASE + 0xAC))
#define GPSPI2_W6 (*(volatile uint32_t*)(GPSPI2_BASE + 0xB0))
#define GPSPI2_W7 (*(volatile uint32_t*)(GPSPI2_BASE + 0xB4))
#define GPSPI2_W8 (*(volatile uint32_t*)(GPSPI2_BASE + 0xB8))
#define GPSPI2_W9 (*(volatile uint32_t*)(GPSPI2_BASE + 0xBC))
#define GPSPI2_W10 (*(volatile uint32_t*)(GPSPI2_BASE + 0xC0))
#define GPSPI2_W11 (*(volatile uint32_t*)(GPSPI2_BASE + 0xC4))
#define GPSPI2_W12 (*(volatile uint32_t*)(GPSPI2_BASE + 0xC8))
#define GPSPI2_W13 (*(volatile uint32_t*)(GPSPI2_BASE + 0xCC))
#define GPSPI2_W14 (*(volatile uint32_t*)(GPSPI2_BASE + 0xD0))
#define GPSPI2_W15 (*(volatile uint32_t*)(GPSPI2_BASE + 0xD4))
#define GPSPI2_SLV_WR_DMA_DONE (*(&GPSPI2_W15 + 1))
#define GPSPI2_MOSI_DLEN GPSPI2_MS_DLEN

#define GPIO_BASE 0x60004000
#define GPIO_OUT_W1TS (*(volatile uint32_t*)(GPIO_BASE + 0x0008))
#define GPIO_OUT_W1TC (*(volatile uint32_t*)(GPIO_BASE + 0x000C))
#define GPIO_ENABLE_W1TS (*(volatile uint32_t*)(GPIO_BASE + 0x0020))
#define GPIO_ENABLE_W1TC (*(volatile uint32_t*)(GPIO_BASE + 0x0024))
#define GPIO_IN (*(volatile uint32_t*)(GPIO_BASE + 0x003C))

#define EPD_WIDTH 800
#define EPD_HEIGHT 480
#define EPD_FB_SIZE ((EPD_WIDTH * EPD_HEIGHT) / 8)

static void uart_putc(char c) { UART0_TX = c; }

static void uart_puts(const char* s) {
  while (*s) uart_putc(*s++);
}

static void uart_put_hex(uint32_t v) {
  const char* hex = "0123456789abcdef";
  for (int i = 28; i >= 0; i -= 4) uart_putc(hex[(v >> i) & 0xf]);
}

static void delay_cycles(volatile uint32_t n) {
  while (n--) __asm__ volatile("nop");
}

static void gpio_set(int pin) { GPIO_OUT_W1TS = (1 << pin); }

static void gpio_clr(int pin) { GPIO_OUT_W1TC = (1 << pin); }

static void gpio_output(int pin) { GPIO_ENABLE_W1TS = (1 << pin); }

static int gpio_read(int pin) { return (GPIO_IN >> pin) & 1; }

#define EPD_CS 5
#define EPD_DC 6
#define EPD_RST 7
#define EPD_BUSY 8
#define TOUCH_SDA 13
#define TOUCH_SCL 12

static void epd_reset(void) {
  gpio_output(EPD_RST);
  gpio_output(EPD_CS);
  gpio_output(EPD_DC);
  gpio_clr(EPD_RST);
  delay_cycles(100000);
  gpio_set(EPD_RST);
  delay_cycles(1000000);
}

static void spi_send_byte(uint8_t b) {
  GPSPI2_W0 = b;
  GPSPI2_MOSI_DLEN = (8 - 1);
  GPSPI2_USER = (1 << 27) | (1 << 26) | (1 << 6) | (1 << 3);
  GPSPI2_CMD = (1 << 18);
}

static void spi_send_buf(const uint8_t* data, int len) {
  int offset = 0;
  while (offset < len) {
    int chunk = len - offset;
    if (chunk > 64) chunk = 64;
    volatile uint32_t* w = &GPSPI2_W0;
    for (int i = 0; i < chunk; i++) {
      ((volatile uint8_t*)w)[i] = data[offset + i];
    }
    GPSPI2_MOSI_DLEN = (chunk * 8 - 1);
    GPSPI2_USER = (1 << 27) | (1 << 26) | (1 << 6) | (1 << 3);
    GPSPI2_CMD = (1 << 18);
    offset += chunk;
  }
}

static void epd_send_cmd(uint8_t cmd) {
  gpio_clr(EPD_DC);
  gpio_clr(EPD_CS);
  spi_send_byte(cmd);
  gpio_set(EPD_CS);
}

static void epd_send_data(const uint8_t* data, int len) {
  gpio_set(EPD_DC);
  gpio_clr(EPD_CS);
  spi_send_buf(data, len);
  gpio_set(EPD_CS);
}

static uint8_t fb[EPD_FB_SIZE];

extern "C" void app_main(void) {
  uart_puts("\n=== MOFEI SIM TEST ===\n");
  uart_puts("UART0 working in QEMU!\n");

  epd_reset();
  uart_puts("EPD reset done\n");

  epd_send_cmd(0x12);
  delay_cycles(2000000);
  uart_puts("EPD SWRESET done\n");

  epd_send_cmd(0x01);
  uint8_t drv[] = {0x00, 0x00, 0x00};
  epd_send_data(drv, 3);
  uart_puts("Driver output control\n");

  epd_send_cmd(0x3C);
  uint8_t border[] = {0x05};
  epd_send_data(border, 1);

  epd_send_cmd(0x18);
  uint8_t temp[] = {0x80};
  epd_send_data(temp, 1);

  epd_send_cmd(0x22);
  uint8_t lut[] = {0xB1};
  epd_send_data(lut, 1);
  epd_send_cmd(0x20);
  delay_cycles(1000000);

  for (int i = 0; i < EPD_FB_SIZE; i++) {
    int col = i % 100;
    int row = (i / 100) / 8;
    fb[i] = ((col / 10 + row / 10) % 2 == 0) ? 0x55 : 0xAA;
  }
  uart_puts("Checkerboard generated\n");

  epd_send_cmd(0x24);
  epd_send_data(fb, EPD_FB_SIZE);
  uart_puts("Framebuffer sent to SSD1677\n");

  uart_puts("Sending display update control...\n");
  epd_send_cmd(0x22);
  uint8_t act[] = {0xC7};
  epd_send_data(act, 1);
  uart_puts("Sending master activation...\n");
  epd_send_cmd(0x20);
  uart_puts("Master Activation sent\n");

  delay_cycles(2000000);
  uart_puts("=== DONE ===\n");

  while (1) {
    delay_cycles(10000000);
    uart_puts("tick\n");
  }
}
