/* SPDX-License-Identifier: GPL-2.0+ */
OUTPUT_ARCH(arm)
ENTRY(_start)
SECTIONS
{
    . = 0x40500000;
    .text : { *(.text.start) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    ASSERT(SIZEOF(.data) == 0, "Context reader requires no globals")
    ASSERT(SIZEOF(.bss) == 0, "Context reader requires no BSS")
    ASSERT(. < 0x40501000, "Context reader exceeds staging page")
    /DISCARD/ : { *(.ARM.attributes) *(.note*) *(.comment) }
}
