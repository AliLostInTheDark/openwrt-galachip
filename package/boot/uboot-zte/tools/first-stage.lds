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
    ASSERT(SIZEOF(.data) == 0, "First stage requires no initialized globals")
    ASSERT(SIZEOF(.bss) == 0, "First stage requires no BSS")
    ASSERT(. <= 0x00202000, "First stage exceeds ROM 8 KiB load window")
    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) *(.debug*) }
}
