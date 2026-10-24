// SPDX-License-Identifier: GPL-2.0+
/* Reversible control readback experiment; no DDR, reset or UART operations. */
typedef unsigned int u32;

static u32 read_reg(unsigned long address)
{
	return *(volatile u32 *)address;
}

static void write_reg(unsigned long address, u32 value)
{
	*(volatile u32 *)address = value;
	asm volatile("dsb sy" : : : "memory");
}

static int ownership_idle(void)
{
	unsigned int q;

	if ((read_reg(0x921c0000) | read_reg(0x92200000)) & 12)
		return 0;
	if (read_reg(0x92154050) & 1 || read_reg(0x92154064) & 1 ||
	    read_reg(0x92168010) || read_reg(0x921680c0) & 8 ||
	    read_reg(0x92168088) != read_reg(0x9216808c) ||
	    read_reg(0x92168080) + read_reg(0x92168084) != 4096)
		return 0;
	for (q = 0; q < 16; q++)
		if (read_reg(0x92164100 + q * 4))
			return 0;
	return 1;
}

#ifdef ZX_ENGINE_RESET
/* Reset invalidates engine tables: never restore the previous enable state. */
static int reset_probe(void)
{
	unsigned int n;
	u32 status;
	int ret = -110;

	write_reg(0x00200e00, 0x50525331);
	write_reg(0x00200e20, read_reg(0x92144048));
	write_reg(0x00200e24, read_reg(0x9214404c));
	write_reg(0x00200e28, read_reg(0x92144018));
	write_reg(0x00200e2c, read_reg(0x9214401c));
	write_reg(0x00200e30, read_reg(0x92144020));
	/* Exact stock initialization request; this is not an AXI-idle assertion. */
	write_reg(0x92144048, 0xfffff);
	for (n = 0; n < 1000000; n++) {
		status = read_reg(0x9214404c);
		if ((status & 0xfffff) == 0xfffff) {
			ret = 0;
			break;
		}
	}
	write_reg(0x00200e34, read_reg(0x92144048));
	write_reg(0x00200e38, status);
	write_reg(0x00200e3c, n);
	write_reg(0x00200e40, read_reg(0x92144018));
	write_reg(0x00200e44, read_reg(0x9214401c));
	write_reg(0x00200e48, read_reg(0x92144020));
	/* Keep ingress off even if the reset changed an enable register. */
	write_reg(0x92168000, 0);
	write_reg(0x92164000, read_reg(0x92164000) & ~0xf0000);
	write_reg(0x00200e18, read_reg(0x92164000));
	write_reg(0x00200e1c, read_reg(0x92168000));
	if (read_reg(0x92168000) || read_reg(0x92164000) & 0xf0000)
		ret = -5;
	write_reg(0x00200e04, ret);
	return ret;
}
#endif

int engine_probe(void)
{
	u32 dma = read_reg(0x92164000), bmu = read_reg(0x92168000);
	unsigned int n;
	int ret = -16;

	/* Result: magic, rc, saved DMA/BMU, disabled DMA/BMU, restored DMA/BMU. */
	write_reg(0x00200e00, 0x454e4731);
	write_reg(0x00200e04, -16);
	write_reg(0x00200e08, dma);
	write_reg(0x00200e0c, bmu);
	for (n = 4; n < 8; n++)
		write_reg(0x00200e00 + n * 4, 0);
	if (bmu != 1 || (dma & 0xf0000) != 0xf0000)
		return ret;
	for (n = 0; n < 32; n++)
		if (!ownership_idle())
			return ret;

	/* Exact stock setters, with no guessed reset mask or clock changes. */
	write_reg(0x92164000, dma & ~0xf0000);
	write_reg(0x92168000, 0);
	write_reg(0x00200e10, read_reg(0x92164000));
	write_reg(0x00200e14, read_reg(0x92168000));
	ret = -5;
	if (read_reg(0x92164000) != (dma & ~0xf0000) ||
	    read_reg(0x92168000) != 0)
		goto restore;
	for (n = 0; n < 1024; n++)
		if (!ownership_idle())
			goto restore;
#ifdef ZX_ENGINE_RESET
	return reset_probe();
#endif
	ret = 0;
restore:
	write_reg(0x92168000, bmu);
	write_reg(0x92164000, dma);
	write_reg(0x00200e18, read_reg(0x92164000));
	write_reg(0x00200e1c, read_reg(0x92168000));
	if (read_reg(0x92164000) != dma || read_reg(0x92168000) != bmu)
		ret = -5;
	write_reg(0x00200e04, ret);
	return ret;
}
