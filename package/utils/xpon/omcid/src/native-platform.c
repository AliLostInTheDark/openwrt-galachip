// SPDX-License-Identifier: GPL-2.0-only
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include "native.h"

int omci_native_emit(char *buf, ssize_t at, const char *format, ...)
{
	va_list args;
	int written;

	if (at < 0 || at >= PAGE_SIZE)
		return 0;
	va_start(args, format);
	written = vsnprintf(buf + at, PAGE_SIZE - at, format, args);
	va_end(args);
	return written < 0 ? 0 : min_t(int, written, PAGE_SIZE - at - 1);
}

/* Bounded, sorted storage keeps the MIB upload in class and entity order. */
#define OMCI_NATIVE_MAX_OBJECTS 1024

static size_t lower_bound(const struct xarray *a, unsigned long key)
{
	size_t lo = 0, hi = a->count;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (a->entries[mid].key < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

void *xa_load(const struct xarray *a, unsigned long key)
{
	size_t i = lower_bound(a, key);

	return i < a->count && a->entries[i].key == key ? a->entries[i].value : NULL;
}

void *xa_store(struct xarray *a, unsigned long key, void *value, int flags)
{
	size_t i = lower_bound(a, key);
	void *old;

	(void)flags;
	if (i < a->count && a->entries[i].key == key) {
		old = a->entries[i].value;
		a->entries[i].value = value;
		return old;
	}
	if (a->count == OMCI_NATIVE_MAX_OBJECTS)
		return (void *)(intptr_t)-ENOSPC;
	if (a->count == a->capacity) {
		size_t capacity = a->capacity ? a->capacity * 2 : 32;
		void *entries = realloc(a->entries, capacity * sizeof(*a->entries));

		if (!entries)
			return (void *)(intptr_t)-ENOMEM;
		a->entries = entries;
		a->capacity = capacity;
	}
	memmove(a->entries + i + 1, a->entries + i,
		(a->count - i) * sizeof(*a->entries));
	a->entries[i] = (struct omci_native_entry){ .key = key, .value = value };
	a->count++;
	return NULL;
}

void *xa_erase(struct xarray *a, unsigned long key)
{
	size_t i = lower_bound(a, key);
	void *old;

	if (i == a->count || a->entries[i].key != key)
		return NULL;
	old = a->entries[i].value;
	memmove(a->entries + i, a->entries + i + 1,
		(a->count - i - 1) * sizeof(*a->entries));
	a->count--;
	return old;
}

void *xa_find(const struct xarray *a, unsigned long *key, unsigned long max_key, int filter)
{
	size_t i = lower_bound(a, *key);

	(void)filter;
	for (; i < a->count && a->entries[i].key <= max_key; i++) {
		if (a->entries[i].value) {
			*key = a->entries[i].key;
			return a->entries[i].value;
		}
	}
	return NULL;
}

int xa_reserve(struct xarray *a, unsigned long key, int flags)
{
	return xa_load(a, key) ? 0 : xa_err(xa_store(a, key, NULL, flags));
}

void *xa_find_after(const struct xarray *a, unsigned long *key, unsigned long max_key, int filter)
{
	if (*key == ULONG_MAX)
		return NULL;
	(*key)++;
	return xa_find(a, key, max_key, filter);
}

void xa_release(struct xarray *a, unsigned long key)
{
	if (!xa_load(a, key))
		xa_erase(a, key);
}

bool xa_empty(const struct xarray *a)
{
	unsigned long key = 0;

	return !xa_find(a, &key, ULONG_MAX, XA_PRESENT);
}

struct sk_buff *alloc_skb(size_t len, int flags)
{
	struct sk_buff *skb = calloc(1, sizeof(*skb));

	(void)flags;
	if (!skb)
		return NULL;
	skb->data = malloc(len ? len : 1);
	if (!skb->data) {
		free(skb);
		return NULL;
	}
	skb->capacity = len;
	return skb;
}

void *skb_put_data(struct sk_buff *skb, const void *data, size_t len)
{
	void *dest = skb->data + skb->len;

	assert(len <= skb->capacity - skb->len);
	if (len)
		memcpy(dest, data, len);
	skb->len += len;
	return dest;
}

void kfree_skb(struct sk_buff *skb)
{
	if (skb)
		free(skb->data);
	free(skb);
}

void *kmemdup(const void *data, size_t len, int flags)
{
	void *copy = malloc(len ? len : 1);

	(void)flags;
	if (copy && len)
		memcpy(copy, data, len);
	return copy;
}

void *kmalloc_array(size_t count, size_t size, int flags)
{
	(void)flags;
	return size && count > SIZE_MAX / size ? NULL : malloc(count * size);
}

unsigned long *bitmap_zalloc(unsigned int bits, int flags)
{
	(void)flags;
	return calloc((bits + sizeof(long) * 8 - 1) / (sizeof(long) * 8), sizeof(long));
}

unsigned int bitmap_weight(const unsigned long *map, unsigned int bits)
{
	unsigned int i, n = 0;

	for (i = 0; i < bits; i++)
		n += test_bit(i, map);
	return n;
}

/* One counter of /sys/class/net/<ifname>/statistics. */
int omci_native_netdev_counter(const char *ifname, const char *name, u64 *value)
{
	unsigned long long v;
	char path[96];
	FILE *f;
	int ret;

	snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", ifname, name);
	f = fopen(path, "r");
	if (!f)
		return -ENOENT;
	ret = fscanf(f, "%llu", &v) == 1 ? 0 : -EIO;
	fclose(f);
	if (!ret)
		*value = v;
	return ret;
}

int omci_native_ifname(u32 ifindex, char *name, size_t len)
{
	char buf[IF_NAMESIZE];

	if (!ifindex || !if_indextoname(ifindex, buf))
		return -ENODEV;
	snprintf(name, len, "%s", buf);
	return 0;
}

unsigned long omci_native_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int put_attr(struct sk_buff *msg, int type, const void *value, size_t len)
{
	struct nlattr *attr;
	size_t size = NLA_ALIGN(NLA_HDRLEN + len);

	if (size > msg->capacity - msg->len)
		return -EMSGSIZE;
	attr = (void *)(msg->data + msg->len);
	memset(attr, 0, size);
	attr->nla_len = NLA_HDRLEN + len;
	attr->nla_type = type;
	if (len)
		memcpy((char *)attr + NLA_HDRLEN, value, len);
	msg->len += size;
	return 0;
}

int nla_put_u8(struct sk_buff *msg, int type, u8 v) { return put_attr(msg, type, &v, sizeof(v)); }
int nla_put(struct sk_buff *msg, int type, size_t len, const void *value)
{
	return put_attr(msg, type, value, len);
}
int nla_put_s32(struct sk_buff *msg, int type, s32 v) { return put_attr(msg, type, &v, sizeof(v)); }
int nla_put_string(struct sk_buff *msg, int type, const char *value)
{
	return put_attr(msg, type, value, strlen(value) + 1);
}
struct nlattr *nla_nest_start(struct sk_buff *msg, int type)
{
	struct nlattr *start = (void *)(msg->data + msg->len);

	return put_attr(msg, type | NLA_F_NESTED, NULL, 0) ? NULL : start;
}
void nla_nest_end(struct sk_buff *msg, struct nlattr *start)
{
	start->nla_len = msg->len - ((u8 *)start - msg->data);
}
void nla_nest_cancel(struct sk_buff *msg, struct nlattr *start)
{
	msg->len = (u8 *)start - msg->data;
}
int nla_put_u16(struct sk_buff *msg, int type, u16 v) { return put_attr(msg, type, &v, sizeof(v)); }
int nla_put_u32(struct sk_buff *msg, int type, u32 v) { return put_attr(msg, type, &v, sizeof(v)); }
int nla_put_u64_64bit(struct sk_buff *msg, int type, u64 v, int pad)
{
	(void)pad;
	return put_attr(msg, type, &v, sizeof(v));
}

int omci_identity_load(struct device *dev, struct omci_identity *identity)
{
	if (!dev || !dev->identity)
		return -ENODATA;
	*identity = *dev->identity;
	return 0;
}

void omci_native_load_board(struct omci_device *odev)
{
	odev->agent.config.onu_type = odev->parent->onu_type;
	odev->agent.config.uni_count = odev->parent->uni_count;
	odev->agent.config.pots_count = odev->parent->pots_count;
}

void omci_native_timers(struct omci_device *odev)
{
	struct delayed_work *work[] = { &odev->agent.alarm_work, &odev->agent.pm_work,
		&odev->agent.reboot_work };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(work); i++) {
		if (!work[i]->pending || time_before(jiffies, work[i]->due))
			continue;
		work[i]->pending = false;
		work[i]->work.fn(&work[i]->work);
	}
}

int omci_native_receive(struct omci_device *odev, const void *data, size_t len)
{
	struct omci_wire_request parsed;
	struct sk_buff skb = { .data = (void *)data, .len = len, .capacity = len };
	int ret = omci_wire_decode(data, len, &parsed);

	if (ret)
		return ret;
	odev->rx_packets++;
	odev->rx_bytes += len;
	omci_agent_receive(odev, &skb);
	return 0;
}

int omci_device_xmit(struct omci_device *odev, const void *data, size_t len)
{
	struct sk_buff *skb;
	int ret;

	if (!odev->started || !odev->channel_up)
		return -ENOLINK;
	skb = alloc_skb(len, 0);
	if (!skb)
		return -ENOMEM;
	skb_put_data(skb, data, len);
	ret = odev->ops->xmit(odev, skb, odev->gem_port_id);
	if (ret)
		kfree_skb(skb);
	if (ret)
		odev->tx_errors++;
	else {
		odev->tx_packets++;
		odev->tx_bytes += len;
	}
	return ret;
}

void *omci_device_priv(const struct omci_device *odev) { return odev->priv; }

void omci_device_notify(struct omci_device *odev, u8 event)
{
	if (odev->parent->notify)
		odev->parent->notify(odev, event);
}

void omci_native_reboot(struct omci_device *odev)
{
	if (odev->parent->reboot)
		odev->parent->reboot(odev);
}

/*
 * The multicast groups this router receives from the PON, as G.988 9.3.31
 * active group list rows: the kernel's multicast routing cache entries whose
 * input interface is pon0, one of its VLANs or a PPP link (igmpproxy runs
 * the routes). The client address is not known to the router: it reads 0.
 */
#define OMCI_MCAST_SEEN_MAX	64
#define OMCI_MCAST_ROW_V4	24
#define OMCI_MCAST_ROW_V6	58

struct omci_mcast_seen {
	bool used, ipv6, present;
	u8 group[16], source[16];
	time_t first;
	u64 first_bytes;
};

static struct omci_mcast_seen omci_mcast_seen[OMCI_MCAST_SEEN_MAX];

/* The VLAN of a PON input interface (0 if untagged), or -1 if not the PON. */
static int omci_mcast_pon_vif(bool ipv6, int vif)
{
	char line[160], name[IF_NAMESIZE + 1];
	int index, vid = -1;
	FILE *f;

	f = fopen(ipv6 ? "/proc/net/ip6_mr_vif" : "/proc/net/ip_mr_vif", "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%d %16s", &index, name) != 2 || index != vif)
			continue;
		if (!strncmp(name, "pon0.", 5))
			vid = atoi(name + 5);
		else if (!strcmp(name, "pon0") || !strncmp(name, "pppoe-", 6))
			vid = 0;
		break;
	}
	fclose(f);
	return vid;
}

static struct omci_mcast_seen *omci_mcast_track(bool ipv6, const u8 *group,
						const u8 *source, u64 bytes,
						time_t now)
{
	struct omci_mcast_seen *s, *free_slot = NULL;
	unsigned int i;

	for (i = 0; i < OMCI_MCAST_SEEN_MAX; i++) {
		s = &omci_mcast_seen[i];
		if (!s->used) {
			free_slot = free_slot ?: s;
			continue;
		}
		if (s->ipv6 == ipv6 && !memcmp(s->group, group, 16) &&
		    !memcmp(s->source, source, 16))
			return s;
	}
	if (!free_slot)
		return NULL;
	*free_slot = (struct omci_mcast_seen) {
		.used = true, .ipv6 = ipv6, .first = now, .first_bytes = bytes,
	};
	memcpy(free_slot->group, group, 16);
	memcpy(free_slot->source, source, 16);
	return free_slot;
}

int omci_native_mcast_groups(bool ipv6, u8 **table, size_t *table_len)
{
	size_t row = ipv6 ? OMCI_MCAST_ROW_V6 : OMCI_MCAST_ROW_V4, len = 0;
	size_t addr = ipv6 ? 16 : 4;
	time_t now = time(NULL);
	char line[256], g[48], o[48];
	unsigned long long pkts, bytes;
	unsigned int i;
	u8 *out;
	FILE *f;
	int vif;

	out = calloc(OMCI_MCAST_SEEN_MAX, row);
	if (!out)
		return -ENOMEM;
	for (i = 0; i < OMCI_MCAST_SEEN_MAX; i++)
		omci_mcast_seen[i].present = false;

	f = fopen(ipv6 ? "/proc/net/ip6_mr_cache" : "/proc/net/ip_mr_cache", "r");
	while (f && fgets(line, sizeof(line), f) && len + row <= OMCI_MCAST_SEEN_MAX * row) {
		u8 group[16] = { 0 }, source[16] = { 0 };
		struct omci_mcast_seen *s;
		u64 rate = 0;
		u8 *r;
		int vid;

		if (sscanf(line, "%47s %47s %d %llu %llu", g, o, &vif, &pkts, &bytes) != 5)
			continue;
		if (ipv6) {
			if (inet_pton(AF_INET6, g, group) != 1 ||
			    inet_pton(AF_INET6, o, source) != 1)
				continue;
		} else {
			u32 gv, ov;

			/* the kernel prints the network-order words as host integers */
			if (sscanf(g, "%x", &gv) != 1 || sscanf(o, "%x", &ov) != 1)
				continue;
			memcpy(group, &gv, 4);
			memcpy(source, &ov, 4);
		}
		vid = omci_mcast_pon_vif(ipv6, vif);
		if (vid < 0)
			continue;
		s = omci_mcast_track(ipv6, group, source, bytes, now);
		if (!s)
			continue;
		s->present = true;
		if (now > s->first)
			rate = (bytes - s->first_bytes) / (now - s->first);

		r = out + len;
		put_unaligned_be16(vid, r);
		memcpy(r + 2, source, addr);
		memcpy(r + 2 + addr, group, addr);
		put_unaligned_be32(min_t(u64, rate, U32_MAX), r + 2 + 2 * addr);
		/* client address: unknown, left 0 */
		put_unaligned_be32(now - s->first, r + 6 + 3 * addr);
		len += row;
	}
	if (f)
		fclose(f);

	/* groups the router no longer routes start over if they come back */
	for (i = 0; i < OMCI_MCAST_SEEN_MAX; i++)
		if (omci_mcast_seen[i].ipv6 == ipv6 && !omci_mcast_seen[i].present)
			omci_mcast_seen[i].used = false;

	*table = out;
	*table_len = len;
	return 0;
}
