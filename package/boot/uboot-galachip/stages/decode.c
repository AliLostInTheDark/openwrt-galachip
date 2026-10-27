// SPDX-License-Identifier: GPL-2.0-only
#include "decode.h"

int decode_image(const struct lzma_header *h, const unsigned char *src,
		 size_t size, void *out, size_t capacity, ISzAllocPtr allocator)
{
	ELzmaStatus status;
	SizeT input, output;
	uint32_t dictionary, declared, high;
	int ret;

	if (!lzma_header_valid(h, size) || h->expanded > capacity ||
	    stage_crc(src, size) != h->compressed_crc)
		return -1;
	dictionary = src[1] | src[2] << 8 | src[3] << 16 | (uint32_t)src[4] << 24;
	declared = src[5] | src[6] << 8 | src[7] << 16 | (uint32_t)src[8] << 24;
	high = src[9] | src[10] << 8 | src[11] << 16 | (uint32_t)src[12] << 24;
	/* Fixed lc=3/lp=0/pb=2 bounds the probability allocation to 15980 bytes. */
	if (src[0] != 0x5d || dictionary > 0x100000 ||
	    high || declared != h->expanded)
		return -1;
	input = size - 13;
	output = h->expanded;
	ret = LzmaDecode(out, &output, src + 13, &input, src, 5,
			 LZMA_FINISH_END, &status, allocator);
	if (ret != SZ_OK || status != LZMA_STATUS_FINISHED_WITH_MARK ||
	    input != size - 13 || output != h->expanded ||
	    stage_crc(out, output) != h->expanded_crc)
		return -1;
	return 0;
}
