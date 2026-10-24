// SPDX-License-Identifier: GPL-2.0-only
/* ZX279125 timers, reconstructed from the vendor kernel's zx_timer_* code. */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/slab.h>

#define ZX_TIMER_MODE		0x04
#define ZX_TIMER_RELOAD		0x08
#define ZX_TIMER_ENABLE		0x0c
#define ZX_TIMER_LATCH		0x10
#define ZX_TIMER_BUSY		0x14
#define ZX_TIMER_COUNT		0x18

#define ZX_TIMER_PERIODIC	2

struct zx279125_timer {
	void __iomem *event_base;
	void __iomem *source_base;
	struct clk_bulk_data clocks[2];
	struct clock_event_device event;
	unsigned long event_rate;
};

static int zx279125_timer_stop(void __iomem *base)
{
	u32 val;

	writel(0, base + ZX_TIMER_ENABLE);
	return readl_poll_timeout_atomic(base + ZX_TIMER_BUSY, val,
					!(val & BIT(0)), 1, 10000);
}

static void zx279125_timer_start(void __iomem *base, u32 count, u32 mode)
{
	u32 val;

	writel(count, base + ZX_TIMER_RELOAD);
	writel(mode, base + ZX_TIMER_MODE);
	/* Preserve the stock pre-start latch sequence; bit meanings are unknown. */
	val = readl(base + ZX_TIMER_LATCH);
	writel(val ^ 0xf, base + ZX_TIMER_LATCH);
	writel(1, base + ZX_TIMER_ENABLE);
}

static int zx279125_timer_shutdown(struct clock_event_device *event)
{
	struct zx279125_timer *timer = container_of(event, struct zx279125_timer,
							 event);

	return zx279125_timer_stop(timer->event_base);
}

static int zx279125_timer_next_event(unsigned long delta,
				   struct clock_event_device *event)
{
	struct zx279125_timer *timer = container_of(event, struct zx279125_timer,
							 event);
	int ret;

	ret = zx279125_timer_stop(timer->event_base);
	if (ret)
		return ret;

	zx279125_timer_start(timer->event_base, delta, 0);
	return 0;
}

static int zx279125_timer_periodic(struct clock_event_device *event)
{
	struct zx279125_timer *timer = container_of(event, struct zx279125_timer,
							 event);
	int ret;

	ret = zx279125_timer_stop(timer->event_base);
	if (ret)
		return ret;

	zx279125_timer_start(timer->event_base,
			     DIV_ROUND_CLOSEST(timer->event_rate, HZ),
			     ZX_TIMER_PERIODIC);
	return 0;
}

static irqreturn_t zx279125_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *event = dev_id;

	/* The stock edge-triggered timer interrupt has no peripheral ack. */
	event->event_handler(event);
	return IRQ_HANDLED;
}

static int __init zx279125_timer_init(struct device_node *np)
{
	struct zx279125_timer *timer;
	unsigned long source_rate;
	int ret, index, irq;

	timer = kzalloc(sizeof(*timer), GFP_KERNEL);
	if (!timer)
		return -ENOMEM;

	index = of_property_match_string(np, "reg-names", "event");
	if (index < 0) {
		ret = index;
		goto err_free;
	}
	timer->event_base = of_iomap(np, index);
	if (!timer->event_base) {
		ret = -ENOMEM;
		goto err_free;
	}
	index = of_property_match_string(np, "reg-names", "source");
	if (index < 0) {
		ret = index;
		goto err_unmap_event;
	}
	timer->source_base = of_iomap(np, index);
	if (!timer->source_base) {
		ret = -ENOMEM;
		goto err_unmap_event;
	}

	timer->clocks[0].clk = of_clk_get_by_name(np, "event");
	if (IS_ERR(timer->clocks[0].clk)) {
		ret = PTR_ERR(timer->clocks[0].clk);
		goto err_unmap_source;
	}
	timer->clocks[1].clk = of_clk_get_by_name(np, "source");
	if (IS_ERR(timer->clocks[1].clk)) {
		ret = PTR_ERR(timer->clocks[1].clk);
		goto err_put_event_clk;
	}
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(timer->clocks), timer->clocks);
	if (ret)
		goto err_put_source_clk;

	timer->event_rate = clk_get_rate(timer->clocks[0].clk);
	source_rate = clk_get_rate(timer->clocks[1].clk);
	if (!timer->event_rate || !source_rate) {
		ret = -EINVAL;
		goto err_disable_clks;
	}
	ret = zx279125_timer_stop(timer->event_base);
	if (ret)
		goto err_disable_clks;
	ret = zx279125_timer_stop(timer->source_base);
	if (ret)
		goto err_disable_clks;

	irq = irq_of_parse_and_map(np, 0);
	if (!irq) {
		ret = -EINVAL;
		goto err_disable_clks;
	}

	timer->event.name = "zx279125-event";
	timer->event.rating = 200;
	timer->event.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT;
	timer->event.set_state_shutdown = zx279125_timer_shutdown;
	timer->event.set_state_oneshot = zx279125_timer_shutdown;
	timer->event.set_state_periodic = zx279125_timer_periodic;
	timer->event.tick_resume = zx279125_timer_shutdown;
	timer->event.set_next_event = zx279125_timer_next_event;
	timer->event.cpumask = cpumask_of(0);
	timer->event.irq = irq;

	ret = request_irq(irq, zx279125_timer_interrupt, IRQF_TIMER | IRQF_IRQPOLL,
			 "zx279125-timer", &timer->event);
	if (ret)
		goto err_dispose_irq;

	zx279125_timer_start(timer->source_base, U32_MAX, ZX_TIMER_PERIODIC);
	ret = clocksource_mmio_init(timer->source_base + ZX_TIMER_COUNT,
				   "zx279125-source", source_rate, 200, 32,
				   clocksource_mmio_readl_down);
	if (ret)
		goto err_stop_source;

	clockevents_config_and_register(&timer->event, timer->event_rate,
				       15, U32_MAX);
	return 0;

err_stop_source:
	zx279125_timer_stop(timer->source_base);
	free_irq(irq, &timer->event);
err_dispose_irq:
	irq_dispose_mapping(irq);
err_disable_clks:
	clk_bulk_disable_unprepare(ARRAY_SIZE(timer->clocks), timer->clocks);
err_put_source_clk:
	clk_put(timer->clocks[1].clk);
err_put_event_clk:
	clk_put(timer->clocks[0].clk);
err_unmap_source:
	iounmap(timer->source_base);
err_unmap_event:
	iounmap(timer->event_base);
err_free:
	kfree(timer);
	pr_err("%pOF: timer initialization failed: %d\n", np, ret);
	return ret;
}
TIMER_OF_DECLARE(zx279125_timer, "zte,zx279125-timer", zx279125_timer_init);
