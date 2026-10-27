// SPDX-License-Identifier: GPL-2.0-only
#include "../stages/decode.h"
static unsigned char heap[0x10000];
static int busy;
static void *allocate(ISzAllocPtr a, size_t length)
{
	(void)a;
	if (busy || length > sizeof(heap))
		return NULL;
	busy = 1;
	return heap;
}
static void release(ISzAllocPtr a, void *p)
{
	(void)a;
	(void)p;
	busy = 0;
}
int test_decode(const struct lzma_header *h, const unsigned char *src,
		size_t size, void *out, size_t capacity)
{
	const ISzAlloc allocator = { allocate, release };
	return decode_image(h, src, size, out, capacity, &allocator);
}
