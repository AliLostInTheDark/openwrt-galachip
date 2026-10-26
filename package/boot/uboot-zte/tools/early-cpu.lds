/* SPDX-License-Identifier: GPL-2.0+ */
OUTPUT_ARCH(arm)
ENTRY(zx279125_early_cpu_init)
SECTIONS
{
    . = 0x00200000;
    .text : { *(.text.early_cpu) *(.text.early_vectors) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    ASSERT(SIZEOF(.data) == 0, "Cold CPU support requires no globals")
    ASSERT(SIZEOF(.bss) == 0, "Cold CPU support requires no BSS")
    ASSERT(. <= 0x00200e00, "Cold CPU support overlaps its fault record")
    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) *(.debug*) }
}
