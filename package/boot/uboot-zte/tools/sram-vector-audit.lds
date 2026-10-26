/* SPDX-License-Identifier: GPL-2.0+ */
OUTPUT_ARCH(arm)
ENTRY(_start)
SECTIONS
{
    . = 0x00200000;
    .text : { *(.text.start) *(.text.early_vectors) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    ASSERT(SIZEOF(.data) == 0, "Vector audit requires no globals")
    ASSERT(SIZEOF(.bss) == 0, "Vector audit requires no BSS")
    ASSERT(. <= 0x00200c00, "Vector audit overlaps exception-stack space")
    /DISCARD/ : { *(.text.early_cpu) *(.ARM.attributes) *(.comment) *(.note*) *(.debug*) }
}
