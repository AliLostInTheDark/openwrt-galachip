// SPDX-License-Identifier: GPL-2.0-only
/*
 * In-kernel factory calibration parser for ZTE Sanechips ZX279125
 * Reads and decodes active TAGH0201 records from the 'parameter tags' MTD partition.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/mtd/mtd.h>
#include <linux/module.h>
#include <linux/soc/zte/zx279125-tagparam.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <net/xpon.h>
#include <net/xpon/omci.h>

#include "zx279125-pon.h"

#define TAGPARAM_MAGIC		"TAGH0201"
#define TAGPARAM_MAGIC_LEN	8
#define TAGPARAM_START_OFFSET	0x100
#define TAGPARAM_ENTRY_OFFSET	0x10c
#define TAGPARAM_MAX_VALUE_LEN	0x400
#define TAGPARAM_REGION_SIZE	0x10000

/* Tag identifiers in V2801SG parameter tags partition */
#define TAG_BASE_MAC		0x0100
#define TAG_LAN1_MAC		0x0101
#define TAG_VENDORID		0x0880
#define TAG_GPONSN		0x0881
#define TAG_GPONPWD		0x0882
#define TAG_PONLOID		0x0884
#define TAG_PONPASSWD		0x0885
#define TAG_EPONSN		0x0886
#define TAG_MODELID		0x0887
#define TAG_CUSPONMODE		0x088e
#define TAG_PON_MODE		0x088f
#define TAG_EQUIPMENTID		0x0892
#define TAG_HARDWARE_VER	0x089f
#define TAG_PONMAC		0x8001

struct tagparam_entry_hdr {
	__le16 tag;
	__le16 flag;
	__le16 csum;
	__le16 len;
} __packed;

/* TagReadParse in the vendor module sums tag, length and payload, not flags. */
static int tagparam_validate(const u8 *buf, size_t size)
{
	size_t off = TAGPARAM_ENTRY_OFFSET;

	if (size < off || memcmp(buf + TAGPARAM_START_OFFSET,
				TAGPARAM_MAGIC, TAGPARAM_MAGIC_LEN))
		return -EINVAL;

	while (off < size) {
		const struct tagparam_entry_hdr *hdr;
		const u8 *val;
		size_t step;
		u16 tag, len, sum;
		unsigned int i;

		if (size - off < sizeof(*hdr))
			return -EBADMSG;
		hdr = (const void *)(buf + off);
		tag = le16_to_cpu(hdr->tag);
		if (tag == 0xffff)
			return 0;
		len = le16_to_cpu(hdr->len);
		step = sizeof(*hdr) + ALIGN((size_t)len, 4);
		if (len > TAGPARAM_MAX_VALUE_LEN || step > size - off)
			return -EBADMSG;
		if (le16_to_cpu(hdr->flag) != 0xffff &&
		    le16_to_cpu(hdr->flag) != 0xfffe)
			return -EBADMSG;
		val = buf + off + sizeof(*hdr);
		sum = tag + len + le16_to_cpu(hdr->csum);
		for (i = 0; i < len; i++)
			sum += val[i];
		if (sum)
			return -EBADMSG;
		off += step;
	}

	return 0;
}

static u8 *tagparam_read(void)
{
	struct mtd_info *mtd;
	u8 *buf;
	size_t retlen;
	int ret;

	/* 1. Locate the 'tagparam' MTD partition */
	mtd = get_mtd_device_nm("tagparam");
	if (IS_ERR(mtd))
		mtd = get_mtd_device_nm("parameter tags");
	if (IS_ERR(mtd))
		return ERR_PTR(PTR_ERR(mtd) == -ENODEV ? -EPROBE_DEFER : PTR_ERR(mtd));
	if (mtd->size != TAGPARAM_REGION_SIZE) {
		put_mtd_device(mtd);
		return ERR_PTR(-EINVAL);
	}

	buf = vmalloc(mtd->size);
	if (!buf) {
		put_mtd_device(mtd);
		return ERR_PTR(-ENOMEM);
	}

	ret = mtd_read(mtd, 0, mtd->size, &retlen, buf);
	put_mtd_device(mtd);

	if (ret && ret != -EUCLEAN) {
		vfree(buf);
		return ERR_PTR(ret);
	}

	/* Validate every record before publishing any factory fields. */
	ret = retlen == TAGPARAM_REGION_SIZE ? tagparam_validate(buf, retlen) : -EIO;
	if (ret) {
		vfree(buf);
		return ERR_PTR(ret);
	}
	return buf;
}

int zx279125_tagparam_lan_mac(u8 *addr)
{
	u8 *buf = tagparam_read();
	size_t off;
	int selected = 0, ret = -ENOENT;

	if (IS_ERR(buf))
		return PTR_ERR(buf);
	for (off = TAGPARAM_ENTRY_OFFSET; off + 8 <= TAGPARAM_REGION_SIZE;) {
		const struct tagparam_entry_hdr *hdr = (const void *)(buf + off);
		u16 tag = le16_to_cpu(hdr->tag), len = le16_to_cpu(hdr->len);
		const u8 *val = buf + off + sizeof(*hdr);
		int priority = tag == TAG_LAN1_MAC ? 2 : tag == TAG_BASE_MAC ? 1 : 0;

		if (tag == 0xffff)
			break;
		if (priority >= selected && priority && le16_to_cpu(hdr->flag) == 0xffff) {
			selected = priority;
			ret = len == ETH_ALEN && is_valid_ether_addr(val) ? 0 : -EINVAL;
			if (!ret)
				ether_addr_copy(addr, val);
		}
		off += sizeof(*hdr) + ALIGN((size_t)len, 4);
	}
	vfree(buf);
	return ret;
}
EXPORT_SYMBOL_GPL(zx279125_tagparam_lan_mac);

int zx279125_tagparam_load(struct zx279125_pon *pon, struct omci_identity *id)
{
	u8 *buf;
	size_t off, retlen = TAGPARAM_REGION_SIZE;
	bool has_vendor = false, has_gpon_sn = false;
	u8 vendor_tmp[4] = {}, gpon_sn_tmp[4] = {};

	if (!pon || !id)
		return -EINVAL;
	memset(id, 0, sizeof(*id));
	buf = tagparam_read();
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	/* The factory auto setting has no concrete mode in the xPON API. */
	pon->mode_pref = __XPON_MODE_MAX;
	pon->has_pon_mac = false;
	pon->has_base_mac = false;
	pon->has_epon_sn = false;

	/* 3. Scan append-only tag records */
	off = TAGPARAM_ENTRY_OFFSET;
	while (off + sizeof(struct tagparam_entry_hdr) <= retlen) {
		const struct tagparam_entry_hdr *hdr = (const void *)(buf + off);
		u16 tag = le16_to_cpu(hdr->tag);
		u16 flag = le16_to_cpu(hdr->flag);
		u16 len = le16_to_cpu(hdr->len);
		size_t aligned_len = (len + 3) & ~3;
		const u8 *val;

		if (tag == 0xffff)
			break;

		if (off + sizeof(*hdr) + aligned_len > retlen)
			break;

		val = buf + off + sizeof(*hdr);

		/* Process only active entries (0xffff) */
		if (flag == 0xffff) {
			switch (tag) {
			case TAG_VENDORID:
				if (len == 8) {
					/* 8-character hex string (e.g. '4E4B4F54' -> 'NKOT') */
					if (hex2bin(vendor_tmp, val, 4) == 0) {
						memcpy(id->vendor_id, vendor_tmp, 4);
						id->valid |= OMCI_IDENTITY_F_VENDOR_ID;
						id->vendor_source = OMCI_CONFIG_SOURCE_DRIVER;
						has_vendor = true;
					}
				} else if (len == 4) {
					memcpy(vendor_tmp, val, 4);
					memcpy(id->vendor_id, val, 4);
					id->valid |= OMCI_IDENTITY_F_VENDOR_ID;
					id->vendor_source = OMCI_CONFIG_SOURCE_DRIVER;
					has_vendor = true;
				}
				break;

			case TAG_GPONSN:
				if (len == 8 && hex2bin(gpon_sn_tmp, val, 4) == 0)
					has_gpon_sn = true;
				break;

			case TAG_GPONPWD:
				if (len > 0) {
					size_t copy_len = min_t(size_t, len, sizeof(id->password));

					memcpy(id->password, val, copy_len);
					id->valid |= OMCI_IDENTITY_F_PASSWORD;
				}
				break;

			case TAG_PONLOID:
				if (len > 0) {
					size_t copy_len = min_t(size_t, len, sizeof(id->loid) - 1);

					memcpy(id->loid, val, copy_len);
					id->valid |= OMCI_IDENTITY_F_LOID;
				}
				break;

			case TAG_PONPASSWD:
				if (len > 0) {
					size_t copy_len = min_t(size_t, len, sizeof(id->loid_password) - 1);

					memcpy(id->loid_password, val, copy_len);
					id->valid |= OMCI_IDENTITY_F_LOID_PASSWORD;
				}
				break;

			case TAG_EPONSN:
				if (len > 0) {
					size_t copy_len = min_t(size_t, len, sizeof(pon->epon_sn) - 1);

					memcpy(pon->epon_sn, val, copy_len);
					pon->has_epon_sn = true;
				}
				break;

			case TAG_EQUIPMENTID:
				if (len > 0) {
					size_t copy_len = min_t(size_t, len, sizeof(id->equipment_id) - 1);

					memcpy(id->equipment_id, val, copy_len);
					id->valid |= OMCI_IDENTITY_F_EQUIPMENT_ID;
				}
				break;

			case TAG_HARDWARE_VER:
				if (len > 0) {
					size_t copy_len = min_t(size_t, len, sizeof(id->version) - 1);

					memcpy(id->version, val, copy_len);
					id->valid |= OMCI_IDENTITY_F_VERSION;
				}
				break;

			case TAG_PONMAC:
				if (len == ETH_ALEN && is_valid_ether_addr(val)) {
					ether_addr_copy(pon->pon_mac, val);
					pon->has_pon_mac = true;
				}
				break;

			case TAG_BASE_MAC:
				if (len == ETH_ALEN && is_valid_ether_addr(val)) {
					ether_addr_copy(pon->base_mac, val);
					pon->has_base_mac = true;
				}
				break;

			case TAG_CUSPONMODE:
				if (len == 1) {
					if (val[0] == '2' || val[0] == 2)
						pon->mode_pref = XPON_MODE_EPON;
					else if (val[0] == '1' || val[0] == 1)
						pon->mode_pref = XPON_MODE_GPON;
					else
						/* Auto/unknown is not a concrete xPON mode. */
						pon->mode_pref = __XPON_MODE_MAX;
				}
				break;
			}
		}

		off += sizeof(*hdr) + aligned_len;
	}

	vfree(buf);

	if (has_vendor && has_gpon_sn) {
		memcpy(id->serial_number, vendor_tmp, 4);
		memcpy(id->serial_number + 4, gpon_sn_tmp, 4);
		id->valid |= OMCI_IDENTITY_F_SERIAL_NUMBER;
		id->serial_source = OMCI_CONFIG_SOURCE_DRIVER;
	}

	dev_info(pon->dev, "tagparam: validated factory parameters\n");

	return 0;
}
EXPORT_SYMBOL_GPL(zx279125_tagparam_load);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ZX279125 checked factory parameter reader");
