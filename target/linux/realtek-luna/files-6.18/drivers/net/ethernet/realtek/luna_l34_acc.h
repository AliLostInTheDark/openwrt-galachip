/* SPDX-License-Identifier: GPL-2.0-only */
/* Luna indirect table access: NAPT tables and FLOWBASED SRAM/interface tables. */
#ifndef _LUNA_L34_ACC_H
#define _LUNA_L34_ACC_H

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/types.h>

/** The engine clears EXE well inside this; a longer wait is a dead engine. */
#define LUNA_L34_EXE_POLL_US	2000

/** Largest data-bank transfer any table on this family needs. */
#define LUNA_L34_WORDS_MAX	8

/**
 * struct luna_l34_cmd_layout - where the command word's fields sit, per die.
 * @rd_exe:	start-read bit (self-clearing)
 * @wr_exe:	start-write bit (self-clearing)
 * @type_shift:	shift of the table-type field
 * @type_mask:	mask of the table-type field, AFTER the shift
 * @idx_mask:	mask of the entry index, which sits at bit 0
 *
 * A die whose layout nobody has measured has NO instance of this -- see the
 * file header.  There is deliberately no "default" layout to fall back to.
 */
struct luna_l34_cmd_layout {
	u32	rd_exe;
	u32	wr_exe;
	u8	type_shift;
	u32	type_mask;
	u32	idx_mask;
};

/**
 * struct luna_l34_acc - one die's indirect table-access registers.
 * @cmd:	command/trigger register offset
 * @clr:	per-table-type reset (self-clearing)
 * @rdata:	read-data bank base (word 0)
 * @wdata:	write-data bank base (word 0)
 * @cmd_layout:	the command word's field positions, or NULL when NOT ESTABLISHED
 */
struct luna_l34_acc {
	u32					cmd;
	u32					clr;
	u32					rdata;
	u32					wdata;
	const struct luna_l34_cmd_layout	*cmd_layout;
};

/*
 * RTL9602C -- the NAPT model.  Every value here is what `rtl9602c_l34.h` has
 * carried and what the shipping driver has been using; moving it changes no
 * byte the engine sees.
 */
static const struct luna_l34_cmd_layout luna_l34_cmd_rtl9602c = {
	.rd_exe		= BIT(25),
	.wr_exe		= BIT(24),
	.type_shift	= 16,		/* [19:16] table type */
	.type_mask	= 0xf,
	.idx_mask	= 0xffff,	/* [15:0] entry index */
};

static const struct luna_l34_acc luna_l34_acc_rtl9602c = {
	.cmd		= 0x800100,
	.clr		= 0x800104,
	.rdata		= 0x800108,
	.wdata		= 0x80011c,
	.cmd_layout	= &luna_l34_cmd_rtl9602c,
};

/* RTL9603CVD layout verified against stock and the RTL9607C register list. */
static const struct luna_l34_cmd_layout luna_l34_cmd_rtl9603cvd = {
	.rd_exe		= BIT(25),
	.wr_exe		= BIT(24),
	.type_shift	= 16,		/* [19:16] table type */
	.type_mask	= 0xf,
	.idx_mask	= 0xffff,	/* [15:0] entry index */
};

static const struct luna_l34_acc luna_l34_acc_rtl9603cvd = {
	.cmd		= 0x801100,
	.clr		= 0x801104,
	.rdata		= 0x801110,
	.wdata		= 0x801180,
	.cmd_layout	= &luna_l34_cmd_rtl9603cvd,
};

/**
 * luna_l34_tbl_op() - one indirect table access.  -> 0, or a negative errno.
 * @sw:		the switch-core MMIO base
 * @a:		this die's access table
 * @type:	table-type selector, as that die's engine numbers them
 * @idx:	entry index
 * @w:		the words -- written from on a write, filled in on a read
 * @n:		how many words this table moves
 * @write:	true to write, false to read
 *
 * ⚠ THREE ANSWERS, never two: -ENXIO means this die's command-word layout was
 * never measured, so nothing was attempted; -ETIMEDOUT means the engine did not
 * clear EXE, which is a fact about the silicon; 0 means it completed.  An
 * unmeasured layout must never read as a failed engine.
 */
static inline int luna_l34_tbl_op(void __iomem *sw,
				  const struct luna_l34_acc *a,
				  u8 type, u16 idx, u32 *w, unsigned int n,
				  bool write)
{
	const struct luna_l34_cmd_layout *L;
	u32 cmd, exe;
	unsigned int i, t;

	if (!sw || !a || !a->cmd_layout)
		return -ENXIO;
	L = a->cmd_layout;
	if (!w || !n || n > LUNA_L34_WORDS_MAX ||
	    ((u32)type & ~L->type_mask) || ((u32)idx & ~L->idx_mask))
		return -EINVAL;
	exe = write ? L->wr_exe : L->rd_exe;

	/* ★★ A COMMAND THAT TIMED OUT CAN STILL OWN THE SHARED DATA BANK.  The
	 * bank is four registers, not a per-command buffer: the engine reads it
	 * while EXE is set, so writing the next op's words into it before BOTH
	 * request bits have cleared hands the engine a mixture of two entries
	 * and it will store one of them.  Nothing reports that -- the entry
	 * reads back exactly as written.  Wait for the previous op to let go,
	 * and refuse rather than proceed if it never does.
	 * ⚠ It is BOTH bits, not `exe`: a read left executing owns the bank just
	 * as a write does, and the poll below only ever clears our own. */
	for (t = 0; t < LUNA_L34_EXE_POLL_US; t++) {
		if (!(readl(sw + a->cmd) & (L->rd_exe | L->wr_exe)))
			break;
		udelay(1);
	}
	if (readl(sw + a->cmd) & (L->rd_exe | L->wr_exe))
		return -ETIMEDOUT;

	if (write)
		for (i = 0; i < n; i++)
			writel(w[i], sw + a->wdata + 4 * i);

	cmd = exe | ((u32)(type & L->type_mask) << L->type_shift) |
	      ((u32)idx & L->idx_mask);
	writel(cmd, sw + a->cmd);

	for (t = 0; t < LUNA_L34_EXE_POLL_US; t++) {
		if (!(readl(sw + a->cmd) & exe))
			break;
		udelay(1);
	}
	if (readl(sw + a->cmd) & exe)
		return -ETIMEDOUT;

	if (!write)
		for (i = 0; i < n; i++)
			w[i] = readl(sw + a->rdata + 4 * i);
	return 0;
}

#endif /* _LUNA_L34_ACC_H */
