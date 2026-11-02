#!/usr/bin/env python3
"""Check current REI packing, timing and state guards with stubbed kernel APIs."""
import os
import pathlib
import subprocess
import tempfile

src = pathlib.Path(__file__).resolve().parents[1] / "files/drivers/soc/galachip/gc1601-gpon.c"
text = src.read_text()
start = text.index("static unsigned long gc1601_gpon_rei_delay")
end = text.index("static void gc1601_gpon_retain", start)
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define O5 5
#define UP_REI 8
#define PON_OPT_RX 0
#define RX_BIP_ERRORS 0x58
#define RX_BIP_BLOCKS 0x5c
enum { CNT_BIP_ERRORS, CNT_BIP_BLOCKS, GPON_COUNTERS };
#define MAX_JIFFY_OFFSET 2147483647UL
#define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t,a,b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define msecs_to_jiffies(ms) ((unsigned long)(ms) / 10)
#define nsecs_to_jiffies64(ns) ((u64)(ns) / 10000000)
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define to_delayed_work(p) container_of(p, struct delayed_work, work)
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct gc1601_gpon {
    struct delayed_work rei;
    bool running;
    int state;
    u32 ber_interval_frames;
    u8 rei_sequence;
    u64 counter[GPON_COUNTERS], rei_sent, rei_failed;
};
struct gc1601_pon { struct gc1601_gpon gpon; unsigned char *mac; int lock; };
static struct gc1601_pon *gpon_to_pon(struct gc1601_gpon *g) { return container_of(g, struct gc1601_pon, gpon); }
static unsigned reads, scheduled;
static int send_error;
static u8 sent[10];
static int system_wq;
static u32 readl(void *p) { u32 v = *(u32 *)p; reads++; *(u32 *)p = 0; return v; }
static u32 gc1601_gpon_sample(struct gc1601_pon *pon, unsigned int i) {
    u32 val = readl(pon->mac + (i == CNT_BIP_ERRORS ? RX_BIP_ERRORS : RX_BIP_BLOCKS));
    pon->gpon.counter[i] += val;
    return val;
}
static void put_unaligned_be32(u32 v, u8 *p) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static int gc1601_gpon_send(struct gc1601_pon *p, u8 id, const u8 *d) {
    (void)p; assert(id == UP_REI); memcpy(sent,d,10); return send_error;
}
static void mod_delayed_work(int q, struct delayed_work *w, unsigned long delay) {
    (void)q; (void)w; assert(delay >= 2); scheduled++;
}
'''
program += text[start:end]
program += r'''
int main(void) {
    u32 mmio[32] = { 0 };
    struct gc1601_pon p = { .mac = (unsigned char *)mmio };
    struct gc1601_gpon *g = &p.gpon;
    assert(gc1601_gpon_rei_delay(8000) == 100);
    assert(gc1601_gpon_rei_delay(1) == 2);
    assert(gc1601_gpon_rei_delay(UINT32_MAX) > 100);
    gc1601_gpon_rei(&g->rei.work);
    assert(reads == 0 && scheduled == 0);
    g->running = true; g->state = O5; g->ber_interval_frames = 8000; g->rei_sequence = 15;
    mmio[RX_BIP_ERRORS/4] = 0x12345678; mmio[RX_BIP_BLOCKS/4] = 9;
    gc1601_gpon_rei(&g->rei.work);
    assert(memcmp(sent, "\x12\x34\x56\x78\x0f", 5) == 0);
    assert(g->rei_sequence == 0 && g->rei_sent == 1 && g->counter[CNT_BIP_BLOCKS] == 9);
    send_error = -1;
    gc1601_gpon_rei(&g->rei.work);
    assert(g->rei_failed == 1 && g->rei_sequence == 0);
    unsigned old = reads;
    g->state = 6; gc1601_gpon_rei(&g->rei.work); assert(reads == old);
    g->state = O5; g->ber_interval_frames = 0;
    gc1601_gpon_rei(&g->rei.work); assert(reads == old);
    puts("REI packing, interval and state tests passed");
}
'''
with tempfile.TemporaryDirectory(prefix="g7-rei-test-") as tmp:
    c, exe = pathlib.Path(tmp) / "test.c", pathlib.Path(tmp) / "test"
    c.write_text(program)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}, check=True)
