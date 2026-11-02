#!/usr/bin/env python3
"""Host checks of the pon0 service table (unicast, multicast, GEM cleanup) and TX skb ownership."""
import pathlib
import subprocess
import tempfile

source = pathlib.Path(__file__).resolve().parents[1] / "files/drivers/soc/galachip/gc1601-xpon.c"
text = source.read_text()


def function(name):
    start = text.index("static ", text.index(name) - 30)
    begin = text.index("{", text.index(name, start))
    depth = 1
    end = begin + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint16_t u16;
#define U16_MAX 0xffff
#define GPON_SERVICES 64
#define GPON_DIAG(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define spin_lock_bh(...) ((void)0)
#define spin_unlock_bh(...) ((void)0)
struct gc1601_service {
    unsigned cookie, gem_port, alloc_id, vlan_id, queue, pcp, uni_mode, uni_vid;
    bool pcp_valid, default_service, vlan_valid, used;
};
struct gc1601_mcast { unsigned cookie; u16 gem_port; bool used; };
struct gc1601_xpon {
    struct gc1601_service service[64]; struct gc1601_mcast multicast[64];
    unsigned services; int service_lock;
};
struct gc1601_pon { struct gc1601_xpon xp; int dev; };
struct omci_device { struct gc1601_pon *priv; };
struct omci_service_config {
    unsigned cookie, gem_port_id, alloc_id, vlan_id, queue, pcp, uni_vlan_mode, uni_vlan_id;
    bool pcp_valid, default_service, vlan_valid, multicast;
};
static struct gc1601_pon *omci_device_priv(struct omci_device *d) { return d->priv; }
static unsigned updates;
static void gc1601_xpon_sfu_update(struct gc1601_xpon *xp) { (void)xp; updates++; }
static unsigned restores, disabled_gem = U16_MAX;
static int gc1601_gpon_restore_service(struct gc1601_pon *p, unsigned gem, unsigned alloc) {
    (void)p; (void)gem; (void)alloc; restores++; return 0;
}
static void gc1601_xpon_disable_unused_gem(struct gc1601_pon *p, u16 gem) { (void)p; disabled_gem = gem; }
typedef unsigned netdev_tx_t;
#define NETDEV_TX_OK 0
#define NETDEV_TX_BUSY 1
#define GFP_ATOMIC 0
struct sk_buff { unsigned value; bool freed; };
struct net_device { struct { unsigned tx_dropped; } stats; };
static struct sk_buff clone;
static bool clone_fail;
static netdev_tx_t result;
static struct sk_buff *skb_clone(struct sk_buff *s, int flags) {
    (void)flags; clone = *s; return clone_fail ? NULL : &clone;
}
static void dev_kfree_skb_any(struct sk_buff *s) { assert(!s->freed); s->freed = true; }
static netdev_tx_t gc1601_pon0_xmit_frame(struct sk_buff *s, struct net_device *d) {
    (void)d; s->value++;
    if (result != NETDEV_TX_BUSY) dev_kfree_skb_any(s);
    return result;
}
'''
program += function("gc1601_xpon_replace_service")
program += function("gc1601_pon0_xmit(struct")
program += r'''
int main(void) {
    struct gc1601_pon pon = { 0 };
    struct omci_device dev = { &pon };
    struct omci_service_config svc = { .cookie = 99 };
    for (unsigned i = 0; i < 64; i++) {
        pon.xp.service[i].used = true; pon.xp.service[i].cookie = i;
    }
    pon.xp.services = 64;
    assert(gc1601_xpon_replace_service(&dev, &svc) == -ENOSPC);
    assert(pon.xp.services == 64 && updates == 0);
    svc.cookie = 63; svc.gem_port_id = 500;
    assert(gc1601_xpon_replace_service(&dev, &svc) == 0);
    assert(pon.xp.service[63].gem_port == 500 && pon.xp.services == 64);
    pon.xp.service[0].used = false; pon.xp.services--;
    svc.cookie = 63; svc.gem_port_id = 501;
    assert(gc1601_xpon_replace_service(&dev, &svc) == 0);
    assert(!pon.xp.service[0].used && pon.xp.service[63].gem_port == 501);
    assert(disabled_gem == 500);
    svc.cookie = 99;
    assert(gc1601_xpon_replace_service(&dev, &svc) == 0);
    assert(pon.xp.services == 64 && pon.xp.service[0].cookie == 99);
    /* A full table refuses a new service and undoes its GEM restore. */
    svc.cookie = 100; svc.gem_port_id = 777; disabled_gem = U16_MAX;
    assert(gc1601_xpon_replace_service(&dev, &svc) == -ENOSPC);
    assert(disabled_gem == 777);
    /* Multicast GEMs live in their own table and skip the SFU update. */
    unsigned before = updates;
    struct omci_service_config mc = { .cookie = 7, .gem_port_id = 4000, .multicast = true };
    assert(gc1601_xpon_replace_service(&dev, &mc) == 0);
    assert(pon.xp.multicast[0].used && pon.xp.multicast[0].gem_port == 4000);
    mc.gem_port_id = 4001; disabled_gem = U16_MAX;
    assert(gc1601_xpon_replace_service(&dev, &mc) == 0);
    assert(pon.xp.multicast[0].gem_port == 4001 && !pon.xp.multicast[1].used);
    assert(disabled_gem == 4000 && updates == before);
    mc.gem_port_id = 0x1000;
    assert(gc1601_xpon_replace_service(&dev, &mc) == -ERANGE);
    struct net_device netdev = { 0 };
    struct sk_buff skb = { .value = 100 };
    result = NETDEV_TX_BUSY;
    assert(gc1601_pon0_xmit(&skb, &netdev) == NETDEV_TX_BUSY);
    assert(skb.value == 100 && !skb.freed && clone.freed);
    result = NETDEV_TX_OK;
    assert(gc1601_pon0_xmit(&skb, &netdev) == NETDEV_TX_OK);
    assert(skb.freed && clone.freed);
    skb.freed = false; clone_fail = true;
    assert(gc1601_pon0_xmit(&skb, &netdev) == NETDEV_TX_OK);
    assert(skb.freed && netdev.stats.tx_dropped == 1);
    puts("pon0 service table, multicast, GEM cleanup and TX ownership tests passed");
}
'''
with tempfile.TemporaryDirectory(prefix="g7-gpon-test-") as tmp:
    c = pathlib.Path(tmp) / "test.c"
    exe = pathlib.Path(tmp) / "test"
    c.write_text(program)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g", str(c), "-o", str(exe)], check=True)
    import os
    subprocess.run([str(exe)], env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}, check=True)
