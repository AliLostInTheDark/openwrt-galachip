// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 packet engine: switch, scheduler and queue tables; bring-up
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include "gc1601-eth.h"

static int gc1601_cvrd_write(struct gc1601_eth *priv, u32 index, u32 data);

int gc1601_queue_read(struct gc1601_eth *priv, u32 selector,
		      unsigned int index, u32 *data, unsigned int words)
{
	void __iomem *base = priv->base[GC1601_DESCRIPTORS];
	u32 done;
	unsigned int i;
	int ret;

	ASSERT_RTNL();
	writel(0, base + GC_DESC_TABLE_DONE);
	writel(GC_DESC_TABLE_READ | selector | index, base + GC_DESC_TABLE_INDEX);
	ret = readl_poll_timeout(base + GC_DESC_TABLE_DONE, done, done == 1, 1, 1000);
	if (ret) {
		/* Do not issue further commands after an uncompleted transaction. */
		priv->queue_read_failed = true;
		return ret;
	}
	for (i = 0; i < words; i++)
		data[i] = readl(base + GC_DESC_TABLE_DATA0 + i * sizeof(u32));
	return 0;
}

static void gc1601_cvrd_check(struct gc1601_eth *priv, const char *name,
			      unsigned int index, u32 expected, u32 actual)
{
	if (expected == actual)
		return;
	priv->cvrd_mismatches++;
	dev_warn(priv->dev, "CVRD %s %03x wrote %08x read %08x\n",
		 name, index, expected, actual);
}

/* Read back every CVRD word setup programmed; the setup log is not evidence. */
static int gc1601_cvrd_verify(struct gc1601_eth *priv)
{
	u32 inmax, outmax, cfg[3], done;
	unsigned int i, index;
	int ret;

	/* Loader-style default writes do not poll; let the last one complete. */
	if (readl_poll_timeout(priv->base[GC1601_DESCRIPTORS] + GC_DESC_TABLE_DONE,
			       done, done == 1, 1, 1000))
		dev_warn(priv->dev, "CVRD completion %08x before readback\n", done);

	priv->cvrd_mismatches = 0;
	for (i = 0; i < GC_CVRD_UNI_QUEUES; i++) {
		index = GC_CVRD_UNI0 + i;
		ret = gc1601_queue_read(priv, GC_CVRD_INMAX, index, &inmax, 1);
		if (!ret)
			ret = gc1601_queue_read(priv, GC_CVRD_OUTMAX, index, &outmax, 1);
		if (!ret)
			ret = gc1601_queue_read(priv, GC_CVRD_ALLCFG, index, cfg, 3);
		if (ret) {
			dev_err(priv->dev, "CVRD readback timed out at %03x\n", index);
			return ret;
		}
		if (priv->cvrd_inmax_written)
			gc1601_cvrd_check(priv, "inmax", index, priv->cvrd_uni_inmax, inmax);
		gc1601_cvrd_check(priv, "outmax", index, priv->cvrd_uni_outmax[i], outmax);
		gc1601_cvrd_check(priv, "allcfg0", index, priv->cvrd_uni_allcfg[i], cfg[0]);
		gc1601_cvrd_check(priv, "allcfg1", index, 0, cfg[1]);
		gc1601_cvrd_check(priv, "allcfg2", index, 0, cfg[2]);
		dev_dbg(priv->dev, "CVRD %03x in %u/%u out %u/%u all %u (raw %08x %08x %08x)%s\n",
			index, GC_CVRD_LO(inmax), GC_CVRD_HI(inmax),
			GC_CVRD_LO(outmax), GC_CVRD_HI(outmax), cfg[0],
			inmax, outmax, cfg[0],
			priv->cvrd_inmax_written ? "" : " inmax inherited");
	}
	for (i = 0; i < GC_CVRD_CPU_QUEUES; i++) {
		index = GC_CVRD_CPU + i;
		ret = gc1601_queue_read(priv, GC_CVRD_OUTMAX, index, &outmax, 1);
		if (ret) {
			dev_err(priv->dev, "CVRD readback timed out at %03x\n", index);
			return ret;
		}
		gc1601_cvrd_check(priv, "cpu outmax", index, priv->cvrd_cpu_outmax, outmax);
	}
	if (priv->cvrd_mismatches)
		dev_warn(priv->dev, "CVRD readback: %u queue mismatches\n",
			 priv->cvrd_mismatches);
	return 0;
}

#define GC1601_SCU_GE_CTRL	0x810
#define GC1601_SCU_GE_PHY_EN	BIT(13)	/* the GE PHY block's clock and reset */

/*
 * The GE PHY's SGMII SerDes: enable the PHY block in the SCU, then step the
 * SerDes control word through the values U-Boot's network setup writes. The
 * SCU bit resets set, but a flash boot never runs U-Boot's networking and
 * leaves the SerDes at 0x09e000b1 instead of 0x09e400b3 without this.
 */
static void gc1601_cold_phy_init(struct gc1601_eth *priv)
{
	static const u32 steps[] = { 0x4000, 0x20000, 0x00020002, 0x00020003, 0x3 };
	void __iomem *sgmii = priv->base[GC1601_SGMII];
	unsigned int i;

	regmap_set_bits(priv->scu, GC1601_SCU_GE_CTRL, GC1601_SCU_GE_PHY_EN);
	usleep_range(50000, 51000);
	for (i = 0; i < ARRAY_SIZE(steps); i++) {
		writel(steps[i], sgmii);
		usleep_range(10000, 11000);
	}
	usleep_range(90000, 91000);
	dev_dbg(priv->dev, "cold init: PHY SerDes initialized\n");
}

/*
 * U-Boot's packet engine set-up up to its SRAM commands, minus the shared
 * global reset and optical writes: switch defaults, TX queue configuration
 * and buffer enable must be in place before SRAM initialisation.
 */
static int gc1601_cold_prepare(struct gc1601_eth *priv)
{
	gc1601_cold_phy_init(priv);
	writel(1, priv->base[GC1601_BUFFER_ENABLE]);
	writel(U32_MAX, priv->switch_base + 0x38);
	writel(U32_MAX, priv->switch_base + 0x3c);
	writel(0x000a0003, priv->switch_base + 0x60);
	writel(0x00001555, priv->switch_base + 0x800c);
	writel(0xfd, priv->switch_base + 0x100fc);
	writel(0x607, priv->base[GC1601_TX_CONFIG]);
	writel(100, priv->base[GC1601_TX_CONFIG] + 0x08);
	writel(0x20, priv->base[GC1601_TX_CONFIG] + 0x0c);
	return 0;
}

/* The loader steps that follow its descriptor table and ring setup. */
static void gc1601_cold_finish(struct gc1601_eth *priv)
{
	writel(0x10, priv->base[GC1601_TX_CONFIG] + 0x14);
	writel(0, priv->queue_base + 0x0c);
	writel(GC1601_BUFFER_COUNT << 16, priv->base[GC1601_RX] + GC_RX_PARTITION);
	writel(0x51555555, priv->switch_base + 0x4198);
}

/*
 * T-CONT queue limits in buffers. Queue 0's output stage carries the rate
 * only with 256 (at 128 it stays empty and upload falls to ~118). Input 256
 * gives burst capacity for 1 Gbps LAN bursts (peak 240/256 with zero drops
 * versus 192/192 saturation at 192) inside the 512-buffer pool.
 */
unsigned int gc1601_tcont_depth = 256;
unsigned int gc1601_tcont_out = 256;

static int gc1601_descriptor_setup(struct gc1601_eth *priv)
{
	void __iomem *base = priv->base[GC1601_DESCRIPTORS];
	unsigned int i;

	writel(readl(base + GC_DESC_CTRL) | 0x14, base + GC_DESC_CTRL);

	/*
	 * Queue 0 may hold 256 buffers and queues 1-7 64 of the 512-buffer
	 * pool: the larger stock limits let a queue drain the pool and hang
	 * the engine. The loader's 32 overflow when the 2.5G PON bursts into
	 * the 1G UNI.
	 */
	for (i = 0; i < GC_CVRD_UNI_QUEUES; i++) {
		u32 index = GC_CVRD_UNI0 + i;
		int ret;

		priv->cvrd_uni_outmax[i] = 32 | ((i ? 64 : 256) << 11);
		priv->cvrd_uni_allcfg[i] = i ? 0x390 : 0x4f8;
		ret = gc1601_cvrd_write(priv, GC_CVRD_INMAX | index, 0x00040020);
		if (!ret)
			ret = gc1601_cvrd_write(priv, GC_CVRD_OUTMAX | index,
						priv->cvrd_uni_outmax[i]);
		if (!ret)
			ret = gc1601_cvrd_write(priv, GC_CVRD_ALLCFG | index,
						priv->cvrd_uni_allcfg[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < GC_CVRD_CPU_QUEUES; i++) {
		int ret = gc1601_cvrd_write(priv, GC_CVRD_OUTMAX | (GC_CVRD_CPU + i),
					    0x00040060);

		if (ret)
			return ret;
	}
	priv->cvrd_inmax_written = true;
	priv->cvrd_uni_inmax = 0x00040020;
	priv->cvrd_cpu_outmax = 0x00040060;
	return 0;
}

static int gc1601_cvrd_write(struct gc1601_eth *priv, u32 index, u32 data)
{
	void __iomem *base = priv->base[GC1601_DESCRIPTORS];
	u32 done;
	int ret;

	writel(0, base + GC_DESC_TABLE_DONE);
	writel(index, base + GC_DESC_TABLE_INDEX);
	writel(data, base + GC_DESC_TABLE_DATA0);
	ret = readl_poll_timeout(base + GC_DESC_TABLE_DONE, done, done == 1, 1, 1000);
	if (ret)
		dev_err(priv->dev, "CVRD write %08x timed out\n", index);
	return ret;
}

/*
 * Every T-CONT queue needs a buffer profile. With the reset value of zero
 * the packet engine drops each upstream frame on enqueue, so the GPON MAC
 * answers its grants with idle bursts only. The GPON upstream is also
 * selected in the scheduler (bits 0 and 2; bit 1 is EPON).
 */
static int gc1601_tcont_queue_setup(struct gc1601_eth *priv)
{
	unsigned int i;
	int ret;

	for (i = 0; i < GC_CVRD_TCONT_QUEUES; i++) {
		bool q0 = !(i % 8);

		ret = gc1601_cvrd_write(priv, GC_CVRD_INMAX | i, 32 | (gc1601_tcont_depth << 11));
		if (!ret)
			ret = gc1601_cvrd_write(priv, GC_CVRD_OUTMAX | i,
						32 | ((q0 ? gc1601_tcont_out : 64) << 11));
		if (!ret)
			ret = gc1601_cvrd_write(priv, GC_CVRD_ALLCFG | i, q0 ? 0x4f8 : 0x390);
		if (ret)
			return ret;
	}
	writel((readl(priv->sched_base) & ~BIT(1)) | BIT(2) | BIT(0), priv->sched_base);
	return 0;
}

/*
 * Switch port VLAN controls (SRC + 0x04). The loader leaves bit 16 set,
 * which drops VLAN-tagged frames on egress to the UNI: nothing tagged
 * leaves the LAN port, so a router tagging the service VLAN itself never
 * hears the OLT. The stock firmware clears it and sets each port's bit in
 * the top byte.
 */
static void gc1601_port_vlan_setup(struct gc1601_eth *priv)
{
	void __iomem *reg;
	u32 val;

	if (!priv->switch_base)
		return;
	reg = priv->switch_base + GC_SR_PORT_VLAN;
	val = readl(reg);
	val &= ~(BIT(16) | GENMASK(9, 8) | GENMASK(1, 0));
	writel(val | GENMASK(25, 24), reg);
}

void gc1601_sr_update(struct gc1601_eth *priv, u32 off, u32 clear, u32 set)
{
	void __iomem *reg = priv->switch_base + off;

	writel((readl(reg) & ~clear) | set, reg);
}

/*
 * Switch set-up as the stock firmware runs it with hardware bridge flooding:
 * table init, L3 lookup and port defaults, isolation, multicast permits,
 * ageing and the CPU trap queue rate limits. The loader leaves the engine
 * minimal, so frames the switch takes are dropped without it.
 */
/*
 * CAR set-up: 20M token reference, fill time 0x30d4, CAR A/B enable, and a
 * packets-per-second limit for CPU queues 1..4 on ports 0 and 1.
 */
static void gc1601_car_init(struct gc1601_eth *priv)
{
	static const u16 pps[4] = { 800, 200, 500, 2000 };
	void __iomem *ca = priv->switch_base + GC_SR_CA;
	void __iomem *car = priv->switch_base + GC_SR_CAR;
	unsigned int q;

	writel(20000000, ca + 0x44);
	writel(20000000, car + 0x0c);
	writel(0x30d4 - 1, car + 0x34);
	writel(readl(car) | 1, car);
	writel(readl(ca) | 1, ca);

	for (q = 1; q <= ARRAY_SIZE(pps); q++) {
		u32 limit = BIT(21) | ((pps[q - 1] / 10) & 0x1fffff);

		writel((readl(ca + 0x80 + 4 * q) & 0xffe00000) | limit,
		       ca + 0x80 + 4 * q);
		writel((readl(ca + 0xc0 + 4 * q) & 0xffe00000) | limit,
		       ca + 0xc0 + 4 * q);
	}
}

int gc1601_sr_hw_init(struct gc1601_eth *priv)
{
	void __iomem *sw = priv->switch_base;
	u32 src0 = readl(sw + GC_SR_SRC);
	u32 val;
	int ret;

	writel(U32_MAX, sw + GC_SR_HA + 0x38);
	writel(U32_MAX, sw + GC_SR_HA + 0x3c);
	ret = readx_poll_timeout(readl, sw + GC_SR_HA + 0x40, val,
				 val || readl(sw + GC_SR_HA + 0x44), 100, 2500000);
	if (ret)
		return ret;

	gc1601_sr_update(priv, GC_SR_L2, 0x0f000000, 0);
	gc1601_sr_update(priv, GC_SR_L2 + 0x40, 0x0f000000, 0);
	writel(0x00050003, sw + GC_SR_SRC + 0x60);
	writel(0xff, sw + GC_SR_SRC + 0x64);
	gc1601_sr_update(priv, GC_SR_SRC + 0x08, 0x42, 0xb1);
	writel(0x03000503, sw + GC_SR_SRC + 0x70);
	writel(0x00030000, sw + GC_SR_SRC + 0x74);
	writel(0x000a0000, sw + GC_SR_SRC + 0x78);

	val = readl(sw + GC_SR_L3);
	val = (val & 0xfff5ffff) | 0x80c0;
	writel((val & 0x9ffffdff) | 0x90f10900, sw + GC_SR_L3);
	val = readl(sw + GC_SR_L3 + 0x0c);
	val = (val & 0xffffffc5) | 0x05;
	writel((val & 0xffffc5ff) | 0x500, sw + GC_SR_L3 + 0x0c);

	gc1601_sr_update(priv, GC_SR_SRC + 0x0c, 0x4, 0);
	writel(0, sw + GC_SR_HA + 0x04);
	writel((src0 & ~0x20000) | 0x200000, sw + GC_SR_SRC);
	gc1601_sr_update(priv, GC_SR_L3 + 0x04, ~0xf744ff44, 0x07440044);
	gc1601_sr_update(priv, GC_SR_HM, 0x1f, 0x60);
	gc1601_sr_update(priv, GC_SR_HM + 0x40, ~0xff249249, 0x249249);
	gc1601_sr_update(priv, GC_SR_SRC + 0x04, BIT(16), 0);

	/* Isolation: UNI (port 0) and PON (port 1) only reach each other. */
	writel(0xfe, sw + GC_SR_SRC + 0xe0);
	writel(0xfd, sw + GC_SR_SRC + 0xe4);
	/* Multicast permits of the UNI and the PON port, as stock */
	gc1601_sr_update(priv, GC_SR_SRC + 0x68,
			 0x0303000f, BIT(24) | BIT(17) | 0x5);
	/* MAC ageing on, 300 s, as stock */
	gc1601_sr_update(priv, GC_SR_SRC, BIT(16), BIT(16));
	writel(0x367e * 300, sw + GC_SR_SRC + 0x58);
	/* Stock switch default: L2+0x70 bits 2:0 */
	gc1601_sr_update(priv, GC_SR_L2 + 0x70, 0x7, 0x6);
	gc1601_sr_update(priv, GC_SR_SRC, BIT(20), 0);

	gc1601_sr_update(priv, GC_SR_HA + 0x2c, ~0xffffe328, 9000);
	gc1601_sr_update(priv, GC_SR_HA + 0x2c, 0x8000, 0x4000);
	priv->sr_ha_frame = readl(sw + GC_SR_HA + 0x2c) & 0x3fff;
	gc1601_car_init(priv);
	return 0;
}

/* Default flow, word 0: flow info 0, queue, egress GEM or UNI. */
#define GC_SR_FLOW_GEM(gem, q)	(0xc0000000 | ((gem) & 0xfff) << 18 | (q) << 9)
#define GC_SR_FLOW_UNI(q)	(0x80000000 | 1 << 18 | (q) << 9)

/*
 * Bypass mode: flow lookup off, everything from the UNI onto one
 * GEM (default class 1) and everything from the PON to the UNI (class 2),
 * forwarded by the switch without the CPU. Upstream carries no T-CONT and
 * leaves on instance 0.
 */
void gc1601_sr_bypass(struct gc1601_eth *priv, bool on, u32 gem)
{
	void __iomem *sw = priv->switch_base;

	writel(0, sw + GC_SR_L3 + 0xa4);
	writel(on ? GC_SR_FLOW_GEM(gem, 0) : 0, sw + GC_SR_L3 + 0xa0);
	writel(0, sw + GC_SR_L3 + 0xac);
	writel(on ? GC_SR_FLOW_UNI(0) : 0, sw + GC_SR_L3 + 0xa8);
	writel(on ? 0x30000 : 0, sw + GC_SR_SRC + 0x74);
	writel(on ? 0x03000503 : 0xff, sw + GC_SR_SRC + 0x70);
	writel(on ? 0 : 0xff, sw + GC_SR_SRC + 0x64);
	writel(on ? 0 : 0x1000f, sw + GC_SR_SRC + 0x60);
	writel(on ? 0 : 0x515, sw + GC_SR_L3 + 0x0c);
	gc1601_sr_update(priv, GC_SR_L3, BIT(23), on ? 0 : BIT(23));
	writel(on ? 0xfe : 0, sw + GC_SR_SRC + 0xc0);
	writel(on ? 0xfd : 0, sw + GC_SR_SRC + 0xc4);
}

/*
 * Both limits reprogram every T-CONT queue live, from debugfs; together
 * they must fit the pool.
 */
int gc1601_tcont_limit_set(struct gc1601_eth *priv, unsigned int *limit, u64 value)
{
	unsigned int depth, out, i;
	int ret = 0;

	rtnl_lock();
	depth = limit == &gc1601_tcont_depth ? value : gc1601_tcont_depth;
	out = limit == &gc1601_tcont_out ? value : gc1601_tcont_out;
	if (value < 32 || value > 256 || depth + out > GC1601_BUFFER_COUNT) {
		rtnl_unlock();
		return -ERANGE;
	}
	*limit = value;
	for (i = 0; i < GC_CVRD_TCONT_QUEUES && !ret; i++) {
		ret = gc1601_cvrd_write(priv, GC_CVRD_INMAX | i, 32 | (depth << 11));
		if (!ret && !(i % 8))
			ret = gc1601_cvrd_write(priv, GC_CVRD_OUTMAX | i, 32 | (out << 11));
	}
	rtnl_unlock();
	return ret;
}

/* Indirect L3 table write: index, then the data words, last word first. */
int gc1601_sr_tbl_write(struct gc1601_eth *priv, u32 table, u32 index,
			const u32 *w, unsigned int n)
{
	void __iomem *l3 = priv->switch_base + GC_SR_L3;
	u32 done;
	int k;

	writel(table << 22 | index, l3 + 0x14);
	for (k = n - 1; k >= 0; k--)
		writel(w[k], l3 + 0x1c + 4 * k);
	return readl_poll_timeout(l3 + 0x18, done, done == 1, 1, 1000);
}

/*
 * L2+0x180..0x19c: 2-bit protocol actions, as the stock firmware leaves
 * them. The loader's values trap nearly every frame from the UNI to the CPU,
 * ahead of any flow or default class.
 */
static const u32 gc1601_sr_traps[] = {
	0x50555155, 0x95545555, 0x10554150, 0xaa550055,
	0x50555155, 0x95555555, 0x11554150, 0xaa550055,
};

/*
 * CPU MAC slot 0: frames to the device's own MAC go to the CPU,
 * ahead of any flow, so the device stays reachable when an untagged flow
 * takes the UNI's frames. The slot's mode in L2+0x18c/0x19c bits 25:24 is 1
 * for a slot holding a MAC; the stock 2 is for an empty slot and drops
 * frames to the MAC.
 */
void gc1601_sfu_cpu_mac(struct gc1601_eth *priv)
{
	const u8 *mac = priv->netdev->dev_addr;

	writel((u32)mac[2] << 24 | mac[3] << 16 | mac[4] << 8 | mac[5],
	       priv->switch_base + GC_SR_L2 + 0xa0);
	writel(mac[0] << 8 | mac[1], priv->switch_base + GC_SR_L2 + 0xa4);
	gc1601_sr_update(priv, GC_SR_L2 + 0x18c, 3 << 24, 1 << 24);
	gc1601_sr_update(priv, GC_SR_L2 + 0x19c, 3 << 24, 1 << 24);
}

/*
 * The stock default parser rules, reduced to what VLAN services use: rule 3
 * (outer VID) and 7 (untagged), enabled in the direction 0 and 1 index
 * lists. Rule 3 matches the stock firmware's table 1 entry.
 */
int gc1601_sfu_rules(struct gc1601_eth *priv)
{
	static const u32 rule_vid[17] = { [0] = 0xe, [4] = 0x00fff000, [14] = 0x00106000 };
	/* Stock rule 6, "svlan": the outer TCI less CFI, P-bit and VID. */
	static const u32 rule_tci[17] = { [0] = 0xe, [4] = 0x0efff000, [14] = 0x00106000 };
	static const u32 rule_untagged[17] = { [14] = 0x00006000 };
	/* Upstream tries rules 3, 6, 7 as stock does; downstream 3, 7. */
	static const u32 index_up[4] = { 0x7183, 0, 0, 0x00150007 };
	static const u32 index_down[4] = { 0x1c3, 0, 0, 0x00150003 };
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(gc1601_sr_traps); i++)
		writel(gc1601_sr_traps[i], priv->switch_base + GC_SR_L2 + 0x180 + 4 * i);
	gc1601_sfu_cpu_mac(priv);
	ret = gc1601_sr_tbl_write(priv, 1, 3, rule_vid, 17);
	if (!ret)
		ret = gc1601_sr_tbl_write(priv, 1, 6, rule_tci, 17);
	if (!ret)
		ret = gc1601_sr_tbl_write(priv, 1, 7, rule_untagged, 17);
	if (!ret)
		ret = gc1601_sr_tbl_write(priv, 0, 0, index_up, 4);
	if (!ret)
		ret = gc1601_sr_tbl_write(priv, 0, 1, index_down, 4);
	return ret;
}

/* Indirect HM table write: index and table, then the data words, last first. */
int gc1601_hm_write(struct gc1601_eth *priv, u32 table, u32 index,
		    const u32 *w, unsigned int n)
{
	void __iomem *hm = priv->switch_base + GC_SR_HM;
	u32 done;
	int k;

	writel(table | index, hm + 0x14);
	for (k = n - 1; k >= 0; k--)
		writel(w[k], hm + 0x1c + 4 * k);
	return readl_poll_timeout(hm + 0x18, done, done == 1, 1, 1000);
}

/*
 * Pause frames go out with the interface's MAC as source: +0x300 holds
 * bytes 4..5 in bits 15:0, +0x304 bytes 0..3.
 */
void gc1601_gmac_pause_addr_apply(struct gc1601_eth *priv)
{
	void __iomem *gmac = priv->base[GC1601_GMAC];
	const u8 *mac = priv->netdev->dev_addr;
	u32 hi = readl(gmac + GC_GMAC_PAUSE_SA_HI) & 0xffff0000;

	writel(hi | get_unaligned_le16(mac + 4), gmac + GC_GMAC_PAUSE_SA_HI);
	writel(get_unaligned_le32(mac), gmac + GC_GMAC_PAUSE_SA_LO);
}

/*
 * VLAN header modifications (add, delete or replace a tag), as the stock
 * firmware programs them: a flow info entry names a
 * sub data entry, which names a 4-step cmddat list and a vlandat entry. Each
 * list starts at the outer tag (offset 12). Flow info fi uses cmddat list
 * fi - 1 and vlandat fi.
 */
const u32 gc1601_hm_cmds[][2] = {
	[GC_HM_PUSH]		= { 0x0002000c, 0x0006ba80 },
	[GC_HM_PUSH_COPY]	= { 0x0002000c, 0x0046a280 },
	[GC_HM_POP]		= { 0x0002000c, 0x00090000 },
	[GC_HM_REPLACE]		= { 0x0002000c, 0x0007d480 },
};

int gc1601_hw_setup(struct gc1601_eth *priv)
{
	u32 mode = readl(priv->base[GC1601_BUFFERS] + GC_BUF_READ_CLEAR);
	bool cold;
	int ret;

	/* An engine the loader never initialised still has reset-default bases. */
	cold = readl(priv->base[GC1601_BUFFERS] + GC_BUF_FREE_LIST_BASE) !=
		GC1601_FREE_LIST_PHYS;
	if (cold) {
		ret = gc1601_cold_prepare(priv);
		if (ret) {
			dev_err(priv->dev, "Cold init needs the switch/queue/scheduler banks\n");
			return ret;
		}
	}

	if (cold) {
		__be16 *free_list = phys_to_virt(GC1601_FREE_LIST_PHYS);
		u32 val;
		int ret;
		unsigned int j;

		writel(U32_MAX, priv->base[GC1601_BUFFERS] + GC_BUF_PM_SRAM_INIT);
		ret = readl_poll_timeout_atomic(priv->base[GC1601_BUFFERS] + GC_BUF_PM_SRAM_STATUS,
						val, val != 0, 1, 1000);
		if (ret) {
			dev_err(priv->dev, "PM SRAM init timed out\n");
			return ret;
		}
		priv->pm_sram_status = val;

		writel(U32_MAX, priv->base[GC1601_BUFFERS] + GC_BUF_OS_SRAM_INIT);
		ret = readl_poll_timeout_atomic(priv->base[GC1601_BUFFERS] + GC_BUF_OS_SRAM_STATUS,
						val, val != 0, 1, 1000);
		if (ret) {
			dev_err(priv->dev, "OS SRAM init timed out\n");
			return ret;
		}
		priv->os_sram_status = val;
		dev_dbg(priv->dev, "SRAM init: PM %08x, OS %08x\n",
			 priv->pm_sram_status, priv->os_sram_status);
		writel(readl(priv->sched_base) | 0x8, priv->sched_base);

		for (j = 0; j < GC1601_BUFFER_COUNT; j++)
			free_list[j] = cpu_to_be16(j);
		dma_sync_single_for_device(priv->dev, GC1601_FREE_LIST_PHYS,
					   GC1601_BUFFER_COUNT * sizeof(__be16),
					   DMA_TO_DEVICE);
	}

	writel(mode | GC_BUF_READ_CLEAR_EN,
	       priv->base[GC1601_BUFFERS] + GC_BUF_READ_CLEAR);
	if (!(readl(priv->base[GC1601_BUFFERS] + GC_BUF_READ_CLEAR) &
	      GC_BUF_READ_CLEAR_EN)) {
		dev_err(priv->dev, "Failed to enable completion read-clear mode\n");
		return -EIO;
	}
	/* Discard inherited completions before publishing our descriptor ring. */
	priv->tx_completion_drained = readl(priv->base[GC1601_TX_RING] +
					 GC_TX_COMPLETIONS) & 0xffff;

	gc1601_rx_drain_inherited(priv, !cold);
	memset(priv->rx_ring, 0, GC1601_RX_CONTAINERS * GC1601_RX_RING_STRIDE);

	/* Invalidate entire packet pool cache at bringup so no stale lines linger */
	dma_sync_single_for_cpu(priv->dev, GC1601_PKT_POOL_PHYS,
				GC1601_BUFFER_COUNT * GC1601_BUFFER_SIZE,
				DMA_FROM_DEVICE);

	memset(priv->tx_ring, 0, GC1601_RING_SIZE * sizeof(struct gc1601_desc));
	memset(priv->rx_consumer, 0, sizeof(priv->rx_consumer));
	priv->rx_synced = 0;
	memset(priv->rx_late_since, 0, sizeof(priv->rx_late_since));
	memset(priv->rx_phantom_since, 0, sizeof(priv->rx_phantom_since));
	priv->rx_next_container = 0;
	priv->rx_stalled = 0;
	priv->tx_producer = 0;
	priv->tx_consumer = 0;
	priv->tx_pending = 0;
	priv->tx_buf_wait = false;
	priv->tx_detected_slot = -1;
	priv->rx_first_slot = -1;

	/* The hardware TX head persists across close; every open re-measures it. */
	priv->tx_probe_sent = 0;
	priv->tx_probe_aligned = false;
	priv->tx_auto_scheduled = false;
	priv->tx_stopping = false;
	priv->tx_probe_failed = false;
	priv->tx_probe_retries = 0;
	priv->tx_replicated = false;
	priv->tx_probe_completed_base = priv->tx_completed_total;
	priv->tx_probe_excess_base = priv->tx_completion_excess;
	memset(priv->tx_probe_octets, 0, sizeof(priv->tx_probe_octets));

	dma_wmb();

	writel(0x10, priv->base[GC1601_BUFFERS] + GC_BUF_CTRL);
	/* Stock value; the loader's 2 held the CPU path to 31 Mbit/s upstream. */
	writel(0xa, priv->base[GC1601_BUFFERS] + GC_BUF_PM_CTRL);
	/* 2000-byte frames, as the stock firmware sets; the loader leaves 1800. */
	writel((readl(priv->base[GC1601_BUFFERS] + GC_BUF_MTU) & ~0x3fff) | 2000,
	       priv->base[GC1601_BUFFERS] + GC_BUF_MTU);
	/*
	 * OS_hw_init, run by stock right after the PMU init: T-CONT shaper
	 * (bit 0), T-CONT scheduler (bit 1) and UNI shaper (bit 4) on, plus
	 * the bits stock never names, over reset-default tables; +0x28 as
	 * stock (the reset value is 0x400).
	 */
	if (priv->os_base) {
		writel(readl(priv->os_base) | 0x8df, priv->os_base);
		writel(0x7cf, priv->os_base + 0x28);
	}
	ret = gc1601_descriptor_setup(priv);
	if (ret)
		return ret;
	ret = gc1601_tcont_queue_setup(priv);
	if (ret)
		return ret;
	if (!priv->tcont_boot_valid && priv->os_base) {
		unsigned int t;

		for (t = 0; t < ARRAY_SIZE(priv->tcont_boot) && !ret; t++)
			ret = gc1601_qos_tables(priv, t, &priv->tcont_boot[t], false);
		if (ret)
			dev_warn(priv->dev, "T-CONT scheduler snapshot failed: %d\n", ret);
		priv->tcont_boot_valid = !ret;
		ret = 0;
	}
	gc1601_port_vlan_setup(priv);
	ret = gc1601_cvrd_verify(priv);
	if (ret)
		return ret;

	writel(GC1601_FREE_LIST_PHYS, priv->base[GC1601_BUFFERS] + GC_BUF_FREE_LIST_BASE);
	writel(GC1601_PKT_POOL_PHYS, priv->base[GC1601_BUFFERS] + GC_BUF_PKT_POOL_BASE);
	writel(priv->rx_ring_dma, priv->base[GC1601_BUFFERS] + GC_BUF_RX_RING_BASE);
	writel(priv->tx_ring_dma, priv->base[GC1601_TX_RING] + GC_TX_RING_BASE);

	writel(0x30000800, priv->base[GC1601_BUFFERS] + GC_BUF_POOL_CFG);
	if (cold)
		writel(0x60, priv->base[GC1601_RX] + GC_RX_PARTITION);
	writel(GC_RX_CTRL_VAL, priv->base[GC1601_RX] + GC_RX_CTRL);

	dev_dbg(priv->dev, "RX setup: partition readback=%08x, rx_ctrl=%08x (readback %08x)\n",
		readl(priv->base[GC1601_RX] + GC_RX_PARTITION),
		GC_RX_CTRL_VAL,
		readl(priv->base[GC1601_RX] + GC_RX_CTRL));

	if (cold) {
		gc1601_cold_finish(priv);
		dev_dbg(priv->dev, "Cold init done: partition readback %08x, tx cfg 00/08/0c/14=%08x/%08x/%08x/%08x\n",
			 readl(priv->base[GC1601_RX] + GC_RX_PARTITION),
			 readl(priv->base[GC1601_TX_CONFIG]),
			 readl(priv->base[GC1601_TX_CONFIG] + 0x08),
			 readl(priv->base[GC1601_TX_CONFIG] + 0x0c),
			 readl(priv->base[GC1601_TX_CONFIG] + 0x14));
	}

	if (priv->base[GC1601_BUFFER_CONTROL])
		writel(0x0800001f, priv->base[GC1601_BUFFER_CONTROL]);
	if (priv->base[GC1601_BUFFER_ENABLE])
		writel(1, priv->base[GC1601_BUFFER_ENABLE]);

	dev_dbg(priv->dev, "SRAM status at end of setup: PM %08x OS %08x\n",
		readl(priv->base[GC1601_BUFFERS] + GC_BUF_PM_SRAM_STATUS),
		readl(priv->base[GC1601_BUFFERS] + GC_BUF_OS_SRAM_STATUS));
	return 0;
}

void gc1601_irq_mask(struct gc1601_eth *priv, bool enable)
{
	u32 mask = enable ? ~(GC_INTR_RX | GC_INTR_TX) : U32_MAX;

	writel(mask, priv->base[GC1601_BUFFERS] + GC_BUF_INTR_MASK);
	writel(mask, priv->base[GC1601_BUFFERS] + GC_BUF_EVENT_MASK);
}

void gc1601_hw_start(struct gc1601_eth *priv)
{
	unsigned long flags;
	u32 acir = 0;
	int ret;

	spin_lock_irqsave(&priv->irq_lock, flags);
	priv->running = true;
	gc1601_irq_mask(priv, true);
	spin_unlock_irqrestore(&priv->irq_lock, flags);

	/* Enable RX gate */
	writel(1, priv->base[GC1601_RX] + GC_RX_ENABLE);

	/* Set GMAC configuration (1000M full duplex default) */
	spin_lock_irqsave(&priv->irq_lock, flags);
	writel(GC_GMAC_CFG_BASE | GC_GMAC_CFG_FULL_DUPLEX |
	       (priv->uni_disabled ? 0 : GC_GMAC_CFG_TX_EN | GC_GMAC_CFG_RX_EN),
	       priv->base[GC1601_GMAC] + GC_GMAC_CFG);
	spin_unlock_irqrestore(&priv->irq_lock, flags);

	/* GMAC interrupt and receive-error control, as the stock firmware sets them. */
	writel(0x0000fffe, priv->base[GC1601_GMAC] + GC_GMAC_IER);
	writel(0x00014001, priv->base[GC1601_GMAC] + GC_GMAC_ERCR);
	/*
	 * The loader polls ACIR bit 2 for 4096 reads and ignores the result.
	 * On the G7 ERCR reads back 0 and ACIR never sets, so keep it short.
	 */
	ret = readl_poll_timeout_atomic(priv->base[GC1601_GMAC] + GC_GMAC_ACIR,
					acir, (acir & 4) != 0, 1, 100);
	if (ret)
		dev_dbg(priv->dev, "GMAC ACIR bit 2 not set (acir=%08x)\n", acir);
	writel(0x80000001, priv->base[GC1601_GMAC] + GC_GMAC_MACCR);
	gc1601_gmac_pause_addr_apply(priv);

	dev_dbg(priv->dev, "GMAC started: cfg=%08x ier=%08x ercr=%08x acir=%08x maccr=%08x\n",
		readl(priv->base[GC1601_GMAC] + GC_GMAC_CFG),
		readl(priv->base[GC1601_GMAC] + GC_GMAC_IER),
		readl(priv->base[GC1601_GMAC] + GC_GMAC_ERCR),
		readl(priv->base[GC1601_GMAC] + GC_GMAC_ACIR),
		readl(priv->base[GC1601_GMAC] + GC_GMAC_MACCR));
}

void gc1601_hw_stop(struct gc1601_eth *priv)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->irq_lock, flags);
	priv->running = false;
	gc1601_irq_mask(priv, false);
	spin_unlock_irqrestore(&priv->irq_lock, flags);

	/* Quiesce GMAC and RX gate */
	writel(GC_GMAC_CFG_BASE, priv->base[GC1601_GMAC] + GC_GMAC_CFG);
	writel(0, priv->base[GC1601_RX] + GC_RX_ENABLE);
}

/*
 * Flow control: program GMAC RX pause (+0x90), GMAC TX pause (+0x70,
 * quanta << 16 | BIT(1)), and switch CAR B +0x2c bit 1, from the resolved
 * pause modes with a quanta of 0xffff.
 */
void gc1601_flow_ctrl_apply(struct gc1601_eth *priv)
{
	struct phy_device *phydev = gc1601_lan_phy(priv);
	void __iomem *gmac = priv->base[GC1601_GMAC];
	bool rx_en = false, tx_en = false;
	u16 quanta = 0xffff;

	if (priv->dsa_lan) {
		/* phylink resolved the pause modes for the LAN port. */
		if (priv->link && priv->duplex == DUPLEX_FULL) {
			rx_en = priv->rx_pause;
			tx_en = priv->tx_pause;
		}
	} else if (phydev && phydev->link && phydev->duplex == DUPLEX_FULL) {
		if (priv->pause_autoneg) {
			phy_get_pause(phydev, &tx_en, &rx_en);
		} else {
			rx_en = priv->rx_pause;
			tx_en = priv->tx_pause;
		}
	} else if (!phydev) {
		rx_en = priv->rx_pause;
		tx_en = priv->tx_pause;
	}

	writel(rx_en ? 1 : 0, gmac + GC_GMAC_PAUSE_RX);
	writel(tx_en ? ((u32)quanta << 16 | BIT(1)) : 0, gmac + GC_GMAC_PAUSE_TX);
	if (priv->switch_base)
		gc1601_sr_update(priv, GC_SR_CAR + 0x2c, BIT(1), tx_en ? BIT(1) : 0);
}

/* Indirect table write: index at +0x14, data from +0x1c, +0x18 reads 1 when done. */
static int gc1601_tbl_write(void __iomem *base, u32 index, const u32 *w, unsigned int n)
{
	unsigned int i;
	u32 done;

	writel(index, base + 0x14);
	for (i = 0; i < n; i++)
		writel(w[i], base + 0x1c + 4 * i);
	return readl_poll_timeout(base + 0x18, done, done == 1, 0, 100);
}

int gc1601_qos_tables(struct gc1601_eth *priv, unsigned int tcont,
		      struct gc1601_qos_tables *tables, bool write)
{
	void __iomem *os = priv->os_base;
	void __iomem *reg;
	unsigned int q, shift = 8 * (tcont % 4);
	u32 done;
	int ret;

	if (!os)
		return -ENODEV;
	reg = os + 0x40 + 4 * (tcont / 4);
	if (write) {
		ret = gc1601_tbl_write(os, tcont, &tables->sp, 1);
	} else {
		writel(BIT(27) | tcont, os + 0x14);
		ret = readl_poll_timeout(os + 0x18, done, done == 1, 0, 100);
		tables->sp = readl(os + 0x1c);
		tables->enable = readl(reg) >> shift;
	}
	for (q = 0; q < 8 && !ret; q++) {
		u32 index = tcont * 8 + q;

		if (write) {
			ret = gc1601_tbl_write(os, 0x400000 | index, &tables->fill[q], 1);
			ret = ret ?: gc1601_cvrd_write(priv, GC_CVRD_INMAX | index,
							 tables->inmax[q]);
			ret = ret ?: gc1601_cvrd_write(priv, GC_CVRD_OUTMAX | index,
							 tables->outmax[q]);
			if (!ret) {
				void __iomem *cvrd = priv->base[GC1601_DESCRIPTORS];
				int k;

				writel(0, cvrd + 0x18);
				writel(GC_CVRD_ALLCFG | index, cvrd + 0x14);
				for (k = 2; k >= 0; k--)
					writel(tables->cfg[q][k], cvrd + 0x1c + 4 * k);
				ret = readl_poll_timeout(cvrd + 0x18, done, done == 1, 0, 100);
			}
		} else {
			writel(BIT(27) | 0x400000 | index, os + 0x14);
			ret = readl_poll_timeout(os + 0x18, done, done == 1, 0, 100);
			tables->fill[q] = readl(os + 0x1c);
			ret = ret ?: gc1601_queue_read(priv, GC_CVRD_INMAX, index,
							 &tables->inmax[q], 1);
			ret = ret ?: gc1601_queue_read(priv, GC_CVRD_OUTMAX, index,
							 &tables->outmax[q], 1);
			ret = ret ?: gc1601_queue_read(priv, GC_CVRD_ALLCFG, index,
							 tables->cfg[q], 3);
		}
	}
	if (!ret && write)
		writel((readl(reg) & ~(0xffU << shift)) | (u32)tables->enable << shift, reg);
	return ret;
}

/*
 * Enable the T-CONT's scheduler when it is set up, the counterpart of the
 * disable in gc1601_pe_clear_tcont(). Without it a
 * T-CONT withdrawn by PLOAM and assigned again gets grants but sends
 * nothing.
 */
int gc1601_pe_enable_tcont(unsigned int tcont)
{
	struct gc1601_eth *priv;
	void __iomem *reg;
	int ret = 0;

	if (tcont >= 16)
		return -EINVAL;
	rtnl_lock();
	mutex_lock(&gc1601_uni_lock);
	if (!gc1601_pe_netdev) {
		ret = -ENODEV;
		goto out;
	}
	priv = netdev_priv(gc1601_pe_netdev);
	if (!priv->os_base) {
		ret = -ENODEV;
		goto out;
	}
	reg = priv->os_base + 0x40 + 4 * (tcont / 4);
	writel(readl(reg) | 0xff << (8 * (tcont % 4)), reg);
out:
	mutex_unlock(&gc1601_uni_lock);
	rtnl_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(gc1601_pe_enable_tcont);

/* Removing a T-CONT disables only its instance, not the shared scheduler. */
int gc1601_pe_clear_tcont(unsigned int tcont)
{
	struct gc1601_eth *priv;
	void __iomem *os, *reg;
	u32 sp = 0xff, zero = 0, max = 0x7ffffff, fill = 0x1fffff;
	unsigned int q, shift;
	int ret = 0, err;

	if (tcont >= 16)
		return -EINVAL;
	rtnl_lock();
	mutex_lock(&gc1601_uni_lock);
	if (!gc1601_pe_netdev) {
		ret = -ENODEV;
		goto out;
	}
	priv = netdev_priv(gc1601_pe_netdev);
	os = priv->os_base;
	if (!os) {
		ret = -ENODEV;
		goto out;
	}
	reg = os + 0x40 + 4 * (tcont / 4);
	shift = 8 * (tcont % 4);
	writel(readl(reg) & ~(0xff << shift), reg);
	gc1601_pe_flush_tcont(tcont);
	if (priv->tcont_boot_valid) {
		/*
		 * Back to the boot policy rather than stock's all-strict reset,
		 * so a T-CONT that carries several queues schedules the same
		 * after an outage. The enable byte stays off until reassigned.
		 */
		struct gc1601_qos_tables boot = priv->tcont_boot[tcont];

		boot.enable = 0;
		err = gc1601_qos_tables(priv, tcont, &boot, true);
	} else {
		err = gc1601_tbl_write(os, tcont, &sp, 1);
		for (q = tcont * 8; q < (tcont + 1) * 8 && !err; q++)
			err = gc1601_tbl_write(os, 0x400000 | q, &zero, 1);
	}
	ret = ret ?: err;
	for (q = tcont * 8; q < (tcont + 1) * 8; q++) {
		err = gc1601_tbl_write(os, 0xc00000 | q, &max, 1);
		ret = ret ?: err;
		err = gc1601_tbl_write(os, 0x800000 | q, &fill, 1);
		ret = ret ?: err;
	}
	err = gc1601_tbl_write(os, 0x1400000 | tcont, &max, 1);
	ret = ret ?: err;
	err = gc1601_tbl_write(os, 0x1000000 | tcont, &fill, 1);
	ret = ret ?: err;
out:
	mutex_unlock(&gc1601_uni_lock);
	rtnl_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(gc1601_pe_clear_tcont);
