// SPDX-License-Identifier: GPL-2.0-only
/* ZX279125 UART register layout recovered from the vendor zx_uart driver. */

#include <linux/clk.h>
#include <linux/console.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/tty_flip.h>

#define ZX_UART_DATA		0x04
#define ZX_UART_ERRORS		0x10
#define ZX_UART_FLAGS		0x14
#define ZX_UART_IBRD		0x24
#define ZX_UART_FBRD		0x28
#define ZX_UART_LINE		0x30
#define ZX_UART_CONTROL		0x34
#define ZX_UART_FIFO		0x38
#define ZX_UART_MASK		0x40
#define ZX_UART_STATUS		0x48
#define ZX_UART_CLEAR		0x4c

#define ZX_UART_RX_EMPTY	BIT(4)
#define ZX_UART_TX_FULL		BIT(5)
#define ZX_UART_TX_EMPTY	BIT(7)
#define ZX_UART_FE		BIT(8)
#define ZX_UART_PE		BIT(9)
#define ZX_UART_BE		BIT(10)
#define ZX_UART_OE		BIT(11)
#define ZX_UART_RX_DUMMY	BIT(12)
#define ZX_UART_IRQ_RX		BIT(4)
#define ZX_UART_IRQ_TX		BIT(5)
#define ZX_UART_IRQ_TIMEOUT	BIT(6)
#define ZX_UART_IRQ_ERROR	BIT(10)
#define ZX_UART_FIFO_ENABLE	BIT(4)
#define ZX_UART_LINE_8BIT	GENMASK(6, 5)
#define ZX_UART_ENABLE		0x301
#define ZX_UART_PORTS		2

static struct uart_port *zx279125_ports[ZX_UART_PORTS];
static struct uart_driver zx279125_uart_driver;

static void zx279125_stop_tx(struct uart_port *port)
{
	writel(readl(port->membase + ZX_UART_MASK) & ~ZX_UART_IRQ_TX,
	       port->membase + ZX_UART_MASK);
}

static void zx279125_tx_chars(struct uart_port *port)
{
	u8 ch;

	uart_port_tx_limited(port, ch, 512,
		!(readl(port->membase + ZX_UART_FLAGS) & ZX_UART_TX_FULL),
		writel(ch, port->membase + ZX_UART_DATA), ({}));
}

static void zx279125_start_tx(struct uart_port *port)
{
	writel(readl(port->membase + ZX_UART_MASK) | ZX_UART_IRQ_TX,
	       port->membase + ZX_UART_MASK);
	zx279125_tx_chars(port);
}

static void zx279125_stop_rx(struct uart_port *port)
{
	writel(readl(port->membase + ZX_UART_MASK) &
	       ~(ZX_UART_IRQ_RX | ZX_UART_IRQ_TIMEOUT | ZX_UART_IRQ_ERROR),
	       port->membase + ZX_UART_MASK);
}

static unsigned int zx279125_tx_empty(struct uart_port *port)
{
	return readl(port->membase + ZX_UART_FLAGS) & ZX_UART_TX_EMPTY ?
		TIOCSER_TEMT : 0;
}

static void zx279125_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int zx279125_get_mctrl(struct uart_port *port)
{
	return TIOCM_CAR | TIOCM_CTS | TIOCM_DSR;
}

static void zx279125_rx_chars(struct uart_port *port)
{
	unsigned int count = 512;

	while (count-- && !(readl(port->membase + ZX_UART_FLAGS) & ZX_UART_RX_EMPTY)) {
		u32 status = readl(port->membase + ZX_UART_DATA) | ZX_UART_RX_DUMMY;
		u8 ch = status;
		char flag = TTY_NORMAL;

		port->icount.rx++;
		if (status & ZX_UART_BE) {
			port->icount.brk++;
			if (uart_handle_break(port))
				continue;
		} else if (status & ZX_UART_PE) {
			port->icount.parity++;
		} else if (status & ZX_UART_FE) {
			port->icount.frame++;
		}
		if (status & ZX_UART_OE)
			port->icount.overrun++;

		status &= port->read_status_mask;
		if (status & ZX_UART_BE)
			flag = TTY_BREAK;
		else if (status & ZX_UART_PE)
			flag = TTY_PARITY;
		else if (status & ZX_UART_FE)
			flag = TTY_FRAME;

		if (!uart_handle_sysrq_char(port, ch))
			uart_insert_char(port, status, ZX_UART_OE, ch, flag);
	}
	tty_flip_buffer_push(&port->state->port);
}

static irqreturn_t zx279125_uart_irq(int irq, void *data)
{
	struct uart_port *port = data;
	u32 status = readl(port->membase + ZX_UART_STATUS);

	if (!(status & (ZX_UART_IRQ_RX | ZX_UART_IRQ_TX |
		       ZX_UART_IRQ_TIMEOUT | ZX_UART_IRQ_ERROR)))
		return IRQ_NONE;

	uart_port_lock(port);
	if (status & ZX_UART_IRQ_ERROR) {
		readl(port->membase + ZX_UART_DATA);
		writel(0xf, port->membase + ZX_UART_ERRORS);
		writel(ZX_UART_IRQ_ERROR, port->membase + ZX_UART_CLEAR);
		port->icount.overrun++;
	}
	if (status & ZX_UART_IRQ_TIMEOUT)
		writel(ZX_UART_IRQ_TIMEOUT, port->membase + ZX_UART_CLEAR);
	if (status & (ZX_UART_IRQ_RX | ZX_UART_IRQ_TIMEOUT))
		zx279125_rx_chars(port);
	if (status & ZX_UART_IRQ_TX)
		zx279125_tx_chars(port);
	uart_port_unlock(port);
	return IRQ_HANDLED;
}

static int zx279125_startup(struct uart_port *port)
{
	int ret;

	writel(0, port->membase + ZX_UART_MASK);
	ret = request_irq(port->irq, zx279125_uart_irq, 0, "zx279125-uart", port);
	if (ret)
		return ret;

	writel(0xff00, port->membase + ZX_UART_ERRORS);
	writel(0x7f, port->membase + ZX_UART_CLEAR);
	writel((readl(port->membase + ZX_UART_FIFO) & ~0x3f) | 0x12,
	       port->membase + ZX_UART_FIFO);
	writel(readl(port->membase + ZX_UART_LINE) | ZX_UART_FIFO_ENABLE,
	       port->membase + ZX_UART_LINE);
	writel(ZX_UART_ENABLE, port->membase + ZX_UART_CONTROL);
	writel(ZX_UART_IRQ_RX | ZX_UART_IRQ_TIMEOUT | ZX_UART_IRQ_ERROR,
	       port->membase + ZX_UART_MASK);
	return 0;
}

static void zx279125_shutdown(struct uart_port *port)
{
	writel(0, port->membase + ZX_UART_MASK);
	free_irq(port->irq, port);
}

static void zx279125_set_termios(struct uart_port *port, struct ktermios *termios,
				const struct ktermios *old)
{
	unsigned long flags;
	unsigned int baud, divisor;
	u32 val, line, control;
	int ret;

	/* Initial bring-up supports the stock console's 8N1 format only. */
	termios->c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD | CMSPAR | CRTSCTS);
	termios->c_cflag |= CS8;
	baud = uart_get_baud_rate(port, termios, old, 1, port->uartclk / 16);
	divisor = (port->uartclk * 4) / baud;

	uart_port_lock_irqsave(port, &flags);
	ret = readl_poll_timeout_atomic(port->membase + ZX_UART_FLAGS, val,
				       val & (ZX_UART_RX_EMPTY | ZX_UART_TX_EMPTY),
				       1, 10000);
	if (ret)
		goto unlock;

	line = (readl(port->membase + ZX_UART_LINE) & ~0xea) |
	       ZX_UART_LINE_8BIT | ZX_UART_FIFO_ENABLE;
	control = (readl(port->membase + ZX_UART_CONTROL) & ~BIT(15)) |
		  ZX_UART_ENABLE;
	writel(divisor >> 6, port->membase + ZX_UART_IBRD);
	writel(divisor & 0x3f, port->membase + ZX_UART_FBRD);
	writel(line, port->membase + ZX_UART_LINE);
	writel(0, port->membase + ZX_UART_CONTROL);
	writel(line & ~ZX_UART_FIFO_ENABLE, port->membase + ZX_UART_LINE);
	writel(control, port->membase + ZX_UART_CONTROL);
	writel(line, port->membase + ZX_UART_LINE);

	port->read_status_mask = ZX_UART_OE | ZX_UART_RX_DUMMY;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= ZX_UART_FE | ZX_UART_PE;
	if (termios->c_iflag & (IGNBRK | BRKINT | PARMRK))
		port->read_status_mask |= ZX_UART_BE;
	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask |= ZX_UART_FE | ZX_UART_PE;
	if (termios->c_iflag & IGNBRK) {
		port->ignore_status_mask |= ZX_UART_BE;
		if (termios->c_iflag & IGNPAR)
			port->ignore_status_mask |= ZX_UART_OE;
	}
	if (!(termios->c_cflag & CREAD))
		port->ignore_status_mask |= ZX_UART_RX_DUMMY;
	uart_update_timeout(port, termios->c_cflag, baud);
	if (tty_termios_baud_rate(termios))
		tty_termios_encode_baud_rate(termios, baud, baud);
unlock:
	uart_port_unlock_irqrestore(port, flags);
	if (ret)
		dev_warn_ratelimited(port->dev, "UART busy; baud change skipped\n");
}

static const char *zx279125_type(struct uart_port *port)
{
	return "zx279125-uart";
}

static int zx279125_request_port(struct uart_port *port)
{
	return 0;
}

static void zx279125_release_port(struct uart_port *port)
{
}

static void zx279125_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_GENERIC;
}

static const struct uart_ops zx279125_uart_ops = {
	.tx_empty = zx279125_tx_empty,
	.set_mctrl = zx279125_set_mctrl,
	.get_mctrl = zx279125_get_mctrl,
	.stop_tx = zx279125_stop_tx,
	.start_tx = zx279125_start_tx,
	.stop_rx = zx279125_stop_rx,
	.startup = zx279125_startup,
	.shutdown = zx279125_shutdown,
	.set_termios = zx279125_set_termios,
	.type = zx279125_type,
	.request_port = zx279125_request_port,
	.release_port = zx279125_release_port,
	.config_port = zx279125_config_port,
};

#ifdef CONFIG_SERIAL_ZX279125_CONSOLE
static void zx279125_putchar(struct uart_port *port, unsigned char ch)
{
	u32 val;

	if (!readl_poll_timeout_atomic(port->membase + ZX_UART_FLAGS, val,
				      !(val & ZX_UART_TX_FULL), 1, 10000))
		writel(ch, port->membase + ZX_UART_DATA);
}

static void zx279125_console_write(struct console *co, const char *s,
				   unsigned int count)
{
	struct uart_port *port = zx279125_ports[co->index];
	unsigned long flags;
	u32 mask, val;
	bool locked;

	if (oops_in_progress)
		locked = uart_port_trylock_irqsave(port, &flags);
	else {
		uart_port_lock_irqsave(port, &flags);
		locked = true;
	}
	mask = readl(port->membase + ZX_UART_MASK);
	writel(0, port->membase + ZX_UART_MASK);
	uart_console_write(port, s, count, zx279125_putchar);
	readl_poll_timeout_atomic(port->membase + ZX_UART_FLAGS, val,
				  val & ZX_UART_TX_EMPTY, 1, 10000);
	writel(mask, port->membase + ZX_UART_MASK);
	if (locked)
		uart_port_unlock_irqrestore(port, flags);
}

static int zx279125_console_setup(struct console *co, char *options)
{
	int baud = 115200, bits = 8, parity = 'n', flow = 'n';

	if (co->index < 0 || co->index >= ZX_UART_PORTS)
		co->index = 0;
	if (!zx279125_ports[co->index])
		return -ENODEV;
	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);
	return uart_set_options(zx279125_ports[co->index], co, baud, parity, bits, flow);
}

static struct console zx279125_console = {
	.name = "ttyS",
	.write = zx279125_console_write,
	.device = uart_console_device,
	.setup = zx279125_console_setup,
	.flags = CON_PRINTBUFFER,
	.index = -1,
	.data = &zx279125_uart_driver,
};
#define ZX279125_CONSOLE (&zx279125_console)
#else
#define ZX279125_CONSOLE NULL
#endif

static void zx279125_early_putchar(struct uart_port *port, unsigned char ch)
{
	u32 val;

	if (!readl_poll_timeout_atomic(port->membase + ZX_UART_FLAGS, val,
				      !(val & ZX_UART_TX_FULL), 1, 10000))
		writel(ch, port->membase + ZX_UART_DATA);
}

static void zx279125_early_write(struct console *co, const char *s,
				unsigned int count)
{
	struct earlycon_device *dev = co->data;

	uart_console_write(&dev->port, s, count, zx279125_early_putchar);
}

static int __init zx279125_early_setup(struct earlycon_device *dev,
				     const char *options)
{
	if (!dev->port.membase)
		return -ENODEV;
	/* Keep the bootloader's baud and pinmux until normal serial probe. */
	dev->con->write = zx279125_early_write;
	return 0;
}
OF_EARLYCON_DECLARE(zx279125, "zte,zx279125-uart", zx279125_early_setup);

static int zx279125_uart_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct uart_port *port;
	struct resource *res;
	struct clk *clk;
	int line, ret;

	line = of_alias_get_id(dev->of_node, "serial");
	if (line < 0 || line >= ZX_UART_PORTS)
		return -EINVAL;
	if (zx279125_ports[line])
		return -EBUSY;
	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;
	port->membase = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(port->membase))
		return PTR_ERR(port->membase);
	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "failed to enable UART clock\n");
	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	port->irq = ret;
	port->uartclk = clk_get_rate(clk);
	if (!port->uartclk)
		return -EINVAL;
	port->mapbase = res->start;
	port->mapsize = resource_size(res);
	port->dev = dev;
	port->line = line;
	port->iotype = UPIO_MEM32;
	port->fifosize = 16;
	port->flags = UPF_BOOT_AUTOCONF;
	port->ops = &zx279125_uart_ops;
	spin_lock_init(&port->lock);
	platform_set_drvdata(pdev, port);
	zx279125_ports[line] = port;
	ret = uart_add_one_port(&zx279125_uart_driver, port);
	if (ret)
		zx279125_ports[line] = NULL;
	return ret;
}

static void zx279125_uart_remove(struct platform_device *pdev)
{
	struct uart_port *port = platform_get_drvdata(pdev);

	uart_remove_one_port(&zx279125_uart_driver, port);
	zx279125_ports[port->line] = NULL;
}

static const struct of_device_id zx279125_uart_match[] = {
	{ .compatible = "zte,zx279125-uart" },
	{ }
};
MODULE_DEVICE_TABLE(of, zx279125_uart_match);

static struct platform_driver zx279125_uart_platform_driver = {
	.probe = zx279125_uart_probe,
	.remove = zx279125_uart_remove,
	.driver = {
		.name = "zx279125-uart",
		.of_match_table = zx279125_uart_match,
	},
};

static struct uart_driver zx279125_uart_driver = {
	.owner = THIS_MODULE,
	.driver_name = "zx279125-uart",
	.dev_name = "ttyS",
	.major = TTY_MAJOR,
	.minor = 64,
	.nr = ZX_UART_PORTS,
	.cons = ZX279125_CONSOLE,
};

static int __init zx279125_uart_init(void)
{
	int ret;

	ret = uart_register_driver(&zx279125_uart_driver);
	if (ret)
		return ret;
	ret = platform_driver_register(&zx279125_uart_platform_driver);
	if (ret)
		uart_unregister_driver(&zx279125_uart_driver);
	return ret;
}
module_init(zx279125_uart_init);

static void __exit zx279125_uart_exit(void)
{
	platform_driver_unregister(&zx279125_uart_platform_driver);
	uart_unregister_driver(&zx279125_uart_driver);
}
module_exit(zx279125_uart_exit);

MODULE_DESCRIPTION("ZTE Sanechips ZX279125 UART");
MODULE_LICENSE("GPL");
