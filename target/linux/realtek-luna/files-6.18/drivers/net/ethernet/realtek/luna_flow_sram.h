/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LUNA_FLOW_SRAM_H
#define _LUNA_FLOW_SRAM_H

#include <linux/errno.h>
#include "luna_flow_logic.h"
#include "luna_l34_acc.h"

#define LUNA_FLOW_SRAM_MAX 4096
#define LUNA_FLOW_TABLE_PATH5 8

struct luna_flow_slot {
	u16 index;
	bool owned;
};

/* The caller serializes access and keeps owners alive until clear succeeds. */
struct luna_flow_sram {
	void __iomem *sw;
	const struct luna_l34_acc *acc;
	struct luna_flow_hash_config hash;
	struct luna_flow_slot *owner[LUNA_FLOW_SRAM_MAX];
};

static inline int luna_flow_sram_add(struct luna_flow_sram *s,
				     struct luna_flow_slot *slot,
				     const struct gpon_flow_key *key, u32 extra,
				     const struct luna_flow_path5 *action)
{
	u32 words[LUNA_FLOW_WORDS], previous[LUNA_FLOW_WORDS];
	u16 base[2], index;
	unsigned int bucket, way;
	int ret;

	if (!s || !slot || s->hash.storage != LUNA_FLOW_SRAM)
		return -EINVAL;
	if (slot->owned)
		return -EALREADY;
	ret = luna_flow_hash(&s->hash, key, extra, &base[0], &base[1]);
	if (ret)
		return ret;
	ret = luna_flow_path5_encode(key, action, words);
	if (ret)
		return ret;

	for (bucket = 0; bucket < 2; bucket++) {
		for (way = 0; way < 4; way++) {
			index = base[bucket] + way;
			if (s->owner[index])
				continue;
			ret = luna_l34_tbl_op(s->sw, s->acc,
					      LUNA_FLOW_TABLE_PATH5, index,
					      previous, LUNA_FLOW_WORDS, false);
			if (ret)
				return ret;
			if (previous[0] & 1)
				continue;
			/* A timed-out write may still publish this entry. */
			slot->index = index;
			slot->owned = true;
			s->owner[index] = slot;
			return luna_l34_tbl_op(s->sw, s->acc,
					       LUNA_FLOW_TABLE_PATH5, index,
					       words, LUNA_FLOW_WORDS, true);
		}
	}
	return -ENOSPC;
}

static inline int luna_flow_sram_del(struct luna_flow_sram *s,
				     struct luna_flow_slot *slot)
{
	u32 words[LUNA_FLOW_WORDS] = { 0 };
	int ret;

	if (!s || !slot || s->hash.storage != LUNA_FLOW_SRAM)
		return -EINVAL;
	if (!slot->owned)
		return 0;
	if (slot->index >= luna_flow_entry_count(&s->hash) ||
	    s->owner[slot->index] != slot)
		return -EINVAL;
	ret = luna_l34_tbl_op(s->sw, s->acc, LUNA_FLOW_TABLE_PATH5,
			      slot->index, words, LUNA_FLOW_WORDS, true);
	if (ret)
		return ret;
	s->owner[slot->index] = NULL;
	slot->owned = false;
	return 0;
}

#endif
