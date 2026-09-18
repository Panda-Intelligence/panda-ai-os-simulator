MEMORY
{
    IRAM (rwx) : ORIGIN = 0x40370000, LENGTH = 0x80000
    DRAM (rw)  : ORIGIN = 0x3FC88000, LENGTH = 0x100000
}

SECTIONS
{
    .text : {
        *(.literal.entry .literal.entry.*)
        *(.text.entry)
        *(.text.entry.*)
        *(.literal .literal.*)
        *(.text .text.*)
        *(.rodata .rodata.*)
    } > IRAM

    .data : {
        *(.data .data.*)
    } > DRAM

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } > DRAM
}

ENTRY(_start)
