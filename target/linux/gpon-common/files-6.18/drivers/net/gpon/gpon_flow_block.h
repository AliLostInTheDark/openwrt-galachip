/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GPON_FLOW_BLOCK_H
#define GPON_FLOW_BLOCK_H

#include <net/pkt_cls.h>

/* Bind per block: fw4 probes a temporary table while the live table exists. */
static inline int gpon_flow_block_setup(struct net_device *dev,
				       struct flow_block_offload *f,
				       struct list_head *blocks,
				       flow_setup_cb_t *cb, u32 *binds)
{
	struct flow_block_cb *block_cb;
	int err = -EOPNOTSUPP;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		goto out;
	f->driver_block_list = blocks;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, cb, dev);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			break;
		}
		block_cb = flow_block_cb_alloc(cb, dev, dev, NULL);
		if (IS_ERR(block_cb)) {
			err = PTR_ERR(block_cb);
			goto out;
		}
		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, blocks);
		if (binds)
			(*binds)++;
		break;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, cb, dev);
		if (!block_cb) {
			err = -ENOENT;
			goto out;
		}
		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
		}
		break;
	default:
		goto out;
	}
	err = 0;
out:
#if IS_ENABLED(CONFIG_GPON_FLOW_DIAG)
	netdev_info(dev, "flow-block: command=%u binder=%u rc=%d\n",
		    f->command, f->binder_type, err);
#endif
	return err;
}

#endif
