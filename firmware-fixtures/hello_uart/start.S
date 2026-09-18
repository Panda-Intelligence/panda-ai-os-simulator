/*
 * Minimal ESP32-S3 UART test for QEMU.
 * No window register manipulation — just set VECBASE and write to UART.
 */

#define UART0_FIFO  0x60000000
#define STACK_TOP   0x3FD88000

    .section .text, "ax", @progbits

    .balign 4
_litpool:
    .word STACK_TOP
    .word UART0_FIFO

    .globl _start
    .type _start, @function

    .balign 4
_start:
    /* Set stack pointer */
    l32r    a1, _litpool

    /* Write 'X' to UART0 immediately */
    l32r    a8, _litpool + 4
    movi    a9, 0x58              /* 'X' */
    s32i    a9, a8, 0
    memw

    /* Set VECBASE to our handler table */
    movi    a2, 0x4037
    slli    a2, a2, 16
    movi    a3, .Lvecs - 0x40370000
    add     a2, a2, a3
    wsr     a2, vecbase
    rsync

    /* Don't touch WINDOWSTART/WINDOWBASE — keep reset defaults */

    /* Write "HELLO\n" */
    movi    a9, 0x48              /* 'H' */
    s32i    a9, a8, 0
    memw
    movi    a9, 0x45              /* 'E' */
    s32i    a9, a8, 0
    memw
    movi    a9, 0x4C              /* 'L' */
    s32i    a9, a8, 0
    memw
    movi    a9, 0x4C              /* 'L' */
    s32i    a9, a8, 0
    memw
    movi    a9, 0x4F              /* 'O' */
    s32i    a9, a8, 0
    memw
    movi    a9, 0x0A              /* '\n' */
    s32i    a9, a8, 0
    memw

    /* Infinite loop writing dots */
.Lloop:
    movi    a3, 1000000
.Ldelay:
    addi    a3, a3, -1
    bnez    a3, .Ldelay

    movi    a9, 0x2E              /* '.' */
    s32i    a9, a8, 0
    memw

    j       .Lloop

    .size _start, . - _start

/* Exception vector table (1024-byte aligned) */
    .balign 1024
.Lvecs:
    retw; .fill 61, 1, 0     /* 0x000 WindowOverflow4 */
    retw; .fill 61, 1, 0     /* 0x040 WindowOverflow8 */
    retw; .fill 61, 1, 0     /* 0x080 WindowOverflow12 */
    retw; .fill 61, 1, 0     /* 0x0C0 WindowUnderflow4 */
    retw; .fill 61, 1, 0     /* 0x100 WindowUnderflow8 */
    retw; .fill 61, 1, 0     /* 0x140 WindowUnderflow12 */
    rfi 2; .fill 61, 1, 0    /* 0x180 IntLevel2 */
    rfi 3; .fill 61, 1, 0    /* 0x1C0 IntLevel3 */
    rfi 4; .fill 61, 1, 0    /* 0x200 IntLevel4 */
    rfi 5; .fill 61, 1, 0    /* 0x240 IntLevel5 */
    .fill 64, 1, 0           /* 0x280 */
    .fill 64, 1, 0           /* 0x2C0 */
    rfe; .fill 61, 1, 0      /* 0x300 Kernel */
    rfe; .fill 61, 1, 0      /* 0x340 User */
    rfe; .fill 61, 1, 0      /* 0x380 DoubleException */
    .fill 64, 1, 0           /* 0x3C0 */
