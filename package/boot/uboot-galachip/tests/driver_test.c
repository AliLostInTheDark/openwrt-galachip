// SPDX-License-Identifier: GPL-2.0-only
#include "mock.h"
#include <assert.h>
#include <sys/mman.h>
#include "../files/drivers/net/gc1601_eth.c"
#include "../files/drivers/spi/ftspi020.c"
#include "../files/drivers/net/gc1601_mdio.c"
#include "../files/drivers/clk/clk-gc1601.c"

ulong mock_clk_rate = 200000000;
ulong mock_clk_requested;
u32 mock_safe_hz;
struct phy_device mock_phy = { 1, 1000, DUPLEX_FULL };
static bool fifo_stuck, abort_stuck, complete_stuck, tx_stuck;
static bool mdio_stuck, alloc_empty, table_stuck;
static u32 table_reads, global_reset_writes;
static u32 completions, releases, acknowledgments, spi_left, spi_position, spi_opcode;
static u32 spi_written;
const u32 *mock_ranges;
int mock_range_cells;
static u32 raw(uintptr_t a) { return *(u32 *)a; }
static void set(uintptr_t a, u32 v) { *(u32 *)a = v; }

u32 mock_read(uintptr_t a)
{
	if (a == 0xa0000018)
		return fifo_stuck ? 0 : 3;
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
	if (a >= 0x90c00000 && a <= 0x90c40010) {
		static const struct { uintptr_t address; u32 value; } expected[] = {
			{ 0x90c00008, 0x7fffffff }, { 0x90c00008, 0xffffffff },
			{ 0x90c00040, 1 }, { 0x90c00008, 0x80000000 },
			{ 0x90c00008, 0xffffffff }, { 0x90c00000, 0xffffffff },
			{ 0x90c00004, 0xffffffff }, { 0x90c40010, 7 },
			{ 0x90c40008, 0x7d },
		};
		unsigned int step = global_reset_writes++ % ARRAY_SIZE(expected);

		assert(a == expected[step].address && v == expected[step].value);
		assert(!(raw(0x90e00000) & 3) && !raw(0x90dd4000));
		if (!step) {
			set(0x90e00008, 0);
			set(0x90e000e0, 0);
			set(0x90e000e4, 0);
		}
	}
	if (a == 0x90e00008 && v == 0x80000001)
		assert(!(raw(0x90e00000) & 3));
	/* Restoring MAC control must not depend on the unverified command. */
	assert(a != 0x90e000e0);
	if (a == 0x90dc0008 || a == 0x90dc0010)
		assert(global_reset_writes && !(global_reset_writes % 9));
	set(a, v);
	if ((a == 0x90100010 || a == 0x90100014) && v && !mdio_stuck)
		set(0x9010000c, raw(0x9010000c) | (a == 0x90100010 ? BIT(8) : 1));
	if (a == 0xa0000010 && (v & BIT(8)) && !abort_stuck)
		set(a, v & ~BIT(8));
	if (a == 0xa0000100 && spi_left) {
		spi_written += v;
		spi_left -= min(spi_left, 4U);
	}
	if (a == 0xa000000c) {
		spi_opcode = v >> 24;
		spi_left = raw(0xa0000008);
		spi_position = 0;
	}
	if (a == 0x90dc0008)
		set(0x90dc000c, 1);
	if (a == 0x90dc0010)
		set(0x90dc0014, 1);
	if (a == 0x90dd0014 && (v & BIT(27))) {
		table_reads++;
		if (!table_stuck) {
			set(0x90dd0018, 1);
			set(0x90dd001c, v);
		}
	}
	if (a == 0x90dd001c) {
		assert(!raw(0x90dd0020));
		assert(!raw(0x90dd0024));
		assert(!raw(0x90dd0028));
		set(0x90dd0018, 1);
		/* Emulate stale staging left by a preceding transaction. */
		set(0x90dd0020, 0xdeadbeef);
		set(0x90dd0024, 0x12345678);
		set(0x90dd0028, 0xffffffff);
	}
	if (a == 0x90f80038)
		set(0x90f80040, 1);
	if (a == 0x90dd4014) {
		set(a, 0);
		set(0x90dd400c, alloc_empty ? 0 : BIT(31) | 7);
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
	struct ftspi020 p = { .base = (void *)0xa0000000, .input_rate = 200000000 };
	struct udevice bus = { .priv = &p, .name = "spi@a0000000" }, child = { .parent = &bus };
	struct spi_slave slave = { &child };
	u8 bytes[261];
	struct spi_mem_op op = {
		.cmd = { 1, 1, 0x03 }, .addr = { 3, 1, 0 },
		.data = { .nbytes = 259, .buswidth = 1, .dir = SPI_MEM_DATA_IN,
			  .buf.in = bytes + 1 },
	};
	unsigned int i;

	memset(bytes, 0xaa, sizeof(bytes));
	assert(!ftspi020_exec_op(&slave, &op));
	assert(bytes[0] == 0xaa && bytes[260] == 0xaa);
	for (i = 0; i < 259; i++)
		assert(bytes[i + 1] == (i & 255));
	{
		/* Aligned destination: whole words, then the 3-byte tail. */
		static u32 aligned[66];

		memset(aligned, 0xaa, sizeof(aligned));
		op.data.buf.in = aligned;
		assert(!ftspi020_exec_op(&slave, &op));
		for (i = 0; i < 259; i++)
			assert(((u8 *)aligned)[i] == (i & 255));
		assert(((u8 *)aligned)[259] == 0xaa);
		op.data.buf.in = bytes + 1;
	}
	assert(ftspi020_set_speed(&bus, 24999999) == -ERANGE);
	assert(mock_clk_requested == 49999998);
	assert(!ftspi020_set_speed(&bus, 25000000));
	assert(!ftspi020_exec_op(&slave, &op));
	assert((raw(0xa0000010) & 3) == 3);
	{
		/* Bulk reads at spi-max-frequency, everything else at the safe speed. */
		struct spi_mem_op rdid = {
			.cmd = { 1, 1, 0x9f },
			.data = { .nbytes = 63, .buswidth = 1, .dir = SPI_MEM_DATA_IN,
				  .buf.in = bytes },
		};
		struct spi_mem_op wren = { .cmd = { 1, 1, 0x06 } };

		mock_safe_hz = 25000000;
		ftspi020_read_flash(&bus, &p);
		assert(!ftspi020_set_speed(&bus, 100000000));
		assert(mock_clk_requested == 200000000);
		assert(p.bulk_hz == 100000000 && p.safe_hz == 25000000);
		assert(!ftspi020_exec_op(&slave, &op));
		assert((raw(0xa0000010) & 3) == 0);
		assert(!ftspi020_exec_op(&slave, &rdid));
		assert((raw(0xa0000010) & 3) == 3);
		assert(!ftspi020_exec_op(&slave, &op));
		assert(!ftspi020_exec_op(&slave, &wren));
		assert((raw(0xa0000010) & 3) == 3);
		/* The SPI clock stays at 50 MHz: 100 MHz is out of reach, ÷2 is the fastest. */
		mock_clk_rate = 50000000;
		assert(!ftspi020_set_speed(&bus, 100000000));
		assert(!ftspi020_exec_op(&slave, &op));
		assert((raw(0xa0000010) & 3) == 0);
		assert(!ftspi020_exec_op(&slave, &rdid));
		assert((raw(0xa0000010) & 3) == 0);
		mock_clk_rate = 200000000;
		mock_safe_hz = 0;
		p.dt_safe_hz = 0;
		assert(!ftspi020_set_speed(&bus, 50000000));
		assert(p.safe_hz == 50000000);
		assert(!ftspi020_exec_op(&slave, &rdid));
		assert((raw(0xa0000010) & 3) == 1);
		memset(bytes, 0xaa, sizeof(bytes));
	}
	op.data.buswidth = 4;
	assert(!ftspi020_supports_op(&slave, &op));
	op.data.buswidth = 2;
	op.cmd.opcode = 0x3b;
	op.dummy.nbytes = 1;
	op.dummy.buswidth = 1;
	assert(ftspi020_supports_op(&slave, &op));
	memset(bytes, 0xaa, sizeof(bytes));
	assert(!ftspi020_exec_op(&slave, &op));
	assert(raw(0xa000000c) == (0x3b000000 | 1 << 5 | BIT(0)));
	assert((raw(0xa0000004) >> 16 & 0xff) == 8);
	for (i = 0; i < 259; i++)
		assert(bytes[i + 1] == (i & 255));
	op.data.dir = SPI_MEM_DATA_OUT;
	assert(!ftspi020_supports_op(&slave, &op));
	op.data.dir = SPI_MEM_DATA_IN;
	op.cmd.opcode = 0x03;
	op.dummy.nbytes = 0;
	op.data.buswidth = 1;
	assert(ftspi020_set_mode(&bus, SPI_RX_DUAL) == 0);
	assert(ftspi020_set_mode(&bus, SPI_RX_QUAD) == -ENOTSUPP);
	/* Without protected ranges every address and opcode the core sends is run. */
	op.addr.val = 0xfffffe;
	assert(!ftspi020_exec_op(&slave, &op));
	assert(raw(0xa0000000) == 0xfffffe);
	op.addr.val = 0;
	{
		static const u32 data[] = { 1, 2, 3 };
		struct spi_mem_op w = {
			.cmd = { 1, 1, 0x02 }, .addr = { 3, 1, 0x20000 },
			.data = { .nbytes = 11, .buswidth = 1, .dir = SPI_MEM_DATA_OUT,
				  .buf.out = data },
		};
		struct spi_mem_op e = { .cmd = { 1, 1, 0xd8 }, .addr = { 3, 1, 0x3c0000 } };
		struct spi_mem_op wren = { .cmd = { 1, 1, 0x06 } };
		static const u32 addresses[] = { 0, 0x1ff8, 0x20000, 0x3cfff8, 0x3d0000, 0x3f0000 };
		static const u8 other_opcodes[] = { 0x20, 0x52, 0x60, 0xc7, 0x01 };
		unsigned int k;

		assert(!ftspi020_exec_op(&slave, &wren));
		assert(raw(0xa000000c) == (0x06000000 | BIT(1) | BIT(0)));
		assert(!ftspi020_exec_op(&slave, &w));
		assert(spi_opcode == 2 && spi_written == 6 && !spi_left);
		assert(raw(0xa0000008) == 11 && (raw(0xa000000c) & BIT(1)));
		for (k = 0; k < ARRAY_SIZE(addresses); k++) {
			w.addr.val = addresses[k];
			assert(!ftspi020_exec_op(&slave, &w));
			assert(raw(0xa0000000) == addresses[k]);
			e.addr.val = addresses[k];
			assert(!ftspi020_exec_op(&slave, &e));
			assert(raw(0xa0000000) == addresses[k] && spi_opcode == 0xd8);
		}
		e.addr.val = 0x12000;
		for (k = 0; k < ARRAY_SIZE(other_opcodes); k++) {
			e.cmd.opcode = other_opcodes[k];
			e.addr.nbytes = 3;
			assert(!ftspi020_exec_op(&slave, &e));
			assert(spi_opcode == other_opcodes[k]);
			e.addr.nbytes = 0;
			assert(!ftspi020_exec_op(&slave, &e));
			assert(spi_opcode == other_opcodes[k]);
		}

		/* With 0x3d0000-0x3fffff protected, nothing that changes it is run. */
		p.nprotected = 1;
		p.protected[0].start = 0x3d0000;
		p.protected[0].end = 0x400000;
		e.addr.nbytes = 3;
		{
			static const struct { u8 opcode; u32 addr, nbytes; int ret; } cases[] = {
				/* page program, including one that wraps inside its page */
				{ 0x02, 0, 11, 0 }, { 0x02, 0x1ff8, 11, 0 }, { 0x02, 0x20000, 11, 0 },
				{ 0x02, 0x3cff00, 11, 0 }, { 0x02, 0x3cfff8, 11, -EROFS },
				{ 0x02, 0x3d0000, 11, -EROFS }, { 0x02, 0x3f0000, 11, -EROFS },
				{ 0x02, 0x3ffff0, 11, -EROFS },
				/* 4 KiB, 32 KiB and 64 KiB erases, aligned or not */
				{ 0x20, 0x3cf000, 0, 0 }, { 0x20, 0x3cffff, 0, 0 },
				{ 0x20, 0x3d0000, 0, -EROFS }, { 0x20, 0x3d0fff, 0, -EROFS },
				{ 0x52, 0x3c8000, 0, 0 }, { 0x52, 0x3d0000, 0, -EROFS },
				{ 0xd8, 0, 0, 0 }, { 0xd8, 0x3c0000, 0, 0 }, { 0xd8, 0x3cffff, 0, 0 },
				{ 0xd8, 0x3d0000, 0, -EROFS }, { 0xd8, 0x3dffff, 0, -EROFS },
				{ 0xd8, 0x3f0000, 0, -EROFS },
			};

			for (k = 0; k < ARRAY_SIZE(cases); k++) {
				struct spi_mem_op c = cases[k].opcode == 0x02 ? w : e;
				int expect = cases[k].ret;

				c.cmd.opcode = cases[k].opcode;
				c.addr.val = cases[k].addr;
				if (cases[k].opcode == 0x02)
					c.data.nbytes = cases[k].nbytes;
				set(0xa000000c, 0xdeadbeef);
				assert(ftspi020_exec_op(&slave, &c) == expect);
				/* A refused command never reaches the controller. */
				assert((raw(0xa000000c) == 0xdeadbeef) == !!expect);
			}
		}
		/* A chip erase touches the protected range too; other commands are not affected. */
		for (k = 0; k < 2; k++) {
			e.cmd.opcode = k ? 0x60 : 0xc7;
			e.addr.nbytes = 0;
			assert(ftspi020_exec_op(&slave, &e) == -EROFS);
		}
		e.cmd.opcode = 0x01;
		assert(!ftspi020_exec_op(&slave, &e));
		assert(!ftspi020_exec_op(&slave, &wren));
		p.nprotected = 0;
	}
	/* The ranges come from the flash node's property: pairs of offset and size. */
	{
		static const u32 ranges[] = { 0x3d0000, 0x30000, 0x1000, 0, 0x10000, 0x1000,
					      0x20000, 0x1000, 0x30000, 0x1000, 0x40000, 0x1000 };
		struct ftspi020 pp = { 0 };
		struct udevice pbus = { .priv = &pp, .address = (void *)0xa0000000 };

		mock_range_cells = 0;
		assert(!ftspi020_probe(&pbus) && !pp.nprotected);
		mock_ranges = ranges;
		mock_range_cells = ARRAY_SIZE(ranges);
		memset(&pp, 0, sizeof(pp));
		assert(!ftspi020_probe(&pbus));
		/* An empty range is skipped and at most four are kept. */
		assert(pp.nprotected == 4);
		assert(pp.protected[0].start == 0x3d0000 && pp.protected[0].end == 0x400000);
		assert(pp.protected[1].start == 0x10000 && pp.protected[1].end == 0x11000);
		assert(pp.protected[2].start == 0x20000 && pp.protected[2].end == 0x21000);
		assert(pp.protected[3].start == 0x30000 && pp.protected[3].end == 0x31000);
		mock_range_cells = 0;
	}
	fifo_stuck = true;
	assert(ftspi020_exec_op(&slave, &op) == -ETIMEDOUT);
	assert(!p.wedged);
	abort_stuck = true;
	assert(ftspi020_exec_op(&slave, &op) == -ETIMEDOUT);
	assert(p.wedged);
	fifo_stuck = false;
	assert(ftspi020_exec_op(&slave, &op) == -EIO);
	abort_stuck = false;
	assert(!ftspi020_exec_op(&slave, &op));
	complete_stuck = true;
	assert(ftspi020_exec_op(&slave, &op) == -ETIMEDOUT);
	complete_stuck = false;
	assert(!ftspi020_exec_op(&slave, &op));
	puts("SPI: FIFO tails, dual read, per-command speed, protected-range guard, lane/address rejection, timeout, failed abort and recovery passed");
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
	set(0x90dd0020, 0xaaaaaaaa);
	set(0x90dd0024, 0x55555555);
	set(0x90dd0028, 0xffffffff);
	mock_phy.link = 0;
	assert(gc_start(&dev) == -ENOLINK);
	assert(!p.running && p.initialized && !p.failed);
	assert(global_reset_writes == 9);
	mock_phy.link = 1;
	for (i = 0; i < 20; i++) {
		assert(!gc_start(&dev));
		assert(p.initialized && p.running);
		assert(raw(0x90e00008) == 0x80000001);
		assert(!raw(0x90e000e0) && !raw(0x90e000e4));
		assert(raw(0x90dc0028) == FREE && raw(0x90dc0030) == RXRING);
		assert(!gc_send(&dev, input, 14));
		assert(!memcmp((void *)(POOL + 7 * BUFSIZE), input, 14));
		assert(!memcmp((void *)(POOL + 7 * BUFSIZE + 14), (u8[46]){0}, 46));
		gc_stop(&dev);
		assert(!p.failed && !p.tx_owned && !p.running);
	}
	assert(!gc_start(&dev));
	assert(global_reset_writes == 9);
	before = p.tx_submitted;
	alloc_empty = true;
	assert(gc_send(&dev, input, sizeof(input)) == -ENOBUFS);
	assert(p.tx_error == -ENOBUFS && p.tx_errors == 1);
	assert(!p.last_alloc && p.tx_submitted == before && !p.tx_owned);
	alloc_empty = false;
	assert(!gc_send(&dev, input, sizeof(input)));
	assert(!p.tx_error && p.tx_submitted == before + 1);
	assert(p.tx_completed == p.tx_submitted);
	/* Completion can advance without MAC egress: retain both independently. */
	{
		u32 value = 0;

		assert(!diagnostic_count(0x140, 0xc00000, &value));
		assert(value == (BIT(27) | 0xc00000 | 0x140));
		table_stuck = true;
		assert(diagnostic_count(0x140, 0xc00000, &value) == -ETIMEDOUT);
		table_stuck = false;
		before = table_reads;
		diagnostic_queues();
		assert(table_reads == before + 40);
	}
	set(0x90e00714, 12345);
	for (i = 0; i < TRACE_SIZE + 3; i++)
		assert(!gc_send(&dev, input, sizeof(input)));
	{
		struct gc_tx_trace *t = &p.trace[(p.tx_attempts - 1) % TRACE_SIZE];

		assert(t->attempt == p.tx_attempts && t->session == p.starts);
		assert(t->submitted == 1 && !t->error);
		assert(t->completion == 1 && t->alloc == (BIT(31) | 7));
		assert(t->mac_before == 12345 && t->mac_after == 12345);
		assert(!memcmp(t->destination, input, 6));
	}
	d = descriptor(15, 255);
	d[0] = (1U << 29) | sizeof(input);
	d[1] = BIT(12);
	d[2] = (9U << 17) | (3 << 6);
	d[3] = BIT(16);
	set(0x90ddc09c, 1U << 16);
	memcpy((void *)(POOL + 9 * BUFSIZE + 3), input, sizeof(input));
	before = releases;
	capture_stop(&p);
	assert(p.stop[15].pending == 1 && p.stop[15].ready == 1);
	assert(p.stop[15].first_ready == 255 && releases == before);
	assert(pending(15) == 1);
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

	/* Hardware reset value of 0x90a00810 is 0x3900 (BIT(13) already set). */
	set(0x90a00810, 0x3900);
	set(0x91400000, 0);
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
	struct gc1601_clk_priv priv;
	struct udevice dev = { .priv = &priv, .address = (void *)0x90a00000 };
	struct clk clk = { .dev = &dev, .id = GC1601_CLK_APB };
	unsigned int i;

	/* PLL0 from 25 MHz times 16 * 4: 1.6 GHz */
	mock_clk_rate = 25000000;
	set(0x90a00030, 0x10000000);
	set(0x90a00020, 0);
	assert(!gc1601_clk_probe(&dev));
	for (i = 0; i < 4; i++) {
		set(0x90a00020, i << 20);
		assert(gc1601_clk_get_rate(&clk) == (i == 1 ? 133333333 : 66666666));
	}
	clk.id = GC1601_CLK_CPU;
	for (i = 0; i < 4; i++) {
		set(0x90a00020, i << 16);
		assert(gc1601_clk_get_rate(&clk) ==
		       (i == 2 ? 533333333 : i == 1 ? 266666666 : 133333333));
	}
	clk.id = GC1601_CLK_PLL0_OUT3;
	assert(gc1601_clk_get_rate(&clk) == 320000000);
	clk.id = GC1601_CLK_SPI;
	set(0x90a00810, 0);
	assert(gc1601_clk_get_rate(&clk) == 50000000);
	set(0x90a00810, BIT(11));
	assert(gc1601_clk_get_rate(&clk) == 200000000);
	/* The selector bit alone changes; anything above 50 MHz gets 200. */
	set(0x90a00810, 0x5);
	assert(gc1601_clk_set_rate(&clk, 200000000) == 200000000);
	assert(raw(0x90a00810) == (0x5 | BIT(11)));
	assert(gc1601_clk_set_rate(&clk, 50000001) == 200000000);
	assert(gc1601_clk_set_rate(&clk, 50000000) == 50000000);
	assert(raw(0x90a00810) == 0x5);
	clk.id = GC1601_CLK_APB;
	assert(gc1601_clk_set_rate(&clk, 200000000) == (ulong)-EINVAL);
	assert(raw(0x90a00810) == 0x5);
	clk.id = GC1601_CLK_SPI;
	clk.id = GC1601_CLK_COUNT;
	assert(IS_ERR_VALUE(gc1601_clk_get_rate(&clk)));
	set(0x90a00030, 0);
	assert(gc1601_clk_probe(&dev) == -EINVAL);
	if (sizeof(ulong) == 4) {
		set(0x90a00030, 0xff000000);
		assert(gc1601_clk_probe(&dev) == -EINVAL);
	}
	mock_clk_rate = 200000000;
	puts("Clocks: PLL0 outputs, CPU and bus selectors, SPI selector and invalid PLL passed");
}

int main(void)
{
	map(FREE, 0x112000);
	map(0x90dc0000, 0x200000);
	map(0xa0000000, 0x1000);
	map(0x90100000, 0x1000);
	map(0x90c00000, 0x41000);
	map(0x90a00000, 0x1000);
	map(0x91400000, 0x1000);
	/* 32 MiB of DDR: loads between 0x40080000 and 0x41800000, around the DMA area */
	assert(gc1601_ddr_load_range(0x40000000, 0x41800000, 0x41000000, 0x400000));
	assert(gc1601_ddr_load_range(0x40000000, 0x41800000, 0x40080000, 0x780000));
	assert(!gc1601_ddr_load_range(0x40000000, 0x41800000, 0x40070000, 4));
	assert(!gc1601_ddr_load_range(0x40000000, 0x41800000, 0x407fffff, 2));
	assert(!gc1601_ddr_load_range(0x40000000, 0x41800000, 0x40900000, 4));
	assert(!gc1601_ddr_load_range(0x40000000, 0x41800000, 0x417fffff, 2));
	assert(!gc1601_ddr_load_range(0x40000000, 0x41800000, 0xffffffff, 2));
	/* 64 MiB: the top moves with the memory node */
	assert(gc1601_ddr_load_range(0x40000000, 0x43800000, 0x42000000, 0x1000000));
	spi_tests();
	network_tests();
	mdio_tests();
	clock_tests();
	return 0;
}
