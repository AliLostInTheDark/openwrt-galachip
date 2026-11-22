/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * TPL of the GalaChip GC1601: code and data in the SRAM the mask ROM loads,
 * BSS in DDR. Based on arch/arm/cpu/u-boot-spl.lds.
 *
 * Copyright (c) 2004-2008 Texas Instruments
 *
 * (C) Copyright 2002
 * Gary Jennejohn, DENX Software Engineering, <garyj@denx.de>
 */

/*
 * The YMODEM state and the CRC table do not fit in SRAM. crt0 clears BSS
 * after board_init_f(), which has set up DDR by then; nothing uses BSS before.
 */
#define GC1601_TPL_BSS	0x40200000

OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)
SECTIONS
{
	. = 0x00000000;

	. = ALIGN(4);
	__image_copy_start = ADDR(.text);
	.text :
	{
		*(.vectors)
		CPUDIR/start.o (.text*)
		*(.text*)
		*(.glue*)
	}

	. = ALIGN(4);
	.rodata : { *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.rodata*))) }

	. = ALIGN(4);
	.data : {
		*(.data*)
	}

	. = ALIGN(4);
	.binman_sym_table : {
		__binman_sym_start = .;
		KEEP(*(SORT(.binman_sym*)));
		__binman_sym_end = .;
	}

	. = ALIGN(4);
	__u_boot_list : {
		KEEP(*(SORT(__u_boot_list*)));
		. = ALIGN(8);
	}

	__image_copy_end = .;
	_image_binary_end = .;
	_end = .;

	.bss GC1601_TPL_BSS (NOLOAD) : {
		__bss_start = .;
		*(.bss*)
		 . = ALIGN(8);
		__bss_end = .;
	}
	__bss_size = __bss_end - __bss_start;
	.dynsym _image_binary_end : { *(.dynsym) }
	.dynbss : { *(.dynbss) }
	.dynstr : { *(.dynstr*) }
	.dynamic : { *(.dynamic*) }
	.hash : { *(.hash*) }
	.plt : { *(.plt*) }
	.interp : { *(.interp*) }
	.gnu : { *(.gnu*) }
	.ARM.exidx : { *(.ARM.exidx*) }
}

#if defined(IMAGE_MAX_SIZE)
ASSERT(__image_copy_end - __image_copy_start <= (IMAGE_MAX_SIZE), \
	"TPL image too big");
#endif

ASSERT(__bss_end - __bss_start <= 0x10000, "TPL BSS too big");
