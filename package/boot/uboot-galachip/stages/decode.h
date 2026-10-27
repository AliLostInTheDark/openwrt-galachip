/* SPDX-License-Identifier: GPL-2.0-only */
#include "common.h"
#include "LzmaDec.h"
int decode_image(const struct lzma_header *h, const unsigned char *src,
		 size_t size, void *out, size_t capacity, ISzAllocPtr allocator);
