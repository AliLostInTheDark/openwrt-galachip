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
    ASSERT(SIZEOF(.data) == 0, "probe requires no initialized writable globals")
    ASSERT(SIZEOF(.bss) == 0, "probe requires no BSS initialization")
    ASSERT(. <= 0x00201000, "probe exceeds backed-up SRAM prefix")
    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) *(.debug*) }
}
