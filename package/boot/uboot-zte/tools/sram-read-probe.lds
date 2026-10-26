/* SPDX-License-Identifier: GPL-2.0+ */
OUTPUT_ARCH(arm)
ENTRY(_start)
SECTIONS
{
    . = 0x00200000;
    .text : { *(.text.start) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    ASSERT(SIZEOF(.data) == 0, "Probe requires no globals")
    ASSERT(SIZEOF(.bss) == 0, "Probe requires no BSS")
    ASSERT(. <= 0x00200c00, "Probe overlaps abort-stack space")
    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) }
}
