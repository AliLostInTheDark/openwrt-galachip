// SPDX-License-Identifier: GPL-2.0-or-later
/* Realtek "Luna" RTL960x PCIe host controller driver. ONE ...
 * dev/MEASURED-pcie-luna.c.md sec 1. */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/types.h>

/* SoC system controller registers (KSEG1, uncached). Present on BOTH chips
 * except where the table says otherwise -- see SOC_PINMUX. */
#define SOC_PINMUX	((void __iomem *)0xb800004cul)	/* 9602C only          */
#define SOC_PCI_MISC	((void __iomem *)0xb8000504ul)	/* PCIe MDIO reset     */
#define SOC_IP_SEL	((void __iomem *)0xb8000600ul)	/* per-IP MAC enable   */

#define PINMUX_PCIE		0x10000000u	/* PCIe mux bit in SOC_PINMUX     */
#define PCI_MISC_MDIO_CLR	BIT(14)		/* MDIO reset: cleared in reset   */
#define PCI_MISC_MDIO_P0	BIT(24)		/* MDIO reset strobe, port 0      */
#define PCI_MISC_MDIO_P1	BIT(21)		/* MDIO reset strobe, port 1      */
#define IP_SEL_EN_PCIE0		BIT(7)		/* port-0 PCIe MAC enable (gate)  */
#define IP_SEL_EN_PCIE1		BIT(6)		/* port-1 PCIe MAC enable (gate)  */
/* 9602C ONLY: bits the operational config sets alongside the PCIe MAC
 * (IP_SEL = 0x04001887). Without them the endpoint's config space never decodes
 * (reads the 0xeeeeeeee abort pattern) even though the link trains.
 * ⚠ The RTL9603CVD's stock kernel NEVER sets any of them; leave its ip_pre_or 0. */
#define IP_SEL_EN_PCIE_PHY	BIT(26)		/* 9602C PCIe SerDes/PHY enable   */
#define IP_SEL_EN_EXTRA		(BIT(0) | BIT(2) | BIT(11) | BIT(12))

/* hostext offsets, identical on both chips. */
#define HOSTEXT_MDIO	0x000	/* PHY MDIO write: [31:16]=val [15:8]=reg bit0=go */
#define HOSTEXT_LTSSM	0x008	/* bit7 = PHY_RST_N, bit0 = LTSSM enable */
#define HOSTEXT_FN	0x00c	/* PCI function-number select            */

/* hostcfg offsets, identical on both chips. */
#define HOSTCFG_CMD	0x004	/* command/status                        */
#define HOSTCFG_PAYLOAD	0x078	/* MAX_PAYLOAD_SIZE in bits [7:5]        */
#define HOSTCFG_SPEED	0x082	/* [3:0] link speed: 1 = 2.5 GT/s        */
#define HOSTCFG_LINK	0x728	/* LTSSM state; [4:0]==0x11 => link up   */
#define HOSTCFG_CFGCTL	0x80c	/* bit17 enables endpoint config access  */
#define CFGCTL_FWD_EN	BIT(17)	/* write-enable; reads back as 0x100 once active */

#define LINK_UP_STATE	0x11u

struct luna_pcie_phy { u8 reg; u16 val; };

/* RTL9602C PCIe SerDes PHY tuning, written over the host MDIO ...
 * dev/MEASURED-pcie-luna.c.md sec 2. */
static const struct luna_pcie_phy luna_pcie_phy_9602c_stock[] = {
	{ 0x03, 0x3031 }, { 0x06, 0xe0b8 }, { 0x0e, 0x98c5 },
	{ 0x0f, 0x400f }, { 0x19, 0xfc70 },
	{ 0xff, 0xffff },
};

static bool pcie_phy_full;

static int __init pcie_phy_full_setup(char *str)
{
	pcie_phy_full = true;
	(void)str;
	return 1;
}
/* early_param, NOT module_param: this table is consumed from arch PCIe init,
 * which runs before module parameters are parsed. A module_param here would
 * read as a working knob and silently never take effect. */
early_param("pcie_phy_full", pcie_phy_full_setup);

static const struct luna_pcie_phy luna_pcie_phy_9602c[] = {
	/* ★ THE FOUR ENTRIES THIS TREE CAN NAME ARE NAMED AT THE ...
	 * dev/MEASURED-pcie-luna.c.md sec 3. */
	{ 0x01, 0xa852 }, { 0x06, 0x0017 }, { 0x08, 0x3591 }, { 0x09, 0x520c },
	{ 0x0a, 0xf670 }, { 0x0b, 0xa90d }, { 0x0d, 0xe720 }, { 0x0e, 0x1000 },
	{ 0x1c, 0x2001 }, { 0x1e, 0x66eb },
	/* 0x20 = SerDes PLL, 0x21 = its clock divider -- the pair ...
	 * dev/MEASURED-pcie-luna.c.md sec 4. */
	{ 0x20, 0xd4a4 }, { 0x21, 0x485a },
	{ 0x23, 0x0b66 }, { 0x24, 0x4f0c }, { 0x29, 0xf0f3 }, { 0x2b, 0xa0a1 },
	{ 0x09, 0x500c }, { 0x09, 0x520c },
	/* 25 MHz reference-clock SerDes values (the board has a 25 ...
	 * dev/MEASURED-pcie-luna.c.md sec 16. */
	{ 0x03, 0x3031 },	/* refclk PLL multiplier: 0x3031 = 25 MHz
				 * (0x7b31 is the 40 MHz value) */
	{ 0x06, 0xe0b8 },	/* refclk-dependent companion of 0x03
				 * (0xe2b8 on the 40 MHz table) */
	{ 0x0e, 0x98c5 },
	{ 0x0f, 0x400f }, { 0x19, 0xfc70 },
	{ 0xff, 0xffff },	/* the terminator, not a register */
};

/* RTL9603CVD PCIe SerDes ePHY tuning -- port 1. TEN pairs, ...
 * dev/MEASURED-pcie-luna.c.md sec 5. */
static const struct luna_pcie_phy luna_pcie_phy_9603cvd[] = {
	{ 0x00, 0x8a50 }, { 0x02, 0x26f9 }, { 0x03, 0x6bcd }, { 0x06, 0x1088 },
	{ 0x08, 0x4a45 }, { 0x09, 0x6303 }, { 0x0b, 0x0009 }, { 0x0c, 0x0800 },
	{ 0x20, 0x0105 }, { 0x21, 0x1000 },
	{ 0xff, 0xffff },
};

/* The per-chip table. A field that is 0 is NOT PRESENT on ...
 * dev/MEASURED-pcie-luna.c.md sec 6. */
struct luna_pcie_chip {
	const char *name;
	const char *root_compat;	/* DT root compatible selecting this entry */
	const char *intc_compat;	/* which INTC maps the aggregated INTx	 */
	unsigned int hwirq;		/* that INTC's INPUT number for PCIe INTx */

	unsigned long hostcfg;		/* root-bridge config window, KSEG1	 */
	unsigned long devcfg;		/* endpoint config window, KSEG1	 */
	unsigned long hostext;		/* host-controller extension, KSEG1	 */
	u32 mem_phys, mem_size;
	u32 io_phys, io_size;

	u32 pinmux_bit;			/* SOC_PINMUX bit; 0 = no such register	 */
	u32 misc_strobe;		/* SOC_PCI_MISC reset bit(s) for THIS port */
	u32 ip_mac_bit;			/* SOC_IP_SEL MAC gate for THIS port	 */
	u32 ip_pre_or;			/* extra SOC_IP_SEL bits; 0 = none	 */

	/* Endpoint PERST#, active LOW. All four 0 = not software-driven here. */
	unsigned long perst_pad_en;	/* IO_GPIO_EN word holding this pad	 */
	unsigned long perst_dir;	/* GPIO direction word (1 = output)	 */
	unsigned long perst_data;	/* GPIO data word			 */
	u8 perst_bit;

	const struct luna_pcie_phy *phy;
	unsigned int retries;		/* full reset+train attempts		 */
	unsigned int link_polls;	/* 10 ms polls per attempt		 */
};

static const struct luna_pcie_chip luna_pcie_9602c = {
	.name = "RTL9602C", .root_compat = "realtek,rtl9602c",
	/* ★★★ AGGREGATOR INPUT 15, AND IT IS THE PORT THAT DECIDES -- ...
	 * dev/MEASURED-pcie-luna.c.md sec 7. */
	.intc_compat = "realtek,rtl9602c-intc", .hwirq = 15,
	.hostcfg = 0xb8b00000ul, .devcfg = 0xb8b10000ul, .hostext = 0xb8b01000ul,
	.mem_phys = 0x19000000u, .mem_size = 0x01000000u,
	.io_phys = 0x18c00000u, .io_size = 0x00010000u,
	.pinmux_bit = PINMUX_PCIE,
	/* This board trains only with BOTH port reset bits strobed. */
	.misc_strobe = PCI_MISC_MDIO_P0 | PCI_MISC_MDIO_P1,
	.ip_mac_bit = IP_SEL_EN_PCIE0,
	.ip_pre_or = IP_SEL_EN_PCIE_PHY | IP_SEL_EN_EXTRA,
	/* PERST# is not driven by an SoC GPIO here: probing shows the ...
	 * dev/MEASURED-pcie-luna.c.md sec 8. */
	.perst_pad_en = 0, .perst_dir = 0, .perst_data = 0, .perst_bit = 0,
	/* resolved at init: stock's five by default, the full table with
	 * `pcie_phy_full` on the command line. See luna_pcie_phy_9602c_stock. */
	.phy = luna_pcie_phy_9602c_stock, .retries = 3, .link_polls = 10,
};

static const struct luna_pcie_chip luna_pcie_9603cvd = {
	.name = "RTL9603CVD", .root_compat = "realtek,rtl9603cvd",
	/* Aggregator input 16. TIER 1, from the board's own boot ...
	 * dev/MEASURED-pcie-luna.c.md sec 17. */
	.intc_compat = "realtek,rtl9603cvd-intc", .hwirq = 16,
	/* PORT 1, not port 0. Port 0's constants exist in the stock code and are
	 * dead on this product -- its CPU-side interrupt number has no aggregator
	 * input at all (the translate table's row for it is -1). */
	.hostcfg = 0xb8b00000ul, .devcfg = 0xb8b10000ul, .hostext = 0xb8b01000ul,
	.mem_phys = 0x19000000u, .mem_size = 0x01000000u,
	.io_phys = 0x18c00000u, .io_size = 0x00010000u,
	/* ⚠ 0: SOC_PINMUX IS NOT A REGISTER ON THIS DIE. Nothing in this chip's
	 * stock kernel touches 0xb800004c, and its own chipdef names 0x48/0x4c
	 * CFG_PCSXF / CFG_PHY_CTRL -- the same pair that, written as the 9602C's
	 * IO_GPIO_EN, put BASE_PHYAD=25 on this board's PHY bus for weeks. */
	.pinmux_bit = 0,
	.misc_strobe = PCI_MISC_MDIO_P1,
	.ip_mac_bit = IP_SEL_EN_PCIE1,
	/* ⚠ 0 DELIBERATELY: stock never sets BIT(26) or BIT(0|2|11|12) here, and
	 * they enable IP blocks nobody has identified on this silicon. */
	.ip_pre_or = 0,
	/* PERST# = GPIO 40 = bank 1 bit 8, ACTIVE LOW. The pin comes from stock's
	 * own device tree (`board_setting` / `pci0_gpio_rst = <&bank1 8 0>`) and
	 * the board prints `PCIE0 reset pin is set to GPIO 40`. The three words are
	 * the SWCORE pad-enable IO_GPIO_EN+4 and the bank-1 direction/data pair. */
	.perst_pad_en = 0xbb000040ul, .perst_dir = 0xb8003324ul,
	.perst_data = 0xb8003328ul, .perst_bit = 8,
	.phy = luna_pcie_phy_9603cvd, .retries = 4, .link_polls = 9,
};

/* ★★★ THE CHIP TABLE MUST SURVIVE INIT -- IT IS NOT INIT DATA ...
 * dev/MEASURED-pcie-luna.c.md sec 9. */
static const struct luna_pcie_chip *chip;	/* resolved in luna_pcie_init() */

static DEFINE_SPINLOCK(luna_pcie_lock);
static u8 luna_pcie_busnr = 0xff;

static inline void __iomem *pcie_hostcfg(void)
{
	return (void __iomem *)chip->hostcfg;
}

static inline void __iomem *pcie_devcfg(void)
{
	return (void __iomem *)chip->devcfg;
}

static inline void __iomem *pcie_hostext(void)
{
	return (void __iomem *)chip->hostext;
}

/* config-space accessors ---------- Only two devices exist on ...
 * dev/MEASURED-pcie-luna.c.md sec 10. */

static int luna_pcie_access(struct pci_bus *bus, unsigned int devfn, int where,
			    int size, u32 *val, bool is_write)
{
	unsigned int slot = PCI_SLOT(devfn);
	void __iomem *reg;
	unsigned long flags;

	if (luna_pcie_busnr == 0xff)
		luna_pcie_busnr = bus->number;
	if (!chip || bus->number != luna_pcie_busnr || slot > 1)
		return PCIBIOS_DEVICE_NOT_FOUND;
	if (size != 1 && size != 2 && size != 4)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* slot 0 = root bridge, slot 1 = the single downstream endpoint. */
	reg = (slot ? pcie_devcfg() : pcie_hostcfg()) + where;

	spin_lock_irqsave(&luna_pcie_lock, flags);
	writel(PCI_FUNC(devfn), pcie_hostext() + HOSTEXT_FN);
	mb();			/* order the function-select latch before the access */
	if (is_write) {
		if (size == 4)
			writel(*val, reg);
		else if (size == 2)
			writew(*val, reg);
		else
			writeb(*val, reg);
	} else if (size == 4) {
		*val = readl(reg);
	} else if (size == 2) {
		*val = readw(reg);
	} else {
		*val = readb(reg);
	}
	spin_unlock_irqrestore(&luna_pcie_lock, flags);
	return PCIBIOS_SUCCESSFUL;
}

static int luna_pcie_read(struct pci_bus *bus, unsigned int devfn,
			  int where, int size, u32 *val)
{
	int ret = luna_pcie_access(bus, devfn, where, size, val, false);

	if (ret != PCIBIOS_SUCCESSFUL)
		*val = ~0u;	/* absent device reads as all-ones */
	return ret;
}

static int luna_pcie_write(struct pci_bus *bus, unsigned int devfn,
			   int where, int size, u32 val)
{
	return luna_pcie_access(bus, devfn, where, size, &val, true);
}

static struct pci_ops luna_pcie_ops = {
	.read  = luna_pcie_read,
	.write = luna_pcie_write,
};

/* ---------- arch hooks ---------- */

int pcibios_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	static int pcie_virq;

	/* The endpoint's INTx is aggregated by the SoC INTC onto a ...
	 * dev/MEASURED-pcie-luna.c.md sec 11. */
	if (!pcie_virq && chip) {
		struct device_node *np;

		np = of_find_compatible_node(NULL, NULL, chip->intc_compat);
		if (np) {
			struct irq_domain *domain = irq_find_host(np);

			of_node_put(np);
			if (domain)
				pcie_virq = irq_create_mapping(domain,
							       chip->hwirq);
		}
	}
	return pcie_virq;
}

int pcibios_plat_dev_init(struct pci_dev *dev)
{
	return 0;
}

/* ---------- bus resources / controller ---------- */

static struct resource luna_pcie_mem = {
	.name  = "PCIe MEM",
	.flags = IORESOURCE_MEM,
};
static struct resource luna_pcie_io = {
	.name  = "PCIe IO",
	.flags = IORESOURCE_IO,
};
static struct pci_controller luna_pcie_controller = {
	.pci_ops      = &luna_pcie_ops,
	.mem_resource = &luna_pcie_mem,
	.io_resource  = &luna_pcie_io,
};

/* bring-up ---------- Drive the endpoint PERST# pin. ACTIVE ...
 * dev/MEASURED-pcie-luna.c.md sec 12. */
static void __init luna_pcie_perst(bool assert)
{
	u32 bit;

	if (!chip->perst_data)
		return;
	bit = 1u << chip->perst_bit;
	/* claim the pad for GPIO, then drive it as an output */
	writel(readl((void __iomem *)chip->perst_pad_en) | bit,
	       (void __iomem *)chip->perst_pad_en);
	writel(readl((void __iomem *)chip->perst_dir) | bit,
	       (void __iomem *)chip->perst_dir);
	if (assert)
		writel(readl((void __iomem *)chip->perst_data) & ~bit,
		       (void __iomem *)chip->perst_data);
	else
		writel(readl((void __iomem *)chip->perst_data) | bit,
		       (void __iomem *)chip->perst_data);
	mb();
}

/* Full PCIe host bring-up, in the controller's own reset ...
 * dev/MEASURED-pcie-luna.c.md sec 13. */
static int __init luna_pcie_reset(void)
{
	u32 v;
	int i;

	/* 0. Hold the endpoint in reset for the whole bring-up (RTL9603CVD), or
	 *    just spend the bare timing budget where PERST is tied (RTL9602C). */
	luna_pcie_perst(true);
	mdelay(10);

	/* 1. PCIe pin mux where the chip has one, then MDIO reset: clear the
	 *    reset-hold bit and this port's reset bit, then strobe it. */
	if (chip->pinmux_bit)
		writel(readl(SOC_PINMUX) | chip->pinmux_bit, SOC_PINMUX);
	v = readl(SOC_PCI_MISC) & ~(PCI_MISC_MDIO_CLR | chip->misc_strobe);
	writel(v, SOC_PCI_MISC);
	mb();
	writel(v | chip->misc_strobe, SOC_PCI_MISC);
	mdelay(1);

	/* 2. Where the chip needs them, set the PHY + operational gates first;
	 *    then pulse ONLY this port's MAC enable (clear, then set) as the MAC
	 *    reset. On the RTL9602C the PHY-enable bit is left set throughout --
	 *    clearing it mid-bring-up resets the SerDes. */
	if (chip->ip_pre_or)
		writel(readl(SOC_IP_SEL) | chip->ip_pre_or, SOC_IP_SEL);
	v = readl(SOC_IP_SEL) & ~chip->ip_mac_bit;
	writel(v, SOC_IP_SEL);
	mb();
	writel(v | chip->ip_mac_bit, SOC_IP_SEL);
	mdelay(100);

	/* 3. Arm the LTSSM with the PHY held in reset, then release the PHY reset. */
	writel(0, pcie_hostext() + HOSTEXT_FN);
	writel(0x01, pcie_hostext() + HOSTEXT_LTSSM);	/* PHY in reset, LTSSM en */
	mb();
	writel(0x81, pcie_hostext() + HOSTEXT_LTSSM);	/* release PHY reset      */
	mdelay(50);

	/* 4. SerDes PHY tuning over MDIO -- required before POLLING ...
	 * dev/MEASURED-pcie-luna.c.md sec 14. */
	{
		const struct luna_pcie_phy *tbl = chip->phy;
		int n;

		if (pcie_phy_full && chip->phy == luna_pcie_phy_9602c_stock)
			tbl = luna_pcie_phy_9602c;
		for (n = 0; tbl[n].reg != 0xff; n++)
			;
		pr_info("pcie-luna: ePHY table = %s (%d pair(s))\n",
			tbl == luna_pcie_phy_9602c_stock ? "stock's five"
			: tbl == luna_pcie_phy_9602c ? "ours, full (pcie_phy_full)"
			: "per-chip", n);
		for (i = 0; tbl[i].reg != 0xff; i++) {
			writel(((u32)tbl[i].val << 16) |
			       ((u32)tbl[i].reg << 8) | 1,
			       pcie_hostext() + HOSTEXT_MDIO);
			mdelay(1);
		}
	}
	mdelay(20);

	/* 5. Let the endpoint out of reset, immediately before training. */
	luna_pcie_perst(false);

	/* 6. Poll for link-up (L0). */
	for (i = 0; i < (int)chip->link_polls; i++) {
		if ((readl(pcie_hostcfg() + HOSTCFG_LINK) & 0x1f) == LINK_UP_STATE)
			return 0;
		mdelay(10);
	}
	return -ETIMEDOUT;
}

/* -> the chip this kernel is running on, or NULL. Read from the DEVICE TREE
 * root compatible, never from a Kconfig symbol: one image serves two boards on
 * the rtl9607x subtarget, and the DT is the only thing that tells them apart. */
static const struct luna_pcie_chip *__init luna_pcie_which(void)
{
	static const struct luna_pcie_chip *const all[] __initconst = {
		&luna_pcie_9602c, &luna_pcie_9603cvd,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(all); i++)
		if (of_machine_is_compatible(all[i]->root_compat))
			return all[i];
	return NULL;
}

static int __init luna_pcie_init(void)
{
	unsigned int attempt;
	int ret = -ETIMEDOUT;

	chip = luna_pcie_which();
	if (!chip) {
		/* NOT an error and NOT a silence: a board whose root ...
		 * dev/MEASURED-pcie-luna.c.md sec 18. */
		pr_info("realtek-pcie: no PCIe host declared for this board -- not registering\n");
		return 0;
	}

	for (attempt = 0; attempt < chip->retries; attempt++) {
		ret = luna_pcie_reset();
		if (!ret)
			break;
		pr_info("realtek-pcie: %s link not trained (state=0x%x), retry %u/%u\n",
			chip->name,
			readl(pcie_hostcfg() + HOSTCFG_LINK) & 0x1f,
			attempt + 1, chip->retries);
	}
	if (ret) {
		pr_warn("realtek-pcie: %s link did not train after %u attempts (state=0x%x)\n",
			chip->name, chip->retries,
			readl(pcie_hostcfg() + HOSTCFG_LINK) & 0x1f);
		return ret;
	}

	/* Configuration-retry settle before any config/BAR access. */
	mdelay(100);

	/* Program the downstream endpoint's BARs + command register, ...
	 * dev/MEASURED-pcie-luna.c.md sec 15. */
	writel(chip->io_phys | 1u, pcie_devcfg() + 0x10);
	writel(chip->mem_phys | 4u, pcie_devcfg() + 0x18);
	writel(0x00180007, pcie_devcfg() + 0x04);
	writel(0x00100007, pcie_hostcfg() + HOSTCFG_CMD);
	writel(0x00100007, pcie_hostcfg() + HOSTCFG_CMD);
	writeb(readb(pcie_hostcfg() + HOSTCFG_PAYLOAD) & ~0xe0,
	       pcie_hostcfg() + HOSTCFG_PAYLOAD);
	writel(readl(pcie_hostcfg() + HOSTCFG_CFGCTL) | CFGCTL_FWD_EN,
	       pcie_hostcfg() + HOSTCFG_CFGCTL);
	mb();

	/* Wait for the endpoint's config space to answer before the bus scan so the
	 * device is not skipped. Function 0 is selected on the host window. */
	for (attempt = 0; attempt < 20; attempt++) {	/* up to ~1 s */
		u32 id;

		writel(0, pcie_hostext() + HOSTEXT_FN);
		mb();
		id = readl(pcie_devcfg());
		if ((id & 0xffff) == PCI_VENDOR_ID_REALTEK)
			break;
		mdelay(50);
	}

	pr_info("realtek-pcie: %s link up at gen%u, bridge 0x%08x, endpoint 0x%08x\n",
		chip->name, readb(pcie_hostcfg() + HOSTCFG_SPEED) & 0xf,
		readl(pcie_hostcfg()), readl(pcie_devcfg()));

	luna_pcie_mem.start = chip->mem_phys;
	luna_pcie_mem.end   = chip->mem_phys + chip->mem_size - 1;
	luna_pcie_io.start  = chip->io_phys;
	luna_pcie_io.end    = chip->io_phys + chip->io_size - 1;
	register_pci_controller(&luna_pcie_controller);
	return 0;
}

/*
 * late_initcall: the SoC clock/IP_SEL setup must run after core platform init;
 * the PCI core scan that follows enumerates the endpoint.
 */
late_initcall(luna_pcie_init);
