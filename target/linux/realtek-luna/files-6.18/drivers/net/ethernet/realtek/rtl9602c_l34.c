// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL9602C "Luna" hardware L3/L4 forwarding + NAPT engine.
 *
 * Indirect table-access plumbing + NAPT flow programming for the SoC switch
 * core's L3/L4 accelerator. Established connections handed down by the kernel
 * flow-offload path are written into the hardware NAPT tables so they are
 * NAT'd/forwarded by the switch instead of the CPU.
 *
 * Clean-room: the register/table behaviour is hardware fact; all expression
 * (names, packing helpers, the access sequence) is original. Endian-safe: the
 * packed entry words are built with explicit shift/mask on a local u32 array,
 * never struct overlays, so the same code is correct on big-endian MIPS now and
 * little-endian ARM later.
 *
 * Copyright (C) 2026 Confiared <contact@confiared.com>
 */
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include "rtl9602c_l34_logic.h"	/* hoisted logic */
#include <linux/errno.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "luna_l34_acc.h"
#include "luna_gpon_nic.h"
#include "rtl9602c_l34.h"
#include "gpon_flow_offload.h"

#define L34_EXE_POLL_US		2000	/* engine clears EXE well under this */

static int l34_neigh_del(struct rtl9602c_l34 *l, struct l34_flow *f);

static inline u32 l34_rd(struct rtl9602c_l34 *l, u32 off)
{
	return readl(l->sw + off);
}

static inline void l34_wr(struct rtl9602c_l34 *l, u32 off, u32 val)
{
	writel(val, l->sw + off);
}

/* l34_field_get() moved to rtl9602c_l34_logic.c */
/*
 * The indirect table access lives in luna_l34_acc.h, shared with the
 * RTL9603CVD's flow-based engine. These are this chip's four addresses and
 * command-word layout.
 */
static int l34_tbl_op(struct rtl9602c_l34 *l, enum l34_tbl type, u16 idx,
		      u32 *w, unsigned int n, bool write)
{
	return luna_l34_tbl_op(l->sw, &luna_l34_acc_rtl9602c, (u8)type, idx,
			       w, n, write);
}

static int l34_tbl_write(struct rtl9602c_l34 *l, enum l34_tbl type, u16 idx,
			 u32 *w, unsigned int n)
{
	return l34_tbl_op(l, type, idx, w, n, true);
}

static int l34_tbl_read(struct rtl9602c_l34 *l, enum l34_tbl type, u16 idx,
			u32 *w, unsigned int n)
{
	return l34_tbl_op(l, type, idx, w, n, false);
}

/* The writer flowcore's l34_prog_run() drives. */
static int l34_prog_wr(void *ctx, u8 tbl, u16 idx, const u32 *w, unsigned int n)
{
	struct rtl9602c_l34 *l = ctx;
	u32 buf[L34_WORDS_MAX];
	int ret;

	memcpy(buf, w, n * sizeof(*w));
	ret = l34_tbl_write(l, (enum l34_tbl)tbl, idx, buf, n);
	if (ret)
		pr_err("[NAT-005] rtl9602c-l34: table %u entry %u write failed (%d)\n",
		       tbl, idx, ret);
	return ret;
}

/* Enable the L34 NAT engine. DEFERRED from init */
static void l34_engine_on(struct rtl9602c_l34 *l)
{
	u32 v;

	l34_wr(l, L34_CLR, 0xffff);		/* reset the NAT table types we use */
	usleep_range(50, 100);

	v = l34_rd(l, L34_SWTCR0);
	v &= ~(0x3u << L34_SWTCR0_NATMODE_SH);
	v |=  (0x3u << L34_SWTCR0_NATMODE_SH);		/* L3 + L4 NAT enable */
	v &= ~(0x3u << L34_SWTCR0_LIMDBC_SH);
	v |=  (0x2u << L34_SWTCR0_LIMDBC_SH);		/* what stock also holds */
	/*
	 * This is a read-modify-write, so every bit not named keeps what the
	 * reset or bootloader left, and V4FLRT_EN/V6FLRT_EN arrive set. FLRT is
	 * the die's flow-route lookup, with its own tables
	 * (FLOW_ROUTING_TABLE_IPV4, access type 12) that this driver never
	 * programs. With the bit set the engine consults an empty table instead
	 * of the NAPT/NAPTR pair, so an installed entry is never hit. Stock
	 * clears both bits; the value is what the vendor firmware holds on this
	 * board, not a datasheet reading.
	 */
	v &= ~(L34_SWTCR0_V4FLRT_EN | L34_SWTCR0_V6FLRT_EN);
	v |= L34_SWTCR0_TTL_MINUS | L34_SWTCR0_FRAG2CPU;
	l34_wr(l, L34_SWTCR0, v);

#if IS_ENABLED(CONFIG_GPON_FLOW_DIAG)
	/*
	 * Ask the die to keep its own lookup log. Purely diagnostic: it changes
	 * no forwarding decision, only makes HSB/HSA hold the last lookup
	 * instead of zeroes. Behind the declared flag, so an image without the
	 * diagnostics costs nothing. Stock runs in this mode permanently.
	 */
	v = l34_rd(l, L34_HSBA_CTRL);
	v &= ~(L34_HSBA_TST_LOG_MD_M << L34_HSBA_TST_LOG_MD_SH);
	v |=  ((u32)L34_HSBA_MODE_FIRST_DROP << L34_HSBA_TST_LOG_MD_SH);
	l34_wr(l, L34_HSBA_CTRL, v);
#endif

	l34_wr(l, L34_GLB_CFG, l34_rd(l, L34_GLB_CFG) | BIT(0));	/* master enable */

	l->engine_on = true;
}

int rtl9602c_l34_init(struct rtl9602c_l34 *l, void __iomem *sw)
{
	if (!sw)
		return -EINVAL;
	l->sw = sw;
	mutex_init(&l->lock);
	l->ready = true;	/* table-access ready; the engine is enabled lazily */

	/*
	 * Drain the hit bitmap once and discard it. It holds non-zero state
	 * from power-on or the vendor firmware with no flows installed (82 of
	 * 128 words on the X111W). hit_shadow is sticky, so sweeping this into
	 * it would hide genuine hits behind a half-set background.
	 *
	 * The register is clear-on-read, so this loop needs no write and must
	 * not touch hit_shadow or hits_seen.
	 */
	{
		unsigned int i;

		for (i = 0; i < L34_NAPT_ENTRIES / 32; i++)
			(void)l34_rd(l, L34_NAPT_HIT + 4 * i);
	}
	return 0;
}

/* Caller holds the table mutex; cumulative counters are not ownership. */
static bool l34_has_owners(struct rtl9602c_l34 *l)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(l->lan_neigh); i++)
		if (l->lan_neigh[i].users)
			return true;
	return !bitmap_empty(l->out_reserved, L34_NAPT_ENTRIES) ||
	       !bitmap_empty(l->in_reserved, L34_NAPT_ENTRIES);
}

/* Caller holds the table mutex. WHICH ownership is live -- the answer to a
 * refusal that names only "busy". */
static void l34_owner_counts(struct rtl9602c_l34 *l, unsigned int *neigh,
			     unsigned int *out, unsigned int *in)
{
	unsigned int i;

	*neigh = 0;
	for (i = 0; i < ARRAY_SIZE(l->lan_neigh); i++)
		*neigh += l->lan_neigh[i].users;
	*out = bitmap_weight(l->out_reserved, L34_NAPT_ENTRIES);
	*in = bitmap_weight(l->in_reserved, L34_NAPT_ENTRIES);
}

/* The same question from OUTSIDE the lock, for a caller deciding whether a
 * reprogram would cost somebody else their acceleration. */
bool rtl9602c_l34_has_owners(struct rtl9602c_l34 *l)
{
	bool owned;

	mutex_lock(&l->lock);
	owned = l34_has_owners(l);
	mutex_unlock(&l->lock);
	return owned;
}

/* Scan the 4 ways of a hash bucket for the first slot whose VALID field is 0;
 * returns the entry index, a negative table error, or -ENOSPC if full. */
static int l34_free_way(struct rtl9602c_l34 *l, enum l34_tbl type, u16 bucket,
			unsigned int words, unsigned int valid_lsp,
			unsigned int valid_w)
{
	u32 probe[L34_WORDS_NAPTR_IN];	/* sized to the widest entry */
	const unsigned long *reserved = type == L34_TBL_NAPT_OUT ?
		l->out_reserved : l->in_reserved;
	unsigned int way;
	int ret;

	for (way = 0; way < L34_NAPT_WAYS; way++) {
		if (test_bit((bucket << 2) + way, reserved))
			continue;	/* invalid in hardware, still owned here */
		ret = l34_tbl_read(l, type, (bucket << 2) + way, probe, words);
		if (ret)
			return ret;
		if (!l34_field_get(probe, valid_lsp, valid_w))
			return (bucket << 2) + way;
	}
	return -ENOSPC;
}

/* NAPT flow programming. A 4-way hashed pair */
int rtl9602c_l34_flow_add(struct rtl9602c_l34 *l, struct l34_flow *f)
{
	u32 naptr[L34_WORDS_NAPTR_IN] = { 0 };
	u32 napt[L34_WORDS_NAPT_OUT] = { 0 };
	int naptr_idx, napt_idx, ret;
	u16 out_bucket, in_bucket;
	bool is_tcp;

	if (!l->ready)
		return -ENODEV;
	if (f->out_owned || f->in_owned || f->neigh_owned)
		return -EBUSY;	/* installing over a live owner loses its indices */
	if (f->l4proto != IPPROTO_TCP && f->l4proto != IPPROTO_UDP)
		return -EOPNOTSUPP;
	is_tcp = (f->l4proto == IPPROTO_TCP);

	out_bucket = l34_hash_out(is_tcp, f->orig_sip, f->orig_sport,
				  f->orig_dip, f->orig_dport);
	in_bucket  = l34_hash_in(is_tcp, f->nat_sip, f->nat_sport);

	mutex_lock(&l->lock);
	if (!l->engine_on)
		l34_engine_on(l);

	naptr_idx = l34_free_way(l, L34_TBL_NAPTR_IN, in_bucket, L34_WORDS_NAPTR_IN,
				 L34_NAPTR_VALID_LSP, L34_NAPTR_VALID_W);
	if (naptr_idx < 0) {
		ret = naptr_idx;
		goto out;
	}
	napt_idx = l34_free_way(l, L34_TBL_NAPT_OUT, out_bucket, L34_WORDS_NAPT_OUT,
				L34_NAPT_VALID_LSP, L34_NAPT_VALID_W);
	if (napt_idx < 0) {
		ret = napt_idx;
		goto out;
	}

	/*
	 * Reserve both identities before either command can become visible. A
	 * timeout does not establish whether hardware accepted the write, so
	 * from here on the cleanup path must be able to name both rows.
	 */
	f->hw_index = napt_idx;
	f->naptr_index = naptr_idx;
	__set_bit(napt_idx, l->out_reserved);
	__set_bit(naptr_idx, l->in_reserved);
	f->out_owned = f->in_owned = true;
	l34_naptr_encode(naptr, f->orig_sip, f->orig_sport, f->egress_netif,
			 f->nat_sport, is_tcp);
	ret = l34_tbl_write(l, L34_TBL_NAPTR_IN, naptr_idx, naptr, L34_WORDS_NAPTR_IN);
	if (ret)
		goto out;

	l34_napt_encode(napt, naptr_idx);
	ret = l34_tbl_write(l, L34_TBL_NAPT_OUT, napt_idx, napt, L34_WORDS_NAPT_OUT);
	if (ret)
		goto out;	/* the rewrite entry stays OWNED; flow_del clears it */

	f->installed = true;
	l->installs++;
	ret = 0;
out:
	mutex_unlock(&l->lock);
	return ret;
}

int rtl9602c_l34_flow_del(struct rtl9602c_l34 *l, struct l34_flow *f)
{
	u32 zero[L34_WORDS_NAPTR_IN] = { 0 };
	int ret = 0;

	if (!f->out_owned && !f->in_owned && !f->neigh_owned)
		return 0;	/* nothing was ever claimed for this flow */
	if (!l->ready)
		return -ENODEV;
	mutex_lock(&l->lock);
	if (f->neigh_owned) {
		ret = l34_neigh_del(l, f);
		goto out;
	}
	/*
	 * The order matters. The outbound slot is what the lookup matches, so
	 * clearing it first stops the flow, and the rewrite entry it points at
	 * is then unreachable. The reverse order would leave a live flow
	 * rewriting through a zeroed entry, and a failed outbound clear must
	 * not go on to remove a rewrite entry that is still referenced. An
	 * index is released only once its row is proven gone, so a retry finds
	 * both halves where it left them.
	 */
	if (f->out_owned) {
		ret = l34_tbl_write(l, L34_TBL_NAPT_OUT, f->hw_index, zero,
				    L34_WORDS_NAPT_OUT);
		if (ret)
			goto out;
		f->out_owned = false;
		__clear_bit(f->hw_index, l->out_reserved);
	}
	if (f->in_owned) {
		ret = l34_tbl_write(l, L34_TBL_NAPTR_IN, f->naptr_index, zero,
				    L34_WORDS_NAPTR_IN);
		if (ret)
			goto out;
		f->in_owned = false;
		__clear_bit(f->naptr_index, l->in_reserved);
	}
	if (f->installed) {
		l->removals++;	/* one removal per INSTALL, never per attempt */
		f->installed = false;
	}
out:
	mutex_unlock(&l->lock);
	return ret;
}

/*
 * Sweep the clear-on-read hit bitmap into the sticky shadow, at most once per
 * jiffy: at most 128 reads per jiffy however many flows there are. Caller holds
 * l->lock.
 */
static void l34_hit_sweep(struct rtl9602c_l34 *l)
{
	unsigned int i, now = 0;

	/* wrap-safe, and a never-swept driver (hit_swept == 0) always sweeps */
	if (l->hit_swept && !time_after(jiffies, l->hit_swept))
		return;
	l->hit_swept = jiffies;
	for (i = 0; i < ARRAY_SIZE(l->hit_shadow); i++) {
		u32 w = l34_rd(l, L34_NAPT_HIT + 4 * i);

		if (!w)
			continue;
		now += hweight32(w);
		l->hit_shadow[i] |= w;
	}
	l->hits_seen += now;
}

int rtl9602c_l34_flow_hit(struct rtl9602c_l34 *l, u16 hw_index, bool *active)
{
	unsigned int w = hw_index / 32u, b = hw_index % 32u;

	if (!l->ready)
		return -ENODEV;
	if (w >= ARRAY_SIZE(l->hit_shadow))
		return -EINVAL;
	mutex_lock(&l->lock);
	l34_hit_sweep(l);
	*active = !!(l->hit_shadow[w] & BIT(b));
	/*
	 * consume this slot only: liveness is per flow, and clearing a
	 * neighbour's bit ages other flows out
	 */
	l->hit_shadow[w] &= ~BIT(b);
	mutex_unlock(&l->lock);
	return 0;
}

/* WAN forwarding bring-up */
static int l2_busy_wait(struct rtl9602c_l34 *l)
{
	unsigned int t;

	for (t = 0; t < L34_EXE_POLL_US; t++) {
		if (!(l34_rd(l, L2_STS) & L2_STS_BUSY))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/* Insert a static unicast MAC by the MAC-hash method; the engine picks a free
 * way and reports the entry index (used as NEXTHOP/ARP nhIdx). */
static int l2uc_add_static(struct rtl9602c_l34 *l, const u8 *mac, u8 port)
{
	u32 w[L34_WORDS_L2UC] = { 0 };
	int ret;

	l34_l2uc_encode(w, mac, port);	/* key + flags (flowcore) */

	ret = l2_busy_wait(l);
	if (ret)
		return ret;
	/* Hardware words are LSW-first; stock reverses its MSW-first table buffer. */
	l34_wr(l, L2_WDATA + 0, w[0]);
	l34_wr(l, L2_WDATA + 4, w[1]);
	l34_wr(l, L2_WDATA + 8, w[2]);
	l34_wr(l, L2_CMD, L2_CMD_WR | (L2_METHOD_MAC << L2_CMD_METHOD_SH));

	ret = l2_busy_wait(l);
	if (ret)
		return ret;
	/* the assigned-index decode is l34_l2uc_sts_index() (flowcore); the
	 * errno stays here -- flowcore includes no errno.h */
	ret = l34_l2uc_sts_index(l34_rd(l, L2_STS));
	return ret < 0 ? -ENOSPC : ret;
}

static int l2uc_access(struct rtl9602c_l34 *l, u16 idx, u32 *w,
		       bool by_mac, bool write)
{
	/* Address mode is read-only; updates retain the complete MAC/VLAN key. */
	u32 cmd = (by_mac || write) ? 0 : (L2_METHOD_ADDR << L2_CMD_METHOD_SH) |
				     ((u32)idx << L2_CMD_ADDR_SH);
	int ret;
	unsigned int i;

	ret = l2_busy_wait(l);
	if (ret)
		return ret;
	if (by_mac || write)
		for (i = 0; i < L34_WORDS_L2UC; i++)
			l34_wr(l, L2_WDATA + 4 * i, w[i]);
	l34_wr(l, L2_CMD, cmd | (write ? L2_CMD_WR : 0));
	ret = l2_busy_wait(l);
	if (ret)
		return ret;
	ret = l34_l2uc_sts_index(l34_rd(l, L2_STS));
	if (ret < 0)
		return -ENOENT;
	if (!write)
		for (i = 0; i < L34_WORDS_L2UC; i++)
			w[i] = l34_rd(l, L2_RDATA + 4 * i);
	return ret;
}

static bool l2uc_matches(const u32 *w, const u8 *mac)
{
	u32 key[L34_WORDS_L2UC] = {};

	l34_mac48_set(key, L2UC_MAC_LSP, mac);
	return l34_field_get(w, L2UC_VALID_LSP, L2UC_VALID_W) &&
	       w[0] == key[0] && (w[1] & 0xffff) == key[1];
}

/* Caller holds the table lock; failed clears retain their resource owner. */
static int l34_neigh_clear(struct rtl9602c_l34 *l, unsigned int slot)
{
	struct l34_neigh *n = &l->lan_neigh[slot];
	u32 w[L34_WORDS_L2UC] = {};
	unsigned int i;
	int ret;

	n->ready = false;
	if (n->arp_live) {
		ret = l34_tbl_write(l, L34_TBL_ARP, slot, w, L34_WORDS_ARP);
		if (ret)
			return ret;
		n->arp_live = false;
	}
	if (n->l2_pinned && !n->arp_was_used) {
		for (i = 0; i < ARRAY_SIZE(l->lan_neigh); i++)
			if (&l->lan_neigh[i] != n && l->lan_neigh[i].l2_pinned &&
			    l->lan_neigh[i].l2idx == n->l2idx)
				goto released;
		ret = l2uc_access(l, n->l2idx, w, false, false);
		if (ret < 0 && ret != -ENOENT)
			return ret;
		if (ret >= 0 && l2uc_matches(w, n->mac)) {
			l34_field_set(w, L2UC_ARPUSED_LSP, L2UC_ARPUSED_W, 0);
			ret = l2uc_access(l, n->l2idx, w, false, true);
			if (ret < 0)
				return ret;
		} else {
			pr_warn_ratelimited("[NAT-006] rtl9602c-l34: neighbour L2 slot %u changed before unpin\n",
					    n->l2idx);
		}
	}
released:
	n->l2_pinned = false;
	return 0;
}

static int l34_neigh_del(struct rtl9602c_l34 *l, struct l34_flow *f)
{
	struct l34_neigh *n = &l->lan_neigh[f->hw_index];
	int ret;

	if (n->users > 1) {
		n->users--;
		f->neigh_owned = false;
		return 0;
	}
	ret = l34_neigh_clear(l, f->hw_index);
	if (ret)
		return ret;
	memset(n, 0, sizeof(*n));
	f->neigh_owned = false;
	return 0;
}

static int l34_neigh_add(struct rtl9602c_l34 *l, struct l34_flow *f,
			 u32 ip, const u8 *mac)
{
	u32 w[L34_WORDS_L2UC] = {}, arp[L34_WORDS_ARP] = {};
	struct l34_neigh *n;
	unsigned int i, slot;
	bool moving = false;
	int l2idx, ret;

	if (!l->ready || !l->provisioned)
		return -ENODEV;
	if (f->out_owned || f->in_owned || f->neigh_owned)
		return -EBUSY;
	mutex_lock(&l->lock);
	for (i = 0; i < ARRAY_SIZE(l->lan_neigh); i++) {
		n = &l->lan_neigh[i];
		if (!n->users || n->ip != ip)
			continue;
		ret = -EBUSY;
		if (!n->ready)
			goto out;
		ret = -ENOSPC;
		if (n->users == 0xffff)
			goto out;
		if (memcmp(n->mac, mac, 6)) {
			slot = i;
			moving = true;
			goto resolve;
		}
		n->users++;
		f->hw_index = i;
		f->neigh_owned = true;
		ret = 0;
		goto out;
	}
	for (slot = 0; slot < ARRAY_SIZE(l->lan_neigh); slot++) {
		if (l->lan_neigh[slot].users)
			continue;
		ret = l34_tbl_read(l, L34_TBL_ARP, slot, arp, L34_WORDS_ARP);
		if (ret)
			goto out;
		if (!l34_field_get(arp, L34_ARP_VALID_LSP, L34_ARP_VALID_W))
			break;
	}
	ret = -ENOSPC;
	if (slot == ARRAY_SIZE(l->lan_neigh))
		goto out;
resolve:
	l34_mac48_set(w, L2UC_MAC_LSP, mac);
	l2idx = l2uc_access(l, 0, w, true, false);
	ret = l2idx;
	if (ret < 0)
		goto out;
	ret = -EOPNOTSUPP;
	if (!l2uc_matches(w, mac) ||
	    l34_field_get(w, L2UC_SPA_LSP, L2UC_SPA_W) >= GMAC_PON_PORT)
		goto out;
	n = &l->lan_neigh[slot];
	if (moving) {
		/* Existing flows retain ownership throughout the MAC move. */
		ret = l34_neigh_clear(l, slot);
		if (ret)
			goto out;
	}
	n->ip = ip;
	n->l2idx = l2idx;
	if (!moving) {
		n->users = 1;
		f->hw_index = slot;
		f->neigh_owned = true;
	}
	memcpy(n->mac, mac, 6);
	n->arp_was_used = l34_field_get(w, L2UC_ARPUSED_LSP, L2UC_ARPUSED_W);
	for (i = 0; i < ARRAY_SIZE(l->lan_neigh); i++)
		if (l->lan_neigh[i].l2_pinned && l->lan_neigh[i].l2idx == l2idx)
			n->arp_was_used = l->lan_neigh[i].arp_was_used;
	n->l2_pinned = true;
	l34_field_set(w, L2UC_ARPUSED_LSP, L2UC_ARPUSED_W, 1);
	ret = l2uc_access(l, l2idx, w, false, true);
	if (ret < 0)
		goto out;
	memset(arp, 0, sizeof(arp));
	l34_arp_encode(arp, ip, l2idx);
	n->arp_live = true;
	ret = l34_tbl_write(l, L34_TBL_ARP, slot, arp, L34_WORDS_ARP);
	if (!ret) {
		n->ready = true;
		if (moving) {
			n->users++;
			f->hw_index = slot;
			f->neigh_owned = true;
		}
	}
out:
	mutex_unlock(&l->lock);
	return ret;
}

/* Caller holds the L2/L34 table lock. */
static int l34_cf_write(struct rtl9602c_l34 *l, u8 type, u16 addr,
			const u32 *w, unsigned int n)
{
	unsigned int i;
	int ret = l2_busy_wait(l);

	if (ret)
		return ret;
	for (i = 0; i < n; i++)
		l34_wr(l, L2_WDATA + 4 * i, w[i]);
	l34_wr(l, L2_CMD, (addr << L2_CMD_ADDR_SH) | L2_CMD_WR |
		(L2_METHOD_ADDR << L2_CMD_METHOD_SH) | type);
	return l2_busy_wait(l);
}

static int l34_cf_wan_setup(struct rtl9602c_l34 *l, u8 idx)
{
	u32 rule[2], mask[2], action[3], zero[2] = { 0 };
	u32 cfg = l34_rd(l, L34_CF_CFG);
	int ret;

	if (idx >= L2_CF_RULE_BASE - ((cfg >> L34_CF_P1_COUNT_SH) & 255))
		return -ENOSPC;
	l34_cf_wan_encode(rule, mask, action, idx, GPON_DATA_FLOW);
	/* Publish validity last; a failed write cannot expose a partial action. */
	ret = l34_cf_write(l, L2_CF_MATCH_TYPE, L2_CF_RULE_BASE + idx, zero, 2);
	if (ret)
		return ret;
	ret = l34_cf_write(l, L2_CF_MATCH_TYPE, idx, mask, 2);
	if (ret)
		return ret;
	ret = l34_cf_write(l, L2_CF_ACTION_TYPE, idx, action, 3);
	if (ret)
		return ret;
	l34_wr(l, L34_CF_ACTION_CTRL + 4 * idx, L34_CF_SID_ENABLE);
	/* Unrouted L2 traffic must not inherit WAN netif 0. Slot 15 is unused. */
	cfg |= 15u << L34_CF_L2_WAN_SH;
	l34_wr(l, L34_CF_CFG, cfg);
	return l34_cf_write(l, L2_CF_MATCH_TYPE, L2_CF_RULE_BASE + idx, rule, 2);
}

int rtl9602c_l34_wan_setup(struct rtl9602c_l34 *l, u8 idx, u32 wan_ip,
			   const u8 *wan_mac, u32 gw_ip, const u8 *gw_mac,
			   u8 wan_port, u16 vlan, u16 pppoe_sid)
{
	/*
	 * The PPPoE slot is step 7, in the same atomic program as the next hop
	 * that points at it. Writing them separately would leave a window in
	 * which the next hop is typed PPPoE while its slot still holds the
	 * previous session, putting the wrong session id on the wire. pppoe_sid
	 * == 0 keeps the ethernet next hop and writes no slot.
	 */
	struct l34_prog_step step[8] = { { 0 } };
	unsigned int nsteps = 7;
	int l2idx, ret;

	if (!l->ready)
		return -ENODEV;
	if (idx >= L34_EXTIP_SLOTS)	/* NAPTR EXTIP_IDX is only 3 bits wide */
		return -EINVAL;

	mutex_lock(&l->lock);
	/* The NETIF entry is a CLAIM: rewriting it under a live flow moves
	 * that flow's egress without its owner ever being told. */
	if (l34_has_owners(l)) {
		ret = -EBUSY;
		goto out;
	}
	if (wan_port == GMAC_PON_PORT) {
		ret = l34_cf_wan_setup(l, idx);
		if (ret) {
			pr_err("[NAT-007] rtl9602c-l34: WAN classifier setup failed (%d)\n", ret);
			goto out;
		}
	}
	if (!l->engine_on)
		l34_engine_on(l);

	/* gateway dst MAC -> L2 unicast table; capture the assigned index. It
	 * is an ALLOCATION, so it precedes the program rather than joining it. */
	l2idx = l2uc_add_static(l, gw_mac, wan_port);
	if (l2idx < 0) {
		ret = l2idx;
		goto out;
	}

	/*
	 * Step 0 is the claim (see struct l34_prog_step in flowcore): a NETIF
	 * entry takes traffic the instant it is written, so it is what a later
	 * failure must revoke. NETIF[idx]: egress source MAC + VLAN/MTU/IP.
	 */
	step[0].tbl = L34_TBL_NETIF;
	step[0].idx = idx;
	step[0].words = L34_WORDS_NETIF;
	/* Untagged frames still carry the switch's internal PVID; a tagged WAN
	 * egresses with its VID, which is what stock's isCtagIf=1 says. */
	l34_netif_encode(step[0].w, wan_mac, wan_ip,
			 vlan ? vlan : SW_WAN_PVID);
	l34_field_set(step[0].w, L34_NETIF_CTAG_LSP, L34_NETIF_CTAG_W, vlan != 0);

	/* Lower-half WAN route via NEXTHOP[idx]. */
	step[1].tbl = L34_TBL_L3ROUTE;
	step[1].idx = idx;
	step[1].words = L34_WORDS_L3ROUTE;
	l34_rt_wan_encode(step[1].w, idx);

	/* NEXTHOP[idx]: ethernet next-hop via NETIF[idx], dst MAC = L2[l2idx] */
	step[2].tbl = L34_TBL_NEXTHOP;
	step[2].idx = idx;
	step[2].words = L34_WORDS_NEXTHOP;
	if (pppoe_sid) {
		/* PPPOE[idx]: the session this interface negotiated. One slot per
		 * WAN netif, so the index is the netif's own -- the table's 8
		 * slots cover every interface this driver can provision. */
		step[7].tbl = L34_TBL_PPPOE;
		step[7].idx = idx;
		step[7].words = L34_WORDS_PPPOE;
		l34_pppoe_encode(step[7].w, pppoe_sid);
		nsteps = 8;
		/* KEEP_ORIGINAL_OR_ADD: an upstream frame arrives from the LAN
		 * with no session header and must gain one; a frame that already
		 * carries the session keeps it. REPLACE would rewrite a header
		 * that is already correct, and KEEP alone would emit an
		 * unencapsulated frame on a PPPoE WAN. */
		l34_nexthop_pppoe_encode(step[2].w, idx, l2idx, idx,
					 L34_NH_PPPOE_KEEP_OR_ADD);
	} else {
		l34_nexthop_encode(step[2].w, idx, l2idx);
	}

	/* EXTIP[idx]: the WAN source IP a NAPT rewrite applies, via NEXTHOP[idx] */
	step[3].tbl = L34_TBL_EXTIP;
	step[3].idx = idx;
	step[3].words = L34_WORDS_EXTIP;
	l34_extip_encode(step[3].w, wan_ip, idx);

	/* ARP entry: gateway IP -> the same L2 entry (placed in the WAN half). */
	step[4].tbl = L34_TBL_ARP;
	step[4].idx = L34_ARP_WAN_BASE + idx;
	step[4].words = L34_WORDS_ARP;
	l34_arp_encode(step[4].w, gw_ip, l2idx);

	/*
	 * The /32 CPU self-route for the WAN address, which a NATed reply is
	 * addressed to. Without it l34_iface_blackholes() is true for the WAN
	 * netif: the engine claims traffic for this interface and nothing
	 * terminates its own address locally.
	 */
	step[5].tbl = L34_TBL_L3ROUTE;
	step[5].idx = idx + L34_RT_CPU_SLOT_OFF;
	step[5].words = L34_WORDS_L3ROUTE;
	l34_rt_cpu_encode(step[5].w, wan_ip, idx);

	/* Upper-half WAN route via the same nexthop. */
	step[6].tbl = L34_TBL_L3ROUTE;
	step[6].idx = L34_RT_POOL_BASE + idx;
	step[6].words = L34_WORDS_L3ROUTE;
	l34_rt_wan_net_encode(step[6].w, 0x80000000u, 1, idx);

	ret = l34_prog_run(step, nsteps, l34_prog_wr, l);
out:
	mutex_unlock(&l->lock);
	return ret;
}

/* Program a LAN-side interface */
int rtl9602c_l34_lan_setup(struct rtl9602c_l34 *l, u8 idx, u32 lan_ip,
			   const u8 *lan_mac, u32 lan_net, u8 prefix, u16 vlan)
{
	struct l34_prog_step step[3] = { { 0 } };
	int ret;

	if (!l->ready)
		return -ENODEV;
	if (idx >= L34_EXTIP_SLOTS || prefix < 1 || prefix > 32)
		return -EINVAL;

	/* Step 0 is the claim; on this side it is the ONU's. */
	step[0].tbl = L34_TBL_NETIF;
	step[0].idx = idx;
	step[0].words = L34_WORDS_NETIF;
	l34_netif_encode(step[0].w, lan_mac, lan_ip,
			 vlan ? vlan : SW_DEFAULT_VID);

	/* LAN subnet route (mask = prefix code: the off-by-one fact is pinned
	 * at l34_rt_lan_encode() in flowcore) */
	step[1].tbl = L34_TBL_L3ROUTE;
	step[1].idx = idx;
	step[1].words = L34_WORDS_L3ROUTE;
	l34_rt_lan_encode(step[1].w, lan_net, prefix, idx);

	/* The /32 CPU self-route. */
	step[2].tbl = L34_TBL_L3ROUTE;
	step[2].idx = idx + L34_RT_CPU_SLOT_OFF;
	step[2].words = L34_WORDS_L3ROUTE;
	l34_rt_cpu_encode(step[2].w, lan_ip, idx);

	mutex_lock(&l->lock);
	/* The NETIF entry is a CLAIM: rewriting it under a live flow moves
	 * that flow's egress without its owner ever being told. */
	if (l34_has_owners(l)) {
		ret = -EBUSY;
		goto out;
	}
	if (!l->engine_on)
		l34_engine_on(l);
	ret = l34_prog_run(step, ARRAY_SIZE(step), l34_prog_wr, l);
out:
	mutex_unlock(&l->lock);
	return ret;
}

/*
 * The WAN address the tables describe has left the kernel: invalidate every WAN
 * row wan_setup() programs, the NETIF claim first. Left in place, NETIF[WAN]
 * keeps claiming frames to the WAN MAC and routes a unicast DHCP OFFER away
 * from the CPU, and with no lease no flow re-provisions it. Refused while a
 * flow still owns the tables.
 */
/*
 * Caller holds the table mutex. core_empty: the offload core holds no flow, so
 * a reservation still standing belongs to nobody. A safety net that must never
 * fire. Its row is zeroed and its index returned.
 */
static int l34_sweep_orphans(struct rtl9602c_l34 *l, unsigned int *swept)
{
	u32 zero[L34_WORDS_NAPTR_IN] = { 0 };
	unsigned int i;
	int ret;

	*swept = 0;
	for_each_set_bit(i, l->out_reserved, L34_NAPT_ENTRIES) {
		ret = l34_tbl_write(l, L34_TBL_NAPT_OUT, i, zero, L34_WORDS_NAPT_OUT);
		if (ret)
			return ret;
		__clear_bit(i, l->out_reserved);
		(*swept)++;
	}
	for_each_set_bit(i, l->in_reserved, L34_NAPT_ENTRIES) {
		ret = l34_tbl_write(l, L34_TBL_NAPTR_IN, i, zero, L34_WORDS_NAPTR_IN);
		if (ret)
			return ret;
		__clear_bit(i, l->in_reserved);
		(*swept)++;
	}
	/* ...and a LAN neighbour nobody owns (measured the next sitting: neigh=1
	 * with the core empty) */
	for (i = 0; i < ARRAY_SIZE(l->lan_neigh); i++) {
		if (!l->lan_neigh[i].users)
			continue;
		ret = l34_neigh_clear(l, i);
		if (ret)
			return ret;
		l->lan_neigh[i].users = 0;
		(*swept)++;
	}
	return 0;
}

int rtl9602c_l34_wan_release(struct rtl9602c_l34 *l, bool core_empty)
{
	const struct { enum l34_tbl tbl; u16 idx; unsigned int words; } rows[] = {
		{ L34_TBL_NETIF,   L34_NETIF_WAN, L34_WORDS_NETIF },
		{ L34_TBL_L3ROUTE, L34_NETIF_WAN, L34_WORDS_L3ROUTE },
		{ L34_TBL_L3ROUTE, L34_NETIF_WAN + L34_RT_CPU_SLOT_OFF, L34_WORDS_L3ROUTE },
		{ L34_TBL_L3ROUTE, L34_RT_POOL_BASE + L34_NETIF_WAN, L34_WORDS_L3ROUTE },
		{ L34_TBL_NEXTHOP, L34_NETIF_WAN, L34_WORDS_NEXTHOP },
		{ L34_TBL_EXTIP,   L34_NETIF_WAN, L34_WORDS_EXTIP },
		{ L34_TBL_PPPOE,   L34_NETIF_WAN, L34_WORDS_PPPOE },
		{ L34_TBL_ARP,     L34_ARP_WAN_BASE + L34_NETIF_WAN, L34_WORDS_ARP },
	};
	u32 zero[L34_WORDS_NETIF] = { 0 };
	unsigned int i;
	int ret = 0;

	if (!l->ready)
		return -ENODEV;
	mutex_lock(&l->lock);
	if (core_empty) {
		unsigned int swept;

		ret = l34_sweep_orphans(l, &swept);
		if (ret)
			goto out;
		WARN(swept, "rtl9602c-l34: %u orphaned reservation(s) swept (NAPT + LAN neighbour): no flow owned them\n",
		     swept);
	}
	if (l34_has_owners(l)) {
		unsigned int nu, no, ni;

		l34_owner_counts(l, &nu, &no, &ni);
		pr_warn("[NAT-008] rtl9602c-l34: WAN rows kept: owners neigh=%u out_reserved=%u in_reserved=%u\n",
			nu, no, ni);
		ret = -EBUSY;
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(rows) && !ret; i++)
		ret = l34_tbl_write(l, rows[i].tbl, rows[i].idx, zero, rows[i].words);
	if (!ret)
		l->provisioned = false;
out:
	mutex_unlock(&l->lock);
	return ret;
}

/* Program BOTH interface slots from one reading of the */
int rtl9602c_l34_provision(struct rtl9602c_l34 *l, const struct gpon_edge *e)
{
	int ret;

	l->provisioned = false;
	ret = rtl9602c_l34_wan_setup(l, L34_NETIF_WAN, e->wan_ip, e->wan_mac,
				     e->gw_ip, e->gw_mac, GMAC_PON_PORT,
				     e->wan_vlan, e->wan_pppoe_sid);
	if (ret)
		return ret;
	ret = rtl9602c_l34_lan_setup(l, L34_NETIF_LAN, e->lan_ip, e->lan_mac,
				     e->lan_net, e->lan_prefix, e->lan_vlan);
	if (ret)
		return ret;

	l->edge = *e;
	l->provisioned = true;
	pr_info("rtl9602c-l34: provisioned WAN %pI4h via %pI4h (%pM) on netif %u, LAN %pI4h/%u on netif %u\n",
		&e->wan_ip, &e->gw_ip, e->gw_mac, L34_NETIF_WAN,
		&e->lan_net, e->lan_prefix, L34_NETIF_LAN);
	return 0;
}

/* /proc/flowdump: read the outbound NAPT hit bitmap */
static ssize_t l34_proc_write(struct file *fp, const char __user *ub,
			      size_t n, loff_t *off)
{
	struct rtl9602c_l34 *l = pde_data(file_inode(fp));
	u8 wmac[6], gmac[6];
	char buf[192];
	int ret = 0;

	if (n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ub, n))
		return -EFAULT;
	buf[n] = '\0';

	if (buf[0] == 'w') {
		unsigned int port, vlan;
		u32 wip, gip;

		if (sscanf(buf, "w %2hhx%2hhx%2hhx%2hhx%2hhx%2hhx %x %x %2hhx%2hhx%2hhx%2hhx%2hhx%2hhx %u %u",
			   &wmac[0], &wmac[1], &wmac[2], &wmac[3], &wmac[4], &wmac[5],
			   &wip, &gip,
			   &gmac[0], &gmac[1], &gmac[2], &gmac[3], &gmac[4], &gmac[5],
			   &port, &vlan) != 16)
			return -EINVAL;
		ret = rtl9602c_l34_wan_setup(l, L34_NETIF_WAN, wip, wmac, gip, gmac,
					     port, vlan, 0);
		pr_info("rtl9602c_l34: wan_setup -> %d\n", ret);
	} else if (buf[0] == 'l') {
		unsigned int prefix, vlan;
		u32 lip, lnet;

		if (sscanf(buf, "l %2hhx%2hhx%2hhx%2hhx%2hhx%2hhx %x %x %u %u",
			   &wmac[0], &wmac[1], &wmac[2], &wmac[3], &wmac[4], &wmac[5],
			   &lip, &lnet, &prefix, &vlan) != 10)
			return -EINVAL;
		ret = rtl9602c_l34_lan_setup(l, L34_NETIF_LAN, lip, wmac, lnet,
					     prefix, vlan);
		pr_info("rtl9602c_l34: lan_setup -> %d\n", ret);
	} else if (buf[0] == 'f') {
		/*
		 * Retired: a stack-local l34_flow now reserves both indices,
		 * and nothing outlives this call to release them, so one manual
		 * add would take a NAPT pair out of service until the module is
		 * reloaded. Install through TC, whose per-entry owner can retry
		 * the cleanup.
		 */
		return -EOPNOTSUPP;
	} else if (buf[0] == 'x') {
		/* Raw indirect table write */
		unsigned int tbl, tidx;
		u32 w[L34_WORDS_MAX] = { 0 };
		int got = sscanf(buf, "x %u %u %x %x %x %x",
				 &tbl, &tidx, &w[0], &w[1], &w[2], &w[3]);

		if (got < 3)
			return -EINVAL;
		mutex_lock(&l->lock);
		/*
		 * The raw verb can address a row a live flow owns, and the
		 * owner would never learn its entry had been rewritten. One
		 * exit, so no unlock appears before the write in the source.
		 */
		ret = -EBUSY;
		if (!l34_has_owners(l))
			ret = l34_tbl_write(l, (enum l34_tbl)tbl, tidx, w,
					    got - 2);
		mutex_unlock(&l->lock);
		pr_info("rtl9602c_l34: raw write tbl %u idx %u words %u -> %d\n",
			tbl, tidx, got - 2, ret);
	} else {
		return -EINVAL;
	}
	return ret ? ret : n;
}

/* hits_seen is rate-dependent, and the node says so. */
/*
 * CONFIG_GPON_FLOW_DIAG gates the read only. The write below is a control, not
 * a measurement: dropping it with a diagnostic flag would change behaviour.
 */
#if IS_ENABLED(CONFIG_GPON_FLOW_DIAG)

static const char *const l34_rt_process_name[4] = { "CPU", "DROP", "ARP", "NH" };

static void l34_proc_show_rt(struct seq_file *sf, struct rtl9602c_l34 *l,
			     u16 idx)
{
	u32 w[L34_WORDS_L3ROUTE] = { 0 };
	u32 ip, mask, proc;

	if (l34_tbl_read(l, L34_TBL_L3ROUTE, idx, w, L34_WORDS_L3ROUTE)) {
		seq_printf(sf, "route[%2u] READ FAILED\n", idx);
		return;
	}
	if (!l34_field_get(w, L34_RT_VALID_LSP, L34_RT_VALID_W)) {
		seq_printf(sf, "route[%2u] invalid raw %08x %08x\n",
			   idx, w[0], w[1]);
		return;
	}
	ip = l34_field_get(w, L34_RT_IP_LSP, L34_RT_IP_W);
	mask = l34_field_get(w, L34_RT_MASK_LSP, L34_RT_MASK_W);
	proc = l34_field_get(w, L34_RT_PROCESS_LSP, L34_RT_PROCESS_W);
	seq_printf(sf,
		   "route[%2u] %pI4h maskcode %u (/%u) process %s int %u netif %u rt2wan %u raw %08x %08x\n",
		   idx, &ip, mask, mask + 1, l34_rt_process_name[proc],
		   l34_field_get(w, L34_RT_INT_LSP, L34_RT_INT_W),
		   l34_field_get(w, L34_RT_DENTIF_LSP, L34_RT_DENTIF_W),
		   l34_field_get(w, L34_RT_RT2WANINF_LSP, L34_RT_RT2WANINF_W),
		   w[0], w[1]);
}

static void l34_proc_show_iface(struct seq_file *sf, struct rtl9602c_l34 *l)
{
	u32 netif[L34_WORDS_NETIF] = { 0 };
	u32 cpu_rt[L34_WORDS_L3ROUTE] = { 0 };
	u8 mac[6];
	unsigned int i;
	u32 ip, v, mac_lo, mac_hi, valid;

	if (!l->engine_on) {
		seq_puts(sf, "engine off -- interface tables not read back\n");
		return;
	}
	/* The engine control words, read back. */
	v = l34_rd(l, L34_SWTCR0);
	seq_printf(sf,
		   "swtcr0 %08x natmode %u limdbc %u v4flrt %u v6flrt %u\nglb_cfg %08x\n",
		   v, (v >> L34_SWTCR0_NATMODE_SH) & 0x3,
		   (v >> L34_SWTCR0_LIMDBC_SH) & 0x3,
		   (v & L34_SWTCR0_V4FLRT_EN) ? 1 : 0,
		   (v & L34_SWTCR0_V6FLRT_EN) ? 1 : 0,
		   l34_rd(l, L34_GLB_CFG));
	/*
	 * The die's own lookup log. HSB is the hash source block the engine
	 * built from the last frame it looked up; HSA is what it decided.
	 * Printed raw and whole, with no field split that has not been
	 * measured. All-zero means the log is off, not that nothing was looked
	 * up, so the mode in HSBA_CTRL is printed beside it. Under LOG_ALL the
	 * last lookup is the ssh packet carrying the reading command;
	 * FIRST_DROP latches.
	 */
	v = l34_rd(l, L34_HSBA_CTRL);
	seq_printf(sf, "hsba_ctrl %08x log_mode %u (1=off 2=all[SELF-CAPTURING] 3=first-drop)\n",
		   v, (v >> L34_HSBA_TST_LOG_MD_SH) & L34_HSBA_TST_LOG_MD_M);
	seq_puts(sf, "hsb");
	for (i = 0; i < L34_HSB_WORDS; i++)
		seq_printf(sf, " %08x", l34_rd(l, L34_HSB_DESC0 + i * 4));
	seq_puts(sf, "\nhsa");
	for (i = 0; i < L34_HSA_WORDS; i++)
		seq_printf(sf, " %08x", l34_rd(l, L34_HSA_DESC0 + i * 4));
	seq_putc(sf, '\n');
	/* NETIF has 16 entries even though EXTIP can reference only eight.
	 * Keep invalid entries visible: stale fields and a failed read are
	 * different observations. All twelve fields come from this die's own
	 * stock table descriptor (k0_kernel, file offset 0xcb2d68). */
	for (i = 0; i < L34_NETIF_SLOTS; i++) {
		if (l34_tbl_read(l, L34_TBL_NETIF, i, netif, L34_WORDS_NETIF)) {
			seq_printf(sf, "netif[%u] READ FAILED\n", i);
			continue;
		}
		valid = l34_field_get(netif, L34_NETIF_VALID_LSP,
				      L34_NETIF_VALID_W);
		ip = l34_field_get(netif, L34_NETIF_IP_LSP, L34_NETIF_IP_W);
		mac_lo = l34_field_get(netif, L34_NETIF_GMAC_LSP, 32);
		mac_hi = l34_field_get(netif, L34_NETIF_GMAC_LSP + 32, 16);
		mac[0] = mac_hi >> 8;
		mac[1] = mac_hi;
		mac[2] = mac_lo >> 24;
		mac[3] = mac_lo >> 16;
		mac[4] = mac_lo >> 8;
		mac[5] = mac_lo;
		seq_printf(sf,
			   "netif[%u] %pI4h vlan %u claims %u raw %08x %08x %08x %08x valid %u mac %pM mask %u routing %u mtu %u l34 %u dslite %u dslite_idx %u ctag %u ipv6 %u\n",
			   i, &ip,
			   l34_field_get(netif, L34_NETIF_VLANID_LSP,
					 L34_NETIF_VLANID_W),
			   l34_netif_claims(netif) ? 1 : 0,
			   netif[0], netif[1], netif[2], netif[3], valid, mac,
			   l34_field_get(netif, L34_NETIF_MACMASK_LSP, L34_NETIF_MACMASK_W),
			   l34_field_get(netif, L34_NETIF_ENRTR_LSP, L34_NETIF_ENRTR_W),
			   l34_field_get(netif, L34_NETIF_MTU_LSP, L34_NETIF_MTU_W),
			   l34_field_get(netif, L34_NETIF_L34_LSP, L34_NETIF_L34_W),
			   l34_field_get(netif, L34_NETIF_DSLITE_LSP, L34_NETIF_DSLITE_W),
			   l34_field_get(netif, L34_NETIF_DSLITE_IDX_LSP, L34_NETIF_DSLITE_IDX_W),
			   l34_field_get(netif, L34_NETIF_CTAG_LSP, L34_NETIF_CTAG_W),
			   l34_field_get(netif, L34_NETIF_IPV6_LSP, L34_NETIF_IPV6_W));
		if (!valid || i >= L34_EXTIP_SLOTS)
			continue;
		l34_proc_show_rt(sf, l, i);
		l34_proc_show_rt(sf, l, i + L34_RT_CPU_SLOT_OFF);
		/* the safety verdict, on what came BACK from the silicon */
		if (l34_tbl_read(l, L34_TBL_L3ROUTE, i + L34_RT_CPU_SLOT_OFF,
				 cpu_rt, L34_WORDS_L3ROUTE))
			continue;
		seq_printf(sf, "netif[%u] blackholes %u\n", i,
			   l34_iface_blackholes(netif, cpu_rt, ip, i) ? 1 : 0);
	}
	/*
	 * The general LPM pool, which the loop above cannot reach. It prints
	 * route[i] and route[i+8] for valid netifs only, and a real route goes
	 * in L3ROUTE[2..7], since netifs 2..7 are never provisioned.
	 */
	for (i = 0; i < L34_RT_POOL_N; i++)
		l34_proc_show_rt(sf, l, L34_RT_POOL_BASE + i);

}

/*
 * The NAPT entries this driver most recently installed, read back from the
 * silicon and decoded, to answer whether an installed key matches the traffic.
 * It reads what the hardware holds, never what the driver believes it wrote: an
 * entry the silicon rejected or rewrote is the case worth seeing.
 */
static void l34_proc_show_recent(struct seq_file *sf, struct rtl9602c_l34 *l)
{
	u32 w[L34_WORDS_NAPTR_IN] = { 0 };
	unsigned int i, n;

	n = l->offer_n < L34_RECENT ? l->offer_n : L34_RECENT;
	seq_printf(sf, "recent flow keys OFFERED (%u of %u):\n", n, l->offer_n);
	for (i = 0; i < n; i++) {
		const struct l34_offer *o = &l->recent_offer[i];

		seq_printf(sf, "  offer %pI4h:%u -> %pI4h:%u proto %u iif %s ds_leg %u\n",
			   &o->sip, o->sport, &o->dip, o->dport, o->proto,
			   o->iif[0] ? o->iif : "-", o->ds_leg);
	}

	n = l->recent_n < L34_RECENT ? l->recent_n : L34_RECENT;
	seq_printf(sf, "recent NAPT entries (%u of %u installed):\n",
		   n, l->recent_n);
	for (i = 0; i < n; i++) {
		u16 idx = l->recent_idx[i];
		u32 out[L34_WORDS_NAPT_OUT] = { 0 };
		u32 intip, extport, intport, tcp, valid, in_idx;

		/*
		 * Two index spaces. hw_index addresses NAPT_OUT (the hash
		 * slot), and that word carries the NAPTR_IN index
		 * (L34_NAPT_HASHIN_IDX). Reading NAPTR_IN at the NAPT_OUT index
		 * prints all-zero entries. The hit bitmap is indexed by
		 * hw_index too.
		 */
		if (l34_tbl_read(l, L34_TBL_NAPT_OUT, idx, out,
				 L34_WORDS_NAPT_OUT)) {
			seq_printf(sf, "  napt_out[%4u] READ FAILED\n", idx);
			continue;
		}
		in_idx = l34_field_get(out, L34_NAPT_HASHIN_IDX_LSP, 12);
		seq_printf(sf, "  napt_out[%4u] valid %u -> naptr_in[%u] raw %08x\n",
			   idx, l34_field_get(out, L34_NAPT_VALID_LSP, 1),
			   in_idx, out[0]);
		if (l34_tbl_read(l, L34_TBL_NAPTR_IN, in_idx, w,
				 L34_WORDS_NAPTR_IN)) {
			seq_printf(sf, "  naptr[%4u] READ FAILED\n", in_idx);
			continue;
		}
		idx = (u16)in_idx;
		intip   = l34_field_get(w, L34_NAPTR_INTIP_LSP, 32);
		intport = l34_field_get(w, L34_NAPTR_INTPORT_LSP, 16);
		extport = l34_field_get(w, L34_NAPTR_EXTPORT_LSP, 16);
		tcp     = l34_field_get(w, L34_NAPTR_TCP_LSP, 1);
		valid   = l34_field_get(w, L34_NAPTR_VALID_LSP, 2);
		seq_printf(sf,
			   "  naptr[%4u] %pI4h:%u -> extport %u %s valid %u extipidx %u raw %08x %08x %08x\n",
			   idx, &intip, intport, extport, tcp ? "tcp" : "udp",
			   valid,
			   l34_field_get(w, L34_NAPTR_EXTIPIDX_LSP, 3),
			   w[0], w[1], w[2]);
	}
}

static int l34_proc_show(struct seq_file *sf, void *v)
{
	struct rtl9602c_l34 *l = sf->private;
	unsigned int i, now = 0;

	/*
	 * The gate's own state comes first, because this node exists even when
	 * the engine is off and has to say why it installs nothing.
	 *
	 * Before mutex_lock: with the gate closed rtl9602c_l34_init() never
	 * ran, so l->lock was never initialised. l->ready is safe to read
	 * because the netdev's private area is zeroed by devm_alloc_etherdev()
	 * long before this can be opened.
	 */
	if (!l || !l->ready) {
		seq_puts(sf,
			 "engine NOT INITIALISED\n"
			 "The L34 flow engine is gated on the rtl9602c_eth.hw_nat module\n"
			 "parameter and it is 0 in this boot, so the engine was never brought\n"
			 "up and nothing below it was ever established. This is a CONFIG\n"
			 "state, not a device finding: no flow was refused, because none was\n"
			 "ever offered to an engine that does not exist.\n");
		return 0;
	}

	/* A table read writes the shared command register. Serialize the whole
	 * snapshot against flow installs, raw writes and other readers. */
	mutex_lock(&l->lock);

	/*
	 * A diag reads the shadow and consumes nothing: reading the
	 * clear-on-read register itself would clear bits the flowtable's
	 * liveness op is about to test.
	 *
	 * The sweep runs here so the flow-diag line reports hits_seen from this
	 * read's own sweep, not the previous one.
	 */
	l34_hit_sweep(l);
	seq_printf(sf, "provisioned %u\n", l->provisioned ? 1 : 0);
	{
		unsigned int nu, no, ni;

		l34_owner_counts(l, &nu, &no, &ni);
		seq_printf(sf, "owners neigh=%u out_reserved=%u in_reserved=%u\n",
			   nu, no, ni);
	}
	if (l->provisioned)
		seq_printf(sf, "wan %pI4h via %pI4h %pM netif %u\nlan %pI4h/%u %pM netif %u\n",
			   &l->edge.wan_ip, &l->edge.gw_ip, l->edge.gw_mac,
			   L34_NETIF_WAN, &l->edge.lan_net, l->edge.lan_prefix,
			   l->edge.lan_mac, L34_NETIF_LAN);
	/*
	 * The core's own line, identical on every family: which of the
	 * lifecycle's sixteen refusal causes fired, and how often. The four
	 * fields below it are what only this driver can read; cap is the chip's
	 * own table size and hits its hit witness.
	 */
	{
		struct gpon_flow_diag d = {
			.valid = GPON_FDIAG_HAS_OFFERED |
				 GPON_FDIAG_HAS_LIVE |
				 GPON_FDIAG_HAS_CAPACITY |
				 GPON_FDIAG_HAS_HITS,
			.offered = l->offered,
			.live = l->installs - l->removals,
			.capacity = L34_NAPT_ENTRIES,
			.hw_hits = l->hits_seen,
		};
		char line[256];

		gpon_flow_offload_diag(l->fo, &d, line, sizeof(line));
		seq_printf(sf, "%s\n", line);
	}
	seq_printf(sf, "binds %u\noffered %u\ninstalls %u\nremovals %u\nrefusals %u\nds_legs %u\n",
		   l->binds, l->offered, l->installs,
		   l->removals, l->refusals, l->ds_legs);
	/* The few lines that decide, on demand rather than as a log flood. */
	if (l->vlan_refused)
		seq_printf(sf, "vlan_refused %u last_vid %u\n",
			   l->vlan_refused, l->vlan_refused_vid);
	if (l->refuse_why)
		seq_printf(sf, "last_refusal %s\n", l->refuse_why);
	/*
	 * The split, which last_refusal cannot give: it names a cause without
	 * saying whether it was 1 of 722 or 722 of 722. Only non-zero rows, so
	 * a healthy run prints nothing here.
	 */
	{
		unsigned int i;

		for (i = 0; i < L34_REFUSE_REASONS; i++)
			if (l->refuse_tally[i].n)
				seq_printf(sf, "refused_by %u %s\n",
					   l->refuse_tally[i].n,
					   l->refuse_tally[i].why);
	}

	l34_proc_show_iface(sf, l);
	l34_proc_show_recent(sf, l);

	seq_puts(sf, "outbound NAPT hit bitmap (set bits = slots matched and not yet consumed by the liveness op):\n");
	for (i = 0; i < ARRAY_SIZE(l->hit_shadow); i++) {
		u32 w = l->hit_shadow[i];

		now += hweight32(w);
		if (w)
			seq_printf(sf, "  [%4u] 0x%08x\n", i * 32, w);
	}
	seq_printf(sf, "hits_now %u\nhits_seen %u\n", now, l->hits_seen);
	mutex_unlock(&l->lock);
	return 0;
}

#else	/* the facility is ABSENT, and the node says so rather than printing
	 * counters nobody measured */
static int l34_proc_show(struct seq_file *sf, void *v)
{
	(void)v;
	seq_puts(sf,
		 "flowdump: diagnostics not compiled in (CONFIG_GPON_FLOW_DIAG=n); the bring-up write still works\n");
	return 0;
}
#endif

static int l34_proc_open(struct inode *ino, struct file *fp)
{
	return single_open(fp, l34_proc_show, pde_data(ino));
}

static const struct proc_ops l34_proc_ops = {
	.proc_open	= l34_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= l34_proc_write,
};

void rtl9602c_l34_proc_init(struct rtl9602c_l34 *l)
{
	proc_create_data("flowdump", 0600, NULL, &l34_proc_ops, l);
}
