// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Board support common to GalaChip GC1601 devices
 *
 * What differs between boards comes from the device tree, as Linux reads it:
 * the memory node, the flash partitions (the firmware partition by its
 * "galachip,gc1601-fw" compatible) and the Ethernet node's "mac-address"
 * NVMEM cell inside a flash partition.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <dm.h>
#include <env.h>
#include <fdt_support.h>
#include <fdtdec.h>
#include <hang.h>
#include <init.h>
#include <net.h>
#include <spi_flash.h>
#include <wdt.h>
#include <asm/global_data.h>
#include <asm/arch/gc1601.h>
#include <dm/ofnode.h>
#include <linux/sizes.h>

DECLARE_GLOBAL_DATA_PTR;

int board_init(void)
{
	struct udevice *wdt;

	gd->bd->bi_boot_params = CFG_SYS_SDRAM_BASE + 0x100;

	/* An earlier boot stage may have left the watchdog running. */
	if (!uclass_first_device_err(UCLASS_WDT, &wdt))
		wdt_stop(wdt);

	return 0;
}

/* The TPL set up DDR; its size is the memory node's. */
int dram_init(void)
{
	return fdtdec_setup_mem_size_base();
}

int dram_init_banksize(void)
{
	return fdtdec_setup_memory_banksize();
}

/* Loads stay in DDR below the last 8 MiB, which U-Boot relocates into. */
int gc1601_load_range(ulong address, ulong size)
{
	return gc1601_ddr_load_range(gd->ram_base, gd->ram_base + gd->ram_size - SZ_8M,
				     address, size);
}

/* Offset and size of a flash partition node */
static int gc1601_partition(ofnode part, u32 *offset, u32 *size)
{
	if (ofnode_read_u32_index(part, "reg", 0, offset) ||
	    ofnode_read_u32_index(part, "reg", 1, size))
		return -EINVAL;

	return 0;
}

int gc1601_fw_partition(u32 *offset, u32 *size)
{
	ofnode part = ofnode_by_compatible(ofnode_null(), "galachip,gc1601-fw");

	if (!ofnode_valid(part))
		return -ENOENT;

	return gc1601_partition(part, offset, size);
}

/* Flash offset of the Ethernet node's "mac-address" cell */
static int gc1601_mac_offset(u32 *offset)
{
	struct ofnode_phandle_args cell;
	ofnode eth = ofnode_get_aliases_node("ethernet0");
	u32 cell_offset, cell_size, part_offset, part_size;
	int index;

	index = ofnode_stringlist_search(eth, "nvmem-cell-names", "mac-address");
	if (index < 0 ||
	    ofnode_parse_phandle_with_args(eth, "nvmem-cells", NULL, 0, index, &cell) ||
	    ofnode_read_u32_index(cell.node, "reg", 0, &cell_offset) ||
	    ofnode_read_u32_index(cell.node, "reg", 1, &cell_size) || cell_size != ARP_HLEN ||
	    gc1601_partition(ofnode_get_parent(cell.node), &part_offset, &part_size) ||
	    cell_offset > part_size - ARP_HLEN)
		return -ENOENT;

	*offset = part_offset + cell_offset;

	return 0;
}

int misc_init_r(void)
{
	struct udevice *dev;
	struct spi_flash *flash = NULL;
	u32 offset, size;
	u8 mac[ARP_HLEN];

	/* For the upgrade and backup scripts */
	if (!gc1601_fw_partition(&offset, &size)) {
		env_set_hex("fw_offset", offset);
		env_set_hex("fw_size", size);
	}
	if (!spi_flash_probe_bus_cs(0, 0, &dev)) {
		flash = dev_get_uclass_priv(dev);
		env_set_hex("flash_size", flash->size);
	}

	/* A valid caller-supplied volatile address takes precedence. */
	if (eth_env_get_enetaddr("ethaddr", mac))
		return 0;

	if (flash && !gc1601_mac_offset(&offset) &&
	    !spi_flash_read_dm(dev, offset, sizeof(mac), mac) && is_valid_ethaddr(mac))
		eth_env_set_enetaddr("ethaddr", mac);
	else
		puts("Factory MAC unavailable; setenv ethaddr before networking\n");

	return 0;
}

/* Stop the packet engine's DMA before Linux takes over its memory. */
void board_quiesce_devices(void)
{
	if (gc1601_eth_handoff())
		hang();
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
	/* Keep the DMA area reserved for kernels whose DTB lacks it. */
	return fdt_add_mem_rsv(blob, GC1601_ETH_DMA_BASE, GC1601_ETH_DMA_SIZE);
}
