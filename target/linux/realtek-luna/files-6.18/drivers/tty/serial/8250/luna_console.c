// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Asynchronous kernel console for the Luna 8250 UART.
 *
 * The 8250 console writes every character with the port lock held and IRQs
 * off, waiting on the UART: ~6 ms per line at 115200 on a single core. The
 * GPON FSM timer printing one line lost the production OLT's Password
 * deadline (98 deactivations, 2026-10-07). nbcon does not help without
 * PREEMPT_RT: its printer thread writes under the same port lock.
 *
 * This console only copies into a ring (onu_console_ring.h). A thread puts
 * the ring into the UART FIFO when it is empty and sleeps while it drains, so
 * the port lock is held for one FIFO load. Panic, oops and shutdown write
 * synchronously so the last lines still reach the wire. At late_initcall it
 * takes over the 8250 console of the same port: same name, same /dev/console.
 *
 * The tty shares the UART. A kernel line starts only while the tty transmitter
 * is idle, and the tty's start_tx waits for the line's last byte, so neither
 * cuts into the other (a FIFO-sized slice of a kernel line inside a shell
 * reply broke the rig's console reads).
 */
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/irq_work.h>
#include <linux/kthread.h>
#include <linux/onu_console_ring.h>
#include <linux/panic.h>
#include <linux/reboot.h>
#include <linux/serial_8250.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>
#include <linux/string.h>
#include <linux/wait.h>

#include "8250.h"

#define LUNA_CON_RING_BYTES	(64 * 1024)
#define LUNA_CON_BYTE_US	87	/* one 8N1 character at 115200 */
#define LUNA_CON_SPIN_US	10000	/* bound on a synchronous wait for THRE */

static unsigned char luna_con_buf[LUNA_CON_RING_BYTES];
static struct onu_con_ring luna_con_ring = {
	.buf = luna_con_buf,
	.mask = LUNA_CON_RING_BYTES - 1,
};
static struct uart_8250_port *luna_con_up;
static struct console *luna_con_vendor;
static struct task_struct *luna_con_thread;
static DECLARE_WAIT_QUEUE_HEAD(luna_con_wq);
static bool luna_con_sync;
static const struct uart_ops *luna_con_port_ops;
static struct uart_ops luna_con_ops;
static struct onu_con_gate luna_con_gate;	/* port lock */

static bool luna_con_fifo_on(struct uart_8250_port *up)
{
	struct uart_port *port = &up->port;

	return (up->capabilities & UART_CAP_FIFO) && up->tx_loadsz > 1 &&
	       (up->fcr & UART_FCR_ENABLE_FIFO) && port->state &&
	       test_bit(TTY_PORT_INITIALIZED, &port->state->port.iflags);
}

/* One FIFO load, never past the end of a line, when the transmitter is empty.
 * Port lock held. -> bytes sent */
static unsigned int luna_con_load_fifo(struct uart_8250_port *up, int *eol)
{
	unsigned char chunk[64];
	unsigned int n, i, room;

	*eol = 0;
	if (!(serial_lsr_in(up) & UART_LSR_THRE))
		return 0;
	room = luna_con_fifo_on(up) ? min_t(unsigned int, up->tx_loadsz, sizeof(chunk)) : 1;
	n = onu_con_ring_get_line(&luna_con_ring, chunk, room, eol);
	for (i = 0; i < n; i++)
		serial_port_out(&up->port, UART_TX, chunk[i]);
	return n;
}

/* The tty's start_tx: deferred while a kernel line is on the wire. Port lock held. */
static void luna_con_start_tx(struct uart_port *port)
{
	if (onu_con_gate_tty(&luna_con_gate))
		luna_con_port_ops->start_tx(port);
}

/* One step of the drain thread. -> bytes worth of time to sleep */
static unsigned int luna_con_drain_step(struct uart_8250_port *up)
{
	unsigned long flags;
	unsigned int n = 0;
	int eol;

	uart_port_lock_irqsave(&up->port, &flags);
	if (onu_con_gate_kernel(&luna_con_gate, up->ier & UART_IER_THRI)) {
		n = luna_con_load_fifo(up, &eol);
		if (onu_con_gate_loaded(&luna_con_gate, &luna_con_ring, n, eol))
			luna_con_port_ops->start_tx(&up->port);
	} else {
		n = up->tx_loadsz;	/* the tty is sending: one FIFO of its time */
	}
	uart_port_unlock_irqrestore(&up->port, flags);
	return n;
}

static void luna_con_spin_byte(struct uart_8250_port *up, unsigned char c)
{
	unsigned int us;

	for (us = 0; us < LUNA_CON_SPIN_US && !(serial_lsr_in(up) & UART_LSR_THRE); us++)
		udelay(1);
	serial_port_out(&up->port, UART_TX, c);
}

/* Panic, oops, shutdown: empty the ring, then @s, on the caller's time. */
static void luna_con_write_sync(const char *s, unsigned int n)
{
	struct uart_8250_port *up = luna_con_up;
	unsigned char c;
	unsigned long flags;
	unsigned int i;
	int locked = 1;

	if (oops_in_progress)
		locked = uart_port_trylock_irqsave(&up->port, &flags);
	else
		uart_port_lock_irqsave(&up->port, &flags);
	while (onu_con_ring_get(&luna_con_ring, &c, 1))
		luna_con_spin_byte(up, c);
	for (i = 0; i < n; i++) {
		if (s[i] == '\n')
			luna_con_spin_byte(up, '\r');
		luna_con_spin_byte(up, s[i]);
	}
	if (locked)
		uart_port_unlock_irqrestore(&up->port, flags);
}

static void luna_con_kick(struct irq_work *w)
{
	wake_up(&luna_con_wq);
}

static DEFINE_IRQ_WORK(luna_con_kick_work, luna_con_kick);

static void luna_con_write(struct console *co, const char *s, unsigned int n)
{
	if (READ_ONCE(luna_con_sync) || oops_in_progress || panic_in_progress()) {
		luna_con_write_sync(s, n);
		return;
	}
	if (onu_con_ring_put(&luna_con_ring, s, n))
		irq_work_queue(&luna_con_kick_work);
}

static int luna_con_drain(void *unused)
{
	while (!kthread_should_stop()) {
		wait_event_interruptible(luna_con_wq, kthread_should_stop() ||
					 onu_con_ring_used(&luna_con_ring));
		while (onu_con_ring_used(&luna_con_ring) && !kthread_should_stop()) {
			unsigned int us = max(luna_con_drain_step(luna_con_up), 1u) * LUNA_CON_BYTE_US;

			usleep_range(us, us + 100);
		}
	}
	return 0;
}

static int luna_con_reboot(struct notifier_block *nb, unsigned long action, void *data)
{
	WRITE_ONCE(luna_con_sync, true);
	luna_con_write_sync("", 0);
	return NOTIFY_DONE;
}

static struct notifier_block luna_con_reboot_nb = {
	.notifier_call = luna_con_reboot,
	.priority = INT_MIN,
};

static struct console luna_con = {
	.name	= "ttyS",
	.write	= luna_con_write,
	.flags	= CON_ANYTIME,
};

static int __init luna_con_init(void)
{
	struct console *c;
	unsigned long flags;

	console_list_lock();
	for_each_console(c) {
		if (!strcmp(c->name, "ttyS") && (c->flags & CON_ENABLED) &&
		    !(c->flags & (CON_BOOT | CON_NBCON))) {
			luna_con_vendor = c;
			break;
		}
	}
	console_list_unlock();
	if (!luna_con_vendor) {
		pr_err("luna-console: no enabled ttyS console to make asynchronous -- printk stays synchronous\n");
		return -ENODEV;
	}
	luna_con_up = serial8250_get_port(luna_con_vendor->index);
	uart_port_lock_irqsave(&luna_con_up->port, &flags);
	luna_con_port_ops = luna_con_up->port.ops;
	luna_con_ops = *luna_con_port_ops;
	luna_con_ops.start_tx = luna_con_start_tx;
	luna_con_up->port.ops = &luna_con_ops;
	uart_port_unlock_irqrestore(&luna_con_up->port, flags);
	luna_con_thread = kthread_run(luna_con_drain, NULL, "luna-console");
	if (IS_ERR(luna_con_thread)) {
		pr_err("luna-console: drain thread not started (%ld) -- printk stays synchronous\n",
		       PTR_ERR(luna_con_thread));
		return PTR_ERR(luna_con_thread);
	}
	register_reboot_notifier(&luna_con_reboot_nb);

	luna_con.index = luna_con_vendor->index;
	luna_con.device = luna_con_vendor->device;
	luna_con.data = luna_con_vendor->data;
	register_console(&luna_con);
	if (!(luna_con.flags & CON_ENABLED)) {
		pr_err("luna-console: ttyS%d refused the asynchronous console -- printk stays synchronous\n",
		       luna_con.index);
		return -EBUSY;
	}
	console_lock();
	luna_con.seq = luna_con_vendor->seq;
	console_unlock();
	unregister_console(luna_con_vendor);
	pr_info("luna-console: ttyS%d asynchronous, %u-byte ring\n",
		luna_con.index, LUNA_CON_RING_BYTES);
	return 0;
}
late_initcall(luna_con_init);
