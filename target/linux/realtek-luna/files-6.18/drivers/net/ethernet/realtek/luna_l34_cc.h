/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * luna_l34_cc.h - the FLOWBASED dies' flow-cache COMMAND BLOCK, at family tier
 *
 * `luna_l34_acc.h` says what a FLOWBASED die owes -- an allocator, a 32-byte
 * packer, a coherent publish and an invalidate command -- and that it does NOT
 * owe a row in the indirect-access table.  This is the command half of that,
 * and it is family tier because BOTH dies measured carry the same 18 registers
 * at the same 18 offsets: a third FLOWBASED die costs a check, not a table.
 *
 * MEASURED 2026-09-16 from the G24W's own vendor kernel (tier 2), walked from
 * each die's OWN register list at two different index bases, so the agreement
 * is not one list read twice.  Account, with the disassembly:
 * dev/re-tools/FINDING-flowbased-ddr-publish-path-2026-09-16.md
 *
 * ⚠ THE OPTION BITS' POSITIONS ARE ESTABLISHED AND THEIR MEANING IS NOT.  The
 * vendor's field-name strings are not shipped.  Only the two command shapes the
 * vendor actually issues are expressed here; a third must be measured first.
 */
#ifndef _LUNA_L34_CC_H
#define _LUNA_L34_CC_H

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/types.h>

/** CC_CMD[31]: written 1 to start, cleared by the engine when it is done. */
#define LUNA_CC_GO		BIT(31)
/** The vendor polls this many times and then reports a timeout. */
#define LUNA_CC_POLL_TRIES	100

#define LUNA_CC_OP_INVALIDATE	2
#define LUNA_CC_OP_ADD		4

/** CC_CMD[20:16], five bits whose MEANING is not established. */
#define LUNA_CC_OPT(n)		BIT(16 + (n))

/** The base must be 1 KiB aligned -- CC_BAB[9:0] is reserved. */
#define LUNA_CC_BASE_ALIGN	1024

/**
 * struct luna_l34_cc - one FLOWBASED die's flow-cache block.
 * @cfg:	CC_CFG, 13 fields, none of them traced to a writer yet
 * @bab:	CC_BAB, the DDR table base in [31:10]
 * @intr:	CC_INTR
 * @cmd:	CC_CMD, the command word
 * @sflw:	CC_SFLW_0, first of @sflw_words entry words
 * @sts:	CC_STS_0, first of four
 * @sta:	CC_STA
 * @sflw_words:	how many CC_SFLW_n the publish path writes
 *
 * ⚠ CC_SFLW_8 exists at @sflw + 32 and the vendor's publish NEVER writes it;
 * it is not entry data and is deliberately not reachable from here.
 */
struct luna_l34_cc {
	u32	cfg;
	u32	bab;
	u32	intr;
	u32	cmd;
	u32	sflw;
	u32	sts;
	u32	sta;
	u8	sflw_words;
};

/*
 * PROVEN identical on the RTL9603CVD and the RTL9607C.  The RTL9603D is the
 * third die of this model and has NOT been read -- it may use this table once
 * someone walks its register list, and not before.
 */
static const struct luna_l34_cc luna_l34_cc_flowbased = {
	.cfg		= 0x801300,
	.bab		= 0x801304,
	.intr		= 0x801308,
	.cmd		= 0x80130c,
	.sflw		= 0x801310,
	.sts		= 0x801334,
	.sta		= 0x801344,
	.sflw_words	= 8,
};

static inline int luna_l34_cc_wait(void __iomem *sw,
				 const struct luna_l34_cc *c)
{
	unsigned int t;

	for (t = 0; t < LUNA_CC_POLL_TRIES; t++) {
		if (!(readl(sw + c->cmd) & LUNA_CC_GO))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/**
 * luna_l34_cc_base_set() - point the engine at the DRAM flow table.
 * @sw:		the switch-core MMIO base
 * @c:		this die's command block
 * @phys:	physical address of the table, 1 KiB aligned
 *
 * -> 0, or -EINVAL when the base is not aligned -- which is the vendor's own
 * refusal, at the driver and not in the silicon, so an unaligned base gets no
 * fault to diagnose.  ⚠ THE CALLER OWNS THE GATE: the vendor keeps a flag
 * (`fb_cc_init`) set by exactly this call, and refuses every command until it
 * reads 1.  A header holds no state, so the caller must not issue a command
 * before this has returned 0.
 */
static inline int luna_l34_cc_base_set(void __iomem *sw,
				       const struct luna_l34_cc *c, u32 phys)
{
	u32 v;
	int err;

	if (!sw || !c)
		return -ENXIO;
	if (phys % LUNA_CC_BASE_ALIGN)
		return -EINVAL;
	err = luna_l34_cc_wait(sw, c);
	if (err)
		return err;

	v = readl(sw + c->bab) & (LUNA_CC_BASE_ALIGN - 1);
	writel(v | (phys & ~(u32)(LUNA_CC_BASE_ALIGN - 1)), sw + c->bab);
	return 0;
}

/**
 * luna_l34_cc_run() - issue one command and wait for the engine.
 * @sw:		the switch-core MMIO base
 * @c:		this die's command block
 * @op:	 	LUNA_CC_OP_*
 * @idx:	flow index, CC_CMD[14:0]
 * @opts:	an OR of LUNA_CC_OPT(0..4)
 *
 * -> 0, -ENXIO when there is no block, -EINVAL on an index the field cannot
 * carry, or -ETIMEDOUT when the engine never cleared GO.
 */
static inline int luna_l34_cc_run(void __iomem *sw, const struct luna_l34_cc *c,
				  u8 op, u16 idx, u32 opts)
{
	u32 word;
	int err;

	if (!sw || !c)
		return -ENXIO;
	if (idx > 0x7fff || op > 0x7f || (opts & ~0x1f0000U))
		return -EINVAL;
	/* A timed-out command still owns the register until GO clears. */
	err = luna_l34_cc_wait(sw, c);
	if (err)
		return err;

	word = ((u32)op << 24) | (opts & 0x1f0000) | idx;
	writel(word, sw + c->cmd);
	writel(word | LUNA_CC_GO, sw + c->cmd);

	return luna_l34_cc_wait(sw, c);
}

/**
 * luna_l34_cc_invalidate() - tell the cache the DRAM row at @idx changed.
 * @valid: the VALID bit (word 0 bit 0) of the entry as it now stands in DRAM
 *
 * The vendor calls this on BOTH arms of its valid test, with the option bit
 * carrying the COMPLEMENT of the entry's own valid bit.  Clearing a bit in DRAM
 * does not tell an engine that may already hold the row.
 */
static inline int luna_l34_cc_invalidate(void __iomem *sw,
					 const struct luna_l34_cc *c,
					 u16 idx, bool valid)
{
	return luna_l34_cc_run(sw, c, LUNA_CC_OP_INVALIDATE, idx,
			       LUNA_CC_OPT(0) | LUNA_CC_OPT(1) |
			       (valid ? 0 : LUNA_CC_OPT(2)));
}

/**
 * luna_l34_cc_add_ddr() - publish the DRAM row at @idx.
 *
 * The entry is already in DRAM, so no CC_SFLW word is written -- the same ADD
 * command serves the register-fed store, which is why MODE selects a STORE and
 * not a different engine.
 */
static inline int luna_l34_cc_add_ddr(void __iomem *sw,
				      const struct luna_l34_cc *c, u16 idx)
{
	return luna_l34_cc_run(sw, c, LUNA_CC_OP_ADD, idx, LUNA_CC_OPT(2));
}

#endif /* _LUNA_L34_CC_H */
