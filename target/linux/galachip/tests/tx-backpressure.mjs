// SPDX-License-Identifier: GPL-2.0-only
import { readFileSync } from 'node:fs';
const source = readFileSync(new URL('../files/drivers/net/ethernet/galachip/gc1601-eth.c', import.meta.url), 'utf8');
const rxCheck = !process.argv.includes('--tx-only');
const rxStall = source.includes('static void gc1601_rx_stall(') ? fn('gc1601_rx_stall') : '';
// Run the production functions against deterministic pool, completion and timer state.
function fn(name) {
 const at = source.indexOf(name + '(');
 if (at < 0) throw new Error('Missing production function: ' + name);
 const start = source.lastIndexOf('\nstatic ', at) + 1;
 const brace = source.indexOf('{', at);
 let depth = 1, end = brace + 1;
 while (depth) { if (source[end] == '{') depth++; if (source[end] == '}') depth--; end++; }
 return source.slice(start, end);
}
const optional = source.includes('static void gc1601_tx_backpressure(') ? fn('gc1601_tx_backpressure') : '';
process.stdout.write(`
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
typedef unsigned long phys_addr_t;
#define BIT(n) (1U << (n))
#define GC1601_BUFFER_SIZE 2048
#define GC1601_BUFFER_COUNT 512
#define GC1601_RING_SIZE 256
#define GC1601_PKT_POOL_PHYS 0x40801000
#define GC1601_SLOT_PROBE_COUNT 4
#define GC1601_SLOT_PROBE_SIZE 320
#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_ZLEN 60
#define GC1601_RX 0
#define GC1601_TX_RING 1
#define GC1601_GMAC 2
#define GC1601_BUFFERS 3
#define GC_RX_ALLOC_REQ 0x14
#define GC_RX_ALLOC_RESP 0xc
#define GC_RX_RELEASE_BUF 0x10
#define GC_RX_PUBLISH_TX 0x38
#define GC_TX_DOORBELL 4
#define GC_GMAC_TX_OCTETS 0x714
#define GC_BUF_INTR_MASK 4
#define GC_BUF_EVENT_MASK 0x44
#define GC1601_IRQ_RETRY_MAX_MS 10000
#define GC1601_IRQ_RETRY_MS 200
#define NAPI_STATE_SCHED 0
#define GC1601_TX_DEPTH 16
#define DMA_TO_DEVICE 0
#define HZ 100
#define spin_lock_irqsave(p,f) do { (void)(p); (f)=0; } while(0)
#define spin_unlock_irqrestore(p,f) do { (void)(p); (void)(f); } while(0)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define max_t(t,a,b) ((t)(a)>(t)(b)?(t)(a):(t)(b))
#define min(a,b) ((a)<(b)?(a):(b))
#define container_of(p,t,m) ((t *)((char *)(p) - __builtin_offsetof(t,m)))
#define timer_container_of(var,t,m) container_of(t,struct gc1601_eth,m)
struct sk_buff { unsigned int len; bool freed; };
struct gc1601_desc { u32 word[4]; };
struct timer_list { unsigned long expires; };
struct net_device { struct gc1601_eth *priv; bool stopped; struct { unsigned int tx_dropped,tx_bytes,tx_packets; } stats; };
struct gc1601_pon_meta { u16 gem_port; u8 tcont,queue; bool omci; };
struct gc1601_eth {
 void *dev; struct net_device *netdev; void *base[4], *pkt_pool;
 int tx_lock,irq_lock,irq; bool tx_stopping,tx_probe_failed,tx_probe_aligned,link,tx_buf_wait;
 unsigned int tx_probe_sent,tx_pending,tx_busy_events,tx_producer,tx_consumer;
 unsigned int tx_completion_reads,tx_completion_last,tx_completed_total,tx_completion_excess;
 int tx_detected_slot;u32 tx_detected_w3;
 bool tx_replicated,running,irq_poll_only; unsigned int irq_poll_ticks,irq_empty_streak,irq_retry_ms;
 unsigned long irq_retry_at; u16 rx_stalled;
 struct { unsigned long state; } napi;
 struct timer_list poll_timer;
 unsigned long tx_id_seen[8]; struct gc1601_desc tx_ring[GC1601_RING_SIZE];
 struct sk_buff *tx_skb[GC1601_RING_SIZE]; u16 tx_buf_id[GC1601_RING_SIZE]; u32 tx_probe_octets[4];
};
static struct net_device *gc1601_pon_dev;
static unsigned long jiffies=100;
static bool alloc_timeout, rx_pending;
static unsigned int napi_calls;
static u32 regs[4][512];
static char pool[GC1601_BUFFER_SIZE * GC1601_BUFFER_COUNT];
static struct gc1601_eth *netdev_priv(struct net_device *d) { return d->priv; }
static void netif_stop_queue(struct net_device *d) { d->stopped=true; }
static bool skb_put_padto(struct sk_buff *s, int n) { if(s->len<(unsigned int)n)s->len=n;return false; }
static void dev_kfree_skb_any(struct sk_buff *s) { assert(!s->freed);s->freed=true; }
static void udelay(int n) { (void)n; }
static u32 gc1601_tx_depth(struct gc1601_eth *p) { return p->tx_probe_aligned?GC1601_TX_DEPTH:1; }
static void gc1601_tx_probe_schedule_locked(struct gc1601_eth *p) { (void)p; }
#define lockdep_assert_held(p) ((void)(p))
#define le32_to_cpu(v) (v)
#define GC_TX_COMPLETIONS 8
static bool netif_running(struct net_device *d) { (void)d;return true; }
static bool netif_queue_stopped(struct net_device *d) { return d->stopped; }
static void netif_wake_queue(struct net_device *d) { d->stopped=false; }
static void dev_consume_skb_any(struct sk_buff *s) { assert(!s->freed);s->freed=true; }
static u32 readl(void *p) { return *(u32 *)p; }
static void writel(u32 v,void *p) { *(u32 *)p=v; }
#define readl_poll_timeout_atomic(p,v,cond,a,b) ((v)=0,(void)(v),alloc_timeout?-ETIMEDOUT:0)
static void set_bit(u32 bit,unsigned long *p) { (void)bit;(void)p; }
static void eth_broadcast_addr(u8 *p) { memset(p,255,6); }
static void ether_addr_copy(u8 *p,const u8 *s) { memcpy(p,s,6); }
static void put_unaligned_be16(u16 v,u8 *p) { p[0]=v>>8;p[1]=v; }
static int skb_copy_bits(struct sk_buff *s,int o,void *p,unsigned int n) { (void)s;(void)o;memset(p,0,n);return 0; }
static void dma_sync_single_for_device(void *d,phys_addr_t p,unsigned int n,int dir) { (void)d;(void)p;(void)n;(void)dir; }
static u32 cpu_to_le32(u32 x) { return x; }
static void dma_wmb(void) {}
static unsigned long msecs_to_jiffies(unsigned int n) { return (n+9)/10; }
static void timer_reduce(struct timer_list *t,unsigned long expires) { if(t->expires>expires)t->expires=expires; }
static void mod_timer(struct timer_list *t,unsigned long expires) { t->expires=expires; }
static bool gc1601_rx_any_pending(struct gc1601_eth *p) { (void)p;return rx_pending; }
static bool test_bit(unsigned int b,unsigned long *s) { return *s & BIT(b); }
static bool napi_schedule_prep(void *p) { (void)p;return true; }
static void __napi_schedule(void *p) { (void)p;napi_calls++; }
static void enable_irq(int n) { (void)n; }
static bool time_before(unsigned long a,unsigned long b) { return a<b; }
static void gc1601_irq_mask(struct gc1601_eth *p,bool on) { (void)p;(void)on; }
${optional}
${fn('gc1601_tx_complete_locked')}
${fn('gc1601_xmit')}
${fn('gc1601_poll_timer')}
${rxStall}
static void reset(struct gc1601_eth *p,struct net_device *d,struct net_device *pon)
{
 memset(p,0,sizeof(*p));memset(d,0,sizeof(*d));memset(pon,0,sizeof(*pon));
 memset(regs,0,sizeof(regs));p->netdev=d;d->priv=p;gc1601_pon_dev=pon;
 for(unsigned int i=0;i<4;i++)p->base[i]=regs[i];
 p->pkt_pool=pool;p->tx_probe_aligned=true;p->link=true;p->running=true;
 p->poll_timer.expires=jiffies+5;alloc_timeout=false;rx_pending=false;napi_calls=0;
}
int main(void)
{
 struct gc1601_eth p;struct net_device d,pon;struct sk_buff skb={.len=1500};
 struct gc1601_pon_meta meta={.gem_port=256,.tcont=1};
 reset(&p,&d,&pon);
 int ret=gc1601_xmit(&skb,&d,&meta);
 printf("empty pool: ret=%d freed=%d drops=%u stopped=%d/%d retry=%lu ticks\\n",ret,skb.freed,d.stats.tx_dropped,d.stopped,pon.stopped,p.poll_timer.expires-jiffies);fflush(stdout);
 assert(ret==-EBUSY && !skb.freed && !d.stats.tx_dropped);
 assert(d.stopped && pon.stopped && p.tx_buf_wait);
 assert(p.poll_timer.expires==jiffies+1);
 gc1601_poll_timer(&p.poll_timer);
 assert(napi_calls==1 && p.poll_timer.expires==jiffies+1);
 gc1601_tx_complete_locked(&p);
 assert(!d.stopped && !pon.stopped);
 regs[0][GC_RX_ALLOC_RESP/4]=BIT(31)|3;
 ret=gc1601_xmit(&skb,&d,&meta);
 assert(!ret && !p.tx_buf_wait && p.tx_pending==1 && p.tx_skb[0]==&skb && !skb.freed);
 gc1601_tx_complete_locked(&p);
 assert(p.tx_pending==1 && !skb.freed);
 regs[1][GC_TX_COMPLETIONS/4]=1;
 gc1601_tx_complete_locked(&p);
 assert(!p.tx_pending && skb.freed && !p.tx_skb[0]);
 reset(&p,&d,&pon);d.stopped=pon.stopped=true;p.tx_stopping=true;
 gc1601_tx_complete_locked(&p);assert(d.stopped && pon.stopped);
 reset(&p,&d,&pon);skb=(struct sk_buff){.len=1500};
 alloc_timeout=true;assert(gc1601_xmit(&skb,&d,&meta)==-EIO && skb.freed && d.stats.tx_dropped==1);
 reset(&p,&d,&pon);skb=(struct sk_buff){.len=1500};
 regs[0][GC_RX_ALLOC_RESP/4]=BIT(31)|512;
 assert(gc1601_xmit(&skb,&d,&meta)==-EIO && skb.freed);
 reset(&p,&d,&pon);skb=(struct sk_buff){.len=1500};
 p.tx_pending=GC1601_TX_DEPTH;
 assert(gc1601_xmit(&skb,&d,&meta)==-EBUSY && !skb.freed && d.stopped && pon.stopped);
 if (${rxCheck ? 1 : 0}) {
 reset(&p,&d,&pon);rx_pending=true;
 ${rxStall ? 'gc1601_rx_stall(&p,0);assert(p.rx_stalled==1 && p.poll_timer.expires==jiffies+1);' : 'p.rx_stalled=1;'}
 gc1601_poll_timer(&p.poll_timer);
 printf("stalled RX: scheduled=%u retry=%lu ticks\\n",napi_calls,p.poll_timer.expires-jiffies);fflush(stdout);
 assert(napi_calls==1 && !p.rx_stalled && p.poll_timer.expires==jiffies+1);
 }
 reset(&p,&d,&pon);p.tx_pending=1;
 gc1601_poll_timer(&p.poll_timer);assert(napi_calls==1 && p.poll_timer.expires==jiffies+1);
 reset(&p,&d,&pon);gc1601_poll_timer(&p.poll_timer);
 assert(!napi_calls && p.poll_timer.expires==jiffies+5);
 p.running=false;unsigned long expires=p.poll_timer.expires;
 gc1601_poll_timer(&p.poll_timer);assert(p.poll_timer.expires==expires);
 puts("GC1601 TX backpressure, ownership and active/idle retry checks passed");
}
`);
