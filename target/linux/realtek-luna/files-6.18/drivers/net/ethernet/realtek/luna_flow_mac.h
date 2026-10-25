/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LUNA_FLOW_MAC_H
#define _LUNA_FLOW_MAC_H

#include <linux/string.h>
#include "luna_l2_access.h"
#include "luna_l34_acc.h"

#define LUNA_FLOW_MAC_COUNT 256
#define LUNA_FLOW_TABLE_MAC 10

struct luna_flow_mac_layout {
	const struct luna_l2_layout *l2;
	u8 port_shift, port_bits, valid_shift, arp_shift, static_shift, age_shift;
};

static const struct luna_flow_mac_layout luna_flow_mac_rtl9603cvd = {
	.l2 = &luna_l2_rtl9603cvd,
	.port_shift = 2, .port_bits = 3, .valid_shift = 14, .arp_shift = 10,
	.static_shift = 29, .age_shift = 5,
};

struct luna_flow_mac_entry {
	u32 key[3];
	u16 users, l2_index;
	bool ready, pinned, original_pin;
};

struct luna_flow_mac_table {
	void __iomem *sw;
	const struct luna_l34_acc *acc;
	const struct luna_flow_mac_layout *layout;
	struct luna_flow_mac_entry entry[LUNA_FLOW_MAC_COUNT];
};

struct luna_flow_mac_ref {
	u8 index;
	bool owned;
};

struct luna_flow_local_mac {
	u32 original[3], installed[3];
	bool owned;
};

static inline int luna_flow_local_put(struct luna_flow_mac_table *t,
				      struct luna_flow_local_mac *m)
{
	u32 words[3];
	int ret;

	if (!m->owned)
		return 0;
	memcpy(words, m->installed, sizeof(words));
	ret = luna_l2_access(t->sw, t->layout->l2, 0, words, true, false);
	if (ret < 0 && ret != -ENOENT)
		return ret;
	/* A replaced entry belongs to its new owner. */
	if (ret >= 0 && !memcmp(words, m->installed, sizeof(words))) {
		memcpy(words, m->original, sizeof(words));
		ret = luna_l2_access(t->sw, t->layout->l2, 0, words, false, true);
		if (ret < 0)
			return ret;
	}
	m->owned = false;
	return 0;
}

static inline int luna_flow_local_get(struct luna_flow_mac_table *t,
				      struct luna_flow_local_mac *m,
				      const u8 *mac, unsigned int cpu_port)
{
	u32 words[3] = { 0 }, port_mask;
	int ret;

	if (!t || !m || !mac || !t->layout || cpu_port >= (1u << t->layout->port_bits))
		return -EINVAL;
	if (m->owned)
		return -EALREADY;
	if ((mac[0] & 1) || !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]))
		return -EINVAL;
	words[0] = ((u32)mac[2] << 24) | ((u32)mac[3] << 16) |
		   ((u32)mac[4] << 8) | mac[5];
	words[1] = ((u32)mac[0] << 8) | mac[1];
	memcpy(m->original, words, sizeof(words));
	ret = luna_l2_access(t->sw, t->layout->l2, 0, words, true, false);
	if (ret < 0 && ret != -ENOENT)
		return ret;
	port_mask = ((1u << t->layout->port_bits) - 1) << t->layout->port_shift;
	if (ret >= 0) {
		if (((words[2] & port_mask) >> t->layout->port_shift) != cpu_port)
			return -EADDRINUSE;
		memcpy(m->original, words, sizeof(words));
	} else {
		memcpy(words, m->original, sizeof(words));
	}
	words[1] |= 1u << t->layout->static_shift;
	/* Static unicast entries still require a nonzero age. */
	words[2] = (words[2] & ~(7u << t->layout->age_shift)) |
		   (1u << t->layout->age_shift);
	words[2] = (words[2] & ~port_mask) | (cpu_port << t->layout->port_shift) |
		   (1u << t->layout->valid_shift);
	memcpy(m->installed, words, sizeof(words));
	/* A timed-out write may still complete; retain its rollback state. */
	m->owned = true;
	ret = luna_l2_access(t->sw, t->layout->l2, 0, words, false, true);
	if (ret < 0)
		return ret;
	ret = luna_l2_access(t->sw, t->layout->l2, 0, words, true, false);
	if (ret < 0)
		return ret;
	return memcmp(words, m->installed, sizeof(words)) ? -EIO : 0;
}

static inline bool luna_flow_mac_same_key(const u32 *a, const u32 *b)
{
	if (a[0] != b[0] || ((a[1] ^ b[1]) & 0x5000ffff))
		return false;
	if (a[1] & (1u << 30))
		return !((a[1] ^ b[1]) & 0x0fff0000);
	return !((a[1] ^ b[1]) & (1u << 31)) && !((a[2] ^ b[2]) & 1);
}

static inline int luna_flow_mac_put(struct luna_flow_mac_table *t,
				    struct luna_flow_mac_ref *ref)
{
	struct luna_flow_mac_entry *m;
	u32 words[3], zero = 0;
	int ret;

	if (!t || !ref || !t->layout)
		return -EINVAL;
	if (!ref->owned)
		return 0;
	m = &t->entry[ref->index];
	if (!m->users)
		return -EINVAL;
	if (m->users > 1) {
		m->users--;
		ref->owned = false;
		return 0;
	}
	m->ready = false;
	ret = luna_l34_tbl_op(t->sw, t->acc, LUNA_FLOW_TABLE_MAC,
			      ref->index, &zero, 1, true);
	if (ret)
		return ret;
	if (m->pinned && !m->original_pin) {
		memcpy(words, m->key, sizeof(words));
		ret = luna_l2_access(t->sw, t->layout->l2, m->l2_index,
				     words, true, false);
		if (ret < 0 && ret != -ENOENT)
			return ret;
		if (ret >= 0 && luna_flow_mac_same_key(words, m->key) &&
		    (words[2] & (1u << t->layout->valid_shift))) {
			words[2] &= ~(1u << t->layout->arp_shift);
			ret = luna_l2_access(t->sw, t->layout->l2, m->l2_index,
					     words, false, true);
			if (ret < 0)
				return ret;
		}
	}
	memset(m, 0, sizeof(*m));
	ref->owned = false;
	return 0;
}

static inline int luna_flow_mac_get(struct luna_flow_mac_table *t,
				    struct luna_flow_mac_ref *ref,
				    const u8 *mac, u32 allowed_ports)
{
	struct luna_flow_mac_entry *m;
	u32 words[3] = { 0 }, index, port, pin;
	int ret, free_slot = -1;
	unsigned int i;

	if (!t || !ref || !mac || !t->layout)
		return -EINVAL;
	if (ref->owned)
		return -EALREADY;
	words[0] = ((u32)mac[2] << 24) | ((u32)mac[3] << 16) |
		   ((u32)mac[4] << 8) | mac[5];
	words[1] = ((u32)mac[0] << 8) | mac[1];
	ret = luna_l2_access(t->sw, t->layout->l2, 0, words, true, false);
	if (ret < 0)
		return ret;
	index = ret;
	if (!(words[2] & (1u << t->layout->valid_shift)))
		return -ENOENT;
	port = (words[2] >> t->layout->port_shift) &
	       ((1u << t->layout->port_bits) - 1);
	if (!(allowed_ports & (1u << port)))
		return -EOPNOTSUPP;
	for (i = 0; i < LUNA_FLOW_MAC_COUNT; i++) {
		m = &t->entry[i];
		if (!m->users) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (!luna_flow_mac_same_key(words, m->key))
			continue;
		if (!m->ready || m->l2_index != index ||
		    !(words[2] & (1u << t->layout->arp_shift)))
			return -EAGAIN;
		if (m->users == 0xffff)
			return -ENOSPC;
		m->users++;
		ref->index = i;
		ref->owned = true;
		return 0;
	}
	if (free_slot < 0)
		return -ENOSPC;
	m = &t->entry[free_slot];
	memcpy(m->key, words, sizeof(words));
	m->users = 1;
	m->l2_index = index;
	ref->index = free_slot;
	ref->owned = true;
	pin = 1u << t->layout->arp_shift;
	m->original_pin = !!(words[2] & pin);
	m->pinned = true;
	if (!m->original_pin) {
		words[2] |= pin;
		ret = luna_l2_access(t->sw, t->layout->l2, index, words, false, true);
		if (ret < 0)
			return ret;
		m->l2_index = ret;
	}
	index = m->l2_index;
	ret = luna_l34_tbl_op(t->sw, t->acc, LUNA_FLOW_TABLE_MAC,
			      ref->index, &index, 1, true);
	if (!ret)
		m->ready = true;
	return ret;
}

#endif
