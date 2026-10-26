// SPDX-License-Identifier: GPL-2.0-or-later
/* Clock declarations and peripheral gates from the ZX279125 stock kernel. */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <dt-bindings/clock/zte,zx279125-clock.h>

struct zx279125_gate {
	struct clk_hw hw;
	void __iomem *reg;
};

static DEFINE_SPINLOCK(zx279125_clk_lock);

static int zx279125_gate_enable(struct clk_hw *hw)
{
	struct zx279125_gate *gate = container_of(hw, struct zx279125_gate, hw);
	unsigned long flags;

	spin_lock_irqsave(&zx279125_clk_lock, flags);
	writel(readl(gate->reg) | 3, gate->reg);
	spin_unlock_irqrestore(&zx279125_clk_lock, flags);
	return 0;
}

static void zx279125_gate_disable(struct clk_hw *hw)
{
	struct zx279125_gate *gate = container_of(hw, struct zx279125_gate, hw);
	unsigned long flags;

	spin_lock_irqsave(&zx279125_clk_lock, flags);
	writel(readl(gate->reg) & ~3, gate->reg);
	spin_unlock_irqrestore(&zx279125_clk_lock, flags);
}

static int zx279125_gate_is_enabled(struct clk_hw *hw)
{
	struct zx279125_gate *gate = container_of(hw, struct zx279125_gate, hw);

	return (readl(gate->reg) & 3) == 3;
}

static const struct clk_ops zx279125_gate_ops = {
	.enable = zx279125_gate_enable,
	.disable = zx279125_gate_disable,
	.is_enabled = zx279125_gate_is_enabled,
};

static struct clk_hw *__init
zx279125_register_gate(const char *name, const char *parent, void __iomem *reg)
{
	struct clk_init_data init = {
		.name = name,
		.ops = &zx279125_gate_ops,
		.parent_names = &parent,
		.num_parents = 1,
		.flags = CLK_IGNORE_UNUSED,
	};
	struct zx279125_gate *gate;
	int ret;

	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return ERR_PTR(-ENOMEM);

	gate->reg = reg;
	gate->hw.init = &init;
	ret = clk_hw_register(NULL, &gate->hw);
	if (ret) {
		kfree(gate);
		return ERR_PTR(ret);
	}
	return &gate->hw;
}

static void __init zx279125_clk_init(struct device_node *np)
{
	static const struct {
		unsigned int id;
		const char *name;
		unsigned long rate;
	} fixed[] = {
		{ ZX279125_CLK_CPU, "zx279125-cpu", 600000000 },
		{ ZX279125_CLK_DDR, "zx279125-ddr", 266400000 },
		{ ZX279125_CLK_AXI, "zx279125-axi", 166500000 },
		{ ZX279125_CLK_APB, "zx279125-apb", 83250000 },
		{ ZX279125_CLK_SPIFC, "zx279125-spifc", 100000000 },
		{ ZX279125_CLK_MDIO_REF, "zx279125-mdio-ref", 2500000 },
	};
	static const struct {
		unsigned int id;
		const char *name;
		const char *parent;
		u32 offset;
		bool network;
	} gates[] = {
		{ ZX279125_CLK_TIMER0, "zx279125-timer0", "zx279125-osc", 0x10 },
		{ ZX279125_CLK_TIMER1, "zx279125-timer1", "zx279125-osc", 0x14 },
		{ ZX279125_CLK_TIMER2, "zx279125-timer2", "zx279125-osc", 0x18 },
		{ ZX279125_CLK_UART0, "zx279125-uart0", "zx279125-osc", 0x20 },
		{ ZX279125_CLK_UART1, "zx279125-uart1", "zx279125-osc", 0x24 },
		{ ZX279125_CLK_MDIO, "zx279125-mdio", "zx279125-mdio-ref", 0x10, true },
		{ ZX279125_CLK_I2C, "zx279125-i2c", "zx279125-spifc", 0x14, true },
	};
	struct clk_hw_onecell_data *data;
	void __iomem *peripheral;
	void __iomem *network;
	const char *osc;
	unsigned int i;
	int ret;

	peripheral = of_iomap(np, 1);
	if (!peripheral)
		return;
	network = of_iomap(np, 2);
	if (!network) {
		iounmap(peripheral);
		return;
	}

	data = kzalloc(struct_size(data, hws, ZX279125_CLK_COUNT), GFP_KERNEL);
	if (!data) {
		iounmap(network);
		iounmap(peripheral);
		return;
	}

	data->num = ZX279125_CLK_COUNT;
	for (i = 0; i < data->num; i++)
		data->hws[i] = ERR_PTR(-ENOENT);

	osc = of_clk_get_parent_name(np, 0);
	data->hws[ZX279125_CLK_OSC] =
		clk_hw_register_fixed_factor(NULL, "zx279125-osc", osc, 0, 1, 1);
	data->hws[ZX279125_CLK_TIMER] =
		clk_hw_register_fixed_factor(NULL, "zx279125-timer",
					     "zx279125-osc", 0, 1, 1);

	/* Preserve stock nominal rates without reprogramming its PLLs. */
	for (i = 0; i < ARRAY_SIZE(fixed); i++)
		data->hws[fixed[i].id] = clk_hw_register_fixed_rate(NULL,
				fixed[i].name, NULL, 0, fixed[i].rate);

	for (i = 0; i < ARRAY_SIZE(gates); i++)
		data->hws[gates[i].id] = zx279125_register_gate(gates[i].name,
				gates[i].parent, (gates[i].network ? network : peripheral) +
				gates[i].offset);

	for (i = 0; i < data->num; i++) {
		if (i == ZX279125_CLK_GMAC)
			continue;
		if (IS_ERR(data->hws[i])) {
			ret = PTR_ERR(data->hws[i]);
			goto unregister;
		}
	}

	ret = of_clk_add_hw_provider(np, of_clk_hw_onecell_get, data);
	if (!ret)
		return;

unregister:
	for (i = 0; i < ARRAY_SIZE(gates); i++) {
		struct clk_hw *hw = data->hws[gates[i].id];

		if (!IS_ERR(hw)) {
			clk_hw_unregister(hw);
			kfree(container_of(hw, struct zx279125_gate, hw));
		}
	}
	for (i = 0; i < ARRAY_SIZE(fixed); i++)
		if (!IS_ERR(data->hws[fixed[i].id]))
			clk_hw_unregister_fixed_rate(data->hws[fixed[i].id]);
	if (!IS_ERR(data->hws[ZX279125_CLK_TIMER]))
		clk_hw_unregister_fixed_factor(data->hws[ZX279125_CLK_TIMER]);
	if (!IS_ERR(data->hws[ZX279125_CLK_OSC]))
		clk_hw_unregister_fixed_factor(data->hws[ZX279125_CLK_OSC]);
	kfree(data);
	iounmap(network);
	iounmap(peripheral);
	pr_err("ZX279125 clock registration failed: %d\n", ret);
}

CLK_OF_DECLARE(zx279125_crm, "zte,zx279125-crm", zx279125_clk_init);
