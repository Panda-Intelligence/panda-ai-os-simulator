/* Minimal simulator guest: no Panda OS, SDK, libc or ROM file dependency at build time. */
.section .text,"ax",@progbits
.balign 4
constants:
.word 0x60000000
.global _start
.type _start,@function
.balign 4
_start:
  l32r a8, constants
  movi a9, 80
  s32i a9, a8, 0
  memw
  movi a9, 65
  s32i a9, a8, 0
  memw
  movi a9, 78
  s32i a9, a8, 0
  memw
  movi a9, 68
  s32i a9, a8, 0
  memw
  movi a9, 65
  s32i a9, a8, 0
  memw
  movi a9, 10
  s32i a9, a8, 0
  memw
loop:
  j loop
.size _start, .-_start
