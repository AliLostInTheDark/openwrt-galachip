/* SPDX-License-Identifier: GPL-2.0+ */
OUTPUT_ARCH(arm)
ENTRY(_start)
SECTIONS
{
    . = 0x00200000;
    .text : { *(.text.start) *(.text.early_cpu) *(.text.early_vectors) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    ASSERT(SIZEOF(.data) == 0, "Cold diagnostic requires no globals")
    ASSERT(SIZEOF(.bss) == 0, "Cold diagnostic requires no BSS")
    ASSERT(. <= 0x00200c00, "Cold diagnostic overlaps exception-stack space")
    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) *(.debug*) }
}
