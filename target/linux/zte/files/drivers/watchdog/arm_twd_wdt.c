// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cortex-A9 private (TWD) watchdog. The ZX279125 vendor kernel's zxwdt drives
 * the same block at PERIPHBASE + 0x620. The registers are banked per CPU, so
 * this driver assumes a uniprocessor kernel.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#define TWD_WDT_LOAD		0x00
#define TWD_WDT_COUNTER		0x04
#define TWD_WDT_CONTROL		0x08
#define TWD_WDT_INTSTAT		0x0c
#define TWD_WDT_RESETSTAT	0x10
#define TWD_WDT_DISABLE		0x14

#define TWD_WDT_ENABLE		BIT(0)
#define TWD_WDT_WDMODE		BIT(3)
#define TWD_WDT_PRESCALER	255

#define TWD_WDT_DEFAULT_TIMEOUT	30

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

struct twd_wdt {
	struct watchdog_device wdd;
	void __iomem *base;
	unsigned long rate;
};

static void twd_wdt_disable(struct twd_wdt *wdt)
{
	/* Watchdog mode can only be left through this two-word sequence. */
	writel(0x12345678, wdt->base + TWD_WDT_DISABLE);
	writel(0x87654321, wdt->base + TWD_WDT_DISABLE);
	writel(0, wdt->base + TWD_WDT_CONTROL);
}

static int twd_wdt_ping(struct watchdog_device *wdd)
{
	struct twd_wdt *wdt = watchdog_get_drvdata(wdd);

	/* In watchdog mode a load write also reloads the counter. */
	writel(wdd->timeout * wdt->rate, wdt->base + TWD_WDT_LOAD);
	return 0;
}

static int twd_wdt_start(struct watchdog_device *wdd)
{
	struct twd_wdt *wdt = watchdog_get_drvdata(wdd);

	twd_wdt_disable(wdt);
	writel(1, wdt->base + TWD_WDT_INTSTAT);
	writel(wdd->timeout * wdt->rate, wdt->base + TWD_WDT_LOAD);
	writel(TWD_WDT_PRESCALER << 8 | TWD_WDT_WDMODE | TWD_WDT_ENABLE,
	       wdt->base + TWD_WDT_CONTROL);
	return 0;
}

static int twd_wdt_stop(struct watchdog_device *wdd)
{
	twd_wdt_disable(watchdog_get_drvdata(wdd));
	return 0;
}

static int twd_wdt_set_timeout(struct watchdog_device *wdd, unsigned int timeout)
{
	wdd->timeout = timeout;
	if (watchdog_active(wdd))
		twd_wdt_ping(wdd);
	return 0;
}

static unsigned int twd_wdt_get_timeleft(struct watchdog_device *wdd)
{
	struct twd_wdt *wdt = watchdog_get_drvdata(wdd);

	return readl(wdt->base + TWD_WDT_COUNTER) / wdt->rate;
}

static const struct watchdog_info twd_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE |
		   WDIOF_CARDRESET,
	.identity = "ARM TWD watchdog",
};

static const struct watchdog_ops twd_wdt_ops = {
	.owner = THIS_MODULE,
	.start = twd_wdt_start,
	.stop = twd_wdt_stop,
	.ping = twd_wdt_ping,
	.set_timeout = twd_wdt_set_timeout,
	.get_timeleft = twd_wdt_get_timeleft,
};

static int twd_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct twd_wdt *wdt;
	struct clk *clk;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wdt->base))
		return PTR_ERR(wdt->base);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "no clock\n");
	wdt->rate = clk_get_rate(clk) / (TWD_WDT_PRESCALER + 1);
	if (!wdt->rate)
		return -EINVAL;

	wdt->wdd.info = &twd_wdt_info;
	wdt->wdd.ops = &twd_wdt_ops;
	wdt->wdd.parent = dev;
	wdt->wdd.min_timeout = 1;
	wdt->wdd.max_timeout = U32_MAX / wdt->rate;
	wdt->wdd.timeout = TWD_WDT_DEFAULT_TIMEOUT;
	watchdog_init_timeout(&wdt->wdd, 0, dev);
	watchdog_set_nowayout(&wdt->wdd, nowayout);
	/* A clean reboot passes through the stock loader, which never feeds it. */
	watchdog_stop_on_reboot(&wdt->wdd);
	watchdog_stop_on_unregister(&wdt->wdd);
	watchdog_set_drvdata(&wdt->wdd, wdt);

	/*
	 * The core keeps this flag across its own watchdog reset, but the
	 * ZX279125 turns that reset into a chip reset that clears it too.
	 */
	if (readl(wdt->base + TWD_WDT_RESETSTAT) & 1) {
		wdt->wdd.bootstatus = WDIOF_CARDRESET;
		writel(1, wdt->base + TWD_WDT_RESETSTAT);
	}
	if (readl(wdt->base + TWD_WDT_CONTROL) & TWD_WDT_WDMODE) {
		twd_wdt_ping(&wdt->wdd);
		set_bit(WDOG_HW_RUNNING, &wdt->wdd.status);
	}

	dev_info(dev, "%lu Hz, timeout %u s%s\n", wdt->rate, wdt->wdd.timeout,
		 wdt->wdd.bootstatus ? ", last reset by watchdog" : "");
	return devm_watchdog_register_device(dev, &wdt->wdd);
}

static const struct of_device_id twd_wdt_of_match[] = {
	{ .compatible = "arm,cortex-a9-twd-wdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, twd_wdt_of_match);

static struct platform_driver twd_wdt_driver = {
	.probe = twd_wdt_probe,
	.driver = {
		.name = "arm-twd-wdt",
		.of_match_table = twd_wdt_of_match,
	},
};
module_platform_driver(twd_wdt_driver);

MODULE_DESCRIPTION("ARM Cortex-A9 private watchdog driver");
MODULE_LICENSE("GPL");
