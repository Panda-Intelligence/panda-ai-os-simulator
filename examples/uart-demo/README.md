# Minimal UART guest

This independent example contains assembly and a linker script only. It emits
PANDA followed by newline to the ESP32-S3 UART register and then loops. No Panda
OS firmware, source access, SDK download, company credential, book or font is
needed to build it. Use an explicitly installed Xtensa compiler:

```sh
XTENSA_CC=xtensa-esp32s3-elf-gcc scripts/build-example.sh .runtime/uart-example
```

A built ELF is not evidence of a booted guest. Native QEMU still needs its
reviewed machine/runtime installation and applicable ROM. Browser loading also
needs a compatible bundle. Do not flash this example onto hardware or describe
it as a complete Panda OS demo. Public licensing/ROM gates are still pending.

The generic multi-core `xtensa-esp-elf-gcc` is not an ESP32-S3 target by itself.
The build checks the output ELF for the required little-endian Xtensa format
and refuses to report success for a wrong-core artifact.
