// SPDX-License-Identifier: GPL-2.0-only
/*
 * Firmware parser for the GalaChip GC1601 stock loader image tag.
 *
 * The loader reads a 512-byte tag from the start of the firmware area,
 * takes the kernel length from offset 0x78 and boots the zImage that
 * follows the tag. The root filesystem starts on the next erase block.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/of.h>
#include <linux/slab.h>

#include "mtdsplit.h"

#define GC1601_TAG_SIZE		0x200
#define GC1601_TAG_MAGIC	0x03000003
#define GC1601_TAG_KERNEL_LEN	0x78
#define GC1601_NR_PARTS		2

static int mtdsplit_parse_gc1601(struct mtd_info *master,
				 const struct mtd_partition **pparts,
				 struct mtd_part_parser_data *data)
{
	__le32 tag[GC1601_TAG_KERNEL_LEN / 4 + 1];
	enum mtdsplit_part_type type;
	struct mtd_partition *parts;
	size_t retlen, kernel_end, rootfs_offset, squashfs_len;
	int err;

	/*
	 * Any error from a subpartition parser makes the MTD core drop every
	 * partition of the flash, misc and hwconfig included. Leave the
	 * firmware partition unsplit instead.
	 */
	err = mtd_read(master, 0, sizeof(tag), &retlen, (void *)tag);
	if (err || retlen != sizeof(tag) ||
	    le32_to_cpu(tag[0]) != GC1601_TAG_MAGIC) {
		pr_warn("%s: no GC1601 image tag, not splitting\n", master->name);
		return 0;
	}

	kernel_end = GC1601_TAG_SIZE +
		     le32_to_cpu(tag[GC1601_TAG_KERNEL_LEN / 4]);
	if (kernel_end >= master->size)
		return 0;

	err = mtd_find_rootfs_from(master, mtd_roundup_to_eb(kernel_end, master),
				   master->size, &rootfs_offset, &type);
	if (err) {
		pr_warn("%s: no rootfs after the kernel, not splitting\n",
			master->name);
		return 0;
	}

	/*
	 * A squashfs that fills the partition leaves squashfs-split a
	 * zero-sized rootfs_data, which the MTD core extends to the end of
	 * the flash, over the factory data partitions. Refuse that layout.
	 */
	if (type == MTDSPLIT_PART_TYPE_SQUASHFS) {
		err = mtd_get_squashfs_len(master, rootfs_offset, &squashfs_len);
		if (err ||
		    mtd_roundup_to_eb(rootfs_offset + squashfs_len, master) >=
		    master->size) {
			pr_warn("%s: no room for rootfs_data, not splitting\n",
				master->name);
			return 0;
		}
	}

	parts = kcalloc(GC1601_NR_PARTS, sizeof(*parts), GFP_KERNEL);
	if (!parts)
		return -ENOMEM;

	parts[0].name = KERNEL_PART_NAME;
	parts[0].offset = 0;
	parts[0].size = rootfs_offset;

	parts[1].name = ROOTFS_PART_NAME;
	parts[1].offset = rootfs_offset;
	parts[1].size = master->size - rootfs_offset;

	*pparts = parts;
	return GC1601_NR_PARTS;
}

static const struct of_device_id mtdsplit_gc1601_of_match_table[] = {
	{ .compatible = "galachip,gc1601-fw" },
	{},
};
MODULE_DEVICE_TABLE(of, mtdsplit_gc1601_of_match_table);

static struct mtd_part_parser mtdsplit_gc1601_parser = {
	.owner = THIS_MODULE,
	.name = "gc1601-fw",
	.of_match_table = mtdsplit_gc1601_of_match_table,
	.parse_fn = mtdsplit_parse_gc1601,
	.type = MTD_PARSER_TYPE_FIRMWARE,
};

module_mtd_part_parser(mtdsplit_gc1601_parser);
