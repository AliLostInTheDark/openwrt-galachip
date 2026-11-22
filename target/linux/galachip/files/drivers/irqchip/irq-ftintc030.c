// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Faraday FTINTC030 interrupt controller, as found in the GalaChip GC1601
 *
 * The controller is laid out like a GIC: a distributor part with enable,
 * clear, trigger, priority and target registers for the shared peripheral
 * interrupts (SPIs), and a CPU interface with priority mask, binary point,
 * acknowledge and end-of-interrupt registers. Vectors 0-31 are reserved for
 * software and private interrupts, so SPI n is acknowledged as vector n + 32.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * The register layout and the setup values come from Faraday Technology's
 * driver (Copyright (C) 2014 Faraday Technology, Yan-Pai Chen).
 */
#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/spinlock.h>

#include <asm/exception.h>

/* Distributor, one bit per SPI: a 0x20-byte block of these per 32 SPIs */
#define FTINTC030_SPI_ENABLE		0x004
#define FTINTC030_SPI_CLEAR		0x008
#define FTINTC030_SPI_EDGE		0x00c
#define FTINTC030_SPI_ACTIVE_LOW	0x010
/* One byte per SPI */
#define FTINTC030_SPI_PRIORITY		0x240
#define FTINTC030_SPI_TARGET		0x420

/* CPU interface */
#define FTINTC030_PRIORITY_MASK		0x684
#define FTINTC030_BINARY_POINT		0x688
#define FTINTC030_ACK			0x68c
#define FTINTC030_EOI			0x690
#define FTINTC030_CPU_CTRL		0x69c
#define FTINTC030_CPU_MATCH		0x6a0

#define FTINTC030_FEATURE		0x700
#define FTINTC030_FEATURE_SPIS		GENMASK(10, 2)

#define FTINTC030_VECTOR		GENMASK(8, 0)
#define FTINTC030_VECTOR_SPURIOUS	511
#define FTINTC030_VECTOR_SPI_BASE	32

/*
 * CPU interface setup as Faraday's driver programs it: every priority is let
 * through, priorities never preempt each other, and bit 2 of the control
 * register is set (its meaning is not documented).
 */
#define FTINTC030_PRIORITY_MASK_ALL	0xf
#define FTINTC030_BINARY_POINT_NONE	7
#define FTINTC030_CPU_CTRL_VAL		BIT(2)

struct ftintc030 {
	void __iomem *base;
	struct irq_domain *domain;
	raw_spinlock_t lock;
	unsigned int nr_spis;
};

static struct ftintc030 ftintc030;

/* Block offset of a one-bit-per-SPI register; a FIQ block sits after the second one. */
static unsigned int ftintc030_bank(irq_hw_number_t hwirq)
{
	unsigned int bank = hwirq / 32 * 0x20;

	return hwirq >= 64 ? bank + 0x20 : bank;
}

static void ftintc030_update(struct irq_data *d, unsigned int reg, bool set)
{
	void __iomem *addr = ftintc030.base + reg + ftintc030_bank(d->hwirq);
	u32 bit = BIT(d->hwirq % 32);
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&ftintc030.lock, flags);
	val = readl(addr);
	writel(set ? val | bit : val & ~bit, addr);
	raw_spin_unlock_irqrestore(&ftintc030.lock, flags);
}

static void ftintc030_mask(struct irq_data *d)
{
	ftintc030_update(d, FTINTC030_SPI_ENABLE, false);
}

static void ftintc030_unmask(struct irq_data *d)
{
	ftintc030_update(d, FTINTC030_SPI_ENABLE, true);
}

static void ftintc030_eoi(struct irq_data *d)
{
	writel(BIT(d->hwirq % 32), ftintc030.base + FTINTC030_SPI_CLEAR + ftintc030_bank(d->hwirq));
	writel(d->hwirq + FTINTC030_VECTOR_SPI_BASE, ftintc030.base + FTINTC030_EOI);
}

static int ftintc030_set_type(struct irq_data *d, unsigned int type)
{
	switch (type) {
	case IRQ_TYPE_LEVEL_HIGH:
	case IRQ_TYPE_LEVEL_LOW:
	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_EDGE_FALLING:
		break;
	default:
		return -EINVAL;
	}

	ftintc030_update(d, FTINTC030_SPI_EDGE, type & IRQ_TYPE_EDGE_BOTH);
	ftintc030_update(d, FTINTC030_SPI_ACTIVE_LOW,
			 type & (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_EDGE_FALLING));

	return 0;
}

static struct irq_chip ftintc030_chip = {
	.name = "FTINTC030",
	.irq_mask = ftintc030_mask,
	.irq_unmask = ftintc030_unmask,
	.irq_eoi = ftintc030_eoi,
	.irq_set_type = ftintc030_set_type,
};

static int ftintc030_map(struct irq_domain *domain, unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &ftintc030_chip, handle_fasteoi_irq);
	irq_set_probe(irq);

	return 0;
}

static const struct irq_domain_ops ftintc030_domain_ops = {
	.map = ftintc030_map,
	.xlate = irq_domain_xlate_twocell,
};

static void __exception_irq_entry ftintc030_handle_irq(struct pt_regs *regs)
{
	u32 vector = FIELD_GET(FTINTC030_VECTOR, readl(ftintc030.base + FTINTC030_ACK));

	if (vector == FTINTC030_VECTOR_SPURIOUS)
		return;

	if (vector < FTINTC030_VECTOR_SPI_BASE ||
	    generic_handle_domain_irq(ftintc030.domain, vector - FTINTC030_VECTOR_SPI_BASE)) {
		writel(vector, ftintc030.base + FTINTC030_EOI);
		pr_warn_ratelimited("FTINTC030: unhandled vector %u\n", vector);
	}
}

static void __init ftintc030_hw_init(u32 cpu_match)
{
	void __iomem *base = ftintc030.base;
	unsigned int i;

	for (i = 0; i < ftintc030.nr_spis; i += 32) {
		writel(0, base + FTINTC030_SPI_ENABLE + ftintc030_bank(i));
		writel(0, base + FTINTC030_SPI_EDGE + ftintc030_bank(i));
		writel(0, base + FTINTC030_SPI_ACTIVE_LOW + ftintc030_bank(i));
		writel(~0U, base + FTINTC030_SPI_CLEAR + ftintc030_bank(i));
	}

	/* Every SPI to every CPU, at the highest priority. */
	for (i = 0; i < ftintc030.nr_spis; i += 4) {
		writel(~0U, base + FTINTC030_SPI_TARGET + i);
		writel(0, base + FTINTC030_SPI_PRIORITY + i);
	}

	writel(cpu_match, base + FTINTC030_CPU_MATCH);
	writel(FTINTC030_CPU_CTRL_VAL, base + FTINTC030_CPU_CTRL);
	writel(FTINTC030_BINARY_POINT_NONE, base + FTINTC030_BINARY_POINT);
	writel(FTINTC030_PRIORITY_MASK_ALL, base + FTINTC030_PRIORITY_MASK);
}

static int __init ftintc030_of_init(struct device_node *np, struct device_node *parent)
{
	u32 cpu_match, feature;
	int ret;

	ret = of_property_read_u32(np, "faraday,cpu-match-id", &cpu_match);
	if (ret)
		return ret;

	ftintc030.base = of_iomap(np, 0);
	if (!ftintc030.base)
		return -ENOMEM;
	raw_spin_lock_init(&ftintc030.lock);

	feature = readl(ftintc030.base + FTINTC030_FEATURE);
	ftintc030.nr_spis = FIELD_GET(FTINTC030_FEATURE_SPIS, feature);
	if (!ftintc030.nr_spis ||
	    ftintc030.nr_spis > FTINTC030_VECTOR_SPURIOUS - FTINTC030_VECTOR_SPI_BASE) {
		ret = -EINVAL;
		goto err_unmap;
	}

	ftintc030.domain = irq_domain_create_linear(of_fwnode_handle(np), ftintc030.nr_spis,
						    &ftintc030_domain_ops, NULL);
	if (!ftintc030.domain) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	pr_debug("FTINTC030: %u SPIs, CPU control %x, binary point %x, priority mask %x before setup\n",
		ftintc030.nr_spis, readl(ftintc030.base + FTINTC030_CPU_CTRL),
		readl(ftintc030.base + FTINTC030_BINARY_POINT),
		readl(ftintc030.base + FTINTC030_PRIORITY_MASK));

	ftintc030_hw_init(cpu_match);
	set_handle_irq(ftintc030_handle_irq);

	return 0;

err_unmap:
	iounmap(ftintc030.base);
	return ret;
}

IRQCHIP_DECLARE(gc1601_intc, "galachip,gc1601-intc", ftintc030_of_init);
