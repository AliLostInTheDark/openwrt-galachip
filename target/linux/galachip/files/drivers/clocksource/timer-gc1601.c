// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2009 Faraday Technology
 * Copyright (C) 2009 Po-Yu Chuang <ratbert@faraday-tech.com>
 *
 * The GC1601 timer is a Faraday FTTMR010, but the mainline timer-fttmr010
 * driver does not work on it: that driver never clears the interrupt
 * status, and this timer raises its interrupt again only after a clear
 * (edge or level). Tried on the XZ000-G7: one clockevent interrupt, then
 * the boot stalls at the first sleep.
 */
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>
#include <linux/slab.h>

#include <asm/delay.h>

#define GC1601_T1_COUNT	0x00
#define GC1601_T1_LOAD	0x04
#define GC1601_T1_MATCH1	0x08
#define GC1601_T1_MATCH2	0x0c
#define GC1601_T2_COUNT	0x10
#define GC1601_T2_LOAD	0x14
#define GC1601_T2_MATCH1	0x18
#define GC1601_T2_MATCH2	0x1c
#define GC1601_TMR_CTRL	0x30
#define GC1601_TMR_STATUS	0x34
#define GC1601_TMR_MASK	0x38
#define GC1601_T1_ENABLE	BIT(0)
#define GC1601_T1_OVERFLOW BIT(2)
#define GC1601_T2_ENABLE	BIT(3)
#define GC1601_T1_IRQS	GENMASK(2, 0)
#define GC1601_ALL_IRQS	GENMASK(8, 0)

struct gc1601_timer {
	void __iomem *base;
	unsigned long rate;
	struct clock_event_device event;
	struct delay_timer delay;
};

static struct gc1601_timer *gc1601_timer;

static struct gc1601_timer *gc1601_to_timer(struct clock_event_device *evt)
{
	return container_of(evt, struct gc1601_timer, event);
}

static int gc1601_timer_shutdown(struct clock_event_device *evt)
{
	struct gc1601_timer *t = gc1601_to_timer(evt);
	u32 ctrl = readl(t->base + GC1601_TMR_CTRL);

	ctrl &= ~(GC1601_T1_ENABLE | GC1601_T1_OVERFLOW);
	writel(ctrl, t->base + GC1601_TMR_CTRL);
	writel(GC1601_T1_IRQS, t->base + GC1601_TMR_STATUS);
	return 0;
}

static void gc1601_timer_start(struct gc1601_timer *t)
{
	u32 ctrl = readl(t->base + GC1601_TMR_CTRL);

	ctrl |= GC1601_T1_ENABLE | GC1601_T1_OVERFLOW;
	writel(ctrl, t->base + GC1601_TMR_CTRL);
}

static int gc1601_timer_next(unsigned long cycles, struct clock_event_device *evt)
{
	struct gc1601_timer *t = gc1601_to_timer(evt);

	gc1601_timer_shutdown(evt);
	writel(0, t->base + GC1601_T1_LOAD);
	writel(cycles, t->base + GC1601_T1_COUNT);
	gc1601_timer_start(t);
	return 0;
}

static int gc1601_timer_periodic(struct clock_event_device *evt)
{
	struct gc1601_timer *t = gc1601_to_timer(evt);
	u32 cycles = DIV_ROUND_CLOSEST(t->rate, HZ);

	gc1601_timer_shutdown(evt);
	writel(cycles, t->base + GC1601_T1_LOAD);
	writel(cycles, t->base + GC1601_T1_COUNT);
	gc1601_timer_start(t);
	return 0;
}

static irqreturn_t gc1601_timer_interrupt(int irq, void *dev_id)
{
	struct gc1601_timer *t = dev_id;
	u32 status = readl(t->base + GC1601_TMR_STATUS) & GC1601_T1_IRQS;

	if (!status)
		return IRQ_NONE;
	writel(status, t->base + GC1601_TMR_STATUS);
	if (clockevent_state_oneshot(&t->event))
		gc1601_timer_shutdown(&t->event);
	t->event.event_handler(&t->event);
	return IRQ_HANDLED;
}

static unsigned long gc1601_read_timer(void)
{
	return ~readl(gc1601_timer->base + GC1601_T2_COUNT);
}

static u64 notrace gc1601_read_sched_clock(void)
{
	return gc1601_read_timer();
}

static int __init gc1601_timer_init(struct device_node *np)
{
	struct gc1601_timer *t;
	struct clk *clk;
	int irq, ret;

	if (gc1601_timer)
		return -EINVAL;
	clk = of_clk_get_by_name(np, "PCLK");
	if (IS_ERR(clk))
		return PTR_ERR(clk);
	ret = clk_prepare_enable(clk);
	if (ret)
		goto put_clock;
	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t) {
		ret = -ENOMEM;
		goto disable_clock;
	}
	t->rate = clk_get_rate(clk);
	if (!t->rate) {
		ret = -EINVAL;
		goto free_timer;
	}
	t->base = of_iomap(np, 0);
	if (!t->base) {
		ret = -ENOMEM;
		goto free_timer;
	}
	irq = irq_of_parse_and_map(np, 0);
	if (!irq) {
		ret = -EINVAL;
		goto unmap;
	}

	writel(0, t->base + GC1601_TMR_CTRL);
	writel(GC1601_ALL_IRQS, t->base + GC1601_TMR_MASK);
	writel(GC1601_ALL_IRQS, t->base + GC1601_TMR_STATUS);
	writel(0, t->base + GC1601_T1_MATCH1);
	writel(0, t->base + GC1601_T1_MATCH2);
	writel(0, t->base + GC1601_T2_MATCH1);
	writel(0, t->base + GC1601_T2_MATCH2);
	writel(~0U, t->base + GC1601_T2_LOAD);
	writel(~0U, t->base + GC1601_T2_COUNT);
	writel(GC1601_T2_ENABLE, t->base + GC1601_TMR_CTRL);

	t->event.name = "gc1601-timer1";
	t->event.rating = 300;
	t->event.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT;
	t->event.set_next_event = gc1601_timer_next;
	t->event.set_state_shutdown = gc1601_timer_shutdown;
	t->event.set_state_oneshot = gc1601_timer_shutdown;
	t->event.set_state_periodic = gc1601_timer_periodic;
	t->event.tick_resume = gc1601_timer_shutdown;
	t->event.cpumask = cpumask_of(0);
	t->event.irq = irq;
	ret = request_irq(irq, gc1601_timer_interrupt, IRQF_TIMER, t->event.name, t);
	if (ret)
		goto dispose_irq;
	ret = clocksource_mmio_init(t->base + GC1601_T2_COUNT, "gc1601-timer2",
				    t->rate, 300, 32, clocksource_mmio_readl_down);
	if (ret)
		goto free_irq;

	gc1601_timer = t;
	sched_clock_register(gc1601_read_sched_clock, 32, t->rate);
	t->delay.read_current_timer = gc1601_read_timer;
	t->delay.freq = t->rate;
	register_current_timer_delay(&t->delay);
	/* GC1601 masks on one and acknowledges pending sources by writing one. */
	writel(GC1601_ALL_IRQS & ~GC1601_T1_IRQS, t->base + GC1601_TMR_MASK);
	clockevents_config_and_register(&t->event, t->rate, 0xf, U32_MAX);
	return 0;

free_irq:
	free_irq(irq, t);
dispose_irq:
	writel(0, t->base + GC1601_TMR_CTRL);
	irq_dispose_mapping(irq);
unmap:
	iounmap(t->base);
free_timer:
	kfree(t);
disable_clock:
	clk_disable_unprepare(clk);
put_clock:
	clk_put(clk);
	return ret;
}
TIMER_OF_DECLARE(gc1601, "galachip,gc1601-timer", gc1601_timer_init);
