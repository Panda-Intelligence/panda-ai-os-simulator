/* Minimal ESP32-S3 bare-metal UART hello world for QEMU testing.
   Sets up stack and writes to UART0 directly. */

#include <stdint.h>

/* Place stack at top of DRAM */
#define DRAM_BASE 0x3FC88000UL
#define DRAM_SIZE 0x100000UL
#define STACK_TOP (DRAM_BASE + DRAM_SIZE)

/* UART0 registers at 0x60000000 */
#define UART0_BASE 0x60000000UL
#define UART_FIFO (*(volatile uint32_t*)(UART0_BASE + 0x00))
#define UART_STATUS (*(volatile uint32_t*)(UART0_BASE + 0x1C))

static void uart_putc(char c) {
  for (volatile int i = 0; i < 100; i++);
  UART_FIFO = (uint32_t)c;
}

static void uart_puts(const char* s) {
  while (*s) uart_putc(*s++);
}

void _start(void) {
  /* Set up stack pointer before any function calls */
  __asm__ volatile("movi a1, %0" ::"i"(STACK_TOP));
  __asm__ volatile("movi sp, %0" ::"i"(STACK_TOP));

  /* Write 'X' to UART0 FIFO directly — bypass everything */
  UART_FIFO = (uint32_t)'X';
  UART_FIFO = (uint32_t)'X';
  UART_FIFO = (uint32_t)'X';

  uart_puts("HELLO QEMU\n");

  while (1) {
    for (volatile int i = 0; i < 10000000; i++);
    UART_FIFO = (uint32_t)'.';
  }
}
