// SPDX-License-Identifier: GPL-2.0-only
// Compile the actual core entry points against deterministic lock/netns mocks.
const fs = require('fs');
const path = require('path');
const source = fs.readFileSync(path.resolve(__dirname,
    '../../../../../target/linux/galachip/files/net/xpon/omci/core.c'), 'utf8');
function extract(name) {
    const start = source.indexOf('static int ' + name + '(');
    if (start < 0) throw Error(name);
    let end = source.indexOf('{', start) + 1, depth = 1;
    while (depth) {
        if (source[end] === '{') depth++;
        if (source[end] === '}') depth--;
        end++;
    }
    return source.slice(start, end);
}
process.stdout.write(`
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <linux/omci.h>
#include <linux/omci-backend.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define GFP_KERNEL 0
#define NLMSG_DEFAULT_SIZE 4096
#define __aligned(n) __attribute__((aligned(n)))
#define READ_ONCE(v) (v)
struct net { unsigned refs; };
struct nlattr { u32 value; };
struct sk_buff { unsigned len; unsigned char data[128]; };
struct genl_info { struct nlattr *attrs[OMCI_ATTR_MAX+1]; u32 snd_portid; struct net *net; };
struct omci_device;
struct ops { int (*xmit)(struct omci_device *,struct sk_buff *,u16);
 int (*backend_begin)(struct omci_device *); void (*backend_end)(struct omci_device *); };
struct omci_device { unsigned owner_lock,state_lock,rx_work,rx_queue;
 struct net *owner_net; u32 owner_portid,owner_flags,generation;
 bool userspace_mode,started,channel_up; u16 gem_port_id;
 unsigned tx_packets,tx_bytes,tx_errors; struct ops *ops; };
static unsigned omci_devices_lock,omci_genl_family,checks,allocs,xmits,work,backend_writes;
static unsigned provider_lock,provider_error,handoffs;
static bool bump_generation;
static struct omci_device d;
static struct net n,other;
static struct omci_device *omci_get_from_info(struct genl_info *i) { return &d; }
static struct net *genl_info_net(struct genl_info *i) { return i->net; }
static struct net *get_net(struct net *v) { v->refs++; return v; }
static void put_net(struct net *v) { assert(v->refs); v->refs--; }
static void schedule_work(unsigned *w) { work++; }
static void mutex_lock(unsigned *l) { assert(!*l); *l=1; }
static void mutex_unlock(unsigned *l) { assert(*l); *l=0; }
#define spin_lock_bh mutex_lock
#define spin_unlock_bh mutex_unlock
static u32 nla_get_u32(struct nlattr *a) { return a->value; }
static u16 nla_get_u16(struct nlattr *a) { return a->value; }
static unsigned nla_len(struct nlattr *a) { return 44; }
static void *nla_data(struct nlattr *a) { static unsigned char p[44]; return p; }
static struct sk_buff *alloc_skb(unsigned size,int flags) { allocs++; return calloc(1,sizeof(struct sk_buff)); }
static void skb_put_data(struct sk_buff *s,const void *p,unsigned size) { assert(size<=128);memcpy(s->data,p,size);s->len=size; }
static void skb_queue_purge(unsigned *q) { *q=0; }
static int omci_validate_tx(struct omci_device *dev,struct sk_buff *s) { return 0; }
static void dev_kfree_skb_any(struct sk_buff *s) { free(s); }
static void atomic64_inc(unsigned *p) { ++*p; }
static void atomic64_add(unsigned v,unsigned *p) { *p+=v; }
static int xmit(struct omci_device *dev,struct sk_buff *s,u16 gem) { xmits++;free(s);return 0; }
static int begin(struct omci_device *dev) {
 assert(!dev->owner_lock);
 if(provider_error) return -(int)provider_error;
 mutex_lock(&provider_lock);
 if(bump_generation) dev->generation++;
 return 0;
}
static void end(struct omci_device *dev) { mutex_unlock(&provider_lock); }
/* bind latches userspace_mode under the provider lock: count each handoff. */
static void latch_userspace(bool *mode, bool on) { assert(provider_lock);handoffs++;*mode=on; }
#define WRITE_ONCE(v, x) latch_userspace(&(v), (x))
static int omci_backend_execute(struct omci_device *dev,u16 op,const void *p,size_t len,void *out,size_t *out_len) {
 assert(provider_lock);backend_writes++;*out_len=0;return 0;
}
static struct sk_buff *genlmsg_new(unsigned len,int flags) { return alloc_skb(len,flags); }
static void *genlmsg_put_reply(struct sk_buff *s,struct genl_info *i,unsigned *f,int flags,int cmd) { return s; }
static int nla_put_u16(struct sk_buff *s,int type,u16 value) { return 0; }
static int nla_put_u32(struct sk_buff *s,int type,u32 value) { return 0; }
static int nla_put(struct sk_buff *s,int type,size_t len,void *value) { return 0; }
static void nlmsg_free(struct sk_buff *s) { free(s); }
static void genlmsg_end(struct sk_buff *s,void *hdr) {}
static int genlmsg_reply(struct sk_buff *s,struct genl_info *i) { free(s);return 0; }
#define CHECK(v) do { assert(v);checks++; } while(0)
`);
for (const name of ['omci_cmd_bind', 'omci_cmd_unbind', 'omci_cmd_tx', 'omci_cmd_backend'])
    process.stdout.write(extract(name) + '\n');
process.stdout.write(`
int main(void) {
 struct ops ops={xmit,begin,end};
 struct nlattr flags={OMCI_BIND_F_READ_ONLY},gen={8},pdu={0},op={OMCI_BACKEND_RESET};
 struct genl_info i={.snd_portid=42,.net=&n};
 d.ops=&ops;d.started=d.channel_up=true;d.generation=8;
 i.attrs[OMCI_ATTR_BIND_FLAGS]=&flags;i.attrs[OMCI_ATTR_PDU]=&pdu;
 i.attrs[OMCI_ATTR_BACKEND_OP]=&op;
 CHECK(!omci_cmd_bind(0,&i)&&d.owner_portid==42&&n.refs==1);
 CHECK(!omci_cmd_bind(0,&i)&&n.refs==1);
 CHECK(omci_cmd_tx(0,&i)==-EPERM&&!xmits);
 flags.value=0;CHECK(omci_cmd_bind(0,&i)==-EBUSY);
 i.net=&other;CHECK(omci_cmd_unbind(0,&i)==-EPERM);i.net=&n;
 CHECK(!omci_cmd_unbind(0,&i)&&!n.refs);
 flags.value=3;CHECK(omci_cmd_bind(0,&i)==-EINVAL);
 flags.value=OMCI_BIND_F_AGENT;CHECK(omci_cmd_bind(0,&i)==-EINVAL);
 i.attrs[OMCI_ATTR_GENERATION]=&gen;
 gen.value=7;CHECK(omci_cmd_bind(0,&i)==-ESTALE&&!handoffs);
 gen.value=8;provider_error=EBUSY;CHECK(omci_cmd_bind(0,&i)==-EBUSY&&!handoffs);
 /* There is no kernel MIB to protect: omcid binds with the OMCC up. */
 provider_error=0;CHECK(!omci_cmd_bind(0,&i)&&handoffs==1&&d.userspace_mode&&n.refs==1);
 CHECK(!omci_cmd_bind(0,&i)&&handoffs==1&&n.refs==1);
 CHECK(!omci_cmd_tx(0,&i)&&xmits==1);
 gen.value=7;CHECK(omci_cmd_tx(0,&i)==-ESTALE&&xmits==1);
 CHECK(omci_cmd_backend(0,&i)==-ESTALE&&!backend_writes);
 gen.value=8;i.net=&other;CHECK(omci_cmd_backend(0,&i)==-EPERM&&!backend_writes);i.net=&n;
 i.snd_portid=43;CHECK(omci_cmd_backend(0,&i)==-EPERM);i.snd_portid=42;
 bump_generation=true;CHECK(omci_cmd_backend(0,&i)==-ESTALE&&!backend_writes);
 bump_generation=false;gen.value=d.generation;
 CHECK(!omci_cmd_backend(0,&i)&&backend_writes==1);
 CHECK(!omci_cmd_unbind(0,&i)&&d.userspace_mode&&!n.refs);
 flags.value=0;CHECK(omci_cmd_bind(0,&i)==-EPERM);
 /* The latch holds after the owner leaves: a restarted omcid binds again. */
 flags.value=OMCI_BIND_F_AGENT;
 CHECK(!omci_cmd_bind(0,&i)&&handoffs==1&&n.refs==1);
 CHECK(!omci_cmd_backend(0,&i)&&backend_writes==2);
 CHECK(!omci_cmd_unbind(0,&i)&&!n.refs);
 CHECK(!provider_lock&&!omci_devices_lock&&!d.owner_lock&&!d.state_lock);
 printf("%u actual kernel ownership/netns/generation/latch checks passed\\n",checks);
}
`);
