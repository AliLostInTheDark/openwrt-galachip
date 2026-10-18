/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The mask ROM loads at most 0x1c00 bytes and runs them at 0x80000000 in
 * on-chip SRAM. The stock SPL keeps its stack at 0x80001f00; so do we.
 */
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
	sram (rwx) : ORIGIN = 0x80000000, LENGTH = 0x1f00
}

SECTIONS
{
	.text : {
		KEEP(*(.text.start))
		*(.text*)
		*(.rodata*)
	} > sram

	.data : {
		*(.data*)
	} > sram

	__image_end = .;
	ASSERT(__image_end - 0x80000000 <= 0x1c00, "image larger than the mask ROM loads")

	.bss (NOLOAD) : ALIGN(4) {
		__bss_start = .;
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);
		__bss_end = .;
	} > sram

	__stack_top = ORIGIN(sram) + LENGTH(sram);

	/DISCARD/ : {
		*(.ARM.exidx*)
		*(.ARM.attributes)
		*(.comment)
	}
}
