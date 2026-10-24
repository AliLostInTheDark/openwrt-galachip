/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_ZTE_ZX279125_PP_H
#define _LINUX_SOC_ZTE_ZX279125_PP_H

#include <linux/types.h>
#include <linux/io.h>

struct sk_buff;

struct zx279125_pp_transport {
	/* Atomic context; payload remains owned by the caller. */
	int (*send_control)(void *priv, const void *data, size_t len, u16 gem);
	void *priv;
};

struct zx279125_pp_receiver {
	/* NAPI context; receiver takes ownership of skb, without Ethernet header. */
	void (*receive_control)(void *priv, struct sk_buff *skb, u16 gem);
	void *priv;
};

void zx279125_pp_update_bits(void __iomem *reg, u32 mask, u32 value);

int zx279125_pp_register_transport(const struct zx279125_pp_transport *transport);
void zx279125_pp_unregister_transport(const struct zx279125_pp_transport *transport);
int zx279125_pp_register_receiver(const struct zx279125_pp_receiver *receiver);
void zx279125_pp_unregister_receiver(const struct zx279125_pp_receiver *receiver);
int zx279125_pp_send_control(const void *data, size_t len, u16 gem);
void zx279125_pp_receive_control(struct sk_buff *skb, u16 gem);

#endif
