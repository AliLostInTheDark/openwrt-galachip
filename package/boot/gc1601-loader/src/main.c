// SPDX-License-Identifier: GPL-2.0-only
/*
 * GC1601 first-stage loader for the TP-Link XZ000-G7.
 *
 * Replaces the stock U-Boot SPL at flash offset 0. It sets up DDR exactly as
 * the stock SPL does, then boots the OpenWrt kernel tag at flash 0x20000 the
 * way the stock U-Boot's fpgaBoot does. A key pressed on the console during
 * the countdown opens a menu: boot a kernel sent over XMODEM (recovery
 * without a programmer), boot from flash, or start the stock U-Boot that
 * still sits at flash 0x2000.
 */

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define REG(a)			(*(volatile u32 *)(a))

/* UART0, 16550 with 32-bit register stride, 33 MHz input clock */
#define UART			0x90400000
#define UART_RBR		(UART + 0x00)
#define UART_THR		(UART + 0x00)
#define UART_DLL		(UART + 0x00)
#define UART_DLM		(UART + 0x04)
#define UART_FCR		(UART + 0x08)
#define UART_LCR		(UART + 0x0c)
#define UART_MCR		(UART + 0x10)
#define UART_LSR		(UART + 0x14)
#define UART_LSR_DR		0x01
#define UART_LSR_THRE		0x20

/* DDR controller, programmed from the stock SPL's table */
#define DDR			0x90b00000

/* FTSPI020, driven as the mask ROM drives it */
#define SPI			0xa0000000
#define SPI_ADDR		(SPI + 0x00)
#define SPI_CMD1		(SPI + 0x04)	/* address length, dummy */
#define SPI_LEN			(SPI + 0x08)
#define SPI_CMD3		(SPI + 0x0c)	/* opcode, start */
#define SPI_CTRL		(SPI + 0x10)
#define SPI_STATUS		(SPI + 0x18)
#define SPI_STATUS_RX		0x02
#define SPI_ISR			(SPI + 0x24)
#define SPI_ISR_CMD_DONE	0x01
#define SPI_RESET		(SPI + 0x48)
#define SPI_FEATURE		(SPI + 0x54)
#define SPI_DATA		(SPI + 0x100)

/* Flash layout: stock U-Boot at 0x2000, OpenWrt firmware tag at 0x20000 */
#define UBOOT_FLASH		0x2000
#define UBOOT_SIZE		0x1e000
#define UBOOT_ENTRY		0x40100000	/* where the stock SPL runs it */
#define FW_FLASH		0x20000
#define FW_MAX			0x3b0000	/* firmware partition minus the tag */
#define TAG_MAGIC		0x03000003
#define TAG_KERNEL_LEN		0x78
#define TAG_SIZE		0x200
#define LOAD_ADDR		0x41000000	/* as the stock fpgaBoot */
#define XMODEM_MAX		0x00800000	/* up to the reserved DMA area */

#define KEY_WAIT_POLLS		1500000		/* about 1 s of LSR reads */
#define HW_WAIT_POLLS		10000000

void jump(u32 entry, u32 r0, u32 r1, u32 r2);

static const u32 ddr_table[][2] = {
	{ 0x000, 0x08629d11 }, { 0x004, 0x00000000 }, { 0x008, 0x00000033 },
	{ 0x00c, 0x00000000 }, { 0x010, 0x40000101 }, { 0x014, 0x06040503 },
	{ 0x018, 0x01210101 }, { 0x01c, 0x05000118 }, { 0x020, 0x00001f41 },
	{ 0x024, 0x00000077 }, { 0x028, 0x00000000 }, { 0x02c, 0x00000000 },
	{ 0x030, 0x16ffd5e5 }, { 0x034, 0x0f050506 }, { 0x038, 0x05080305 },
	{ 0x03c, 0x00010010 }, { 0x040, 0x0000aa00 }, { 0x048, 0x00555500 },
	{ 0x04c, 0x00001005 }, { 0x060, 0x00ff0000 }, { 0x064, 0x00000000 },
	{ 0x068, 0x00000000 }, { 0x06c, 0x00000000 }, { 0x070, 0x000000ff },
	{ 0x074, 0x00000011 }, { 0x078, 0x00000077 }, { 0x07c, 0x00000000 },
	{ 0x0a0, 0x81818181 }, { 0x0a4, 0x81818181 }, { 0x0a8, 0x00004e20 },
	{ 0x0ac, 0x0000c350 }, { 0x0b0, 0x00000002 }, { 0x0b4, 0x03030404 },
	{ 0x0b8, 0x01010202 }, { 0x130, 0x00000077 }, { 0x138, 0x00000005 },
};

/* ---- console ---- */

/* The mask ROM's set-up: 8N1, divisor 18 (115200 baud from 33 MHz) */
static void uart_init(void)
{
	REG(UART_LCR) = 0x83;
	REG(UART_DLL) = 0x12;
	REG(UART_DLM) = 0;
	REG(UART_LCR) = 0x03;
	REG(UART_MCR) = 0x03;
	REG(UART_FCR) = 0x07;
}

static void con_putc(int c)
{
	while (!(REG(UART_LSR) & UART_LSR_THRE))
		;
	REG(UART_THR) = c;
}

static void con_puts(const char *s)
{
	for (; *s; s++) {
		if (*s == '\n')
			con_putc('\r');
		con_putc(*s);
	}
}

static void con_puthex(u32 v)
{
	int i;

	con_puts("0x");
	for (i = 28; i >= 0; i -= 4)
		con_putc("0123456789abcdef"[(v >> i) & 0xf]);
}

/* A received byte, or -1 after @polls status reads. */
static int con_getc_timeout(u32 polls)
{
	do {
		if (REG(UART_LSR) & UART_LSR_DR)
			return REG(UART_RBR) & 0xff;
	} while (polls--);
	return -1;
}

static int con_getc(void)
{
	while (!(REG(UART_LSR) & UART_LSR_DR))
		;
	return REG(UART_RBR) & 0xff;
}

/* ---- DDR ---- */

static int ddr_init(void)
{
	u32 i, wait;

	for (i = 0; i < sizeof(ddr_table) / sizeof(ddr_table[0]); i++)
		REG(DDR + ddr_table[i][0]) = ddr_table[i][1];
	REG(DDR + 0x04) = 1;
	for (wait = HW_WAIT_POLLS; wait; wait--)
		if (REG(DDR + 0x04) == 0xc100)
			break;
	if (!wait)
		return -1;
	for (wait = HW_WAIT_POLLS; wait; wait--)
		if (REG(DDR + 0x20) & 0x03000000)
			return 0;
	return -1;
}

/* ---- SPI-NOR ---- */

static int spi_reset(void)
{
	u32 wait;

	REG(SPI_RESET) = 1;
	for (wait = HW_WAIT_POLLS; wait; wait--)
		if (!(REG(SPI_RESET) & 1))
			break;
	if (!wait)
		return -1;
	REG(SPI_ISR) = 2;
	REG(SPI_CTRL) &= ~0x13;
	return 0;
}

/* READ (0x03), 3-byte address, no dummy cycles; @len a multiple of 4. */
static int spi_read_chunk(u32 addr, u32 *dst, u32 len)
{
	u32 fifo = (REG(SPI_FEATURE) >> 6) & 0x3fc;	/* RX FIFO bytes */
	u32 wait;

	if (!fifo)
		fifo = 4;

	REG(SPI_ADDR) = addr;
	REG(SPI_CMD1) = 0x01000003;
	REG(SPI_LEN) = len;
	REG(SPI_CMD3) = 0x03000001;

	while (len) {
		u32 n = len < fifo ? len : fifo;

		for (wait = HW_WAIT_POLLS; wait; wait--)
			if (REG(SPI_STATUS) & SPI_STATUS_RX)
				break;
		if (!wait)
			return -1;
		len -= n;
		for (; n; n -= 4)
			*dst++ = REG(SPI_DATA);
	}
	for (wait = HW_WAIT_POLLS; wait; wait--)
		if (REG(SPI_ISR) & SPI_ISR_CMD_DONE)
			break;
	if (!wait)
		return -1;
	REG(SPI_ISR) = SPI_ISR_CMD_DONE;
	return 0;
}

static int spi_read(u32 addr, void *dst, u32 len)
{
	u32 *p = dst;

	len = (len + 3) & ~3;
	while (len) {
		u32 n = len > 0x10000 ? 0x10000 : len;

		if (spi_read_chunk(addr, p, n))
			return -1;
		addr += n;
		p += n / 4;
		len -= n;
	}
	return 0;
}

/* ---- boot paths ---- */

static void boot_zimage(u32 zimage)
{
	con_puts("Starting kernel at ");
	con_puthex(zimage);
	con_puts("\n\n");
	/* r1 = ~0: no machine type; the zImage carries its device tree */
	jump(zimage, 0, ~0u, 0);
}

static int boot_flash(void)
{
	u32 *tag = (u32 *)LOAD_ADDR;
	u32 len;

	if (spi_read(FW_FLASH, tag, TAG_SIZE)) {
		con_puts("SPI read failed\n");
		return -1;
	}
	len = tag[TAG_KERNEL_LEN / 4];
	if (tag[0] != TAG_MAGIC || !len || len > FW_MAX) {
		con_puts("No kernel at flash ");
		con_puthex(FW_FLASH);
		con_puts(" (tag ");
		con_puthex(tag[0]);
		con_puts(", length ");
		con_puthex(len);
		con_puts(")\n");
		return -1;
	}
	con_puts("Loading kernel, ");
	con_puthex(len);
	con_puts(" bytes\n");
	if (spi_read(FW_FLASH, tag, len + TAG_SIZE)) {
		con_puts("Kernel SPI read failed\n");
		return -1;
	}
	boot_zimage(LOAD_ADDR + TAG_SIZE);
	return 0;
}

static void boot_uboot(void)
{
	con_puts("Starting stock U-Boot\n");
	if (spi_read(UBOOT_FLASH, (void *)UBOOT_ENTRY, UBOOT_SIZE)) {
		con_puts("U-Boot SPI read failed\n");
		return;
	}
	jump(UBOOT_ENTRY, 0, 0, 0);
}

/*
 * XMODEM receive (128-byte SOH or 1 KiB STX blocks, CRC-16), straight into
 * @dst; a resent block overwrites itself. Returns the length or -1.
 */
#define SOH	0x01
#define STX	0x02
#define EOT	0x04
#define ACK	0x06
#define NAK	0x15
#define CAN	0x18

static u16 crc16(u16 crc, u8 b)
{
	int i;

	crc ^= b << 8;
	for (i = 0; i < 8; i++)
		crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
	return crc;
}

static int xmodem_receive(u8 *dst, u32 max)
{
	u32 len = 0, tries = 0;
	u8 expect = 1;
	int c;

	for (;;) {
		u32 size, i;
		u16 crc = 0;
		int blk, nblk, hi, lo;

		/* 'C' asks the sender for CRC mode until the first block */
		if (len == 0 && expect == 1)
			con_putc('C');
		c = con_getc_timeout(KEY_WAIT_POLLS * 3);
		if (c < 0) {
			if (++tries > 60)
				return -1;
			continue;
		}
		if (c == EOT) {
			con_putc(ACK);
			return len;
		}
		if (c == CAN)
			return -1;
		if (c != SOH && c != STX)
			continue;

		size = c == STX ? 1024 : 128;
		blk = con_getc();
		nblk = con_getc();
		if (len + size > max) {
			con_putc(CAN);
			con_putc(CAN);
			return -1;
		}
		for (i = 0; i < size; i++) {
			u8 b = con_getc();

			dst[len + i] = b;
			crc = crc16(crc, b);
		}
		hi = con_getc();
		lo = con_getc();

		if ((u8)(blk + nblk) != 0xff || crc != (u16)(hi << 8 | lo)) {
			con_putc(NAK);
			continue;
		}
		if ((u8)blk == (u8)(expect - 1)) {	/* our ACK was lost */
			con_putc(ACK);
			continue;
		}
		if ((u8)blk != expect) {
			con_putc(CAN);
			con_putc(CAN);
			return -1;
		}
		len += size;
		expect++;
		tries = 0;
		con_putc(ACK);
	}
}

static void boot_xmodem(void)
{
	int len;

	con_puts("Send an initramfs zImage (with its DTB) by XMODEM now...\n");
	len = xmodem_receive((u8 *)LOAD_ADDR, XMODEM_MAX);
	if (len <= 0) {
		con_puts("\nTransfer failed\n");
		return;
	}
	con_puts("\nReceived ");
	con_puthex(len);
	con_puts(" bytes\n");
	boot_zimage(LOAD_ADDR);
}

static void menu(void)
{
	for (;;) {
		con_puts("\n1: boot OpenWrt from flash\n"
		     "2: boot a kernel sent by XMODEM (recovery)\n"
		     "3: start the stock U-Boot\n"
		     "> ");
		switch (con_getc()) {
		case '1':
			con_puts("1\n");
			boot_flash();
			break;
		case '2':
			con_puts("2\n");
			boot_xmodem();
			break;
		case '3':
			con_puts("3\n");
			boot_uboot();
			break;
		}
	}
}

int main(void)
{
	uart_init();
	con_puts("\nGC1601 loader " LOADER_VERSION "\n");
	if (ddr_init()) {
		con_puts("DDR init failed; power-cycle to stock loader\n");
		for (;;)
			;
	}
	con_puts("DDR ready\n");
	if (spi_reset()) {
		con_puts("SPI reset failed; UART recovery available\n");
		menu();
	}

	con_puts("Press any key for the boot menu... ");
	if (con_getc_timeout(KEY_WAIT_POLLS) >= 0) {
		con_puts("\n");
		menu();
	}
	con_puts("\n");
	boot_flash();
	/* No kernel in flash: stay reachable. */
	menu();
	return 0;
}
