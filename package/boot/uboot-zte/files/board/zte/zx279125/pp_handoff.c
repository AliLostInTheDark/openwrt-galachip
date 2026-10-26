// SPDX-License-Identifier: GPL-2.0+
#include <fdt_support.h>
#include <linux/errno.h>
#include <zx279125-pp.h>

/* Called for an external Linux FDT, after network cleanup. */
int ft_board_setup(void *blob, struct bd_info *bd)
{
	const fdt32_t *prop;
	u64 start = 0x40000000, size = 0x02000000;
	u32 cursor;
	int eth, pool, len, ret;

	eth = fdt_node_offset_by_compatible(blob, -1, "zte,zx279125-ethernet");
	if (eth < 0)
		return eth;
	prop = fdt_getprop(blob, eth, "memory-region", &len);
	if (!prop || len < sizeof(*prop))
		return -EINVAL;
	pool = fdt_node_offset_by_phandle(blob, fdt32_to_cpu(*prop));
	if (pool < 0)
		return pool;
	if (fdt_parent_offset(blob, pool) != fdt_path_offset(blob, "/reserved-memory"))
		return -EINVAL;
	prop = fdt_getprop(blob, pool, "reg", &len);
	if (!prop || len != 2 * sizeof(*prop) ||
	    fdt32_to_cpu(prop[0]) != ZX279125_PP_RESERVED_BASE ||
	    fdt32_to_cpu(prop[1]) != ZX279125_PP_RESERVED_SIZE ||
	    !fdt_getprop(blob, pool, "no-map", NULL))
		return -EINVAL;

	ret = zx279125_pp_handoff(&cursor);
	if (ret && ret != -ENOENT)
		return ret;
	if (!ret) {
		ret = fdt_setprop_u32(blob, eth, "zte,bootloader-cursors", cursor);
		if (ret)
			return ret;
	}
	/* U-Boot manages 16 MiB; Linux sees physical RAM with its DMA guard. */
	return fdt_fixup_memory_banks(blob, &start, &size, 1);
}
