// SPDX-License-Identifier: GPL-2.0+
/* No UART writes: sample flags while the caller's output finishes. */
typedef unsigned int u32;

int engine_probe(void)
{
	volatile u32 *result = (volatile u32 *)0x00200e00;
	volatile u32 *trace = (volatile u32 *)0x00200e40;
	volatile u32 *flags = (volatile u32 *)0x94405014;
	u32 first = 0, last = 0, any = 0, every = ~0U;
	u32 bit7 = 0, bit8 = 0, changes = 0, value, n;

	result[0] = 0x55534631;
	result[9] = 0;
	for (n = 0; n < 262144; n++) {
		value = *flags;
		if (!n)
			first = value;
		else if (value != last)
			changes++;
		last = value;
		any |= value;
		every &= value;
		bit7 += !!(value & 0x80);
		bit8 += !!(value & 0x100);
		if (!(n & 4095))
			trace[n >> 12] = value;
	}
	result[1] = n;
	result[2] = first;
	result[3] = last;
	result[4] = any;
	result[5] = every;
	result[6] = bit7;
	result[7] = bit8;
	result[8] = changes;
	result[9] = 1;
	asm volatile("dsb sy" : : : "memory");
	return 0;
}
