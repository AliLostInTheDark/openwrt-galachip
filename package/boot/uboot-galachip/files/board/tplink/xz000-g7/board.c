// SPDX-License-Identifier: GPL-2.0-or-later
#include <dm.h>
#include <env.h>
#include <fdt_support.h>
#include <net.h>
#include <spi_flash.h>
#include <asm/global_data.h>
#include <asm/io.h>

DECLARE_GLOBAL_DATA_PTR;

int board_init(void)
{
	gd->bd->bi_boot_params = 0x40000100;
	writel(0, (void *)0x9080000c);
	return 0;
}

int dram_init(void)
{
	/* Deliberately no DDR programming or destructive size probing here. */
	gd->ram_size = 0x02000000;
	return 0;
}

int dram_init_banksize(void)
{
	gd->dram[0].start = 0x40000000;
	gd->dram[0].size = 0x02000000;
	return 0;
}

int misc_init_r(void)
{
	struct udevice *flash;
	u8 mac[6];
	int ret;

	/* A valid caller-supplied volatile address takes precedence. */
	if (eth_env_get_enetaddr("ethaddr", mac))
		return 0;
	ret = spi_flash_probe_bus_cs(0, 0, &flash);
	if (ret)
		return 0;
	ret = spi_flash_read_dm(flash, 0x3ef100, sizeof(mac), mac);
	if (!ret && is_valid_ethaddr(mac))
		eth_env_set_enetaddr("ethaddr", mac);
	else
		puts("Factory MAC unavailable; setenv ethaddr before networking\n");
	return 0;
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
	/* Retain the kernel's established fixed DMA reservation. */
	return fdt_add_mem_rsv(blob, 0x40800000, 0x00112000);
}
