// SPDX-License-Identifier: GPL-2.0-or-later
/* Realtek "Luna" SoC watchdog timer (RTL960x family). The ...
 * dev/MEASURED-luna_wdt.c.md sec 1. */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/watchdog.h>

/* --- the register block, offsets from its base (0x18003260 on RTL9602C) --- */
#define LUNA_WDT_CNT			0x00
#define LUNA_WDT_CNT_KICK		BIT(31)
#define LUNA_WDT_INTR		0x04
#define LUNA_WDT_CTRL		0x08
#define LUNA_WDT_CTRL_EN		BIT(31)
#define LUNA_WDT_BLOCK_SIZE		0x0c

/*
 * WDT_CTRL fields.  Named for what the silicon's own headers call them, so a
 * reader can carry a name straight from a register dump to this file.
 */
#define LUNA_WDT_CLK_SC_SHIFT	29
#define LUNA_WDT_CLK_SC_MSK		0x3u
#define LUNA_WDT_CLK_SC_MAX		3u
#define LUNA_WDT_PH1_TO_SHIFT	22
#define LUNA_WDT_PH2_TO_SHIFT	15
#define LUNA_WDT_PH_TO_MSK		0x1fu
#define LUNA_WDT_RESET_MODE_MSK	0x3u

/* RESET_MODE 0 is the H/W FULL CHIP reset: it takes the CPU ...
 * dev/MEASURED-luna_wdt.c.md sec 2. */
#define LUNA_WDT_RESET_MODE_FULL_CHIP 0u

/* One tick is 2^(SCALE_SHIFT + WDT_CLK_SC) cycles of the LX bus clock. */
#define LUNA_WDT_SCALE_SHIFT		25

/* Each phase field is 5 bits and counts from ONE, so 0 means a single tick. */
#define LUNA_WDT_TICKS		32u

/* Phase 2 is kept at its shortest. It exists so a vendor ...
 * dev/MEASURED-luna_wdt.c.md sec 3. */
#define LUNA_WDT_PH2_TICKS		1u

#define LUNA_WDT_MIN_TIMEOUT		1u
#define LUNA_WDT_DEFAULT_TIMEOUT	30u

static unsigned int timeout;
module_param(timeout, uint, 0444);
MODULE_PARM_DESC(timeout,
		 "Watchdog timeout in seconds (default: device tree, else "
		 __MODULE_STRING(LUNA_WDT_DEFAULT_TIMEOUT) ")");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0444);
MODULE_PARM_DESC(nowayout,
		 "Watchdog cannot be stopped once started (default: "
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

/* struct luna_wdt_timing - the WDT_CTRL timing fields for one ...
 * dev/MEASURED-luna_wdt.c.md sec 4. */
struct luna_wdt_timing {
	u32 clk_sc;
	u32 ph1;
	u32 ph2;
};

/* Pure: the longest timeout this block can express on a @rate ...
 * dev/MEASURED-luna_wdt.c.md sec 5. */
static unsigned int luna_wdt_max_timeout(unsigned long rate)
{
	u64 cycles = (u64)LUNA_WDT_TICKS <<
		     (LUNA_WDT_SCALE_SHIFT + LUNA_WDT_CLK_SC_MAX);

	return rate ? (unsigned int)div_u64(cycles, rate) : 0;
}

/* Pure: turn a timeout in whole seconds into the WDT_CTRL ...
 * dev/MEASURED-luna_wdt.c.md sec 6. */
static int luna_wdt_calc_timing(unsigned long rate, unsigned int secs,
				   struct luna_wdt_timing *t)
{
	unsigned int sc;

	if (!rate || !secs)
		return -ERANGE;

	for (sc = 0; sc <= LUNA_WDT_CLK_SC_MAX; sc++) {
		unsigned int shift = LUNA_WDT_SCALE_SHIFT + sc;
		u64 cycles = (u64)secs * rate;
		u64 ticks = (cycles + (1ULL << shift) - 1) >> shift;

		if (ticks > LUNA_WDT_TICKS)
			continue;
		if (!ticks)
			ticks = 1;

		t->clk_sc = sc;
		t->ph1 = (u32)ticks - 1;
		t->ph2 = LUNA_WDT_PH2_TICKS - 1;
		return 0;
	}

	return -ERANGE;
}

/* Pure: those fields packed into the WDT_CTRL bit positions, enable excluded. */
static u32 luna_wdt_ctrl_timing(const struct luna_wdt_timing *t,
				   u32 reset_mode)
{
	return ((t->clk_sc & LUNA_WDT_CLK_SC_MSK) << LUNA_WDT_CLK_SC_SHIFT) |
	       ((t->ph1 & LUNA_WDT_PH_TO_MSK) << LUNA_WDT_PH1_TO_SHIFT) |
	       ((t->ph2 & LUNA_WDT_PH_TO_MSK) << LUNA_WDT_PH2_TO_SHIFT) |
	       (reset_mode & LUNA_WDT_RESET_MODE_MSK);
}

/* struct luna_wdt - one Luna watchdog instance @wdd: the ...
 * dev/MEASURED-luna_wdt.c.md sec 7. */
struct luna_wdt {
	struct watchdog_device	wdd;
	void __iomem		*base;
	struct clk		*clk;
	unsigned long		rate;
	spinlock_t		lock;		/* see kernel-doc above */
};

static inline struct luna_wdt *to_luna_wdt(struct watchdog_device *wdd)
{
	return container_of(wdd, struct luna_wdt, wdd);
}

/* Reload both phases. ★ READ-MODIFY-WRITE, and it is not a ...
 * dev/MEASURED-luna_wdt.c.md sec 8. */
static void luna_wdt_kick(struct luna_wdt *wdt)
{
	u32 cnt = readl(wdt->base + LUNA_WDT_CNT);

	writel(cnt | LUNA_WDT_CNT_KICK, wdt->base + LUNA_WDT_CNT);


}

static int luna_wdt_start(struct watchdog_device *wdd);
static int luna_wdt_stop(struct watchdog_device *wdd);

static int luna_wdt_ping(struct watchdog_device *wdd)
{
	/* ★★★ THE KICK ALONE DOES NOT RELOAD THIS COUNTER -- MEASURED ...
	 * dev/MEASURED-luna_wdt.c.md sec 9. */
	struct luna_wdt *wdt = to_luna_wdt(wdd);

	/* ⚠ THE RELOAD EXPERIMENTS ARE NOT KEPT. Both were tried on ...
	 * dev/MEASURED-luna_wdt.c.md sec 10. */
	luna_wdt_kick(wdt);

	return 0;
}

/* Program the window and arm. The kick comes LAST and is not ...
 * dev/MEASURED-luna_wdt.c.md sec 11. */
static int luna_wdt_start(struct watchdog_device *wdd)
{
	struct luna_wdt *wdt = to_luna_wdt(wdd);
	struct luna_wdt_timing t;
	unsigned long flags;
	u32 ctrl;
	int ret;

	ret = luna_wdt_calc_timing(wdt->rate, wdd->timeout, &t);
	if (ret)
		return ret;

	spin_lock_irqsave(&wdt->lock, flags);
	ctrl = readl(wdt->base + LUNA_WDT_CTRL);
	ctrl &= ~((LUNA_WDT_CLK_SC_MSK << LUNA_WDT_CLK_SC_SHIFT) |
		  (LUNA_WDT_PH_TO_MSK << LUNA_WDT_PH1_TO_SHIFT) |
		  (LUNA_WDT_PH_TO_MSK << LUNA_WDT_PH2_TO_SHIFT) |
		  LUNA_WDT_RESET_MODE_MSK);
	ctrl |= luna_wdt_ctrl_timing(&t, LUNA_WDT_RESET_MODE_FULL_CHIP);
	writel(ctrl | LUNA_WDT_CTRL_EN, wdt->base + LUNA_WDT_CTRL);
	luna_wdt_kick(wdt);
	spin_unlock_irqrestore(&wdt->lock, flags);

	set_bit(WDOG_HW_RUNNING, &wdd->status);

	return 0;
}

static int luna_wdt_stop(struct watchdog_device *wdd)
{
	struct luna_wdt *wdt = to_luna_wdt(wdd);
	unsigned long flags;
	u32 ctrl;

	spin_lock_irqsave(&wdt->lock, flags);
	ctrl = readl(wdt->base + LUNA_WDT_CTRL);
	writel(ctrl & ~LUNA_WDT_CTRL_EN, wdt->base + LUNA_WDT_CTRL);
	spin_unlock_irqrestore(&wdt->lock, flags);

	clear_bit(WDOG_HW_RUNNING, &wdd->status);

	return 0;
}

static int luna_wdt_set_timeout(struct watchdog_device *wdd,
				   unsigned int new_timeout)
{
	struct luna_wdt *wdt = to_luna_wdt(wdd);
	struct luna_wdt_timing t;
	int ret;

	/* Refuse before recording it: a timeout the block cannot hold must not
	 * end up in wdd->timeout, where the core would derive a feed interval
	 * from a window the hardware was never given. */
	ret = luna_wdt_calc_timing(wdt->rate, new_timeout, &t);
	if (ret)
		return ret;

	wdd->timeout = new_timeout;

	/* Reprogram only a watchdog that is already counting. ...
	 * dev/MEASURED-luna_wdt.c.md sec 18. */
	if (watchdog_hw_running(wdd))
		return luna_wdt_start(wdd);

	return 0;
}

/* No .get_timeleft. WDT_CNT is the kick register; whether its ...
 * dev/MEASURED-luna_wdt.c.md sec 12. */
static const struct watchdog_info luna_wdt_info = {
	.identity	= "Realtek Luna WDT",
	/* No WDIOF_CARDRESET: see the file header. Nothing on this ...
	 * dev/MEASURED-luna_wdt.c.md sec 19. */
	.options	= WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops luna_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= luna_wdt_start,
	.stop		= luna_wdt_stop,
	.ping		= luna_wdt_ping,
	.set_timeout	= luna_wdt_set_timeout,
};

static int luna_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct luna_wdt *wdt;
	struct resource *res;
	resource_size_t phys;
	unsigned int max_timeout;
	bool adopted;
	u32 ctrl;
	int ret;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	spin_lock_init(&wdt->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(dev, -EINVAL,
				     "missing the watchdog register window\n");
	if (resource_size(res) < LUNA_WDT_BLOCK_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "window at %pa is too small, need %#x\n",
				     &res->start, LUNA_WDT_BLOCK_SIZE);
	phys = res->start;

	/* devm_ioremap(), not devm_ioremap_resource(): this block is ...
	 * dev/MEASURED-luna_wdt.c.md sec 13. */
	wdt->base = devm_ioremap(dev, res->start, resource_size(res));

	if (!wdt->base)
		return -ENOMEM;

	wdt->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(wdt->clk))
		return dev_err_probe(dev, PTR_ERR(wdt->clk), "no input clock\n");

	wdt->rate = clk_get_rate(wdt->clk);
	if (!wdt->rate)
		return dev_err_probe(dev, -EINVAL, "input clock reports 0 Hz\n");

	max_timeout = luna_wdt_max_timeout(wdt->rate);
	if (max_timeout < LUNA_WDT_MIN_TIMEOUT)
		return dev_err_probe(dev, -EINVAL,
				     "a %lu Hz input clock cannot reach a 1 s window\n",
				     wdt->rate);

	/* The phase-1 timeout has its own interrupt line on this SoC ...
	 * dev/MEASURED-luna_wdt.c.md sec 14. */

	wdt->wdd.info = &luna_wdt_info;
	wdt->wdd.ops = &luna_wdt_ops;
	wdt->wdd.parent = dev;
	wdt->wdd.min_timeout = LUNA_WDT_MIN_TIMEOUT;
	wdt->wdd.max_timeout = max_timeout;
	wdt->wdd.max_hw_heartbeat_ms = max_timeout * 1000;
	wdt->wdd.timeout = min(LUNA_WDT_DEFAULT_TIMEOUT, max_timeout);


	/* ★ Was it already counting when Linux took over? A boot ...
	 * dev/MEASURED-luna_wdt.c.md sec 15. */
	ctrl = readl(wdt->base + LUNA_WDT_CTRL);
	adopted = !!(ctrl & LUNA_WDT_CTRL_EN);

	ret = watchdog_init_timeout(&wdt->wdd, timeout, dev);
	if (ret)
		dev_warn(dev, "timeout out of range, keeping %u s\n",
			 wdt->wdd.timeout);

	watchdog_set_nowayout(&wdt->wdd, nowayout);

	/* Stop it across an orderly reboot. The alternative -- ...
	 * dev/MEASURED-luna_wdt.c.md sec 16. */
	watchdog_stop_on_reboot(&wdt->wdd);

	/* Armed here and flagged running: the core feeds it until userspace opens
	 * /dev/watchdog, so a hard lockup from probe on resets the chip. An
	 * inherited window is reprogrammed to ours by the same start. */
	ret = luna_wdt_start(&wdt->wdd);
	if (ret)
		return dev_err_probe(dev, ret, "cannot arm the window\n");
	set_bit(WDOG_HW_RUNNING, &wdt->wdd.status);

	platform_set_drvdata(pdev, wdt);

	ret = devm_watchdog_register_device(dev, &wdt->wdd);
	if (ret)
		return ret;

	dev_info(dev,
		 "Luna WDT at %pa on a %lu Hz clock: %u s timeout (max %u s), full-chip reset, %s at kernel entry, armed%s\n",
		 &phys, wdt->rate, wdt->wdd.timeout, wdt->wdd.max_timeout,
		 adopted ? "ALREADY RUNNING (reprogrammed to our window)"
			 : "stopped",
		 nowayout ? ", nowayout" : "");

	return 0;
}

static const struct of_device_id luna_wdt_of_match[] = {
	{ .compatible = "realtek,rtl960x-wdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, luna_wdt_of_match);

static struct platform_driver luna_wdt_driver = {
	.probe	= luna_wdt_probe,
	.driver	= {
		.name		= "rtl960x-wdt",
		.of_match_table	= luna_wdt_of_match,
	},
};
module_platform_driver(luna_wdt_driver);

MODULE_DESCRIPTION("Realtek Luna (RTL960x) SoC watchdog");
MODULE_LICENSE("GPL");
