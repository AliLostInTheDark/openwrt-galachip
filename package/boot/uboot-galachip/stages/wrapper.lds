/* SPDX-License-Identifier: GPL-2.0-only */
ENTRY(_start)
SECTIONS {
	. = 0x40100000;
	.text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) }
	.data : { *(.data*) }
	.payload : { KEEP(*(.payload)) }
	. = ALIGN(4);
	__image_end = .;
	.bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; }
	__stack_bottom = 0x40300000;
	__stack_top = 0x40304000;
	/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment*) *(.note*) }
	ASSERT(__image_end <= 0x4011dfe0, "wrapper/payload exceeds boot allocation")
	ASSERT(__bss_end < 0x40200000, "wrapper BSS overlaps decompressor heap")
}
