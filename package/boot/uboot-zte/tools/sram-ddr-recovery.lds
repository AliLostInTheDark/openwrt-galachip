/* SPDX-License-Identifier: GPL-2.0+ */
OUTPUT_ARCH(arm)
ENTRY(_start)
SECTIONS
{
    . = 0x00200000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    ASSERT(SIZEOF(.data) == 0, "DDR stage requires no initialized globals")
    ASSERT(SIZEOF(.bss) == 0, "DDR stage requires no BSS")
    ASSERT(. <= 0x00201000, "DDR stage exceeds backed-up SRAM prefix")
    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) *(.debug*) }
}
