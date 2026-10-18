/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OMCI_NATIVE_PLATFORM_H
#define OMCI_NATIVE_PLATFORM_H

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <linux/netlink.h>

typedef __u8 u8;
typedef __u16 u16;
typedef __u32 u32;
typedef __u64 u64;
typedef __s16 s16;
typedef __s8 s8;
typedef __s32 s32;
typedef __s64 s64;
typedef u64 atomic64_t;
typedef int spinlock_t;
struct mutex { unsigned int held; };
struct list_head { struct list_head *next, *prev; };
struct net;
struct omci_identity;
struct omci_device;
struct device {
	const struct omci_identity *identity;
	u8 onu_type, uni_count;
	void (*notify)(struct omci_device *, u8 event);
	void (*reboot)(struct omci_device *);
};
struct sk_buff { u8 *data; size_t len, capacity; u8 cb[48]; };
struct sk_buff_head { void *unused; };
struct work_struct { void (*fn)(struct work_struct *); };
struct delayed_work { struct work_struct work; unsigned long due; bool pending; };
struct omci_native_entry { unsigned long key; void *value; };
struct xarray { struct omci_native_entry *entries; size_t count, capacity; };

#define BIT(n) (1UL << (n))
#define GENMASK(h,l) ((~0UL << (l)) & (~0UL >> (sizeof(unsigned long)*8-1-(h))))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min(a,b) ({ __typeof__(a) _a=(a); __typeof__(b) _b=(b); _a < _b ? _a : _b; })
#define max(a,b) ({ __typeof__(a) _a=(a); __typeof__(b) _b=(b); _a > _b ? _a : _b; })
#define min_t(t,a,b) min((t)(a),(t)(b))
#define clamp(a,l,h) min(max(a,l),h)
#define clamp_t(t,a,l,h) clamp((t)(a),(t)(l),(t)(h))
#define U8_MAX UINT8_MAX
#define U16_MAX UINT16_MAX
#define U32_MAX UINT32_MAX
#define S16_MIN INT16_MIN
#define S16_MAX INT16_MAX
#define HZ 1000UL
#define GFP_KERNEL 0
static inline u32 rol32(u32 v, unsigned int n) {return (v<<n)|(v>>(32-n));}
static inline u32 omci_native_cpu32(const void *p) {u32 v;memcpy(&v,p,4);return v;}
#define get_unaligned(p) omci_native_cpu32(p)
#define PAGE_SIZE 4096
#define VLAN_N_VID 4096
#define VLAN_VID_MASK 4095
#define ETH_P_8021Q 0x8100
#define OPTICAL_FRONTEND_ALARM_TX_LOW_POWER BIT(0)
#define OPTICAL_FRONTEND_ALARM_TX_HIGH_POWER BIT(1)
#define OPTICAL_FRONTEND_ALARM_TX_LOW_BIAS BIT(2)
#define OPTICAL_FRONTEND_ALARM_TX_HIGH_BIAS BIT(3)
#define OPTICAL_FRONTEND_ALARM_RX_LOW_POWER BIT(4)
#define OPTICAL_FRONTEND_ALARM_RX_HIGH_POWER BIT(5)
#define OPTICAL_FRONTEND_ALARM_LOW_VOLTAGE BIT(6)
#define OPTICAL_FRONTEND_ALARM_HIGH_VOLTAGE BIT(7)
#define OPTICAL_FRONTEND_ALARM_LOW_TEMP BIT(8)
#define OPTICAL_FRONTEND_ALARM_HIGH_TEMP BIT(9)
#define XA_PRESENT 0
#define fallthrough __attribute__((fallthrough))
#define EXPORT_SYMBOL_GPL(symbol)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define to_delayed_work(w) container_of(w,struct delayed_work,work)
#define time_after(a,b) ((long)((b)-(a))<0)
#define time_before(a,b) time_after(b,a)
#define msecs_to_jiffies(v) (v)
#define jiffies omci_native_now()
#define system_wq NULL
static inline bool omci_native_warn(bool value) {return value;}
#define WARN_ON(v) omci_native_warn(!!(v))
#define unlikely(v) (v)
#define div_u64(v,d) ((u64)(v)/(d))
#define DIV_ROUND_UP_ULL(v,d) (((u64)(v)+(d)-1)/(d))
#define DIV_ROUND_CLOSEST_ULL(v,d) (((u64)(v)+(d)/2)/(d))
#define DIV_ROUND_CLOSEST(v,d) (((v)+(d)/2)/(d))
#define DIV_S64_ROUND_CLOSEST(v,d) (((v) < 0 ? (v)-(d)/2 : (v)+(d)/2)/(d))
void omci_native_log(struct device *dev, const char *fmt, ...);
#define dev_dbg(dev,...) do { if(0)omci_native_log(dev,__VA_ARGS__); } while(0)
#define dev_info dev_dbg
#define dev_info_ratelimited dev_info
#define dev_warn dev_info
int omci_native_emit(char *buf, ssize_t at, const char *format, ...);
#define sysfs_emit_at omci_native_emit
#define kmalloc(n,flags) malloc(n)
#define kzalloc(n,flags) calloc(1,n)
#define kfree(p) free(p)
#define xa_is_err(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define xa_err(p) (xa_is_err(p)?(int)(intptr_t)(p):0)
#define atomic64_inc(p) (++*(p))
#define atomic64_read(p) (*(p))
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v,value) ((v)=(value))
#define spin_lock_bh(p) ((void)(p))
#define spin_unlock_bh(p) ((void)(p))

/* The native engine is confined to one event loop, including control requests. */
static inline void mutex_init(struct mutex *m) {m->held=0;}
static inline void mutex_lock(struct mutex *m) {assert(!m->held);m->held=1;}
static inline void mutex_unlock(struct mutex *m) {assert(m->held);m->held=0;}
static inline void INIT_DELAYED_WORK(struct delayed_work *w, void (*fn)(struct work_struct *))
{memset(w,0,sizeof(*w));w->work.fn=fn;}
static inline int cancel_delayed_work_sync(struct delayed_work *w)
{bool pending=w->pending;w->pending=false;return pending;}
unsigned long omci_native_now(void);
static inline int schedule_delayed_work(struct delayed_work *w,unsigned long delay)
{if(w->pending)return 0;w->due=jiffies+delay;w->pending=true;return 1;}
static inline int mod_delayed_work(void *queue,struct delayed_work *w,unsigned long delay)
{(void)queue;w->pending=false;return schedule_delayed_work(w,delay);}
static inline void xa_init(struct xarray *a) {memset(a,0,sizeof(*a));}
static inline void xa_destroy(struct xarray *a) {free(a->entries);xa_init(a);}
void *xa_load(const struct xarray *a,unsigned long key);
void *xa_store(struct xarray *a,unsigned long key,void *value,int flags);
void *xa_erase(struct xarray *a,unsigned long key);
void *xa_find(const struct xarray *a,unsigned long *key,unsigned long max_key,int filter);
void *xa_find_after(const struct xarray *a,unsigned long *key,unsigned long max_key,int filter);
int xa_reserve(struct xarray *a,unsigned long key,int flags);
void xa_release(struct xarray *a,unsigned long key);
bool xa_empty(const struct xarray *a);
#define xa_for_each(a,k,v) \
 for((k)=0,(v)=xa_find((a),&(k),ULONG_MAX,XA_PRESENT); (v); \
     (v)=xa_find_after((a),&(k),ULONG_MAX,XA_PRESENT))

struct sk_buff *alloc_skb(size_t len,int flags);
void *skb_put_data(struct sk_buff *skb,const void *data,size_t len);
void kfree_skb(struct sk_buff *skb);
#define dev_kfree_skb kfree_skb
void *kmemdup(const void *data,size_t len,int flags);
void *kmalloc_array(size_t count,size_t size,int flags);
unsigned long *bitmap_zalloc(unsigned int bits,int flags);
#define bitmap_free free
unsigned int bitmap_weight(const unsigned long *map,unsigned int bits);
static inline void set_bit(unsigned int n,unsigned long *map) {map[n/(sizeof(long)*8)]|=BIT(n%(sizeof(long)*8));}
#define __set_bit set_bit
static inline bool test_bit(unsigned int n,const unsigned long *map) {return !!(map[n/(sizeof(long)*8)]&BIT(n%(sizeof(long)*8)));}
#define for_each_set_bit(n,map,bits) for((n)=0;(n)<(bits);(n)++) if(test_bit(n,map))
static inline unsigned int hweight16(u16 v) {return __builtin_popcount(v);}
static inline unsigned int fls(u32 v) {return v?32-__builtin_clz(v):0;}
static inline u16 get_unaligned_be16(const void *p) {const u8 *b=p;return (b[0]<<8)|b[1];}
static inline u32 get_unaligned_be32(const void *p) {const u8 *b=p;return ((u32)b[0]<<24)|((u32)b[1]<<16)|(b[2]<<8)|b[3];}
static inline void put_unaligned_be16(u16 v,void *p) {u8 *b=p;b[0]=v>>8;b[1]=v;}
static inline void put_unaligned_be32(u32 v,void *p) {u8 *b=p;b[0]=v>>24;b[1]=v>>16;b[2]=v>>8;b[3]=v;}
static inline int hex_to_bin(u8 c)
{if(c>='0'&&c<='9')return c-'0';c|=32;return c>='a'&&c<='f'?c-'a'+10:-1;}
static inline ssize_t strscpy(void *dest,const void *src,size_t len)
{
	const char *s = src;
	char *d = dest;
	size_t n = 0;

	if (!len)
		return -E2BIG;
	while (n + 1 < len && s[n]) {
		d[n] = s[n];
		n++;
	}
	d[n] = 0;
	return s[n] ? -E2BIG : (ssize_t)n;
}
static inline void *memchr_inv(const void *p,int value,size_t len)
{const u8 *b=p;while(len--){if(*b!=(u8)value)return (void *)b;b++;}return NULL;}
int nla_put_u8(struct sk_buff *msg,int type,u8 value);
int nla_put(struct sk_buff *msg,int type,size_t len,const void *value);
int nla_put_s32(struct sk_buff *msg,int type,s32 value);
int nla_put_string(struct sk_buff *msg,int type,const char *value);
struct nlattr *nla_nest_start(struct sk_buff *msg,int type);
void nla_nest_end(struct sk_buff *msg,struct nlattr *start);
void nla_nest_cancel(struct sk_buff *msg,struct nlattr *start);
int nla_put_u16(struct sk_buff *msg,int type,u16 value);
int nla_put_u32(struct sk_buff *msg,int type,u32 value);
int nla_put_u64_64bit(struct sk_buff *msg,int type,u64 value,int pad);
unsigned int intlog10(u32 value);
#endif
