// SPDX-License-Identifier: GPL-2.0-only
/*
 * Boot commands of GalaChip GC1601 devices: fwload reads the kernel from the
 * tagged firmware partition, zdtb finds a zImage's appended DTB and ubcheck
 * checks a boot region or a u-boot.img before it is written.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */
#include <command.h>
#include <dm.h>
#include <env.h>
#include <fdt_support.h>
#include <image.h>
#include <spi_flash.h>
#include <vsprintf.h>
#include <asm/unaligned.h>
#include <linux/libfdt.h>
#include <u-boot/crc.h>
#include <asm/arch/gc1601.h>

/* The firmware partition starts with a tag of this size, the kernel after it. */
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

/*
 * Stock kernels boot from their appended vendor EVB DTB, whose command line
 * has the wrong flash layout; the stock loader patched it through ATAGs.
 * Write bootargs_stock into that DTB in place: nothing follows it in RAM.
 */
static int stock_bootargs(void *fdt)
{
	const char *args = env_get("bootargs_stock");
	int node;

	if (fdt_node_check_compatible(fdt, 0, "faraday,gc1601"))
		return CMD_RET_SUCCESS;
	if (!args || fdt_open_into(fdt, fdt, fdt_totalsize(fdt) + strlen(args) + 64))
		return CMD_RET_FAILURE;
	node = fdt_find_or_add_subnode(fdt, 0, "chosen");
	if (node < 0 || fdt_setprop_string(fdt, node, "bootargs", args))
		return CMD_RET_FAILURE;
	puts("Stock firmware: using bootargs_stock\n");
	return CMD_RET_SUCCESS;
}

static int do_fwload(struct cmd_tbl *cmd, int flag, int argc, char *const argv[])
{
	struct udevice *flash;
	u8 tag[TAG_SIZE];
	u32 length, fw_offset, fw_size;
	ulong addr;
	int ret;

	if (argc != 2)
		return CMD_RET_USAGE;
	addr = hextoul(argv[1], NULL);
	if (gc1601_fw_partition(&fw_offset, &fw_size) || fw_size <= TAG_SIZE) {
		puts("No firmware partition in the device tree\n");
		return CMD_RET_FAILURE;
	}
	ret = spi_flash_probe_bus_cs(0, 0, &flash);
	if (!ret)
		ret = spi_flash_read_dm(flash, fw_offset, sizeof(tag), tag);
	if (ret)
		return CMD_RET_FAILURE;
	length = get_unaligned_le32(tag + 0x78);
	if (get_unaligned_le32(tag) != 0x03000003 ||
	    length < 0x30 || length > fw_size - TAG_SIZE) {
		puts("No tagged firmware in NOR\n");
		return CMD_RET_FAILURE;
	}
	if (!gc1601_load_range(addr, length)) {
		printf("0x%lx cannot hold %u bytes\n", addr, length);
		return CMD_RET_FAILURE;
	}
	ret = spi_flash_read_dm(flash, fw_offset + TAG_SIZE, length, (void *)addr);
	printf("SF: %u bytes @ %#x Read: ", length, fw_offset + TAG_SIZE);
	if (ret)
		printf("ERROR %d\n", ret);
	else
		puts("OK\n");
	if (ret)
		return CMD_RET_FAILURE;
	/* The tag has no CRC field; do not invent an integrity guarantee for it. */
	env_set_hex("filesize", length);
	if (find_appended_dtb(addr, length))
		return CMD_RET_FAILURE;
	return stock_bootargs((void *)env_get_hex("fdt_addr", 0));
}

U_BOOT_CMD(fwload, 2, 0, do_fwload,
	   "load the kernel of the tagged firmware partition",
	   "<addr>\n    - sets filesize and fdt_addr (appended DTB)");

/*
 * u-boot-with-spl.bin, the boot region: the TPL with its mask ROM header,
 * the SPL as a legacy image at 0x2000 and u-boot.img at 0xa000.
 */
#define REGION_SIZE 0x20000U
#define SPL_OFFSET 0x2000U
#define UBOOT_OFFSET 0xa000U
#define ROM_PAYLOAD_MAX 0x1c00U
/* Load addresses of the flash build, also when checked from the RAM build */
#define SPL_LOAD 0x40100000U
#define UBOOT_LOAD 0x40010000U

static bool legacy_ok(const char *name, const u8 *image, ulong space, ulong load, u8 comp)
{
	const struct legacy_img_hdr *hdr = (const void *)image;

	if (space < sizeof(*hdr) || !image_check_magic(hdr) || !image_check_hcrc(hdr) ||
	    image_get_data_size(hdr) > space - sizeof(*hdr) ||
	    image_get_load(hdr) != load || image_get_ep(hdr) != load ||
	    !image_check_arch(hdr, IH_ARCH_ARM) || !image_check_type(hdr, IH_TYPE_FIRMWARE) ||
	    image_get_comp(hdr) != comp || !image_check_dcrc(hdr)) {
		printf("Not a valid %s\n", name);
		return false;
	}
	printf("%s OK: %u bytes, CRC32 %08x\n", name, image_get_data_size(hdr),
	       image_get_dcrc(hdr));
	return true;
}

static bool uboot_ok(const u8 *image, ulong space)
{
	return legacy_ok("u-boot.img", image, space, UBOOT_LOAD, IH_COMP_LZMA);
}

/* The mask ROM's own checks: magic, size, and the payload's 32-bit word sum. */
static bool rom_ok(const u8 *image)
{
	u32 size = get_unaligned_le32(image + 4);
	u32 sum = 0, i;

	if (get_unaligned_le32(image) != 0x03020100 || !size ||
	    size > ROM_PAYLOAD_MAX || size % 4) {
		puts("Not a valid TPL header\n");
		return false;
	}
	for (i = 0; i < size; i += 4)
		sum += get_unaligned_le32(image + 16 + i);
	if (sum != get_unaligned_le32(image + 12)) {
		puts("TPL checksum mismatch\n");
		return false;
	}
	printf("TPL OK: %u bytes, checksum %08x\n", size, sum);
	return true;
}

static int do_ubcheck(struct cmd_tbl *cmd, int flag, int argc, char *const argv[])
{
	ulong addr, size;
	const u8 *image;

	if (argc != 3)
		return CMD_RET_USAGE;
	addr = hextoul(argv[1], NULL);
	size = hextoul(argv[2], NULL);
	if (!gc1601_load_range(addr, size)) {
		puts("No loaded image at that address and size\n");
		return CMD_RET_FAILURE;
	}
	image = (void *)addr;
	if (size != REGION_SIZE)
		return uboot_ok(image, size) ? CMD_RET_SUCCESS : CMD_RET_FAILURE;

	return rom_ok(image) &&
	       legacy_ok("u-boot-spl.img", image + SPL_OFFSET, UBOOT_OFFSET - SPL_OFFSET,
			 SPL_LOAD, IH_COMP_NONE) &&
	       uboot_ok(image + UBOOT_OFFSET, REGION_SIZE - UBOOT_OFFSET) ?
	       CMD_RET_SUCCESS : CMD_RET_FAILURE;
}

U_BOOT_CMD(ubcheck, 3, 0, do_ubcheck,
	   "check a loaded u-boot-with-spl.bin or u-boot.img",
	   "<addr> <size>\n    - size 0x20000: the whole boot region, else u-boot.img");
