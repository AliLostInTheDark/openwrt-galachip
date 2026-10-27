// SPDX-License-Identifier: GPL-2.0-only
#include "mock.h"
#include <assert.h>
#include <sys/mman.h>
#include "../files/drivers/net/gc1601_eth.c"
#include "../files/drivers/spi/ftspi020_gc1601.c"
#include "../files/drivers/net/gc1601_mdio.c"
#include "../files/drivers/clk/clk-gc1601.c"
#include "../files/include/gc1601_bounds.h"

struct phy_device mock_phy = { 1, 1000, DUPLEX_FULL };
static bool fifo_stuck, abort_stuck, complete_stuck, tx_stuck;
static bool mdio_stuck;
static u32 completions, releases, acknowledgments, spi_left, spi_position, spi_opcode;
static u32 raw(uintptr_t a) { return *(u32 *)a; }
static void set(uintptr_t a, u32 v) { *(u32 *)a = v; }

u32 mock_read(uintptr_t a)
{
	if (a == 0xa0000018)
		return fifo_stuck ? 0 : 2;
	if (a == 0xa0000024)
		return complete_stuck || spi_left ? 0 : 1;
	if (a == 0xa0000100) {
		u32 word = 0;
		unsigned int i, n = min(spi_left, 4U);
		for (i = 0; i < n; i++)
			word |= ((spi_position++ & 255) << (i * 8));
		spi_left -= n;
		return word;
	}
	if (a == 0x90ddc048) {
		u32 result = completions;
		completions = 0;
		return result;
	}
	return raw(a);
}

void mock_write(u32 v, uintptr_t a)
{
	set(a, v);
	if ((a == 0x90100010 || a == 0x90100014) && v && !mdio_stuck)
		set(0x9010000c, raw(0x9010000c) | (a == 0x90100010 ? BIT(8) : 1));
	if (a == 0xa0000010 && (v & BIT(8)) && !abort_stuck)
		set(a, v & ~BIT(8));
	if (a == 0xa000000c) {
		spi_opcode = v >> 24;
		spi_left = raw(0xa0000008);
		spi_position = 0;
	}
	if (a == 0x90dc0008)
		set(0x90dc000c, 1);
	if (a == 0x90dc0010)
		set(0x90dc0014, 1);
	if (a == 0x90dd001c)
		set(0x90dd0018, 1);
	if (a == 0x90f80038)
		set(0x90f80040, 1);
	if (a == 0x90dd4014) {
		set(a, 0);
		set(0x90dd400c, BIT(31) | 7);
	}
	if (a == 0x90ddc044 && !tx_stuck)
		completions++;
	if (a == 0x90dd4010)
		releases++;
	if (a == 0x90dd0060) {
		u32 ack = raw(0x90dd0064);
		uintptr_t count = 0x90ddc080 + (ack & 7) * 4;
		u32 decrement = ack & BIT(14) ? 1U << 16 : 1;
		assert(raw(count) >= decrement);
		set(count, raw(count) - decrement);
		acknowledgments++;
	}
}

static void map(uintptr_t start, size_t size)
{
	assert(mmap((void *)start, size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED);
}

static void spi_tests(void)
{
	struct gc_spi p = { (void *)0xa0000000, 200000000, false };
	struct udevice bus = { .priv = &p }, child = { .parent = &bus };
	struct spi_slave slave = { &child };
	u8 bytes[261];
	struct spi_mem_op op = {
		.cmd = { 1, 1, 0x03 }, .addr = { 3, 1, 0 },
		.data = { .nbytes = 259, .buswidth = 1, .dir = SPI_MEM_DATA_IN,
			  .buf.in = bytes + 1 },
	};
	unsigned int i;

	memset(bytes, 0xaa, sizeof(bytes));
	assert(!gc_spi_exec(&slave, &op));
	assert(bytes[0] == 0xaa && bytes[260] == 0xaa);
	for (i = 0; i < 259; i++)
		assert(bytes[i + 1] == (i & 255));
	assert(gc_spi_speed(&bus, 24999999) == -ERANGE);
	assert(!gc_spi_speed(&bus, 25000000));
	assert((raw(0xa0000010) & 3) == 3);
	op.data.buswidth = 2;
	assert(!gc_spi_supports(&slave, &op));
	op.data.buswidth = 1;
	op.addr.val = 0xfffffe;
	assert(gc_spi_exec(&slave, &op) == -EINVAL);
	op.addr.val = 0;
	op.data.dir = SPI_MEM_DATA_OUT;
	assert(gc_spi_exec(&slave, &op) == -EROFS);
	op.data.dir = SPI_MEM_DATA_IN;
	fifo_stuck = true;
	assert(gc_spi_exec(&slave, &op) == -ETIMEDOUT);
	assert(!p.failed);
	abort_stuck = true;
	assert(gc_spi_exec(&slave, &op) == -ETIMEDOUT);
	assert(p.failed);
	fifo_stuck = false;
	assert(gc_spi_exec(&slave, &op) == -EIO);
	abort_stuck = false;
	assert(!gc_spi_exec(&slave, &op));
	complete_stuck = true;
	assert(gc_spi_exec(&slave, &op) == -ETIMEDOUT);
	complete_stuck = false;
	assert(!gc_spi_exec(&slave, &op));
	puts("SPI: FIFO tails, lane/address rejection, timeout, failed abort and recovery passed");
}

static void network_tests(void)
{
	struct gc_eth p = { .phy = &mock_phy };
	struct eth_pdata pd = { .enetaddr = { 2, 0xaa, 0xfe, 0xdc, 0xba, 0x98 } };
	struct udevice dev = { .priv = &p, .plat = &pd };
	u8 input[70], *packet;
	u32 *d;
	unsigned int i, before;

	memset(input, 0x5a, sizeof(input));
	mock_phy.link = 0;
	assert(gc_start(&dev) == -ENOLINK);
	assert(!p.running && p.initialized && !p.failed);
	mock_phy.link = 1;
	for (i = 0; i < 20; i++) {
		assert(!gc_start(&dev));
		assert(p.initialized && p.running);
		assert(raw(0x90dc0028) == FREE && raw(0x90dc0030) == RXRING);
		assert(!gc_send(&dev, input, 14));
		assert(!memcmp((void *)(POOL + 7 * BUFSIZE), input, 14));
		assert(!memcmp((void *)(POOL + 7 * BUFSIZE + 14), (u8[46]){0}, 46));
		gc_stop(&dev);
		assert(!p.failed && !p.tx_owned && !p.running);
	}
	assert(!gc_start(&dev));
	d = descriptor(15, 255);
	d[0] = (1U << 29) | sizeof(input);
	d[1] = BIT(12);
	d[2] = (9U << 17) | (3 << 6);
	d[3] = BIT(16);
	set(0x90ddc09c, 1U << 16);
	memcpy((void *)(POOL + 9 * BUFSIZE + 3), input, sizeof(input));
	before = releases;
	assert(gc_recv(&dev, 0, &packet) == sizeof(input));
	assert(!memcmp(packet, input, sizeof(input)));
	assert(releases == before + 1 && !pending(15));
	gc_stop(&dev);
	assert(!gc_start(&dev));
	d = descriptor(0, 0);
	d[0] = (1U << 29) | 2048;
	d[1] = 0;
	d[2] = (8U << 17) | (127 << 6);
	d[3] = BIT(16);
	set(0x90ddc080, 1);
	before = releases;
	assert(gc_recv(&dev, 0, &packet) == -EAGAIN);
	assert(releases == before + 1 && !pending(0));
	gc_stop(&dev);
	assert(!gc_start(&dev));
	d = descriptor(0, 0);
	d[0] = (1U << 29) | 64;
	d[1] = 0;
	d[2] = 512U << 17;
	d[3] = BIT(16);
	set(0x90ddc080, 1);
	before = acknowledgments;
	assert(gc_recv(&dev, 0, &packet) == -EIO);
	assert(p.failed && acknowledgments == before);
	assert(gc_start(&dev) == -EIO);
	/* Separate device: timed-out TX must remain hardware-owned on stop/retry. */
	memset(&p, 0, sizeof(p));
	p.phy = &mock_phy;
	set(0x90ddc080, 0);
	assert(!gc_start(&dev));
	tx_stuck = true;
	assert(gc_send(&dev, input, sizeof(input)) == -ETIMEDOUT);
	assert(p.tx_owned && p.failed);
	gc_stop(&dev);
	assert(p.tx_owned && p.failed && gc_start(&dev) == -EIO);
	active = &p;
	assert(gc1601_eth_handoff());
	puts("Ethernet: 20 start/stop cycles, TX padding, RX bounds/ack and timeout ownership passed");
}

static void mdio_tests(void)
{
	struct udevice dev = { .address = (void *)0x90100000 };

	assert(!gc_mdio_probe(&dev));
	assert(raw(0x90a00810) & BIT(13));
	assert(raw(0x91400000) == 3);
	set(0x90100008, 0xbeef);
	assert(gc_mdio_read(&dev, 24, MDIO_DEVAD_NONE, 1) == 0xbeef);
	assert(!gc_mdio_write(&dev, 24, MDIO_DEVAD_NONE, 4, 0x1234));
	assert(raw(0x90100004) == 0x1234);
	assert(gc_mdio_read(&dev, 32, MDIO_DEVAD_NONE, 1) == -EINVAL);
	assert(gc_mdio_read(&dev, 24, 1, 1) == -EINVAL);
	set(0x9010000c, BIT(15));
	assert(gc_mdio_read(&dev, 24, MDIO_DEVAD_NONE, 1) == -ETIMEDOUT);
	set(0x9010000c, 0);
	mdio_stuck = true;
	assert(gc_mdio_read(&dev, 24, MDIO_DEVAD_NONE, 1) == -ETIMEDOUT);
	assert(!raw(0x90100014));
	puts("MDIO: PHY enable, Clause 22, bounds, busy and completion timeouts passed");
}

static void clock_tests(void)
{
	struct udevice dev = { .address = (void *)0x90a00000 };
	struct clk clk = { .dev = &dev, .id = 5 };
	unsigned int i;

	set(0x90a00030, 0x10000000);
	for (i = 0; i < 4; i++) {
		set(0x90a00020, i << 20);
		assert(gc1601_get_rate(&clk) == (i == 1 ? 133333333 : 66666666));
	}
	clk.id = 6;
	set(0x90a00810, 0);
	assert(gc1601_get_rate(&clk) == 50000000);
	set(0x90a00810, BIT(11));
	assert(gc1601_get_rate(&clk) == 200000000);
	clk.id = 0;
	set(0x90a00030, 0xff000000);
	assert(IS_ERR_VALUE(gc1601_get_rate(&clk)));
	set(0x90a00030, 0);
	assert(IS_ERR_VALUE(gc1601_get_rate(&clk)));
	puts("Clocks: all bus selectors, SPI parent division, zero and overflow passed");
}

int main(void)
{
	map(FREE, 0x112000);
	map(0x90dc0000, 0x200000);
	map(0xa0000000, 0x1000);
	map(0x90100000, 0x1000);
	map(0x90a00000, 0x1000);
	map(0x91400000, 0x1000);
	assert(gc1601_load_range(0x41000000, 0x400000));
	assert(!gc1601_load_range(0x407fffff, 2));
	assert(!gc1601_load_range(0xffffffff, 2));
	assert(!gc1601_load_range(0x40900000, 4));
	spi_tests();
	network_tests();
	mdio_tests();
	clock_tests();
	return 0;
}
