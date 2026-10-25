/* SPDX-License-Identifier: GPL-2.0-or-later */
/* One crash-record format for every ONU: append-only, page-aligned, little-endian. Pure. */
#ifndef ONU_CRASHLOG_CORE_H
#define ONU_CRASHLOG_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#define OCL_MAGIC	"ONUCRSH1"
#define OCL_MAGIC_LEN	8
#define OCL_HDR		32
#define OCL_PAGE	256
#define OCL_MAX_TEXT	(15 * OCL_PAGE - OCL_HDR)

enum ocl_kind {
	OCL_BOOT = 1,
	OCL_CLEAN = 2,
	OCL_PANIC = 3,
	OCL_PRETIMEOUT = 4,
	OCL_UNCLEAN = 5,
};

struct ocl_rec {
	u32 seq;
	u32 boot_seq;
	u32 uptime_s;
	u8 kind;
	u16 len;
	const u8 *text;
	size_t bytes;
};

enum ocl_page { OCL_ERASED = 0, OCL_RECORD = 1, OCL_FOREIGN = -1 };

u32 ocl_crc32(u32 crc, const u8 *p, size_t n);
size_t ocl_bytes(size_t text_len);
size_t ocl_encode(u8 *out, size_t cap, const struct ocl_rec *r);
int ocl_parse(const u8 *p, size_t avail, struct ocl_rec *r);
const char *ocl_kind_name(u8 kind);
int ocl_behind_our_kernel(const u8 *uimage_hdr, const char *release, u64 kernel_ofs, u64 region_ofs);

#endif
