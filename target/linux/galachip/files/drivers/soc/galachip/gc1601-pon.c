// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 PON SerDes and MAC bring-up, GPON or EPON.
 *
 * Status reads are side-effect free. The write triggers replay vendor
 * sequences: serdes_init (GPL mach-faraday/serdes.c), gmac_init
 * (xpon_sdk GMAC_hw_init plus optical RX enable) and bosa_program (gccli
 * UX3320S programming from the unit's hwconfig blob). Optical TX is forced
 * off by every path and the BOSA soft TX disable is left set: nothing here
 * enables the laser.
 *
 * With galachip,pon-bringup the BOSA is programmed once after probe, as the
 * stock rcS does with "gccli bob init" before loading xpon_sdk: the UX3320S
 * keeps no calibration across a power cycle on these units. The WAN mode,
 * SerDes and MAC follow on pon_start, once userspace has chosen GPON or EPON.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#include "gc1601-pon.h"

struct gc1601_pon_seq {
	u32 addr;
	u32 val;
};

static const struct gc1601_pon_seq gpon_serdes_config[] = {
	{ 0x00194, 0xb7 },
	{ 0x00198, 0xb7 },
	{ 0x0019c, 0x06 },
	{ 0x001a0, 0x0c },
	{ 0x001a4, 0x1a },
	{ 0x001a8, 0x1a },
	{ 0x001ac, 0x01 },
	{ 0x001b0, 0x02 },
	{ 0x001b4, 0x17 },
	{ 0x001b8, 0x0d },
	{ 0x20194, 0xaa },
	{ 0x20198, 0x00 },
	{ 0x2019c, 0x4f },
	{ 0x201a0, 0xb7 },
	{ 0x201a4, 0x57 },
	{ 0x201a8, 0x06 },
	{ 0x201ac, 0x0c },
	{ 0x201b0, 0x1a },
	{ 0x201b4, 0x0a },
	{ 0x201b8, 0x05 },
	{ 0x201bc, 0x0f },
	{ 0x201c0, 0x10 },
	{ 0x201c4, 0x00 },
	{ 0x201c8, 0x10 },
	{ 0x201cc, 0x00 },
	{ 0x201d0, 0xff },
	{ 0x201d4, 0xb3 },
	{ 0x201d8, 0xf7 },
	{ 0x201dc, 0xd0 },
	{ 0x201e0, 0xef },
	{ 0x201e4, 0xfb },
	{ 0x201e8, 0xff },
	{ 0x201ec, 0xff },
	{ 0x201f0, 0xff },
	{ 0x201f4, 0xff },
	{ 0x201f8, 0xff },
	{ 0x201fc, 0xd3 },
	{ 0x20200, 0xd4 },
	{ 0x20204, 0xe3 },
	{ 0x20208, 0xef },
	{ 0x2020c, 0xfb },
	{ 0x20210, 0xfb },
	{ 0x20214, 0xff },
	{ 0x20218, 0xef },
	{ 0x2021c, 0xff },
	{ 0x20220, 0xff },
	{ 0x20224, 0xd3 },
	{ 0x20228, 0xd4 },
	{ 0x2022c, 0xe3 },
	{ 0x20230, 0xef },
	{ 0x20234, 0xfb },
	{ 0x20238, 0xfb },
	{ 0x2023c, 0xff },
	{ 0x20240, 0xef },
	{ 0x20244, 0xff },
	{ 0x20248, 0xff },
	{ 0x2024c, 0xfb },
	{ 0x20250, 0xff },
	{ 0x20254, 0x3f },
	{ 0x20258, 0x00 },
	{ 0x2025c, 0x7d },
	{ 0x20260, 0x33 },
	{ 0x20264, 0x02 },
	{ 0x20268, 0x01 },
	{ 0x2026c, 0x08 },
	{ 0x20270, 0x04 },
	{ 0x20274, 0x04 },
	{ 0x20278, 0x00 },
	{ 0x2027c, 0x00 },
	{ 0x20280, 0x08 },
	{ 0x20284, 0x04 },
	{ 0x20288, 0x00 },
	{ 0x2028c, 0x00 },
	{ 0x20290, 0x04 },};

static const struct gc1601_pon_seq gpon_serdes_ctrl[] = {
	{ 0x00, 1 }, { 0x08, 1 }, { 0x14, 0 }, { 0x16, 1 },
	{ 0x18, 0 }, { 0x1a, 2 }, { 0x3c, 1 }, { 0x3e, 7 },
};

static const struct gc1601_pon_seq epon_serdes_config[] = {
	{ 0x00194, 0xbf },
	{ 0x00198, 0xbf },
	{ 0x0019c, 0x0a },
	{ 0x001a0, 0x0a },
	{ 0x001a4, 0x0e },
	{ 0x001a8, 0x0e },
	{ 0x001ac, 0x01 },
	{ 0x001b0, 0x01 },
	{ 0x001b4, 0x11 },
	{ 0x001b8, 0x0d },
	{ 0x20194, 0xaa },
	{ 0x20198, 0x00 },
	{ 0x2019c, 0x3f },
	{ 0x201a0, 0xbf },
	{ 0x201a4, 0xbf },
	{ 0x201a8, 0x0a },
	{ 0x201ac, 0x0a },
	{ 0x201b0, 0x0e },
	{ 0x201b4, 0x0e },
	{ 0x201b8, 0x03 },
	{ 0x201bc, 0x1f },
	{ 0x201c0, 0x10 },
	{ 0x201c4, 0x00 },
	{ 0x201c8, 0x10 },
	{ 0x201cc, 0x00 },
	{ 0x201d0, 0xff },
	{ 0x201d4, 0xc3 },
	{ 0x201d8, 0xf5 },
	{ 0x201dc, 0xd9 },
	{ 0x201e0, 0xf2 },
	{ 0x201e4, 0xfc },
	{ 0x201e8, 0xfe },
	{ 0x201ec, 0xff },
	{ 0x201f0, 0xff },
	{ 0x201f4, 0xff },
	{ 0x201f8, 0xff },
	{ 0x201fc, 0xdb },
	{ 0x20200, 0xe9 },
	{ 0x20204, 0xc2 },
	{ 0x20208, 0xf2 },
	{ 0x2020c, 0xfc },
	{ 0x20210, 0xfe },
	{ 0x20214, 0xf2 },
	{ 0x20218, 0xf2 },
	{ 0x2021c, 0xff },
	{ 0x20220, 0xff },
	{ 0x20224, 0xdb },
	{ 0x20228, 0xe9 },
	{ 0x2022c, 0xc2 },
	{ 0x20230, 0xf2 },
	{ 0x20234, 0xfc },
	{ 0x20238, 0xfe },
	{ 0x2023c, 0xf2 },
	{ 0x20240, 0xf2 },
	{ 0x20244, 0xff },
	{ 0x20248, 0xff },
	{ 0x2024c, 0xff },
	{ 0x20250, 0xf2 },
	{ 0x20254, 0x3f },
	{ 0x20258, 0x00 },
	{ 0x2025c, 0x64 },
	{ 0x20260, 0x00 },
	{ 0x20264, 0x02 },
	{ 0x20268, 0x01 },
	{ 0x2026c, 0x05 },
	{ 0x20270, 0x05 },
	{ 0x20274, 0x04 },
	{ 0x20278, 0x00 },
	{ 0x2027c, 0x00 },
	{ 0x20280, 0x08 },
	{ 0x20284, 0x04 },
	{ 0x20288, 0x00 },
	{ 0x2028c, 0x00 },
	{ 0x20290, 0x04 },
};
static const struct gc1601_pon_seq epon_serdes_ctrl[] = {
	{ 0x00, 1 }, { 0x08, 1 }, { 0x14, 1 }, { 0x16, 1 },
	{ 0x18, 1 }, { 0x1a, 1 }, { 0x3c, 1 }, { 0x3e, 7 },
};

/* galachip_serdes_set_{g,e}pon_normal from the vendor GPL kernel */
struct gc1601_pon_serdes {
	const char *name;
	u32 pll_cfg0, pll_cfg1, pll, pll_clear;
	const struct gc1601_pon_seq *ctrl, *config;
	unsigned int config_len;
	u32 check194, check19c;		/* table readback */
};

static const struct gc1601_pon_serdes gc1601_pon_serdes_modes[] = {
	[GC1601_PON_GPON] = {
		.name = "GPON",
		.pll_cfg0 = 0x1001a, .pll_cfg1 = 0x353f7d,
		.pll = 0x3e005f, .pll_clear = 0x7,
		.ctrl = gpon_serdes_ctrl, .config = gpon_serdes_config,
		.config_len = ARRAY_SIZE(gpon_serdes_config),
		.check194 = 0xb7, .check19c = 0x06,
	},
	[GC1601_PON_EPON] = {
		.name = "EPON",
		.pll_cfg0 = 0x1001c, .pll_cfg1 = 0,
		.pll = 0x3c005f, .pll_clear = 0x5,
		.ctrl = epon_serdes_ctrl, .config = epon_serdes_config,
		.config_len = ARRAY_SIZE(epon_serdes_config),
		.check194 = 0xbf, .check19c = 0x0a,
	},
};

static const char *gc1601_pon_mode_name(int mode)
{
	return gc1601_pon_serdes_modes[mode].name;
}

void gc1601_pon_tx_off(struct gc1601_pon *pon)
{
	void __iomem *reg = pon->mac + PON_OPT_TX;
	u32 val;

	writel(readl(reg) & ~(PON_OPT_TX_EN | PON_OPT_TX_FORCE), reg);
	/* The EPON MAC block is held off in GPON mode: leave it alone there. */
	if (pon->emac && pon->mode == GC1601_PON_EPON)
		writel(readl(pon->emac + EMAC_CTRL) & ~EMAC_CTRL_TX_EN, pon->emac + EMAC_CTRL);

	/* txpwr_manual_op(0): manual mode, laser power off */
	val = readl(pon->txpwr) & ~(PON_TXPWR_MANUAL | PON_TXPWR_OFF | PON_TXPWR_SEL);
	writel(val | PON_TXPWR_SEL | PON_TXPWR_OFF, pon->txpwr);
}

/*
 * txpwr_manual_op(1), then chipset_set_gpon_opt_tx_en(1) or
 * chipset_set_epon_tx_en(1) for the active MAC: only via allow_tx.
 */
void gc1601_pon_tx_on(struct gc1601_pon *pon)
{
	u32 val = readl(pon->txpwr) & ~(PON_TXPWR_MANUAL | PON_TXPWR_OFF);

	writel(val | PON_TXPWR_SEL, pon->txpwr);
	if (pon->mode == GC1601_PON_EPON)
		writel(readl(pon->emac + EMAC_CTRL) | EMAC_CTRL_TX_EN, pon->emac + EMAC_CTRL);
	else
		writel(readl(pon->mac + PON_OPT_TX) | PON_OPT_TX_EN, pon->mac + PON_OPT_TX);
}

static int gc1601_pon_wait_scu(struct gc1601_pon *pon, u32 bit, const char *what)
{
	u32 val;
	int ret;

	ret = readl_poll_timeout(pon->scu + SCU_PON_STATUS, val, val & bit, 10,
				 PON_LOCK_TIMEOUT_US);
	if (ret)
		dev_err(pon->dev, "%s timed out (status %08x)\n", what, val);
	return ret;
}

static void gc1601_pon_serdes_clear(struct gc1601_pon *pon, u32 off, u32 bits)
{
	writel(readl(pon->serdes + off) & ~bits, pon->serdes + off);
}

/*
 * xpon_sdk's chip set-up for a WAN mode: GLOBAL_hw_init, the bit GLOBAL_init
 * sets at 0x90c40020 and chipset_config_wanmode(), which selects the GPON or
 * EPON MAC and its upstream clocking.
 * The 0x90c00008 reset pulse of GLOBAL_hw_init is left out: it also resets the
 * packet engine Ethernet runs on. So is the global interrupt mask, which the
 * Ethernet interrupt may depend on.
 */
static void gc1601_pon_global_mode(struct gc1601_pon *pon, int mode)
{
	bool epon = mode == GC1601_PON_EPON;
	u32 top0 = readl(pon->top + TOP_ENABLE);
	u32 top4 = readl(pon->top + TOP_CLK_EN);
	u32 topc = readl(pon->top + TOP_WAN_CLK);
	u32 g4 = readl(pon->global + GLB_CTRL);
	u32 g10 = readl(pon->global + GLB_CFG10);
	u32 g14 = readl(pon->global + GLB_WAN_CFG);
	u32 g18 = readl(pon->global + GLB_CFG18);
	u32 txpwr = readl(pon->txpwr);

	/* GLOBAL_hw_init */
	writel(0xffffffff, pon->top + TOP_CLK_EN);
	writel(7, pon->global + GLB_CFG10);
	writel(epon ? GLB_WAN_CFG_EPON : GLB_WAN_CFG_GPON, pon->global + GLB_WAN_CFG);
	writel(g18 | BIT(16), pon->global + GLB_CFG18);
	/* GLOBAL_init */
	writel(readl(pon->txpwr) | PON_TXPWR_GLOBAL_EN, pon->txpwr);
	/* chipset_config_wanmode() */
	writel((topc & 0xffff8000) | (epon ? TOP_WAN_CLK_EPON : TOP_WAN_CLK_GPON),
	       pon->top + TOP_WAN_CLK);
	writel(((g4 | GLB_CTRL_RUN) & ~GLB_CTRL_EPON) | (epon ? GLB_CTRL_EPON_SEL : 0),
	       pon->global + GLB_CTRL);
	/* Stock reads back 0x7d in GPON and 0x7e in EPON. */
	writel((readl(pon->global + GLB_MAC_SEL) | 3) & ~(epon ? BIT(0) : BIT(1)),
	       pon->global + GLB_MAC_SEL);
	writel(epon ? TOP_ENABLE_EPON : TOP_ENABLE_GPON, pon->top + TOP_ENABLE);

	dev_info(pon->dev,
		 "%s mode: top %08x/%08x/%08x -> %08x/%08x/%08x, global 04/10/14/18 %08x/%08x/%08x/%08x -> %08x/%08x/%08x/%08x, sel %02x, txpwr %08x -> %08x\n",
		 gc1601_pon_mode_name(mode), top0, top4, topc, readl(pon->top + TOP_ENABLE),
		 readl(pon->top + TOP_CLK_EN), readl(pon->top + TOP_WAN_CLK),
		 g4, g10, g14, g18, readl(pon->global + GLB_CTRL),
		 readl(pon->global + GLB_CFG10), readl(pon->global + GLB_WAN_CFG),
		 readl(pon->global + GLB_CFG18), readl(pon->global + GLB_MAC_SEL),
		 txpwr, readl(pon->txpwr));
}

static int gc1601_pon_serdes_init(struct gc1601_pon *pon, int mode)
{
	const struct gc1601_pon_serdes *m = &gc1601_pon_serdes_modes[mode];
	unsigned int i;
	int ret;

	/*
	 * board-dt.c: let the MAC send the PLOAM Dying_Gasp itself when the
	 * supply drops; the stock SDK never handles it in software.
	 */
	writel(readl(pon->scu + SCU_PON_DG) & ~BIT(0), pon->scu + SCU_PON_DG);

	writel(m->pll_cfg0, pon->scu + SCU_PON_PLL_CFG0);
	writel(m->pll_cfg1, pon->scu + SCU_PON_PLL_CFG1);
	writel(m->pll, pon->scu + SCU_PON_PLL);
	writel(readl(pon->scu + SCU_PON_PLL) & ~m->pll_clear, pon->scu + SCU_PON_PLL);

	ret = gc1601_pon_wait_scu(pon, SCU_PON_STATUS_PLL_LOCK, "PON PLL lock");
	if (ret)
		return ret;

	writel(readl(pon->scu + SCU_PON_PLL) & ~0x40, pon->scu + SCU_PON_PLL);
	writel(0x11810, pon->scu + SCU_PON_RESET);
	udelay(20);

	for (i = 0; i < ARRAY_SIZE(gpon_serdes_ctrl); i++)
		writew(m->ctrl[i].val, pon->serdes_ctrl + m->ctrl[i].addr);

	writel(0x51810, pon->scu + SCU_PON_RESET);
	udelay(20);

	for (i = 0; i < m->config_len; i++)
		writel(m->config[i].val, pon->serdes + m->config[i].addr);

	if (readl(pon->serdes + 0x194) != m->check194 ||
	    readl(pon->serdes + 0x19c) != m->check19c) {
		dev_err(pon->dev, "SerDes table readback mismatch\n");
		return -EIO;
	}

	/* De-assert the SerDes synthesizer and TX/RX resets. */
	writel(0x1d1810, pon->scu + SCU_PON_RESET);
	udelay(20);

	writel(0x13, pon->serdes + 0x57 * 4);
	writel(0xdb, pon->serdes + 0x15 * 4);
	writel(0x03, pon->serdes + 0x16 * 4);
	writel(0x00, pon->serdes + 0x17 * 4);
	writel(0x56, pon->serdes + 0x8008 * 4);

	ret = gc1601_pon_wait_scu(pon, SCU_PON_STATUS_READY, "SerDes ready");
	if (ret)
		return ret;

	gc1601_pon_serdes_clear(pon, 0x2001c, 0xc0);
	gc1601_pon_serdes_clear(pon, 0x20184, 0x08);
	writel(0x04, pon->serdes + 0x1c);	/* CDR loopback enable */
	writel(0x13, pon->serdes + 0x20034);	/* loop timing on */
	return 0;
}

/* GMAC_hw_init from xpon_sdk, with the stock opt_polar_data_low = 1. */
void gc1601_pon_gmac_init(struct gc1601_pon *pon)
{
	void __iomem *tx = pon->mac + PON_OPT_TX;
	void __iomem *rx = pon->mac + PON_OPT_RX;
	u32 val;

	writel((readl(rx) & ~PON_OPT_RX_EN) | 0x6, rx);
	writeb((readb(rx + 0x18) & 0xf9) | 0x8, rx + 0x18);

	val = (readl(tx) & ~0x10) | 0x20;
	writel((val & ~(PON_OPT_TX_EN | PON_OPT_TX_FORCE)) | 0x2, tx);

	val = readl(pon->mac + PON_OMCC);
	writel((val & ~0x6) | 0x1, pon->mac + PON_OMCC);
	writel(0xffffffff, pon->mac + PON_IRQ + 0x4);
	writel(0x15ce, pon->mac + PON_ONU + 0xc);
	writel(1, pon->mac + PON_COUNTERS + 0x38);
	writel(1, tx + 0x114);

	/* chipset_set_gpon_opt_delay(16, 2) */
	val = readl(tx + PON_OPT_TX_DELAY) & ~GENMASK(8, 0);
	writel(val | (16 << 4) | 2, tx + PON_OPT_TX_DELAY);

	/* sdk_set_gpon_rx_en: receive path only */
	writel(readl(rx) | PON_OPT_RX_EN, rx);
}

/* Called with pon->lock held. */
static void gc1601_pon_mac_init(struct gc1601_pon *pon)
{
	if (pon->mode == GC1601_PON_EPON)
		gc1601_epon_hw_init(pon);
	else
		gc1601_pon_gmac_init(pon);
}

struct gc1601_bosa_page {
	u8 page;
	u16 blob;	/* offset in the calibration blob */
	u8 len;
	u8 verify;	/* bytes compared on readback */
};

/* gccli order for the UX3320S: bias, modulation, APD, LUT3, then A2 lower. */
static const struct gc1601_bosa_page ux3320s_pages[] = {
	{ 4, 0x200, 0x69, 0x69 },
	{ 5, 0x280, 0x69, 0x69 },
	{ 6, 0x300, 0x69, 0x69 },
	/*
	 * LUT3: 0xd3 onwards holds the ID, markers and live/status registers
	 * (0xde-0xe0, 0xe8-0xea and 0xf2 do not keep written values). Verify
	 * the range gccli's LUT3 checksum covers.
	 */
	{ 3, 0x180, 0x7d, 0x53 },
	/*
	 * A2 lower: thresholds and calibration constants only. gccli writes
	 * all 0x80 bytes, but 0x60 onwards is live readings, flags and the
	 * TX control byte, which the blob would clear (0x6e = 0x02).
	 */
	{ 0, 0x100, 0x60, 0x60 },
};

static u8 gc1601_bosa_sum(const u8 *buf, unsigned int start, unsigned int end)
{
	u8 sum = 0;

	while (start < end)
		sum += buf[start++];
	return sum;
}

/* The checks gccli applies to a UX3320S blob before programming it. */
static int gc1601_bosa_validate(struct gc1601_pon *pon, const u8 *b)
{
	u8 lut3, total;

	if (gc1601_bosa_sum(b, 0, 0x3f) != b[0x3f] ||
	    gc1601_bosa_sum(b, 0x40, 0x5f) != b[0x5f] ||
	    gc1601_bosa_sum(b, 0x100, 0x15f) != b[0x15f]) {
		dev_err(pon->dev, "BOSA blob: A0/A2 checksum mismatch\n");
		return -EINVAL;
	}
	if (b[0x180] != 0xaa || b[0x1dc] != 0x55) {
		dev_err(pon->dev, "BOSA blob: no UX3320S LUT3 markers\n");
		return -EINVAL;
	}
	lut3 = gc1601_bosa_sum(b, 0x181, 0x1a1) + gc1601_bosa_sum(b, 0x1a2, 0x1d3);
	total = lut3 + gc1601_bosa_sum(b, 0x200, 0x269) +
		gc1601_bosa_sum(b, 0x280, 0x2e9) + gc1601_bosa_sum(b, 0x300, 0x369);
	if (total != b[0x1db]) {
		dev_err(pon->dev, "BOSA blob: LUT3 checksum %02x, expected %02x\n",
			total, b[0x1db]);
		return -EINVAL;
	}
	return 0;
}

static int gc1601_bosa_read_blob(struct gc1601_pon *pon, u8 *buf, size_t len)
{
	struct mtd_info *mtd;
	size_t retlen;
	int ret;

	mtd = get_mtd_device_nm("hwconfig");
	if (IS_ERR(mtd))
		return PTR_ERR(mtd);
	ret = mtd_read(mtd, BOSA_BLOB_OFFSET, len, &retlen, buf);
	put_mtd_device(mtd);
	if (ret)
		return ret;
	return retlen == len ? 0 : -EIO;
}

static int gc1601_bosa_write(struct i2c_adapter *adap, u8 reg, const u8 *data,
			     u8 len)
{
	u8 buf[BOSA_CHUNK + 1];
	struct i2c_msg msg = {
		.addr = BOSA_A2_ADDR, .len = len + 1, .buf = buf,
	};

	buf[0] = reg;
	memcpy(buf + 1, data, len);
	return i2c_transfer(adap, &msg, 1) == 1 ? 0 : -EIO;
}

static int gc1601_bosa_read_regs(struct i2c_adapter *adap, u8 reg, u8 *data, u8 len)
{
	struct i2c_msg msgs[] = {
		{ .addr = BOSA_A2_ADDR, .len = 1, .buf = &reg },
		{ .addr = BOSA_A2_ADDR, .flags = I2C_M_RD, .len = len, .buf = data },
	};

	return i2c_transfer(adap, msgs, 2) == 2 ? 0 : -EIO;
}

static int gc1601_bosa_set(struct i2c_adapter *adap, u8 reg, u8 val)
{
	int ret = gc1601_bosa_write(adap, reg, &val, 1);

	msleep(BOSA_WRITE_DELAY_MS);
	return ret;
}

static int gc1601_bosa_program_page(struct gc1601_pon *pon, struct i2c_adapter *adap,
				    const u8 *blob, const struct gc1601_bosa_page *p)
{
	u8 base = p->page ? 0x80 : 0x00;
	u8 back[0x80];
	unsigned int off, n;
	int ret;

	ret = gc1601_bosa_set(adap, BOSA_PAGE, p->page);
	if (ret)
		return ret;

	for (off = 0; off < p->len; off += n) {
		n = min_t(unsigned int, BOSA_CHUNK, p->len - off);
		ret = gc1601_bosa_write(adap, base + off, blob + p->blob + off, n);
		msleep(BOSA_WRITE_DELAY_MS);
		if (ret) {
			dev_err(pon->dev, "BOSA page %u write at %02x failed\n",
				p->page, base + off);
			return ret;
		}
	}

	ret = gc1601_bosa_read_regs(adap, base, back, p->verify);
	if (ret)
		return ret;
	for (off = 0; off < p->verify; off++) {
		if (back[off] != blob[p->blob + off]) {
			dev_err(pon->dev, "BOSA page %u readback %02x: %02x, wrote %02x\n",
				p->page, base + off, back[off], blob[p->blob + off]);
			return -EIO;
		}
	}
	return 0;
}

/* A2 lower half (0x00-0x7f), which is not paged: diagnostics and status. */
int gc1601_bosa_read(struct gc1601_pon *pon, u8 reg, u8 *buf, u8 len)
{
	struct i2c_adapter *adap;
	int ret;

	if (!pon->bosa_bus)
		return -ENODEV;
	adap = of_find_i2c_adapter_by_node(pon->bosa_bus);
	if (!adap)
		return -ENODEV;
	ret = gc1601_bosa_read_regs(adap, reg, buf, len);
	i2c_put_adapter(adap);
	return ret;
}

/* BOSA soft TX disable (A2 0x6e bit 6); the laser also needs the MAC path. */
int gc1601_bosa_soft_tx(struct gc1601_pon *pon, bool enable)
{
	struct i2c_adapter *adap;
	int ret;

	if (!pon->bosa_bus)
		return -ENODEV;
	adap = of_find_i2c_adapter_by_node(pon->bosa_bus);
	if (!adap)
		return -EPROBE_DEFER;
	ret = gc1601_bosa_set(adap, BOSA_PAGE, 0);
	if (!ret)
		ret = gc1601_bosa_set(adap, BOSA_CTRL, enable ? 0 : BOSA_SOFT_TX_DIS);
	i2c_put_adapter(adap);
	return ret;
}

static int gc1601_bosa_program(struct gc1601_pon *pon)
{
	struct i2c_adapter *adap;
	unsigned int i;
	u8 *blob;
	int ret;

	if (!pon->bosa_bus)
		return -ENODEV;

	blob = kmalloc(BOSA_UX3320S_LEN, GFP_KERNEL);
	if (!blob)
		return -ENOMEM;

	ret = gc1601_bosa_read_blob(pon, blob, BOSA_UX3320S_LEN);
	if (ret) {
		dev_err(pon->dev, "cannot read BOSA blob from hwconfig: %d\n", ret);
		goto out_free;
	}
	ret = gc1601_bosa_validate(pon, blob);
	if (ret)
		goto out_free;

	adap = of_find_i2c_adapter_by_node(pon->bosa_bus);
	if (!adap) {
		ret = -EPROBE_DEFER;
		goto out_free;
	}

	/* Hold the laser driver off while its tables change, and leave it so. */
	ret = gc1601_bosa_set(adap, BOSA_PAGE, 0);
	if (!ret)
		ret = gc1601_bosa_set(adap, BOSA_CTRL, BOSA_SOFT_TX_DIS);
	for (i = 0; !ret && i < ARRAY_SIZE(ux3320s_pages); i++)
		ret = gc1601_bosa_program_page(pon, adap, blob, &ux3320s_pages[i]);
	if (!ret)
		ret = gc1601_bosa_set(adap, BOSA_PAGE, 0);
	if (!ret)
		ret = gc1601_bosa_set(adap, BOSA_CTRL, BOSA_SOFT_TX_DIS);

	i2c_put_adapter(adap);
out_free:
	kfree(blob);
	return ret;
}

static const char * const gc1601_bosa_states[] = {
	[GC1601_BOSA_UNKNOWN] = "unknown",
	[GC1601_BOSA_PROGRAMMING] = "programming",
	[GC1601_BOSA_READY] = "ready",
	[GC1601_BOSA_FAILED] = "failed",
};

/* Called with pon->lock held. */
static int gc1601_pon_bosa_load(struct gc1601_pon *pon)
{
	int ret;

	WRITE_ONCE(pon->bosa_state, GC1601_BOSA_PROGRAMMING);
	ret = gc1601_bosa_program(pon);
	WRITE_ONCE(pon->bosa_state, ret ? GC1601_BOSA_FAILED : GC1601_BOSA_READY);
	return ret;
}

#define GC1601_BRINGUP_TRIES	10

#define GC1601_AUTOSTART_SECS	30

static void gc1601_pon_bringup(struct work_struct *w)
{
	struct gc1601_pon *pon = container_of(to_delayed_work(w), struct gc1601_pon,
					      bringup);
	ktime_t start = ktime_get();
	int ret;

	/*
	 * Only the BOSA here: it is the same for both modes (the stock rcS
	 * programs it before loading xpon_sdk). The WAN mode, SerDes and MAC
	 * wait for pon_start, once the mode is known.
	 */
	mutex_lock(&pon->lock);
	gc1601_pon_tx_off(pon);
	ret = gc1601_pon_bosa_load(pon);
	mutex_unlock(&pon->lock);

	/* The BOSA's I2C adapter may probe after us. */
	if (ret == -EPROBE_DEFER && ++pon->bringup_tries < GC1601_BRINGUP_TRIES) {
		queue_delayed_work(system_unbound_wq, &pon->bringup, HZ);
		return;
	}
	if (ret) {
		dev_err(pon->dev, "PON bring-up failed: %d\n", ret);
		return;
	}

	dev_info(pon->dev, "BOSA programmed in %lld ms, laser off\n",
		 ktime_ms_delta(ktime_get(), start));
	/*
	 * The saved GPON/EPON choice lives in UCI, out of reach this early:
	 * pon-mode selects the mode and writes pon_start. Start anyway if it
	 * never does (failsafe, no userspace).
	 */
	if (device_property_read_bool(pon->dev, "galachip,gpon-autostart"))
		queue_delayed_work(system_unbound_wq, &pon->autostart,
				   secs_to_jiffies(GC1601_AUTOSTART_SECS));
}

/*
 * Set up and start the MAC of the current mode, receive-only until
 * allow_tx: chipset_wanmode_probe order, WAN mode first, then the SerDes.
 */
static void gc1601_pon_start(struct gc1601_pon *pon)
{
	ktime_t start = ktime_get();
	int mode, ret;

	/* A mode switch already set up and started the new MAC. */
	if (READ_ONCE(pon->gpon.running) || READ_ONCE(pon->epon.running))
		return;

	mutex_lock(&pon->lock);
	mode = pon->mode;
	gc1601_pon_tx_off(pon);
	gc1601_pon_global_mode(pon, mode);
	ret = gc1601_pon_serdes_init(pon, mode);
	if (!ret)
		gc1601_pon_mac_init(pon);
	mutex_unlock(&pon->lock);
	if (ret) {
		dev_err(pon->dev, "%s mode: SerDes failed: %d\n", gc1601_pon_mode_name(mode), ret);
		return;
	}
	dev_info(pon->dev, "PON up in %s mode in %lld ms: SerDes locked, RX on, laser off\n",
		 gc1601_pon_mode_name(mode), ktime_ms_delta(ktime_get(), start));

	if (mode == GC1601_PON_EPON) {
		mutex_lock(&pon->lock);
		gc1601_epon_start(pon);
		mutex_unlock(&pon->lock);
	} else {
		gc1601_gpon_autostart(pon);
	}
}

static void gc1601_pon_autostart(struct work_struct *w)
{
	struct gc1601_pon *pon = container_of(to_delayed_work(w), struct gc1601_pon,
					      autostart);

	dev_info(pon->dev, "nothing wrote pon_start: starting %s\n",
		 gc1601_pon_mode_name(READ_ONCE(pon->mode)));
	gc1601_pon_start(pon);
}

/*
 * Switch the WAN MAC as chipset_wanmode_probe does: laser off, both
 * machines stopped, mode registers, SerDes and the new MAC set up, then the
 * new machine started receive-only. The 0x90c00008 reset of the stock
 * sdk_top_level_soft_reset is left out: it also resets the packet engine.
 * omci-agent sets allow_tx again.
 */
int gc1601_pon_set_mode(struct gc1601_pon *pon, enum gc1601_pon_mode mode)
{
	ktime_t start = ktime_get();
	enum gc1601_pon_mode old_mode = pon->mode;
	int ret, rollback;

	if (mode == GC1601_PON_EPON && !pon->emac)
		return -EOPNOTSUPP;

	/* Finish a pending boot bring-up (SerDes, BOSA) in the old mode first. */
	flush_delayed_work(&pon->bringup);
	cancel_delayed_work_sync(&pon->autostart);
	/* Both machines' works take the lock: stop them before taking it. */
	gc1601_gpon_stop(pon);
	gc1601_epon_stop(pon);

	mutex_lock(&pon->lock);
	/* Remove stops both machines too; do not start one again on freed state. */
	if (pon->removing) {
		mutex_unlock(&pon->lock);
		return -ESHUTDOWN;
	}
	/* Not gc1601_gpon_set_allow_tx(): it would re-enter the stopped GPON machine. */
	pon->gpon.allow_tx = false;
	gc1601_epon_set_allow_tx(pon, false);
	gc1601_pon_tx_off(pon);
	gc1601_bosa_soft_tx(pon, false);
	gc1601_pon_global_mode(pon, mode);
	ret = gc1601_pon_serdes_init(pon, mode);
	if (ret) {
		gc1601_pon_global_mode(pon, old_mode);
		rollback = gc1601_pon_serdes_init(pon, old_mode);
		if (rollback) {
			dev_err(pon->dev, "mode rollback failed: %d; both MACs stopped\n",
				rollback);
		} else {
			gc1601_pon_mac_init(pon);
			if (old_mode == GC1601_PON_EPON)
				gc1601_epon_start(pon);
			else
				gc1601_gpon_start(pon);
		}
	}
	if (!ret) {
		pon->mode = mode;
		gc1601_pon_mac_init(pon);
		if (mode == GC1601_PON_EPON)
			gc1601_epon_start(pon);
		else
			gc1601_gpon_start(pon);
	}
	mutex_unlock(&pon->lock);
	if (ret) {
		dev_err(pon->dev, "%s mode: SerDes failed: %d\n",
			gc1601_pon_mode_name(mode), ret);
		return ret;
	}

	dev_info(pon->dev, "switched to %s mode in %lld ms, laser off until allow_tx\n",
		 gc1601_pon_mode_name(mode), ktime_ms_delta(ktime_get(), start));
	return 0;
}

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	u32 scu, tx, rx, rxs, onu;

	scu = readl(pon->scu + SCU_PON_STATUS);
	tx = readl(pon->mac + PON_OPT_TX);
	rx = readl(pon->mac + PON_OPT_RX);
	rxs = readl(pon->mac + PON_OPT_RX + PON_OPT_RX_STATUS);
	onu = readl(pon->mac + PON_ONU);

	return sysfs_emit(buf,
			  "mode=%s\n"
			  "pll_locked=%d serdes_ready=%d scu=%08x/%08x/%08x\n"
			  "txpwr=%08x\n"
			  "optical_tx=%08x enable=%d force=%d\n"
			  "optical_rx=%08x rx_enable=%d status=%08x los=%d lof=%d\n"
			  "onu=%08x id=%lu state=%lu\n"
			  "mac_irq=%08x top=%08x/%08x/%08x global=%08x/%08x/%08x/%08x\n"
			  "bosa=%s\n",
			  gc1601_pon_mode_name(READ_ONCE(pon->mode)),
			  !!(scu & SCU_PON_STATUS_PLL_LOCK), !!(scu & SCU_PON_STATUS_READY),
			  scu, readl(pon->scu + SCU_PON_PLL), readl(pon->scu + SCU_PON_RESET),
			  readl(pon->txpwr),
			  tx, !!(tx & PON_OPT_TX_EN), !!(tx & PON_OPT_TX_FORCE),
			  rx, !!(rx & PON_OPT_RX_EN), rxs, !!(rxs & PON_OPT_RX_LOS),
			  !!(rxs & PON_OPT_RX_LOF),
			  onu, FIELD_GET(PON_ONU_ID, onu), FIELD_GET(PON_ONU_STATE, onu),
			  READ_ONCE(pon->gpon.mac_irq_last), readl(pon->top + TOP_ENABLE),
			  readl(pon->top + TOP_CLK_EN), readl(pon->top + TOP_WAN_CLK),
			  readl(pon->global + GLB_CTRL), readl(pon->global + GLB_CFG10),
			  readl(pon->global + GLB_WAN_CFG), readl(pon->global + GLB_CFG18),
			  gc1601_bosa_states[READ_ONCE(pon->bosa_state)]);
}
static DEVICE_ATTR_RO(status);

static ssize_t serdes_init_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	int ret;

	if (!sysfs_streq(buf, "gpon"))
		return -EINVAL;

	mutex_lock(&pon->lock);
	gc1601_pon_tx_off(pon);
	gc1601_pon_global_mode(pon, pon->mode);
	ret = gc1601_pon_serdes_init(pon, pon->mode);
	mutex_unlock(&pon->lock);
	if (ret)
		return ret;

	dev_info(dev, "PON SerDes up in %s mode, optical TX off\n",
		 gc1601_pon_mode_name(pon->mode));
	return count;
}
static DEVICE_ATTR_WO(serdes_init);

static ssize_t gmac_init_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;

	mutex_lock(&pon->lock);
	if (pon->mode != GC1601_PON_GPON) {
		mutex_unlock(&pon->lock);
		return -EBUSY;
	}
	gc1601_pon_tx_off(pon);
	gc1601_pon_gmac_init(pon);
	mutex_unlock(&pon->lock);

	dev_info(dev, "GPON MAC initialised, optical RX on, TX off\n");
	return count;
}
static DEVICE_ATTR_WO(gmac_init);

static ssize_t bosa_program_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	int ret;

	if (!sysfs_streq(buf, "ux3320s"))
		return -EINVAL;

	mutex_lock(&pon->lock);
	gc1601_pon_tx_off(pon);
	ret = gc1601_pon_bosa_load(pon);
	mutex_unlock(&pon->lock);
	if (ret)
		return ret;

	dev_info(dev, "BOSA programmed from hwconfig and verified, soft TX disable set\n");
	return count;
}
static DEVICE_ATTR_WO(bosa_program);

/* pon-mode, once the mode is set: start that mode's MAC (once). */
static ssize_t pon_start_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	flush_delayed_work(&pon->bringup);
	cancel_delayed_work_sync(&pon->autostart);
	gc1601_pon_start(pon);
	return count;
}
static DEVICE_ATTR_WO(pon_start);

static ssize_t tx_hold_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(pon->tx_hold));
}

/*
 * The laser stays off while this is 1, whatever allow_tx says: the OLT's
 * ONU TX power control (CTC 0x00A1) or a saved rogue state at boot.
 */
static void gc1601_pon_update_tx_hold(struct gc1601_pon *pon)
{
	bool hold = pon->tx_hold_admin || pon->tx_hold_timed;

	if (hold == pon->tx_hold)
		return;
	WRITE_ONCE(pon->tx_hold, hold);
	if (pon->mode == GC1601_PON_EPON)
		gc1601_epon_tx_hold(pon, hold);
	else
		gc1601_gpon_tx_hold(pon, hold);
	dev_warn(pon->dev, "transmitter %s\n", hold ? "held off" : "hold released");
}

static void gc1601_pon_tx_hold_expire(struct work_struct *work)
{
	struct gc1601_pon *pon = container_of(to_delayed_work(work),
						 struct gc1601_pon, tx_hold_work);
	unsigned long now;

	mutex_lock(&pon->lock);
	now = jiffies;
	if (!pon->tx_hold_timed || pon->removing)
		goto out;
	if (time_before(now, pon->tx_hold_deadline)) {
		mod_delayed_work(system_wq, &pon->tx_hold_work, pon->tx_hold_deadline - now);
		goto out;
	}
	pon->tx_hold_timed = false;
	pon->tx_hold_deadline = 0;
	gc1601_pon_update_tx_hold(pon);
out:
	mutex_unlock(&pon->lock);
}

static ssize_t tx_hold_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	bool hold;
	int ret;

	ret = kstrtobool(buf, &hold);
	if (ret)
		return ret;
	mutex_lock(&pon->lock);
	if (pon->removing) {
		ret = -ESHUTDOWN;
		goto out;
	}
	/* An explicit admin write supersedes any previous timed ownership. */
	pon->tx_hold_admin = hold;
	pon->tx_hold_timed = false;
	pon->tx_hold_deadline = 0;
	cancel_delayed_work(&pon->tx_hold_work);
	gc1601_pon_update_tx_hold(pon);
out:
	mutex_unlock(&pon->lock);
	return ret ?: count;
}
static DEVICE_ATTR_RW(tx_hold);

static ssize_t tx_hold_seconds_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	unsigned long left = 0, now;

	mutex_lock(&pon->lock);
	now = jiffies;
	if (pon->tx_hold_timed && time_before(now, pon->tx_hold_deadline))
		left = pon->tx_hold_deadline - now;
	mutex_unlock(&pon->lock);
	return sysfs_emit(buf, "%u\n", DIV_ROUND_UP(jiffies_to_msecs(left), 1000));
}

/* Timed CTC holds outlive the daemon; zero releases only timed ownership. */
static ssize_t tx_hold_seconds_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	unsigned int seconds;
	int ret;

	ret = kstrtouint(buf, 0, &seconds);
	if (ret)
		return ret;
	if (seconds >= U16_MAX)
		return -ERANGE;
	mutex_lock(&pon->lock);
	if (pon->removing) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (seconds && (pon->mode != GC1601_PON_EPON || pon->tx_hold_admin)) {
		ret = -EBUSY;
		goto out;
	}
	pon->tx_hold_timed = seconds != 0;
	pon->tx_hold_deadline = seconds ? jiffies + secs_to_jiffies(seconds) : 0;
	if (seconds)
		mod_delayed_work(system_wq, &pon->tx_hold_work, secs_to_jiffies(seconds));
	else
		cancel_delayed_work(&pon->tx_hold_work);
	gc1601_pon_update_tx_hold(pon);
out:
	mutex_unlock(&pon->lock);
	return ret ?: count;
}
static DEVICE_ATTR_RW(tx_hold_seconds);

/*
 * SFF-8472 A2 diagnostics (0x60-0x71), mode independent: EPON has no OMCI
 * ANI-G, so epon-oamd answers CTC optical diagnosis (0x0005) with raw=
 * and LuCI shows the decoded values.
 */
static ssize_t bosa_ddm_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	u8 d[18];
	int ret;

	if (READ_ONCE(pon->bosa_state) != GC1601_BOSA_READY)
		return -EAGAIN;
	ret = gc1601_bosa_read(pon, 0x60, d, sizeof(d));
	if (ret)
		return ret;
	return sysfs_emit(buf,
			  "temp_mc=%d vcc_uv=%u bias_ua=%u tx_nw=%u rx_nw=%u alarms=%#06x raw=%*phN\n",
			  (s16)get_unaligned_be16(d) * 1000 / 256,
			  get_unaligned_be16(d + 2) * 100, get_unaligned_be16(d + 4) * 2,
			  get_unaligned_be16(d + 6) * 100, get_unaligned_be16(d + 8) * 100,
			  get_unaligned_be16(d + 16), 10, d);
}
static DEVICE_ATTR_RO(bosa_ddm);

/*
 * SFF-8472 A2h 0x18-0x27: the module's TX and RX power alarm and warning
 * thresholds, from the calibration blob (high alarm, low alarm, high
 * warning, low warning), in nW like bosa_ddm.
 */
static ssize_t bosa_thresholds_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct gc1601_pon *pon = dev_get_drvdata(dev);
	u8 d[16];
	int ret;

	if (READ_ONCE(pon->bosa_state) != GC1601_BOSA_READY)
		return -EAGAIN;
	ret = gc1601_bosa_read(pon, 0x18, d, sizeof(d));
	if (ret)
		return ret;
	return sysfs_emit(buf,
			  "tx_alarm_high_nw=%u tx_alarm_low_nw=%u tx_warn_high_nw=%u tx_warn_low_nw=%u "
			  "rx_alarm_high_nw=%u rx_alarm_low_nw=%u rx_warn_high_nw=%u rx_warn_low_nw=%u\n",
			  get_unaligned_be16(d) * 100, get_unaligned_be16(d + 2) * 100,
			  get_unaligned_be16(d + 4) * 100, get_unaligned_be16(d + 6) * 100,
			  get_unaligned_be16(d + 8) * 100, get_unaligned_be16(d + 10) * 100,
			  get_unaligned_be16(d + 12) * 100, get_unaligned_be16(d + 14) * 100);
}
static DEVICE_ATTR_RO(bosa_thresholds);

static struct attribute *gc1601_pon_attrs[] = {
	&dev_attr_status.attr,
	&dev_attr_pon_start.attr,
	&dev_attr_bosa_ddm.attr,
	&dev_attr_bosa_thresholds.attr,
	&dev_attr_tx_hold.attr,
	&dev_attr_tx_hold_seconds.attr,
	&dev_attr_serdes_init.attr,
	&dev_attr_gmac_init.attr,
	&dev_attr_bosa_program.attr,
	NULL
};
static const struct attribute_group gc1601_pon_group = {
	.attrs = gc1601_pon_attrs,
};

static const struct attribute_group *gc1601_pon_groups[] = {
	&gc1601_pon_group,
	&gc1601_gpon_group,
	&gc1601_epon_group,
	&gc1601_xpon_group,
	NULL
};

static void __iomem *gc1601_pon_map(struct platform_device *pdev, const char *name)
{
	struct resource *res;

	/* The SCU and global windows belong to other blocks: map, don't claim. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res)
		return IOMEM_ERR_PTR(-ENODEV);
	return devm_ioremap(&pdev->dev, res->start, resource_size(res)) ?:
	       IOMEM_ERR_PTR(-ENOMEM);
}

static int gc1601_pon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gc1601_pon *pon;
	int ret;

	pon = devm_kzalloc(dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;

	pon->dev = dev;
	mutex_init(&pon->lock);
	INIT_DELAYED_WORK(&pon->bringup, gc1601_pon_bringup);
	INIT_DELAYED_WORK(&pon->autostart, gc1601_pon_autostart);
	INIT_DELAYED_WORK(&pon->tx_hold_work, gc1601_pon_tx_hold_expire);

	pon->mac = devm_platform_ioremap_resource_byname(pdev, "mac");
	if (IS_ERR(pon->mac))
		return PTR_ERR(pon->mac);
	pon->serdes = devm_platform_ioremap_resource_byname(pdev, "serdes");
	if (IS_ERR(pon->serdes))
		return PTR_ERR(pon->serdes);
	pon->serdes_ctrl = devm_platform_ioremap_resource_byname(pdev, "serdes-ctrl");
	if (IS_ERR(pon->serdes_ctrl))
		return PTR_ERR(pon->serdes_ctrl);
	pon->scu = gc1601_pon_map(pdev, "scu");
	if (IS_ERR(pon->scu))
		return PTR_ERR(pon->scu);
	pon->txpwr = gc1601_pon_map(pdev, "txpwr");
	if (IS_ERR(pon->txpwr))
		return PTR_ERR(pon->txpwr);
	pon->top = gc1601_pon_map(pdev, "top");
	if (IS_ERR(pon->top))
		return PTR_ERR(pon->top);
	pon->global = gc1601_pon_map(pdev, "global");
	if (IS_ERR(pon->global))
		return PTR_ERR(pon->global);
	/* Older device trees have no EPON MAC window: GPON only. */
	if (platform_get_resource_byname(pdev, IORESOURCE_MEM, "emac")) {
		pon->emac = devm_platform_ioremap_resource_byname(pdev, "emac");
		if (IS_ERR(pon->emac))
			return PTR_ERR(pon->emac);
	}

	pon->bosa_bus = of_parse_phandle(dev->of_node, "galachip,bosa-i2c", 0);

	/* Laser off from boot: the loader leaves the PON block at reset values. */
	gc1601_pon_tx_off(pon);

	platform_set_drvdata(pdev, pon);
	ret = gc1601_gpon_init(pon);
	if (!ret) {
		/* EPON state must exist before xpon_init() makes set_mode reachable. */
		gc1601_epon_init(pon);
		ret = gc1601_xpon_init(pon);
		if (ret)
			gc1601_epon_exit(pon);
	}
	if (ret) {
		/* Drop the diagnostics hooks' pointer to this device. */
		gc1601_gpon_exit(pon);
		of_node_put(pon->bosa_bus);
		return ret;
	}

	if (device_property_read_bool(dev, "galachip,pon-bringup"))
		queue_delayed_work(system_unbound_wq, &pon->bringup, 0);
	return 0;
}

static void gc1601_pon_remove(struct platform_device *pdev)
{
	struct gc1601_pon *pon = platform_get_drvdata(pdev);

	mutex_lock(&pon->lock);
	pon->removing = true;
	mutex_unlock(&pon->lock);
	cancel_delayed_work_sync(&pon->tx_hold_work);
	cancel_delayed_work_sync(&pon->bringup);
	cancel_delayed_work_sync(&pon->autostart);
	gc1601_gpon_exit(pon);
	gc1601_epon_exit(pon);
	gc1601_xpon_exit(pon);
	of_node_put(pon->bosa_bus);
}

static const struct of_device_id gc1601_pon_of_match[] = {
	{ .compatible = "galachip,gc1601-pon" },
	{ }
};
MODULE_DEVICE_TABLE(of, gc1601_pon_of_match);

static struct platform_driver gc1601_pon_driver = {
	.probe = gc1601_pon_probe,
	.remove = gc1601_pon_remove,
	.driver = {
		.name = "gc1601-pon",
		.of_match_table = gc1601_pon_of_match,
		.dev_groups = gc1601_pon_groups,
	},
};
module_platform_driver(gc1601_pon_driver);

MODULE_DESCRIPTION("GalaChip GC1601 GPON and EPON MAC and PON SerDes");
MODULE_LICENSE("GPL");
