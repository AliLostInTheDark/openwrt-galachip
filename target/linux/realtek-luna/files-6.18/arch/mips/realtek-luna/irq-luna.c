// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL960x "Luna" SoC interrupt controller (the "GISR" aggregator).
 *
 * ONE driver, two chips, a table for what differs.  It replaces
 * irq-rtl9602c.c and irq-rtl9603cvd.c, which were 96-98%% identical: the same
 * struct, the same mask/unmask/map/dispatch/of_init, the same offsets, and two
 * spellings of every name (`LUNA_INTC_GIMR` and `GISR_GIMR`, same value).  What
 * actually differed was three things, and all three are now DATA: the routing
 * table, whether a second CPU block must be masked, and nothing else.
 *
 * ★ THE MERGE WAS DEFERRED ON PURPOSE, AND THE DEFERRAL'S OWN TERMS ARE MET.
 * irq-rtl9603cvd.c said: *"kept apart deliberately for now -- that driver
 * carries two routing overrides established empirically on the X111W, which are
 * facts about THAT board's cascade and must not be inherited here.  Merge them
 * into one luna_* driver once this chip has booted and both routings are
 * proven."*  Both halves were checked before merging rather than assumed:
 *   - the per-board routing is NOT inherited: each chip keeps its own `irr[]`,
 *     which is the whole point of the table;
 *   - the RTL9602C's routing is proven by the X111W booting on it;
 *   - the RTL9603CVD's is proven by the G24W's console log of 2026-08-23,
 *     which shows `Linux version 6.18.31` and an INTERACTIVE `root@OpenWrt:~#`
 *     shell at uptime 1:50 answering commands.  That is this file's own stated
 *     test: without this aggregator delivering the UART interrupt, "early
 *     printk would work and the console would never become interactive".
 *
 * Clean-room: written from observed hardware facts only -- register bases and
 * offsets, bit semantics, the routing scheme and the vendor's own init values.
 * No proprietary source text or structure was copied.
 *
 * ---------------------------------------------------------------------
 * WHY THIS FAMILY DOES NOT USE A MIPS GIC
 *
 * The RTL9603CVD has NO MIPS GIC and no Coherence Manager.  Its own vendor
 * kernel says so on this exact board (tier 1, LANLY G24W boot console, captured
 * 2026-08-18):
 *
 *	MIPS CPS SMP unable to proceed without a CM
 *	GIC isn't present!
 *	rtl9603cvd_gisr_init: gisr_translate_d2c = ...
 *
 * so a "mti,gic" node here describes silicon that is not present: the UART
 * interrupt would never arrive, early printk would work and the console would
 * never become interactive.  Describe this aggregator instead.
 * ---------------------------------------------------------------------
 *
 * REGISTER MAP (one block PER CPU where the chip has more than one; stride is
 * a table field, 0 where there is only one block to arrange):
 *
 *	GIMR0	+0x00	mask,   inputs  0..31  (1 = enabled)
 *	GIMR1	+0x04	mask,   inputs 32..63
 *	GISR0	+0x08	status, inputs  0..31  (raw; AND with mask for pending)
 *	GISR1	+0x0c	status, inputs 32..63
 *	IRR0..6	+0x10..	routing, 4 bits per input, 8 inputs per word
 *
 * GIMR0 bit 12 is not an input: it is the master peripheral-IRQ enable and must
 * be set for any peripheral interrupt to be delivered at all.  The dispatch
 * below therefore excludes it from the pending scan.
 *
 * Realtek numbers the routing nibbles in inverted word order: input 0 lives in
 * the *last* IRR word's lowest nibble.
 *
 * ROUTING NIBBLE -> CP0 LINE.  A nibble of 0 disconnects the input; a non-zero
 * value V is delivered on CP0 HW IRQ V.
 *
 * ⚠ THIS USED TO SAY "CP0 HW IRQ (V + 1)" AND THE TREE ITSELF REFUTES IT, two
 * independent ways.  The nibbles both tables actually use are {2, 3, 6}, and
 * the set of parent lines this chip's device tree hooks is {2, 3, 6} -- equal,
 * which V+1 cannot produce.  And GMAC0 (input 26) carries nibble 3 while the
 * board's Ethernet demonstrably works, so nibble 3 cannot be arriving on CP0 4,
 * a line no parent claims.  It matters to anyone ADDING an input: the arithmetic
 * decides which of the three chained parents must exist for that input to be
 * delivered at all, and getting it wrong disconnects a line silently.
 *
 * Cross-checked two ways, and the two agree on every case:
 *
 *   - the vendor's own dispatchers read GISR word 0 from CP0 IP3 and GISR word
 *     1 from CP0 IP4, and take the SoC timer on IP7;
 *   - the vendor's own routing values (the tables below) give nibble 2 to every
 *     input in word 0 that it uses (switch core, GMAC0, PON NIC), nibble 3 to
 *     every input in word 1 that it uses (UART0, GPIO bank ABCD), and nibble 6
 *     to the per-CPU system timer.
 *
 *   2 -> CP0 IRQ 3	3 -> CP0 IRQ 4		6 -> CP0 IRQ 7
 *
 * The strongest single check is the per-CPU one: the vendor loads a DIFFERENT
 * word 4 on each CPU, and the only nibbles that differ are the two system-timer
 * inputs -- TC0 enabled on CPU0 and disconnected on CPU1, TC1 the other way
 * round.  That is exactly what a per-CPU clockevent needs and nothing else in
 * the map would explain it.
 *
 * INPUT NUMBERING IS THE NATIVE GISR BIT, and it is SHARED BY BOTH CHIPS --
 * which is the strongest evidence that this really is one block: the RTL9602C's
 * device tree in this same target independently uses 49 for UART0 and 43 for
 * the system timer, the same numbers the RTL9603CVD's vendor kernel uses.
 * Named inputs (a reserved input is omitted):
 *
 *	 4 ECC		 5 NAND		 8 switch core	14 USB host p2
 *	15 PCIe PORT 0	16 PCIe PORT 1	17 PCM0		18 PCM1
 *	24 PON NIC	25 PON NIC DS	26 GMAC0 int0	27 GMAC0 int1
 *	28 VoIP XSI	29 VoIP SPI	31 LX bus debug	32 GPIO JKMN
 *	39 WDT phase-1
 *	40 WDT phase-2	41 GPIO EFGH	42 GPIO ABCD	43..48 TC0..TC5
 *	49..52 UART0..3	60..62 TC6..TC8
 *
 * ★★★ PCIe IS TWO INPUTS, ONE PER ROOT-COMPLEX PORT, AND THIS LIST USED TO
 * CARRY ONE ENTRY FOR THE PAIR.  It said "16 PCIe" and did not name 15 at all,
 * so a reader checking a chip's `.hwirq` against this map found 16 confirmed
 * and 15 absent -- and on 2026-09-01 that is exactly what moved the RTL9602C
 * (which is on PORT 0) from its correct 15 to 16, leaving its radio deaf with
 * every source apparently agreeing.  MEASURED CAUSALLY on the X111W 2026-09-10
 * (`ONU-test-case/pcie_intx_bind.py`): masking the endpoint's own HIMR/HIMRE
 * clears GISR0 bit 15 and restoring it sets bit 15 again, twice, while bit 16
 * never sets.  The vendor's own map of this same register agrees
 * (BSP_PCIE0_IE = BIT(15), BSP_PCIE1_IE = BIT(16) at 0xB8003000).
 *
 * ⇒ AN INPUT THIS LIST OMITS IS NOT AN INPUT THAT DOES NOT EXIST.  The list is
 * evidence of what HAS been named, never of what is absent; treat a gap as an
 * open question.  `pcie_irq_port_guard.py` now checks each chip's declared
 * PCIe input against the port that chip's own MAC gate selects, so the pair
 * cannot drift apart again silently.
 *
 * Each input is enabled on at most one CPU; affinity moves its mask atomically.
 *
 * Copyright (C) 2026 Confiared <contact@confiared.com>
 */

#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/cpu.h>

#define LUNA_INTC_INPUTS	64
#define LUNA_INTC_WORDS	(LUNA_INTC_INPUTS / 32)	/* mask/status */
#define LUNA_INTC_GIMR(w)	(0x00 + (w) * 4)
#define LUNA_INTC_GISR(w)	(0x08 + (w) * 4)
#define LUNA_INTC_IRR(i)	(0x10 + (i) * 4)
/* ⚠ SEVEN, AND IT IS COUNTED FROM THE HARDWARE, NOT DERIVED ...
 * dev/MEASURED-irq-luna.c.md sec 1. */
#define LUNA_INTC_IRR_WORDS	7
#define LUNA_INTC_TIMER_IRR	4
#define LUNA_INTC_PERIPH_EN	12	/* GIMR0 bit12 = master peripheral enable */

/*
 * ★★ THE PER-CHIP TABLE -- the only thing that differs between these two SoCs.
 * A third Luna chip is a table entry, never a fork of the logic.
 */
struct luna_intc_chip {
	const char	*name;
	/* The vendor's own routing values for CPU 0, word 0 first. */
	u32		irr[LUNA_INTC_IRR_WORDS];
	/* Byte distance to the NEXT CPU's block, or 0 where this chip has only
	 * one block for this driver to arrange. */
	u32		cpu_stride;
	u32		cpu1_timer_irr;
};

static const struct luna_intc_chip rtl9602c_intc_chip = {
	.name	= "RTL9602C",
	/* ⚠ TWO WORDS DIFFER FROM THE SIBLING'S, AND THEY ARE THIS ...
	 * dev/MEASURED-irq-luna.c.md sec 2. */
	.irr	= { 0x03333330, 0x30302222, 0x00020226, 0x22020333,
		    0x63333063, 0x32322022, 0x00333000 },
	.cpu_stride = 0,	/* this driver arranges one block here */
	.cpu1_timer_irr = 0,
};

static const struct luna_intc_chip rtl9603cvd_intc_chip = {
	.name	= "RTL9603CVD",
	.irr	= { 0x03333330, 0x30302222, 0x00020222, 0x22020333,
		    0x33333063, 0x32322022, 0x00333000 },
	.cpu_stride = 0x40,
	.cpu1_timer_irr = 0x33333603,	/* TC1 to CPU1; TC0 disconnected */
};

struct luna_intc {
	void __iomem			*base;	/* CPU 0's block */
	raw_spinlock_t			lock;
	struct irq_domain		*domain;
	const struct luna_intc_chip	*c;
	u8				target[LUNA_INTC_INPUTS];
};

static void luna_intc_mask(struct irq_data *d)
{
	struct luna_intc *ic = irq_data_get_irq_chip_data(d);
	unsigned int word = d->hwirq / 32;
	void __iomem *base;
	u32 val;

	raw_spin_lock(&ic->lock);
	base = ic->base + ic->c->cpu_stride * ic->target[d->hwirq];
	val = readl(base + LUNA_INTC_GIMR(word));
	val &= ~BIT(d->hwirq % 32);
	writel(val, base + LUNA_INTC_GIMR(word));
	raw_spin_unlock(&ic->lock);
}

static void luna_intc_unmask(struct irq_data *d)
{
	struct luna_intc *ic = irq_data_get_irq_chip_data(d);
	unsigned int word = d->hwirq / 32;
	void __iomem *base;
	u32 val;

	raw_spin_lock(&ic->lock);
	base = ic->base + ic->c->cpu_stride * ic->target[d->hwirq];
	val = readl(base + LUNA_INTC_GIMR(word));
	val |= BIT(d->hwirq % 32);
	writel(val, base + LUNA_INTC_GIMR(word));
	raw_spin_unlock(&ic->lock);
}

#ifdef CONFIG_SMP
static int luna_intc_set_affinity(struct irq_data *d,
				 const struct cpumask *mask, bool force)
{
	struct luna_intc *ic = irq_data_get_irq_chip_data(d);
	unsigned int cpu = cpumask_first_and(mask, force ? cpu_possible_mask :
					    cpu_online_mask);
	unsigned int word = d->hwirq / 32;
	u32 bit = BIT(d->hwirq % 32);
	void __iomem *base;
	unsigned long flags;

	if (cpu >= (ic->c->cpu_stride ? 2 : 1))
		return -EINVAL;
	/* The two clockevent routes are wired to their respective CPU blocks. */
	if (ic->c->cpu_stride && (d->hwirq == 43 || d->hwirq == 44)) {
		cpu = d->hwirq - 43;
		if (!cpumask_test_cpu(cpu, mask) ||
		    !cpumask_test_cpu(cpu, force ? cpu_possible_mask : cpu_online_mask))
			return -EINVAL;
	}

	raw_spin_lock_irqsave(&ic->lock, flags);
	base = ic->base + ic->c->cpu_stride * ic->target[d->hwirq];
	writel(readl(base + LUNA_INTC_GIMR(word)) & ~bit,
	       base + LUNA_INTC_GIMR(word));
	ic->target[d->hwirq] = cpu;
	base = ic->base + ic->c->cpu_stride * cpu;
	if (!irqd_irq_masked(d) && !irqd_irq_disabled(d))
		writel(readl(base + LUNA_INTC_GIMR(word)) | bit,
		       base + LUNA_INTC_GIMR(word));
	irq_data_update_effective_affinity(d, cpumask_of(cpu));
	raw_spin_unlock_irqrestore(&ic->lock, flags);
	return IRQ_SET_MASK_OK;
}
#endif

static struct irq_chip luna_intc_irqchip = {
	.name		= "rtl960x-intc",
	.irq_mask	= luna_intc_mask,
	.irq_unmask	= luna_intc_unmask,
#ifdef CONFIG_SMP
	.irq_set_affinity = luna_intc_set_affinity,
#endif
};

static int luna_intc_map(struct irq_domain *d, unsigned int irq,
			    irq_hw_number_t hw)
{
	struct luna_intc *ic = d->host_data;

	irq_set_chip_and_handler(irq, &luna_intc_irqchip, handle_level_irq);
	irq_set_chip_data(irq, ic);
	irq_data_update_effective_affinity(irq_get_irq_data(irq), cpumask_of(0));
	/* routing (GIRR) is pre-loaded with the stock-observed values in init */

	return 0;
}

static const struct irq_domain_ops luna_intc_domain_ops = {
	.map	= luna_intc_map,
	.xlate	= irq_domain_xlate_onecell,
};

static void luna_intc_dispatch(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct luna_intc *ic = irq_desc_get_handler_data(desc);
	void __iomem *base = ic->base + ic->c->cpu_stride * smp_processor_id();
	int word;

	chained_irq_enter(chip, desc);
	for (word = 0; word < LUNA_INTC_WORDS; word++) {
		unsigned long pending = readl(base + LUNA_INTC_GISR(word)) &
					readl(base + LUNA_INTC_GIMR(word));
		unsigned int bit;

		if (word == 0)
			pending &= ~BIT(LUNA_INTC_PERIPH_EN);  /* aggregate, not an input */
		for_each_set_bit(bit, &pending, 32)
			generic_handle_domain_irq(ic->domain, word * 32 + bit);
	}
	chained_irq_exit(chip, desc);
}

static int __init luna_intc_init(struct device_node *node,
				    const struct luna_intc_chip *chip)
{
	struct luna_intc *ic;
	int parent_irq, n = 0, i, ret;

	ic = kzalloc(sizeof(*ic), GFP_KERNEL);
	if (!ic)
		return -ENOMEM;
	raw_spin_lock_init(&ic->lock);
	ic->c = chip;
	ic->base = of_iomap(node, 0);
	if (!ic->base) {
		ret = -ENXIO;
		goto err_free;
	}

	writel(BIT(LUNA_INTC_PERIPH_EN), ic->base + LUNA_INTC_GIMR(0));
	writel(0, ic->base + LUNA_INTC_GIMR(1));
	for (i = 0; i < LUNA_INTC_IRR_WORDS; i++)
		writel(chip->irr[i], ic->base + LUNA_INTC_IRR(i));

	/* Route both blocks before any input is assigned to the second CPU. */
	if (chip->cpu_stride) {
		void __iomem *other = ic->base + chip->cpu_stride;

		writel(BIT(LUNA_INTC_PERIPH_EN), other + LUNA_INTC_GIMR(0));
		writel(0, other + LUNA_INTC_GIMR(1));
		for (i = 0; i < LUNA_INTC_IRR_WORDS; i++)
			writel(i == LUNA_INTC_TIMER_IRR ? chip->cpu1_timer_irr :
			       chip->irr[i], other + LUNA_INTC_IRR(i));
	}

	ic->domain = irq_domain_create_linear(of_fwnode_handle(node),
					      LUNA_INTC_INPUTS,
					      &luna_intc_domain_ops, ic);
	if (!ic->domain) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	for (n = 0; (parent_irq = irq_of_parse_and_map(node, n)) > 0; n++)
		irq_set_chained_handler_and_data(parent_irq,
						 luna_intc_dispatch, ic);
	if (!n) {
		ret = -ENODEV;
		goto err_unmap;
	}

	pr_info("rtl960x-intc: %s aggregator, %d inputs, %d parent line(s)\n",
		chip->name, LUNA_INTC_INPUTS, n);
	return 0;

err_unmap:
	iounmap(ic->base);
err_free:
	kfree(ic);
	return ret;
}

/* IRQCHIP_DECLARE wants a fixed signature, so each compatible gets a two-line
 * entry point that names its own table.  This is the ONLY per-chip code left. */
static int __init rtl9602c_intc_of_init(struct device_node *node,
					struct device_node *parent)
{
	return luna_intc_init(node, &rtl9602c_intc_chip);
}

static int __init rtl9603cvd_intc_of_init(struct device_node *node,
					  struct device_node *parent)
{
	return luna_intc_init(node, &rtl9603cvd_intc_chip);
}

IRQCHIP_DECLARE(rtl9602c_intc, "realtek,rtl9602c-intc", rtl9602c_intc_of_init);
IRQCHIP_DECLARE(rtl9603cvd_intc, "realtek,rtl9603cvd-intc",
		rtl9603cvd_intc_of_init);
