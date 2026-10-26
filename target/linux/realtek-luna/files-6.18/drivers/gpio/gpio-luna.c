// SPDX-License-Identifier: GPL-2.0-or-later
/* Realtek "Luna" SoC GPIO (RTL960x family).
 *
 * 64 lines in two 32-bit banks: ABCD (pins 0-31) and EFGH (pins 32-63), each
 * with a direction and a data register. A pad only reaches the GPIO block once
 * its bit in the SWCORE IO_GPIO_EN pair is set, so that happens on request;
 * the bootloader's pad routing is left alone for every pin nobody asks for.
 * The same sequence drives TX_DISABLE in luna_gpon.c. */

#include <linux/bits.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define LUNA_GPIO_DIR_ABCD	0x08
#define LUNA_GPIO_DATA_ABCD	0x0c
#define LUNA_GPIO_DIR_EFGH	0x24
#define LUNA_GPIO_DATA_EFGH	0x28
#define LUNA_GPIO_NGPIO		64

struct luna_gpio {
	struct gpio_chip gc;
	void __iomem *base;
	void __iomem *pad_en;	/* IO_GPIO_EN word 0, word 1 at +4 */
	spinlock_t lock;
};

static u32 luna_gpio_dir_reg(unsigned int pin)
{
	return pin < 32 ? LUNA_GPIO_DIR_ABCD : LUNA_GPIO_DIR_EFGH;
}

static u32 luna_gpio_data_reg(unsigned int pin)
{
	return pin < 32 ? LUNA_GPIO_DATA_ABCD : LUNA_GPIO_DATA_EFGH;
}

static void luna_gpio_rmw(void __iomem *reg, u32 bit, bool set)
{
	u32 v = ioread32(reg);

	iowrite32(set ? v | bit : v & ~bit, reg);
}

static int luna_gpio_request(struct gpio_chip *gc, unsigned int pin)
{
	struct luna_gpio *lg = gpiochip_get_data(gc);
	unsigned long flags;

	spin_lock_irqsave(&lg->lock, flags);
	luna_gpio_rmw(lg->pad_en + 4 * (pin / 32), BIT(pin % 32), true);
	spin_unlock_irqrestore(&lg->lock, flags);
	return 0;
}

static int luna_gpio_get_direction(struct gpio_chip *gc, unsigned int pin)
{
	struct luna_gpio *lg = gpiochip_get_data(gc);

	if (ioread32(lg->base + luna_gpio_dir_reg(pin)) & BIT(pin % 32))
		return GPIO_LINE_DIRECTION_OUT;
	return GPIO_LINE_DIRECTION_IN;
}

static int luna_gpio_get(struct gpio_chip *gc, unsigned int pin)
{
	struct luna_gpio *lg = gpiochip_get_data(gc);

	return !!(ioread32(lg->base + luna_gpio_data_reg(pin)) & BIT(pin % 32));
}

static int luna_gpio_set(struct gpio_chip *gc, unsigned int pin, int value)
{
	struct luna_gpio *lg = gpiochip_get_data(gc);
	unsigned long flags;

	spin_lock_irqsave(&lg->lock, flags);
	luna_gpio_rmw(lg->base + luna_gpio_data_reg(pin), BIT(pin % 32), value);
	spin_unlock_irqrestore(&lg->lock, flags);
	return 0;
}

static int luna_gpio_direction_input(struct gpio_chip *gc, unsigned int pin)
{
	struct luna_gpio *lg = gpiochip_get_data(gc);
	unsigned long flags;

	spin_lock_irqsave(&lg->lock, flags);
	luna_gpio_rmw(lg->base + luna_gpio_dir_reg(pin), BIT(pin % 32), false);
	spin_unlock_irqrestore(&lg->lock, flags);
	return 0;
}

/* Level first, then direction, so the pad never glitches to a stale level. */
static int luna_gpio_direction_output(struct gpio_chip *gc, unsigned int pin,
				      int value)
{
	struct luna_gpio *lg = gpiochip_get_data(gc);
	unsigned long flags;

	spin_lock_irqsave(&lg->lock, flags);
	luna_gpio_rmw(lg->base + luna_gpio_data_reg(pin), BIT(pin % 32), value);
	luna_gpio_rmw(lg->base + luna_gpio_dir_reg(pin), BIT(pin % 32), true);
	spin_unlock_irqrestore(&lg->lock, flags);
	return 0;
}

static int luna_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct luna_gpio *lg;
	struct resource *res;

	lg = devm_kzalloc(dev, sizeof(*lg), GFP_KERNEL);
	if (!lg)
		return -ENOMEM;

	lg->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(lg->base))
		return PTR_ERR(lg->base);

	/* IO_GPIO_EN sits inside the SWCORE window that luna_gpon.c already
	 * maps, so map it without claiming the region. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res)
		return -EINVAL;
	lg->pad_en = devm_ioremap(dev, res->start, resource_size(res));
	if (!lg->pad_en)
		return -ENOMEM;

	spin_lock_init(&lg->lock);

	lg->gc.label = dev_name(dev);
	lg->gc.parent = dev;
	lg->gc.owner = THIS_MODULE;
	lg->gc.base = -1;
	lg->gc.ngpio = LUNA_GPIO_NGPIO;
	lg->gc.request = luna_gpio_request;
	lg->gc.get_direction = luna_gpio_get_direction;
	lg->gc.direction_input = luna_gpio_direction_input;
	lg->gc.direction_output = luna_gpio_direction_output;
	lg->gc.get = luna_gpio_get;
	lg->gc.set = luna_gpio_set;

	return devm_gpiochip_add_data(dev, &lg->gc, lg);
}

static const struct of_device_id luna_gpio_of_match[] = {
	{ .compatible = "realtek,rtl960x-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, luna_gpio_of_match);

static struct platform_driver luna_gpio_driver = {
	.probe = luna_gpio_probe,
	.driver = {
		.name = "luna-gpio",
		.of_match_table = luna_gpio_of_match,
	},
};
module_platform_driver(luna_gpio_driver);

MODULE_DESCRIPTION("Realtek Luna (RTL960x) SoC GPIO");
MODULE_LICENSE("GPL");
