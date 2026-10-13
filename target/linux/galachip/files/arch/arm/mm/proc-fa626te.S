/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2005 Faraday Corp. (Luke Lee)
 * Copyright (C) 2008-2009 Paulius Zaleckas <paulius.zaleckas@teltonika.lt>
 */
#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/cfi_types.h>
#include <linux/pgtable.h>
#include <asm/assembler.h>
#include <asm/hwcap.h>
#include <asm/pgtable-hwdef.h>
#include <asm/page.h>
#include <asm/ptrace.h>

#include "proc-macros.S"

	.text
SYM_TYPED_FUNC_START(cpu_fa626te_proc_init)
	ret	lr
SYM_FUNC_END(cpu_fa626te_proc_init)

SYM_TYPED_FUNC_START(cpu_fa626te_proc_fin)
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #0x1000
	bic	r0, r0, #0x000e
	mcr	p15, 0, r0, c1, c0, 0
	nop
	nop
	ret	lr
SYM_FUNC_END(cpu_fa626te_proc_fin)

	.align	4
	.pushsection .idmap.text, "ax"
SYM_TYPED_FUNC_START(cpu_fa626te_reset)
	mov	ip, #0
	mcr	p15, 0, ip, c7, c7, 0
	mcr	p15, 0, ip, c7, c10, 4
	mcr	p15, 0, ip, c8, c7, 0
	mrc	p15, 0, ip, c1, c0, 0
	bic	ip, ip, #0x000f
	bic	ip, ip, #0x1100
	bic	ip, ip, #0x0800
	mcr	p15, 0, ip, c1, c0, 0
	nop
	nop
	ret	r0
SYM_FUNC_END(cpu_fa626te_reset)
	.popsection

SYM_TYPED_FUNC_START(cpu_fa626te_do_idle)
	/* Keep IRQs masked; the vendor WFI sequence enables them unsafely. */
	mov	r0, #0
	ret	lr
SYM_FUNC_END(cpu_fa626te_do_idle)

SYM_TYPED_FUNC_START(cpu_fa626te_dcache_clean_area)
1:	mcr	p15, 0, r0, c7, c10, 1
	add	r0, r0, #32
	subs	r1, r1, #32
	bhi	1b
	mcr	p15, 0, r0, c7, c10, 4
	ret	lr
SYM_FUNC_END(cpu_fa626te_dcache_clean_area)

SYM_TYPED_FUNC_START(cpu_fa626te_switch_mm)
	mov	ip, #0
	mcr	p15, 0, ip, c7, c5, 6
	mcr	p15, 0, ip, c7, c10, 4
	mcr	p15, 0, r0, c2, c0, 0
	mcr	p15, 0, ip, c8, c7, 0
	ret	lr
SYM_FUNC_END(cpu_fa626te_switch_mm)

SYM_TYPED_FUNC_START(cpu_fa626te_set_pte_ext)
	armv3_set_pte_ext
	mcr	p15, 0, r0, c7, c10, 1
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	ret	lr
SYM_FUNC_END(cpu_fa626te_set_pte_ext)

	.type	__fa626te_setup, #function
__fa626te_setup:
	mov	r0, #0
	mcr	p15, 0, r0, c7, c7, 0
	mcr	p15, 0, r0, c7, c10, 4
	mcr	p15, 0, r0, c8, c7, 0
	mcr	p15, 0, r0, c7, c5, 5
	mcr	p15, 0, r0, c7, c5, 6
	mcr	p15, 0, r0, c7, c10, 4
	mcr	p15, 0, r0, c7, c5, 4
	mov	r0, #0x1f
	mcr	p15, 0, r0, c3, c0, 0
	adr	r5, fa626te_crval
	ldmia	r5, {r5, r6}
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, r5
	orr	r0, r0, r6
	ret	lr
	.size	__fa626te_setup, . - __fa626te_setup

	.type	fa626te_crval, #object
fa626te_crval:
	crval	clear=0x3f7f, mmuset=0x397d, ucset=0x3174

	__INITDATA
	define_processor_functions fa626te, dabort=v5t_early_abort, pabort=legacy_pabort

	.section ".rodata"
	string	cpu_arch_name, "armv5te"
	string	cpu_elf_name, "v5"
	string	cpu_fa626te_name, "FA626TE"
	.align

	.section ".proc.info.init", "a"
	.type	__fa626te_proc_info, #object
__fa626te_proc_info:
	.long	0x66056260
	.long	0xff0ffff0
	.long	PMD_TYPE_SECT | PMD_SECT_BUFFERABLE | PMD_SECT_CACHEABLE | \
		PMD_BIT4 | PMD_SECT_AP_WRITE | PMD_SECT_AP_READ
	.long	PMD_TYPE_SECT | PMD_BIT4 | PMD_SECT_AP_WRITE | PMD_SECT_AP_READ
	initfn	__fa626te_setup, __fa626te_proc_info
	.long	cpu_arch_name
	.long	cpu_elf_name
	.long	HWCAP_SWP | HWCAP_HALF | HWCAP_THUMB
	.long	cpu_fa626te_name
	.long	fa626te_processor_functions
	.long	fa_tlb_fns
	.long	fa626te_user_fns
	.long	fa_cache_fns
	.size	__fa626te_proc_info, . - __fa626te_proc_info
