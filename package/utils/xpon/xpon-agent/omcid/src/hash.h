/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OMCID_HASH_H
#define OMCID_HASH_H
#include <stddef.h>
#include <stdint.h>

/* 32-bit FNV-1a: request fingerprints and service cookies, stable within a boot */
static inline uint32_t omcid_hash(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t hash = 2166136261u;

	while (len--) {
		hash ^= *p++;
		hash *= 16777619u;
	}
	return hash;
}

#endif
