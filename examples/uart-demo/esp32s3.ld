ENTRY(_start)
MEMORY { iram (rx) : ORIGIN = 0x40370000, LENGTH = 0x70000 }
SECTIONS {
  .text : ALIGN(4) { *(.literal .literal.*) *(.text .text.*) } > iram
  /DISCARD/ : { *(.comment) *(.note*) }
}
