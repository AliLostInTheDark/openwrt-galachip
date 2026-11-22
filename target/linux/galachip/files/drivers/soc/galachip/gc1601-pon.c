// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 PON SerDes and MAC bring-up, GPON or EPON.
 *
 * Status reads are side-effect free. Optical TX is forced off by every
 * bring-up path and the BOSA soft TX disable is left set: nothing here
 * enables the laser.
 *
 * A board with a BOSA (galachip,bosa-i2c) has it programmed once after probe,
 * as the stock firmware does at boot before the PON starts: the UX3320S keeps
 * no calibration across a power cycle on these units. The WAN mode,
 * SerDes and MAC follow, receive-only in GPON mode until userspace allows
 * the transmitter or switches to EPON.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-pon.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <net/xpon.h>

#include "gc1601-pon.h"

static const char *gc1601_pon_mode_name(int mode)
{
	return mode == GC1601_PON_EPON ? "EPON" : "GPON";
}

void gc1601_pon_tx_off(struct gc1601_pon *pon)
{
	void __iomem *reg = pon->mac + PON_OPT_TX;
	u32 val;

	writel(readl(reg) & ~(PON_OPT_TX_EN | PON_OPT_TX_FORCE), reg);
	/* The EPON MAC block is held off in GPON mode: leave it alone there. */
	if (pon->emac && pon->mode == GC1601_PON_EPON)
		writel(readl(pon->emac + EMAC_CTRL) & ~EMAC_CTRL_TX_EN, pon->emac + EMAC_CTRL);

	/* Manual laser power control, laser off */
	val = readl(pon->txpwr) & ~(PON_TXPWR_MANUAL | PON_TXPWR_OFF | PON_TXPWR_SEL);
	writel(val | PON_TXPWR_SEL | PON_TXPWR_OFF, pon->txpwr);
}

/*
 * Laser power on, then the optical TX enable of the active MAC: only via
 * allow_tx.
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

/*
 * Chip set-up for a WAN mode: the global PON enable at 0x90c40020 and the
 * WAN mode register, which selects the GPON or EPON MAC and its upstream
 * clocking.
 * The stock firmware also pulses the 0x90c00008 reset here; that is left out
 * because it also resets the packet engine Ethernet runs on. So is the global
 * interrupt mask, which the Ethernet interrupt may depend on.
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

	/* All PON clocks on */
	writel(0xffffffff, pon->top + TOP_CLK_EN);
	writel(7, pon->global + GLB_CFG10);
	writel(epon ? GLB_WAN_CFG_EPON : GLB_WAN_CFG_GPON, pon->global + GLB_WAN_CFG);
	writel(g18 | BIT(16), pon->global + GLB_CFG18);
	/* Global PON enable */
	writel(readl(pon->txpwr) | PON_TXPWR_GLOBAL_EN, pon->txpwr);
	/* WAN mode and upstream clock */
	writel((topc & 0xffff8000) | (epon ? TOP_WAN_CLK_EPON : TOP_WAN_CLK_GPON),
	       pon->top + TOP_WAN_CLK);
	writel(((g4 | GLB_CTRL_RUN) & ~GLB_CTRL_EPON) | (epon ? GLB_CTRL_EPON_SEL : 0),
	       pon->global + GLB_CTRL);
	/* Stock reads back 0x7d in GPON and 0x7e in EPON. */
	writel((readl(pon->global + GLB_MAC_SEL) | 3) & ~(epon ? BIT(0) : BIT(1)),
	       pon->global + GLB_MAC_SEL);
	writel(epon ? TOP_ENABLE_EPON : TOP_ENABLE_GPON, pon->top + TOP_ENABLE);

	dev_dbg(pon->dev,
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
	/*
	 * Let the MAC send the PLOAM Dying_Gasp itself when the supply drops;
	 * the stock firmware never handles it in software.
	 */
	regmap_clear_bits(pon->scu, GC1601_SCU_PON_DG, GC1601_SCU_PON_DG_SW);

	return phy_set_mode_ext(pon->serdes, PHY_MODE_PON,
				mode == GC1601_PON_EPON ? PHY_PON_EPON : PHY_PON_GPON);
}

/* GPON MAC set-up, with the optical data polarity low as on the stock firmware. */
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

	/* Optical TX delay: 16 and 2, the stock values */
	val = readl(tx + PON_OPT_TX_DELAY) & ~GENMASK(8, 0);
	writel(val | (16 << 4) | 2, tx + PON_OPT_TX_DELAY);

	/* Match stock 0x90c8013c = 0x07d00040 (2000 MTU, 64-byte min frame) */
	if (pon->emac)
		writel((readl(pon->emac + EMAC_MTU) & ~GENMASK(26, 16)) | (2000 << 16) | 0x40,
		       pon->emac + EMAC_MTU);

	/* Receive path only */
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

/* UX3320S pages in the stock programming order: bias, modulation, APD, LUT3, A2 lower. */
static const struct gc1601_bosa_page ux3320s_pages[] = {
	{ 4, 0x200, 0x69, 0x69 },
	{ 5, 0x280, 0x69, 0x69 },
	{ 6, 0x300, 0x69, 0x69 },
	/*
	 * LUT3: 0xd3 onwards holds the ID, markers and live/status registers
	 * (0xde-0xe0, 0xe8-0xea and 0xf2 do not keep written values). Verify
	 * the range the blob's LUT3 checksum covers.
	 */
	{ 3, 0x180, 0x7d, 0x53 },
	/*
	 * A2 lower: thresholds and calibration constants only. The stock tool
	 * writes all 0x80 bytes, but 0x60 onwards is live readings, flags and the
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

/* The checks the stock tool applies to a UX3320S blob before programming it. */
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

/* The unit's calibration, from flash; -EPROBE_DEFER until the flash is registered. */
static u8 *gc1601_bosa_read_blob(struct gc1601_pon *pon)
{
	struct nvmem_cell *cell;
	size_t len;
	u8 *blob;

	cell = nvmem_cell_get(pon->dev, "bosa-calibration");
	if (IS_ERR(cell))
		return ERR_CAST(cell);
	blob = nvmem_cell_read(cell, &len);
	nvmem_cell_put(cell);
	if (!IS_ERR(blob) && len != BOSA_UX3320S_LEN) {
		kfree(blob);
		return ERR_PTR(-EINVAL);
	}
	return blob;
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

static int gc1601_bosa_xfer(struct i2c_adapter *adap, u8 addr, u8 reg, u8 *data, u8 len)
{
	struct i2c_msg msgs[] = {
		{ .addr = addr, .len = 1, .buf = &reg },
		{ .addr = addr, .flags = I2C_M_RD, .len = len, .buf = data },
	};

	return i2c_transfer(adap, msgs, 2) == 2 ? 0 : -EIO;
}

static int gc1601_bosa_read_regs(struct i2c_adapter *adap, u8 reg, u8 *data, u8 len)
{
	return gc1601_bosa_xfer(adap, BOSA_A2_ADDR, reg, data, len);
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

/*
 * SFF-8472 EEPROM for ethtool -m: the A0 identity page, or the unpaged A2
 * lower half, which reads garbage until the BOSA is programmed.
 */
int gc1601_bosa_eeprom_read(struct gc1601_pon *pon, u8 addr, u8 reg, u8 *buf, u8 len)
{
	struct i2c_adapter *adap;
	u8 n;
	int ret = 0;

	if (addr == BOSA_A2_ADDR && READ_ONCE(pon->bosa_state) != GC1601_BOSA_READY)
		return -EAGAIN;
	if (!pon->bosa_bus)
		return -ENODEV;
	adap = of_find_i2c_adapter_by_node(pon->bosa_bus);
	if (!adap)
		return -ENODEV;
	for (; len && !ret; reg += n, buf += n, len -= n) {
		n = min_t(u8, len, BOSA_CHUNK);
		ret = gc1601_bosa_xfer(adap, addr, reg, buf, n);
	}
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

	blob = gc1601_bosa_read_blob(pon);
	if (IS_ERR(blob)) {
		ret = PTR_ERR(blob);
		if (ret != -EPROBE_DEFER)
			dev_err(pon->dev, "cannot read the BOSA calibration: %d\n", ret);
		return ret;
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

/*
 * Set up and start the MAC of the current mode, receive-only until
 * allow_tx: WAN mode first, then the SerDes, as the stock firmware orders it.
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
	dev_info(pon->dev, "%s MAC up\n", gc1601_pon_mode_name(mode));
	dev_dbg(pon->dev, "SerDes locked in %lld ms\n", ktime_ms_delta(ktime_get(), start));

	if (mode == GC1601_PON_EPON) {
		mutex_lock(&pon->lock);
		gc1601_epon_start(pon);
		mutex_unlock(&pon->lock);
	} else {
		gc1601_gpon_autostart(pon);
	}
}

#define GC1601_BRINGUP_TRIES	10

static void gc1601_pon_bringup(struct work_struct *w)
{
	struct gc1601_pon *pon = container_of(to_delayed_work(w), struct gc1601_pon,
					      bringup);
	ktime_t start = ktime_get();
	int ret;

	/* The BOSA first, the same for both modes, as the stock firmware does. */
	mutex_lock(&pon->lock);
	gc1601_pon_tx_off(pon);
	ret = pon->bosa_bus ? gc1601_pon_bosa_load(pon) : 0;
	mutex_unlock(&pon->lock);

	/* The BOSA's I2C adapter may probe after us. */
	if (ret == -EPROBE_DEFER && ++pon->bringup_tries < GC1601_BRINGUP_TRIES) {
		queue_delayed_work(system_unbound_wq, &pon->bringup, HZ);
		return;
	}
	if (ret)
		dev_err(pon->dev, "BOSA bring-up failed: %d\n", ret);
	else
		dev_dbg(pon->dev, "BOSA programmed in %lld ms, laser off\n",
			ktime_ms_delta(ktime_get(), start));

	/* Receive only: the laser waits for allow_tx; pon-mode switches to EPON. */
	gc1601_pon_start(pon);
}

/*
 * Switch the WAN MAC: laser off, both machines stopped, mode registers,
 * SerDes and the new MAC set up, then the new machine started receive-only.
 * The stock firmware's 0x90c00008 reset is left out: it also resets the
 * packet engine.
 * omci.agent sets allow_tx again.
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

/* The PON block's clocks, resets and optical controls, for bring-up debugging. */
static int gc1601_dbg_pon_status_show(struct seq_file *s, void *unused)
{
	struct gc1601_pon *pon = s->private;
	u32 scu = 0, pll = 0, reset = 0, tx, rx, rxs, onu;

	regmap_read(pon->scu, GC1601_SCU_PON_STATUS, &scu);
	regmap_read(pon->scu, GC1601_SCU_PON_PLL, &pll);
	regmap_read(pon->scu, GC1601_SCU_PON_RESET, &reset);
	tx = readl(pon->mac + PON_OPT_TX);
	rx = readl(pon->mac + PON_OPT_RX);
	rxs = readl(pon->mac + PON_OPT_RX + PON_OPT_RX_STATUS);
	onu = readl(pon->mac + PON_ONU);

	seq_printf(s,
		   "mode=%s\n"
		   "pll_locked=%d serdes_ready=%d scu=%08x/%08x/%08x\n"
		   "txpwr=%08x\n"
		   "optical_tx=%08x enable=%d force=%d\n"
		   "optical_rx=%08x rx_enable=%d status=%08x los=%d lof=%d\n"
		   "onu=%08x id=%lu state=%lu\n"
		   "mac_irq=%08x top=%08x/%08x/%08x global=%08x/%08x/%08x/%08x\n"
		   "bosa=%s\n",
		   gc1601_pon_mode_name(READ_ONCE(pon->mode)),
		   !!(scu & GC1601_SCU_PON_STATUS_PLL_LOCK),
		   !!(scu & GC1601_SCU_PON_STATUS_READY), scu, pll, reset,
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
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gc1601_dbg_pon_status);

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

/* Hold the laser off, or release it, until told otherwise. */
int gc1601_pon_set_tx_hold(struct gc1601_pon *pon, bool hold)
{
	int ret = 0;

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
	return ret;
}

/* The transmitter as the xPON core reports it. */
void gc1601_pon_get_tx(struct gc1601_pon *pon, struct xpon_tx_state *tx)
{
	unsigned long left = 0, now;

	mutex_lock(&pon->lock);
	now = jiffies;
	if (pon->tx_hold_timed && time_before(now, pon->tx_hold_deadline))
		left = pon->tx_hold_deadline - now;
	tx->allow = pon->mode == GC1601_PON_EPON ? pon->epon.allow_tx : pon->gpon.allow_tx;
	tx->hold = pon->tx_hold;
	tx->hold_seconds = DIV_ROUND_UP(jiffies_to_msecs(left), 1000);
	tx->identity_wait = pon->gpon.identity_wait;
	mutex_unlock(&pon->lock);
}

/* Timed CTC holds outlive the daemon; zero releases only timed ownership. */
/* The OLT's timed transmitter off (EPON CTC 0x00A1); 0 releases it. */
int gc1601_pon_set_tx_hold_seconds(struct gc1601_pon *pon, u32 seconds)
{
	int ret = 0;

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
	return ret;
}

/*
 * The module's SFF-8472 A2h page: the live readings (0x60-0x71) and the TX
 * and RX power alarm and warning thresholds (0x18-0x27). Mode independent:
 * EPON has no ANI-G, so epon-oamd answers CTC optical diagnosis from these.
 */
int gc1601_pon_get_module(struct gc1601_pon *pon, u8 *diag, u8 *thresholds)
{
	if (READ_ONCE(pon->bosa_state) != GC1601_BOSA_READY)
		return -EAGAIN;
	return gc1601_bosa_read(pon, 0x60, diag, XPON_MODULE_DIAG_LEN) ?:
	       gc1601_bosa_read(pon, 0x18, thresholds, XPON_MODULE_THRESHOLDS_LEN);
}

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
	INIT_DELAYED_WORK(&pon->tx_hold_work, gc1601_pon_tx_hold_expire);

	pon->mac = devm_platform_ioremap_resource_byname(pdev, "mac");
	if (IS_ERR(pon->mac))
		return PTR_ERR(pon->mac);
	pon->serdes = devm_phy_get(dev, "serdes");
	if (IS_ERR(pon->serdes))
		return dev_err_probe(dev, PTR_ERR(pon->serdes), "no SerDes\n");
	pon->scu = syscon_regmap_lookup_by_phandle(dev->of_node, "galachip,scu");
	if (IS_ERR(pon->scu))
		return dev_err_probe(dev, PTR_ERR(pon->scu), "no SCU\n");
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

	pon->debugfs = debugfs_create_dir("gc1601-pon", NULL);
	debugfs_create_file("pon_status", 0400, pon->debugfs, pon, &gc1601_dbg_pon_status_fops);
	gc1601_gpon_debugfs_init(pon);
	gc1601_epon_debugfs_init(pon);
	gc1601_xpon_debugfs_init(pon);

	if (pon->bosa_bus)
		gc1601_pon_hwmon_register(pon);
	queue_delayed_work(system_unbound_wq, &pon->bringup, 0);
	return 0;
}

static void gc1601_pon_remove(struct platform_device *pdev)
{
	struct gc1601_pon *pon = platform_get_drvdata(pdev);

	mutex_lock(&pon->lock);
	pon->removing = true;
	mutex_unlock(&pon->lock);
	debugfs_remove_recursive(pon->debugfs);
	cancel_delayed_work_sync(&pon->tx_hold_work);
	cancel_delayed_work_sync(&pon->bringup);
	gc1601_pon_hwmon_unregister(pon);
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
	},
};
module_platform_driver(gc1601_pon_driver);

MODULE_DESCRIPTION("GalaChip GC1601 GPON and EPON MAC and PON SerDes");
MODULE_LICENSE("GPL");
