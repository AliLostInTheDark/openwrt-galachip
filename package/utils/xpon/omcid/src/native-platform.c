// SPDX-License-Identifier: GPL-2.0-only
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
