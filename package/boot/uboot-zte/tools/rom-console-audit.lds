/* SPDX-License-Identifier: GPL-2.0+ */
OUTPUT_ARCH(arm)
ENTRY(_start)
SECTIONS
{
    . = 0x00200000;
    .text : { *(.text.start) }
    ASSERT(resume == 0x00200100, "ROM return command address changed")
    ASSERT(. <= 0x00200200, "Console wrapper overlaps reserved SRAM")
    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) }
}
