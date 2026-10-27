// SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
#include "common.h"

int __aeabi_idiv0(int value)
{
	(void)value;
	fail("Division by zero in decoder");
}

void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}
void *memset(void *dst, int value, size_t n)
{
	unsigned char *d = dst;
	while (n--)
		*d++ = value;
	return dst;
}
void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	if (d <= s)
		return memcpy(dst, src, n);
	while (n) {
		n--;
		d[n] = s[n];
	}
	return dst;
}
