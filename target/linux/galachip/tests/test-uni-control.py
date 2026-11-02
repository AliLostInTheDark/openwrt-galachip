#!/usr/bin/env python3
"""Exercise the current UNI setter with stubbed MMIO and kernel locks."""
import os
import pathlib
import subprocess
import tempfile

source = pathlib.Path(__file__).resolve().parents[1] / "files/drivers/net/ethernet/galachip/gc1601-eth.c"
text = source.read_text()
start = text.index("int gc1601_pe_set_uni(bool enable)")
begin = text.index("{", start)
depth, end = 1, begin + 1
while depth:
    depth += (text[end] == "{") - (text[end] == "}")
    end += 1

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
#define GC1601_GMAC 0
#define GC_GMAC_CFG 0
#define GC_GMAC_CFG_TX_EN 1
#define GC_GMAC_CFG_RX_EN 2
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define spin_lock_irqsave(p, f) ((void)(p), (f) = 0)
#define spin_unlock_irqrestore(p, f) ((void)(p), (void)(f))
struct gc1601_eth {
    bool running, uni_disabled, link;
    int irq_lock;
    unsigned char *base[1];
};
struct net_device { struct gc1601_eth *priv; };
static struct net_device *gc1601_pe_netdev;
static int gc1601_uni_lock;
static unsigned writes;
static struct gc1601_eth *netdev_priv(struct net_device *d) { return d->priv; }
static u32 readl(void *p) { return *(u32 *)p; }
static void writel(u32 v, void *p) { writes++; *(u32 *)p = v; }
'''
program += text[start:end]
program += r'''
int main(void) {
    u32 cfg = 0x00bb2003;
    struct gc1601_eth priv = { .running = true, .link = true, .base = { (unsigned char *)&cfg } };
    struct net_device dev = { &priv };
    assert(gc1601_pe_set_uni(false) == -ENODEV);
    assert(writes == 0);
    gc1601_pe_netdev = &dev;
    assert(gc1601_pe_set_uni(false) == 0);
    assert(priv.uni_disabled && cfg == 0x00bb2000);
    assert(gc1601_pe_set_uni(true) == 0);
    assert(!priv.uni_disabled && cfg == 0x00bb2003);
    priv.link = false;
    assert(gc1601_pe_set_uni(true) == 0);
    assert(cfg == 0x00bb2000);
    priv.running = false;
    unsigned old_writes = writes;
    assert(gc1601_pe_set_uni(false) == 0);
    assert(priv.uni_disabled && writes == old_writes);
    assert(gc1601_pe_set_uni(true) == 0);
    assert(!priv.uni_disabled && writes == old_writes);
    puts("UNI MMIO gate tests passed");
}
'''
with tempfile.TemporaryDirectory(prefix="g7-uni-test-") as tmp:
    c, exe = pathlib.Path(tmp) / "test.c", pathlib.Path(tmp) / "test"
    c.write_text(program)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}, check=True)
