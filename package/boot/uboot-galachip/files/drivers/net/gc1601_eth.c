// SPDX-License-Identifier: GPL-2.0-only
/*
 * GC1601 DM Ethernet. Descriptor handling from the GC1601 Linux/V5 port.
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 * Initialization register sequence derived from the Faraday GPL SDK.
 */
#include <dm.h>
#include <net.h>
#include <phy.h>
#include <miiphy.h>
#include <asm/io.h>
#include <asm/barriers.h>
#include <linux/delay.h>
#include <linux/iopoll.h>

#define GMAC ((void *)0x90e00000)
#define BUF ((void *)0x90dc0000)
#define DESC ((void *)0x90dd0000)
#define RX ((void *)0x90dd4000)
#define TX ((void *)0x90ddc040)
#define PENDING ((void *)0x90ddc080)
#define SW ((void *)0x90f80000)
#define TXCFG ((void *)0x90ddc000)
#define FREE 0x40800000
#define POOL 0x40801000
#define RXRING 0x40901000
#define TXRING 0x40911000
#define BUFS 512
#define BUFSIZE 2048
#define SLOTS 256
#define CONTAINERS 16

struct gc_eth {
	struct phy_device *phy;
	u16 consumer[CONTAINERS], synced;
	bool initialized, tx_owned, failed, running;
	u32 starts, tx_attempts, tx_submitted, tx_completed, tx_errors;
	u32 last_alloc, last_completion, rx_packets, rx_dropped;
	int start_error, tx_error;
	u8 packet[BUFSIZE];
};
static struct gc_eth *active;
static int gc_write_hwaddr(struct udevice *dev);

static void gc_barrier(void)
{
	asm volatile("" : : : "memory");
	dsb();
	asm volatile("" : : : "memory");
}

static u32 pending(unsigned int c)
{
	u32 count = readl(PENDING + (c & 7) * 4);

	return c < 8 ? count & 0xffff : count >> 16;
}

static u32 *descriptor(unsigned int c, unsigned int s)
{
	return (u32 *)(RXRING + c * 0x1000 + s * 16);
}

static int consume(unsigned int c, u32 *d)
{
	u32 w1 = readl(d + 1), w2 = readl(d + 2), w3 = readl(d + 3);
	u32 buf = w2 >> 17;

	if (buf >= BUFS)
		return -EIO;
	writel((c & 7) | (((w3 >> 16) & 1) << 3) | 0x10 |
	       ((w1 & BIT(12)) << 2), DESC + 0x64);
	writel(1, DESC + 0x60);
	memset(d, 0, 16);
	gc_barrier();
	writel(buf, RX + 0x10);
	return 0;
}

static int tx_wait(struct gc_eth *p)
{
	u32 count;
	int ret;

	if (!p->tx_owned)
		return p->failed ? -EIO : 0;
	ret = readl_poll_timeout(TX + 8, count, count & 0xffff, 1000000);
	p->last_completion = count;
	/* Never return a hardware-owned buffer to the free list on timeout. */
	if (ret || (count & 0xffff) != 1) {
		p->failed = true;
		return ret ? ret : -EIO;
	}
	p->tx_owned = false;
	p->tx_completed++;
	return 0;
}

static int drain(struct gc_eth *p)
{
	unsigned int c, s;
	int ret;

	for (c = 0; c < CONTAINERS; c++) {
		for (s = 0; s < SLOTS && pending(c); s++) {
			u32 *d = descriptor(c, s);

			if (!(readl(d) & 0x3fff) || !readl(d + 3))
				continue;
			ret = consume(c, d);
			if (ret)
				return ret;
		}
		/* Unmatched ownership is an error, never a fabricated ACK/free. */
		if (pending(c))
			return -EIO;
	}
	p->synced = 0;
	memset(p->consumer, 0, sizeof(p->consumer));
	return 0;
}

static int quiesce(struct gc_eth *p)
{
	int ret;

	writel(0, RX);
	clrbits_le32(GMAC, 3);
	gc_barrier();
	udelay(1000);
	ret = tx_wait(p);
	if (!ret && p->initialized)
		ret = drain(p);
	p->running = false;
	if (ret)
		p->failed = true;
	return ret;
}

int gc1601_eth_handoff(void)
{
	return active ? quiesce(active) : 0;
}

static int table_write(u32 selector, u32 data)
{
	u32 done;

	writel(0, DESC + 0x18);
	writel(selector, DESC + 0x14);
	/* DATA0 commits the complete staging bank, including the upper words. */
	writel(0, DESC + 0x28);
	writel(0, DESC + 0x24);
	writel(0, DESC + 0x20);
	writel(data, DESC + 0x1c);
	return readl_poll_timeout(DESC + 0x18, done, done == 1, 1000);
}

static void sr_update(u32 offset, u32 clear, u32 set)
{
	clrsetbits_le32(SW + offset, clear, set);
}

static void switch_defaults(void)
{
	u32 value, src0 = readl(SW + 0x10000);

	/* SR_default_configs from the GPL, before any service/offload rules. */
	sr_update(0x4000, 0x0f000000, 0);
	sr_update(0x4040, 0x0f000000, 0);
	writel(0x00050003, SW + 0x10060);
	writel(0xff, SW + 0x10064);
	sr_update(0x10008, 0x42, 0xb1);
	writel(0x03000503, SW + 0x10070);
	writel(0x00030000, SW + 0x10074);
	writel(0x000a0000, SW + 0x10078);
	value = (readl(SW + 0x8000) & 0xfff5ffff) | 0x80c0;
	writel((value & 0x9ffffdff) | 0x90f10900, SW + 0x8000);
	value = (readl(SW + 0x800c) & 0xffffffc5) | 5;
	writel((value & 0xffffc5ff) | 0x500, SW + 0x800c);
	sr_update(0x1000c, 4, 0);
	writel(0, SW + 4);
	writel((src0 & ~0x20000) | 0x200000, SW + 0x10000);
	sr_update(0x8004, ~0xf744ff44U, 0x07440044);
	sr_update(0xc000, 0x1f, 0x60);
	sr_update(0xc040, ~0xff249249U, 0x249249);
	sr_update(0x10004, BIT(16), 0);
	writel(0xfe, SW + 0x100e0);
	writel(0xfd, SW + 0x100e4);
	sr_update(0x10068, 0x0303000f, BIT(24) | BIT(17) | 5);
	sr_update(0x10000, BIT(16), BIT(16));
	writel(0x367e * 300, SW + 0x10058);
	sr_update(0x4070, 7, 6);
	sr_update(0x2c, ~0xffffe328U, 9000);
	sr_update(0x2c, 0x8000, 0x4000);
}

static int engine_init(struct gc_eth *p)
{
	u32 status;
	unsigned int i;
	int ret;

	/* Always establish our own SRAM, free list, rings and queue profiles. */
	writel(1, (void *)0x90dc8000);
	writel(~0U, SW + 0x38);
	writel(~0U, SW + 0x3c);
	ret = readl_poll_timeout(SW + 0x40, status,
				status || readl(SW + 0x44), 2500000);
	if (ret)
		return ret;
	writel(0x000a0003, SW + 0x60);
	writel(0x1555, SW + 0x800c);
	writel(0xfd, SW + 0x100fc);
	writel(0x607, TXCFG);
	writel(100, TXCFG + 8);
	writel(0x20, TXCFG + 0x0c);
	writel(~0U, BUF + 8);
	ret = readl_poll_timeout(BUF + 0x0c, status, status, 1000);
	if (ret)
		return ret;
	writel(~0U, BUF + 0x10);
	ret = readl_poll_timeout(BUF + 0x14, status, status, 1000);
	if (ret)
		return ret;
	setbits_le32((void *)0x90de8000, 8);
	for (i = 0; i < BUFS; i++)
		((__be16 *)FREE)[i] = cpu_to_be16(i);
	memset((void *)RXRING, 0, 0x11000);
	writel(~0U, BUF + 4);
	writel(~0U, BUF + 0x44);
	setbits_le32(BUF + 0x20, 2);
	readl(TX + 8);
	writel(0x10, BUF + 0x24);
	writel(0xa, BUF + 0x74);
	clrsetbits_le32(BUF + 0x48, 0x3fff, 2000);
	setbits_le32((void *)0x90de0000, 0x8df);
	writel(0x7cf, (void *)0x90de0028);
	setbits_le32(DESC, 0x14);
	for (i = 0; i < 8; i++) {
		ret = table_write(0x800000 | (0x140 + i), 0x40020);
		if (!ret)
			ret = table_write(0x140 + i, i ? 0x168020 : 0x21c020);
		if (!ret)
			ret = table_write(0x1000000 | (0x140 + i), i ? 0x390 : 0x4f8);
		if (ret)
			return ret;
	}
	for (i = 0; i < 16; i++) {
		ret = table_write(0x180 + i, 0x40060);
		if (ret)
			return ret;
	}
	writel(FREE, BUF + 0x28);
	writel(POOL, BUF + 0x34);
	writel(RXRING, BUF + 0x30);
	writel(TXRING, TX);
	writel(0x30000800, BUF + 0x3c);
	writel(0x60, RX + 0x18);
	writel(0x0f, RX + 0x20);
	writel(0x10, TXCFG + 0x14);
	writel(0, (void *)0x90dd800c);
	writel(0x02000000, RX + 0x18);
	writel(0x51555555, SW + 0x4198);
	switch_defaults();
	/* CPU path only; PON, GEM/T-CONT and optical state remain disabled. */
	writel(0x0800001f, (void *)0x90dc4000);
	gc_barrier();
	p->initialized = true;
	return 0;
}

static int tune_phy(struct phy_device *phy)
{
	static const struct { u8 page, reg; u16 clear, set; } tune[] = {
		{ 2, 0x14, 0x0fff, 0x492 }, { 1, 0x1e, 0, BIT(14) },
		{ 0, 0x10, 0, 0x300 }, { 0, 0x12, 0, BIT(11) },
	};
	int page, val, ret = 0, restore;
	unsigned int i;

	page = phy_read(phy, MDIO_DEVAD_NONE, 0x1f);
	if (page < 0)
		return page;
	for (i = 0; i < ARRAY_SIZE(tune); i++) {
		ret = phy_write(phy, MDIO_DEVAD_NONE, 0x1f, tune[i].page);
		if (ret)
			break;
		val = phy_read(phy, MDIO_DEVAD_NONE, tune[i].reg);
		if (val < 0) {
			ret = val;
			break;
		}
		ret = phy_write(phy, MDIO_DEVAD_NONE, tune[i].reg,
				(val & ~tune[i].clear) | tune[i].set);
		if (ret)
			break;
	}
	restore = phy_write(phy, MDIO_DEVAD_NONE, 0x1f, page);
	return ret ? ret : restore;
}

static int gc_open(struct udevice *dev)
{
	struct gc_eth *p = dev_get_priv(dev);
	u32 cfg;
	int ret;

	if (p->failed)
		return -EIO;
	ret = quiesce(p);
	if (ret)
		return ret;
	if (!p->initialized) {
		ret = engine_init(p);
		if (ret) {
			p->failed = true;
			return ret;
		}
	}
	ret = gc_write_hwaddr(dev);
	if (ret)
		return ret;
	ret = phy_startup(p->phy);
	if (ret)
		return ret;
	if (!p->phy->link)
		return -ENOLINK;
	cfg = 0x00bb0000 | (p->phy->duplex == DUPLEX_FULL ? BIT(13) : 0);
	if (p->phy->speed == SPEED_100)
		cfg |= 0x8000;
	else if (p->phy->speed == SPEED_10)
		cfg |= 0xc000;
	else if (p->phy->speed != SPEED_1000)
		return -EINVAL;
	writel(cfg | 3, GMAC);
	writel(1, RX);
	p->running = true;
	return 0;
}

static int gc_start(struct udevice *dev)
{
	struct gc_eth *p = dev_get_priv(dev);

	p->starts++;
	p->start_error = gc_open(dev);
	return p->start_error;
}

static void gc_stop(struct udevice *dev)
{
	struct gc_eth *p = dev_get_priv(dev);

	if (quiesce(p))
		puts("GC1601: unresolved DMA ownership; power cycle required\n");
}

static int gc_xmit(struct udevice *dev, void *packet, int length)
{
	struct gc_eth *p = dev_get_priv(dev);
	u32 val, buf;
	unsigned int i, size = max(length, 60);
	int ret;

	if (!p->running || p->failed)
		return -EIO;
	if (length < 14 || size > BUFSIZE)
		return -EMSGSIZE;
	ret = tx_wait(p);
	if (ret)
		return ret;
	writel(1, RX + 0x14);
	ret = readl_poll_timeout(RX + 0x14, val, !(val & 1), 100000);
	if (ret)
		return ret;
	val = readl(RX + 0x0c);
	p->last_alloc = val;
	if (!(val & BIT(31)) || (val & ~BIT(31)) >= BUFS)
		return -ENOBUFS;
	buf = val & ~BIT(31);
	memcpy((void *)(POOL + buf * BUFSIZE), packet, length);
	memset((void *)(POOL + buf * BUFSIZE + length), 0, size - length);
	/* Persistent hardware TX cursor: one synchronous packet in all slots. */
	for (i = 0; i < SLOTS; i++) {
		u32 *d = (u32 *)(TXRING + i * 16);

		writel(size, d);
		writel(0x1200, d + 1);
		writel((buf << 17) | 0x4000, d + 2);
		writel(BIT(31), d + 3);
	}
	gc_barrier();
	p->tx_owned = true;
	p->tx_submitted++;
	writel(buf, RX + 0x38);
	writel(1, TX + 4);
	return tx_wait(p);
}

static int gc_send(struct udevice *dev, void *packet, int length)
{
	struct gc_eth *p = dev_get_priv(dev);
	int ret;

	p->tx_attempts++;
	ret = gc_xmit(dev, packet, length);
	p->tx_error = ret;
	if (ret)
		p->tx_errors++;
	return ret;
}

static int gc_recv(struct udevice *dev, int flags, uchar **packetp)
{
	struct gc_eth *p = dev_get_priv(dev);
	unsigned int c, s;
	u32 *d, w0, w1, w2, len, buf, offset;
	bool valid;
	int ret;

	if (!p->running || p->failed)
		return -EIO;
	for (c = 0; c < CONTAINERS; c++) {
		if (!pending(c))
			continue;
		if (!(p->synced & BIT(c))) {
			for (s = 0; s < SLOTS; s++)
				if ((readl(descriptor(c, s)) & 0x3fff) &&
				    readl(descriptor(c, s) + 3))
					break;
			if (s == SLOTS)
				continue;
			p->consumer[c] = s;
			p->synced |= BIT(c);
		}
		d = descriptor(c, p->consumer[c]);
		w0 = readl(d);
		if (!(w0 & 0x3fff) || !readl(d + 3))
			continue;
		w1 = readl(d + 1);
		w2 = readl(d + 2);
		len = w0 & 0x3fff;
		buf = w2 >> 17;
		offset = (w2 >> 6) & 0x7f;
		valid = !(w1 & BIT(31)) && buf < BUFS &&
			len >= 14 && len <= BUFSIZE - offset &&
			(w0 >> 29) == 1 && !(w2 & BIT(1));
		if (valid)
			memcpy(p->packet, (void *)(POOL + buf * BUFSIZE + offset), len);
		ret = consume(c, d);
		if (ret) {
			p->failed = true;
			return ret;
		}
		p->consumer[c] = (p->consumer[c] + 1) % SLOTS;
		if (valid) {
			p->rx_packets++;
			*packetp = p->packet;
			return len;
		}
		p->rx_dropped++;
	}
	return -EAGAIN;
}

void gc1601_eth_diagnostic(void)
{
	struct gc_eth *p = active;
	unsigned int c;

	if (!p) {
		puts("eth: not probed\n");
		return;
	}
	printf("eth: starts=%u start_error=%d initialized=%u running=%u failed=%u owned=%u\n",
	       p->starts, p->start_error, p->initialized, p->running,
	       p->failed, p->tx_owned);
	printf("eth: attempts=%u submitted=%u completed=%u errors=%u last_error=%d alloc=%08x completion=%08x\n",
	       p->tx_attempts, p->tx_submitted, p->tx_completed, p->tx_errors,
	       p->tx_error, p->last_alloc, p->last_completion);
	printf("eth: rx_packets=%u rx_dropped=%u\n", p->rx_packets, p->rx_dropped);
	/* Completion and allocator result registers are not read here. */
	for (c = 0; c < CONTAINERS; c++) {
		u32 *d = descriptor(c, p->consumer[c]);

		printf("eth: container=%u pending=%u consumer=%u synced=%u desc=%08x/%08x/%08x/%08x\n",
		       c, pending(c), p->consumer[c], !!(p->synced & BIT(c)),
		       readl(d), readl(d + 1), readl(d + 2), readl(d + 3));
	}
}

static int gc_write_hwaddr(struct udevice *dev)
{
	struct eth_pdata *pd = dev_get_plat(dev);
	const u8 *a = pd->enetaddr;

	writel((u32)a[2] << 24 | a[3] << 16 | a[4] << 8 | a[5], SW + 0x100a0);
	writel(a[0] << 8 | a[1], SW + 0x100a4);
	return 0;
}

static int gc_probe(struct udevice *dev)
{
	struct gc_eth *p = dev_get_priv(dev);
	int ret;

	p->phy = dm_eth_phy_connect(dev);
	if (!p->phy)
		return -ENODEV;
	/* Generic PHY framework supplies negotiation and link state. */
	ret = phy_config(p->phy);
	if (!ret)
		ret = tune_phy(p->phy);
	if (ret)
		return ret;
	active = p;
	return 0;
}

static const struct eth_ops gc_eth_ops = {
	.start = gc_start, .stop = gc_stop, .send = gc_send, .recv = gc_recv,
	.write_hwaddr = gc_write_hwaddr,
};
static const struct udevice_id gc_eth_ids[] = {
	{ .compatible = "galachip,gc1601-eth" }, {}
};
U_BOOT_DRIVER(gc1601_eth) = {
	.name = "gc1601_eth", .id = UCLASS_ETH, .of_match = gc_eth_ids,
	.probe = gc_probe, .ops = &gc_eth_ops,
	.priv_auto = sizeof(struct gc_eth), .plat_auto = sizeof(struct eth_pdata),
};
