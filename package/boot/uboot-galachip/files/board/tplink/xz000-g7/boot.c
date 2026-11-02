// SPDX-License-Identifier: GPL-2.0-only
/* Read-only loader for the stock-tagged firmware in NOR. */
#include <command.h>
#include <dm.h>
#include <env.h>
#include <spi_flash.h>
#include <vsprintf.h>
#include <asm/unaligned.h>
#include <linux/libfdt.h>
#include <u-boot/crc.h>
#include <gc1601_bounds.h>

#define FW_OFFSET 0x20000U
#define FW_SIZE 0x3b0000U
#define TAG_SIZE 0x200U

/* Set fdt_addr to the DTB appended to a zImage of at most @length bytes. */
static int find_appended_dtb(ulong addr, u32 length)
{
	u8 *kernel = (void *)addr;
	u32 start, end;
	ulong fdt;

	env_set("fdt_addr", NULL);
	if (length < 0x30 || get_unaligned_le32(kernel + 0x24) != 0x016f2818) {
		puts("No zImage\n");
		return CMD_RET_FAILURE;
	}
	start = get_unaligned_le32(kernel + 0x28);
	end = get_unaligned_le32(kernel + 0x2c);
	fdt = addr + ALIGN(end - start, 4);
	if (end <= start || fdt - addr > length ||
	    length - (fdt - addr) < sizeof(struct fdt_header) ||
	    fdt_check_header((void *)fdt) ||
	    fdt_totalsize((void *)fdt) > length - (fdt - addr)) {
		puts("No appended DTB\n");
		return CMD_RET_FAILURE;
	}
	env_set_hex("fdt_addr", fdt);
	return CMD_RET_SUCCESS;
}

static int do_zdtb(struct cmd_tbl *cmd, int flag, int argc, char *const argv[])
{
	ulong addr, size;

	if (argc != 2)
		return CMD_RET_USAGE;
	addr = hextoul(argv[1], NULL);
	size = env_get_hex("filesize", 0);
	if (!gc1601_load_range(addr, size)) {
		puts("No loaded image at that address (filesize)\n");
		return CMD_RET_FAILURE;
	}
	return find_appended_dtb(addr, size);
}
U_BOOT_CMD(zdtb, 2, 0, do_zdtb,
	   "set fdt_addr to the DTB appended to a loaded zImage",
	   "<addr>\n    - uses filesize as the loaded image length");

static int do_fwload(struct cmd_tbl *cmd, int flag, int argc, char *const argv[])
{
	struct udevice *flash;
	u8 tag[TAG_SIZE];
	ulong addr;
	u32 length;
	int ret;

	if (argc != 2)
		return CMD_RET_USAGE;
	addr = hextoul(argv[1], NULL);
	ret = spi_flash_probe_bus_cs(0, 0, &flash);
	if (!ret)
		ret = spi_flash_read_dm(flash, FW_OFFSET, sizeof(tag), tag);
	if (ret)
		return CMD_RET_FAILURE;
	length = get_unaligned_le32(tag + 0x78);
	if (get_unaligned_le32(tag) != 0x03000003 ||
	    length < 0x30 || length > FW_SIZE - TAG_SIZE) {
		puts("No tagged firmware in NOR\n");
		return CMD_RET_FAILURE;
	}
	if (!gc1601_load_range(addr, length)) {
		printf("0x%lx cannot hold %u bytes\n", addr, length);
		return CMD_RET_FAILURE;
	}
	printf("NOR: reading %u bytes @ 0x%x to 0x%lx ... ", length,
	       FW_OFFSET + TAG_SIZE, addr);
	ret = spi_flash_read_dm(flash, FW_OFFSET + TAG_SIZE, length, (void *)addr);
	puts(ret ? "failed\n" : "OK\n");
	if (ret)
		return CMD_RET_FAILURE;
	/* The tag has no CRC field; do not invent an integrity guarantee for it. */
	env_set_hex("filesize", length);
	return find_appended_dtb(addr, length);
}
U_BOOT_CMD(fwload, 2, 0, do_fwload,
	   "load the tagged OpenWrt kernel from NOR",
	   "<addr>\n    - sets filesize and fdt_addr (appended DTB)");

/* proper.img: wrapper, padding, then a 32-byte footer of CRC32-checked words. */
#define PROPER_SIZE 0x1e000U

static int do_ubcheck(struct cmd_tbl *cmd, int flag, int argc, char *const argv[])
{
	ulong addr, size;
	const u8 *image;
	const u32 *footer;
	u32 length;

	if (argc != 3)
		return CMD_RET_USAGE;
	addr = hextoul(argv[1], NULL);
	size = hextoul(argv[2], NULL);
	if (size != PROPER_SIZE || !gc1601_load_range(addr, size)) {
		printf("proper.img must be 0x%x bytes\n", PROPER_SIZE);
		return CMD_RET_FAILURE;
	}
	image = (void *)addr;
	footer = (void *)(image + PROPER_SIZE - 32);
	length = get_unaligned_le32(&footer[2]);
	if (crc32(0, (void *)footer, 28) != get_unaligned_le32(&footer[7]) ||
	    get_unaligned_le32(&footer[0]) != 0x31503747 ||
	    get_unaligned_le32(&footer[1]) != 1 ||
	    get_unaligned_le32(&footer[3]) != 0x40100000 ||
	    length < 64 || length > PROPER_SIZE - 32 ||
	    crc32(0, image, length) != get_unaligned_le32(&footer[4])) {
		puts("Not a valid XZ000-G7 proper.img\n");
		return CMD_RET_FAILURE;
	}
	printf("proper.img OK: wrapper %u bytes, CRC32 %08x\n", length,
	       get_unaligned_le32(&footer[4]));
	return CMD_RET_SUCCESS;
}
U_BOOT_CMD(ubcheck, 3, 0, do_ubcheck,
	   "check a loaded XZ000-G7 proper.img",
	   "<addr> <size>");
