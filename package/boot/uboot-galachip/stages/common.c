// SPDX-License-Identifier: GPL-2.0-only
#include "common.h"

uint32_t stage_crc(const void *data, size_t length)
{
	const uint8_t *p = data;
	uint32_t crc = ~0U;
	unsigned int i;

	while (length--) {
		crc ^= *p++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1));
	}
	return ~crc;
}

int stage_valid(const void *data, size_t length)
{
	const struct stage_footer *f;

	if (length != STAGE_SIZE)
		return 0;
	f = (const void *)((const uint8_t *)data + STAGE_SIZE - sizeof(*f));
	return f->magic == STAGE_MAGIC && f->version == 1 &&
		f->length >= 64 && f->length <= STAGE_SIZE - sizeof(*f) &&
		f->entry == WRAPPER_ENTRY && !f->reserved[0] && !f->reserved[1] &&
		stage_crc(f, sizeof(*f) - 4) == f->header_crc &&
		stage_crc(data, f->length) == f->crc;
}

int lzma_header_valid(const struct lzma_header *h, size_t compressed_size)
{
	return h->magic == LZMA_MAGIC && h->compressed == compressed_size &&
		h->compressed >= 18 && h->compressed <= STAGE_SIZE &&
		h->expanded && h->expanded <= PROPER_MAX &&
		h->destination == PROPER_ENTRY && h->entry == PROPER_ENTRY &&
		stage_crc(h, sizeof(*h) - 4) == h->header_crc;
}

int uart_image_valid(const void *data, size_t received)
{
	const uint32_t *h = data;

	if (received < 16 || received > UART_MAX)
		return 0;
	return h[0] == 0x31523747 && h[1] && h[1] <= PROPER_MAX &&
		h[1] <= received - 16 && received - 16 - h[1] < 1024 &&
		h[2] == 0x41000000 &&
		stage_crc((const uint8_t *)data + 16, h[1]) == h[3];
}
