/* SPDX-License-Identifier: GPL-2.0+ */
OUTPUT_ARCH(arm)
ENTRY(_start)
SECTIONS
{
	. = 0x00200000;
	.text : { *(.text.start) *(.text*) }
	.rodata : { *(.rodata*) }
	/DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) }
}
