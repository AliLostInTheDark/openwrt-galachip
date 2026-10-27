/* SPDX-License-Identifier: GPL-2.0-only */
ENTRY(_start)
SECTIONS {
	. = 0x80000000;
	.text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) }
	.data : { *(.data*) }
	. = ALIGN(4);
	__image_end = .;
	.bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .; }
	__stack_bottom = 0x80001d00;
	__stack_top = 0x80002000;
	/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment*) *(.note*) }
	ASSERT(__image_end <= 0x80001c00, "ROM payload exceeds 0x1c00")
	ASSERT(__bss_end <= 0x80001c00, "SPL BSS reaches ROM metadata")
	ASSERT(__stack_bottom >= 0x80001d00, "SPL stack reaches ROM metadata")
}
