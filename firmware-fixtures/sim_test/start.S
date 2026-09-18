/*
 * Startup code for bare-metal ESP32-S3 test firmware.
 * Sets up stack, window registers, VECBASE, zero-fills BSS,
 * then calls app_main() using windowed calling convention.
 *
 * Entry point loaded at 0x40370000 (IRAM) by QEMU -kernel.
 */

    .section .text, "ax", @progbits

    .balign 4
_litpool:
    .word 0x3FCF0000          /* stack top */
    .word __bss_start
    .word __bss_end

    .globl _start
    .type _start, @function
    .balign 4

_start:
    l32r    a1, _litpool      /* sp = 0x3FD88000 */

    movi    a2, 1
    wsr     a2, windowstart   /* only window 0 active */
    movi    a2, 0
    wsr     a2, windowbase
    rsync

    movi    a2, 0x4037
    slli    a2, a2, 16
    movi    a3, .Lvecs - 0x40370000
    add     a2, a2, a3
    wsr     a2, vecbase       /* exception vectors in IRAM */
    rsync

    l32r    a2, _litpool + 4  /* bss start */
    l32r    a3, _litpool + 8  /* bss end */
.Lzero_bss:
    bge     a2, a3, .Lbss_done
    s32i    a0, a2, 0
    addi    a2, a2, 4
    j       .Lzero_bss
.Lbss_done:

    mov     a9, a1            /* a9 = sp for call8 callee's a1 */
    call8   app_main

.Lhalt:
    waiti   0
    j       .Lhalt

    .size _start, . - _start

    .balign 1024
.Lvecs:
    retw; .fill 61, 1, 0     /* WindowOverflow4 */
    retw; .fill 61, 1, 0     /* WindowOverflow8 */
    retw; .fill 61, 1, 0     /* WindowOverflow12 */
    retw; .fill 61, 1, 0     /* WindowUnderflow4 */
    retw; .fill 61, 1, 0     /* WindowUnderflow8 */
    retw; .fill 61, 1, 0     /* WindowUnderflow12 */
    rfi 2; .fill 61, 1, 0    /* IntLevel2 */
    rfi 3; .fill 61, 1, 0    /* IntLevel3 */
    rfi 4; .fill 61, 1, 0    /* IntLevel4 */
    rfi 5; .fill 61, 1, 0    /* IntLevel5 */
    .fill 64, 1, 0           /* NMI */
    .fill 64, 1, 0           /* UserInt */
    rfe; .fill 61, 1, 0      /* Kernel */
    rfe; .fill 61, 1, 0      /* User */
    rfe; .fill 61, 1, 0      /* DoubleException */
    .fill 64, 1, 0           /* reserved */
