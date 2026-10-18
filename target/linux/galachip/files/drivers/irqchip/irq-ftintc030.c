// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2014 Faraday Technology (Yan-Pai Chen) */
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <asm/exception.h>
#include <asm/mach/irq.h>

#define FT030_ENABLE	0x004
#define FT030_CLEAR	0x008
#define FT030_MODE	0x00c
#define FT030_POLARITY	0x010
#define FT030_PRIORITY	0x240
#define FT030_TARGET	0x420
#define FT030_PRIMASK	0x684
#define FT030_BINPOINT	0x688
#define FT030_ACK		0x68c
#define FT030_EOI		0x690
#define FT030_INTCR	0x69c
#define FT030_CPU_MATCH	0x6a0
#define FT030_FEATURE	0x700
#define FT030_SPURIOUS	511
#define FT030_VECTOR_BASE 32

struct ft030_irqchip {
	void __iomem *base;
	struct irq_domain *domain;
	raw_spinlock_t lock;
	unsigned int nr_irqs;
};

static struct ft030_irqchip *ft030_root;

static unsigned int ft030_bank(unsigned long hwirq)
{
	/* The FIQ register window separates the second and third SPI banks. */
	return (hwirq / 32) * 32 + (hwirq >= 64 ? 0x20 : 0);
}

static void ft030_update_enable(struct irq_data *d, bool enable)
{
	struct ft030_irqchip *f = irq_data_get_irq_chip_data(d);
	void __iomem *reg = f->base + FT030_ENABLE + ft030_bank(d->hwirq);
	u32 bit = BIT(d->hwirq % 32);
	unsigned long flags;
	u32 value;

	raw_spin_lock_irqsave(&f->lock, flags);
	value = readl(reg);
	writel(enable ? value | bit : value & ~bit, reg);
	raw_spin_unlock_irqrestore(&f->lock, flags);
}

static void ft030_mask(struct irq_data *d)
{
	ft030_update_enable(d, false);
}

static void ft030_unmask(struct irq_data *d)
{
	ft030_update_enable(d, true);
}

static void ft030_eoi(struct irq_data *d)
{
	struct ft030_irqchip *f = irq_data_get_irq_chip_data(d);

	writel(BIT(d->hwirq % 32),
	       f->base + FT030_CLEAR + ft030_bank(d->hwirq));
	writel(d->hwirq + FT030_VECTOR_BASE, f->base + FT030_EOI);
}

static int ft030_set_type(struct irq_data *d, unsigned int type)
{
	struct ft030_irqchip *f = irq_data_get_irq_chip_data(d);
	unsigned int bank = ft030_bank(d->hwirq);
	u32 bit = BIT(d->hwirq % 32), mode, polarity;
	unsigned long flags;
	bool edge, low;

	switch (type) {
	case IRQ_TYPE_LEVEL_HIGH:
	case IRQ_TYPE_LEVEL_LOW:
	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_EDGE_FALLING:
		edge = type & IRQ_TYPE_EDGE_BOTH;
		low = type == IRQ_TYPE_LEVEL_LOW || type == IRQ_TYPE_EDGE_FALLING;
		break;
	default:
		return -EINVAL;
	}

	raw_spin_lock_irqsave(&f->lock, flags);
	mode = readl(f->base + FT030_MODE + bank);
	polarity = readl(f->base + FT030_POLARITY + bank);
	writel(edge ? mode | bit : mode & ~bit, f->base + FT030_MODE + bank);
	writel(low ? polarity | bit : polarity & ~bit,
	       f->base + FT030_POLARITY + bank);
	raw_spin_unlock_irqrestore(&f->lock, flags);
	return 0;
}

static struct irq_chip ft030_chip = {
	.name = "FTINTC030",
	.irq_mask = ft030_mask,
	.irq_unmask = ft030_unmask,
	.irq_eoi = ft030_eoi,
	.irq_set_type = ft030_set_type,
};

static int ft030_map(struct irq_domain *domain, unsigned int irq,
		     irq_hw_number_t hwirq)
{
	struct ft030_irqchip *f = domain->host_data;

	if (hwirq >= f->nr_irqs)
		return -EINVAL;
	irq_set_chip_data(irq, f);
	irq_set_chip_and_handler(irq, &ft030_chip, handle_fasteoi_irq);
	irq_set_probe(irq);
	return 0;
}

static const struct irq_domain_ops ft030_domain_ops = {
	.map = ft030_map,
	.xlate = irq_domain_xlate_twocell,
};

static void __exception_irq_entry ft030_handle_irq(struct pt_regs *regs)
{
	struct ft030_irqchip *f = ft030_root;
	u32 vector = readl(f->base + FT030_ACK) & 0x1ff;

	if (vector == FT030_SPURIOUS)
		return;
	if (vector < FT030_VECTOR_BASE ||
	    vector >= FT030_VECTOR_BASE + f->nr_irqs ||
	    generic_handle_domain_irq(f->domain, vector - FT030_VECTOR_BASE)) {
		writel(vector, f->base + FT030_EOI);
		pr_warn_ratelimited("FTINTC030: unhandled vector %u\n", vector);
	}
}

static int __init ft030_init(struct device_node *np, struct device_node *parent)
{
	struct ft030_irqchip *f;
	u32 cpu_match;
	unsigned int irq, bank;
	int ret;

	if (parent || ft030_root)
		return -EINVAL;
	ret = of_property_read_u32(np, "faraday,cpu-match-id", &cpu_match);
	if (ret)
		return ret;
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->base = of_iomap(np, 0);
	if (!f->base) {
		ret = -ENOMEM;
		goto free_chip;
	}
	raw_spin_lock_init(&f->lock);
	f->nr_irqs = (readl(f->base + FT030_FEATURE) >> 2) & 0x1ff;
	if (!f->nr_irqs || f->nr_irqs > FT030_SPURIOUS - FT030_VECTOR_BASE) {
		ret = -EINVAL;
		goto unmap;
	}
	f->domain = irq_domain_create_linear(of_fwnode_handle(np), f->nr_irqs,
					     &ft030_domain_ops, f);
	if (!f->domain) {
		ret = -ENOMEM;
		goto unmap;
	}

	for (irq = 0; irq < f->nr_irqs; irq += 32) {
		bank = ft030_bank(irq);
		writel(0, f->base + FT030_ENABLE + bank);
		writel(0, f->base + FT030_MODE + bank);
		writel(0, f->base + FT030_POLARITY + bank);
		writel(~0U, f->base + FT030_CLEAR + bank);
	}
	/* GC1601 has one target byte per SPI, unlike other FTINTC030 variants. */
	for (irq = 0; irq < f->nr_irqs; irq += 4) {
		writel(~0U, f->base + FT030_TARGET + irq);
		writel(0, f->base + FT030_PRIORITY + irq);
	}
	writel(cpu_match, f->base + FT030_CPU_MATCH);
	writel(4, f->base + FT030_INTCR);
	writel(7, f->base + FT030_BINPOINT);
	writel(15, f->base + FT030_PRIMASK);
	ft030_root = f;
	set_handle_irq(ft030_handle_irq);
	return 0;

unmap:
	iounmap(f->base);
free_chip:
	kfree(f);
	return ret;
}
IRQCHIP_DECLARE(gc1601_intc, "galachip,gc1601-intc", ft030_init);
