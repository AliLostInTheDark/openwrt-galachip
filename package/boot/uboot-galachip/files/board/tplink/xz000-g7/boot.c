// SPDX-License-Identifier: GPL-2.0-only
/* Read-only stock-tagged firmware loader. */
#include <command.h>
#include <dm.h>
#include <spi_flash.h>
#include <image.h>
#include <asm/unaligned.h>
#include <linux/libfdt.h>

#define FW_SIZE 0x3b0000U
#define TAG_SIZE 0x200U
#define KERNEL 0x41000000UL

static int do_g7boot(struct cmd_tbl *cmd, int flag, int argc, char *const argv[])
{
	struct udevice *flash;
	u8 tag[TAG_SIZE], *kernel = (void *)KERNEL;
	u32 length, start, end;
	ulong fdt;
	int ret;

	ret = spi_flash_probe_bus_cs(0, 0, &flash);
	if (ret)
		return CMD_RET_FAILURE;
	ret = spi_flash_read_dm(flash, 0x20000, sizeof(tag), tag);
	if (ret)
		return CMD_RET_FAILURE;
	length = get_unaligned_le32(tag + 0x78);
	if (get_unaligned_le32(tag) != 0x03000003 ||
	    length < 0x30 || length > FW_SIZE - TAG_SIZE)
		return CMD_RET_FAILURE;
	ret = spi_flash_read_dm(flash, 0x20200, length, kernel);
	if (ret || get_unaligned_le32(kernel + 0x24) != 0x016f2818)
		return CMD_RET_FAILURE;
	start = get_unaligned_le32(kernel + 0x28);
	end = get_unaligned_le32(kernel + 0x2c);
	if (end <= start || end - start > length)
		return CMD_RET_FAILURE;
	if (argc == 2) {
		if (strict_strtoul(argv[1], 16, &fdt) ||
		    fdt < 0x40912000 || fdt > 0x41800000 - sizeof(struct fdt_header))
			return CMD_RET_FAILURE;
		if (fdt_check_header((void *)fdt) ||
		    fdt_totalsize((void *)fdt) > 0x41800000 - fdt)
			return CMD_RET_FAILURE;
	} else {
		fdt = KERNEL + ALIGN(end - start, 4);
		if (fdt - KERNEL > length ||
		    length - (fdt - KERNEL) < sizeof(struct fdt_header) ||
		    fdt_check_header((void *)fdt) ||
		    fdt_totalsize((void *)fdt) > length - (fdt - KERNEL)) {
			puts("No appended DTB; use g7boot <external-dtb-address>\n");
			return CMD_RET_FAILURE;
		}
	}
	/* The tag has no CRC field; do not invent an integrity guarantee for it. */
	return run_commandf("bootz %lx - %lx", KERNEL, fdt);
}
U_BOOT_CMD(g7boot, 2, 0, do_g7boot, "boot stock-tagged OpenWrt from NOR",
	   "[external-dtb-address]");
