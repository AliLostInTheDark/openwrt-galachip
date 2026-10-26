// SPDX-License-Identifier: GPL-2.0-only
/* Shared CPU packet transport between the ZX279125 PP and PON control plane. */
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/soc/zte/zx279125-pp.h>

static DEFINE_MUTEX(zx_pp_lock);
static DEFINE_RAW_SPINLOCK(zx_pp_register_lock);

void zx279125_pp_update_bits(void __iomem *reg, u32 mask, u32 value)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&zx_pp_register_lock, flags);
	writel((readl(reg) & ~mask) | (value & mask), reg);
	readl(reg);
	raw_spin_unlock_irqrestore(&zx_pp_register_lock, flags);
}
EXPORT_SYMBOL_GPL(zx279125_pp_update_bits);
static const struct zx279125_pp_transport __rcu *zx_transport;
static const struct zx279125_pp_receiver __rcu *zx_receiver;

int zx279125_pp_register_transport(const struct zx279125_pp_transport *transport)
{
	int ret = 0;

	if (!transport || !transport->send_control)
		return -EINVAL;
	mutex_lock(&zx_pp_lock);
	if (rcu_access_pointer(zx_transport))
		ret = -EBUSY;
	else
		rcu_assign_pointer(zx_transport, transport);
	mutex_unlock(&zx_pp_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(zx279125_pp_register_transport);

void zx279125_pp_unregister_transport(const struct zx279125_pp_transport *transport)
{
	mutex_lock(&zx_pp_lock);
	if (rcu_access_pointer(zx_transport) == transport)
		RCU_INIT_POINTER(zx_transport, NULL);
	mutex_unlock(&zx_pp_lock);
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(zx279125_pp_unregister_transport);

int zx279125_pp_register_receiver(const struct zx279125_pp_receiver *receiver)
{
	int ret = 0;

	if (!receiver || !receiver->receive_control)
		return -EINVAL;
	mutex_lock(&zx_pp_lock);
	if (rcu_access_pointer(zx_receiver))
		ret = -EBUSY;
	else
		rcu_assign_pointer(zx_receiver, receiver);
	mutex_unlock(&zx_pp_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(zx279125_pp_register_receiver);

void zx279125_pp_unregister_receiver(const struct zx279125_pp_receiver *receiver)
{
	mutex_lock(&zx_pp_lock);
	if (rcu_access_pointer(zx_receiver) == receiver)
		RCU_INIT_POINTER(zx_receiver, NULL);
	mutex_unlock(&zx_pp_lock);
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(zx279125_pp_unregister_receiver);

int zx279125_pp_send_control(const void *data, size_t len, u16 gem)
{
	const struct zx279125_pp_transport *transport;
	int ret = -ENETDOWN;

	if (!data || !len || len > 2016 || gem > 0xfff)
		return -EINVAL;
	rcu_read_lock();
	transport = rcu_dereference(zx_transport);
	if (transport)
		ret = transport->send_control(transport->priv, data, len, gem);
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(zx279125_pp_send_control);

void zx279125_pp_receive_control(struct sk_buff *skb, u16 gem)
{
	const struct zx279125_pp_receiver *receiver;

	rcu_read_lock();
	receiver = rcu_dereference(zx_receiver);
	if (receiver)
		receiver->receive_control(receiver->priv, skb, gem);
	else
		dev_kfree_skb_any(skb);
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(zx279125_pp_receive_control);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ZX279125 shared CPU packet transport");
