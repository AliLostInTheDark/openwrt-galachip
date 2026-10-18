// SPDX-License-Identifier: GPL-2.0-only
import { readFileSync } from 'node:fs';

const source = readFileSync(new URL('../files/drivers/soc/galachip/gc1601-gpon.c', import.meta.url), 'utf8');
const header = readFileSync(new URL('../files/drivers/soc/galachip/gc1601-pon.h', import.meta.url), 'utf8');
const stats = header.match(/struct gc1601_dbru_stats \{[\s\S]*?\n\};/)[0];
const start = source.indexOf('static void gc1601_gpon_dbru_check(');
const check = source.slice(start, source.indexOf('\n}\n', start) + 3);

// Exercise the production recovery function with deterministic MMIO and time.
process.stdout.write(`
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef unsigned long long u64;
#define BIT(n) (1U << (n))
#define GPON_TCONTS 4
#define CNT_UP_BYTES 0
#define DBRU_FREE_MIN 0x2fc
#define DBRU_LEVEL_DROP 900
#define TX_DBRU_TCONT 0x6c
#define TX_DBRU_LEVEL 0x70
#define TX_DBRU_LOAD 0x74
#define TX_DBRU_CLEAR 0x84
#define TX_DBRU_ERR 0x108
#define TX_QUEUE_LEVEL(n) (0x200 + 4 * (n))
#define spin_lock_irq(p) ((void)(p))
#define spin_unlock_irq(p) ((void)(p))
#define dev_info(...) ((void)0)
${stats}
struct gc1601_gpon {
 struct gc1601_dbru_stats dbru_stats;
 u64 counter[1], dbru_bytes;
 unsigned long dbru_last;
 int lock;
};
struct gc1601_pon { struct gc1601_gpon gpon; };
static u32 registers[256], readback;
static unsigned int error_reads, writes, loads, clears, qmu_calls;
static int free_buffers, qmu_error;
static unsigned long jiffies;
static u32 *tx_reg(struct gc1601_pon *p, unsigned int offset)
{ (void)p; return &registers[offset / 4]; }
static u32 readl(u32 *p)
{
 if (p == &registers[TX_DBRU_ERR / 4] && error_reads++) return readback;
 return *p;
}
static void writel(u32 value, u32 *p)
{
 writes++;
 loads += p == &registers[TX_DBRU_LOAD / 4];
 clears += p == &registers[TX_DBRU_CLEAR / 4];
 *p = value;
}
static int gc1601_pe_free_buffers(void) { return free_buffers; }
static int gc1601_pe_qmu_aful(int value)
{ (void)value; qmu_calls++; return qmu_error ? -19 : 2; }
static unsigned int jiffies_to_msecs(unsigned long value) { return value; }
static u64 div_u64(u64 value, unsigned int divisor) { return value / divisor; }
static u64 div_u64_rem(u64 value, unsigned int divisor, u32 *rem)
{ *rem = value % divisor; return value / divisor; }
${check}
static void reset(struct gc1601_pon *p, u32 flags, u32 after)
{
 memset(p, 0, sizeof(*p));
 memset(registers, 0, sizeof(registers));
 registers[TX_DBRU_ERR / 4] = flags;
 readback = after;
 error_reads = writes = loads = clears = qmu_calls = 0;
 free_buffers = DBRU_FREE_MIN + 1;
 qmu_error = 0;
 jiffies = 2000;
 p->gpon.dbru_last = 1000;
}
int main(void)
{
 struct gc1601_pon p;
 reset(&p, 0, 0);
 gc1601_gpon_dbru_check(&p);
 assert(!writes && !p.gpon.dbru_stats.detected);

 reset(&p, BIT(1), 0);
 p.gpon.dbru_last = 0;
 gc1601_gpon_dbru_check(&p);
 assert(p.gpon.dbru_stats.detected == 1 && !writes);
 assert(!p.gpon.dbru_stats.passes);

 reset(&p, BIT(1), 0);
 p.gpon.counter[0] = 250;
 gc1601_gpon_dbru_check(&p);
 assert(p.gpon.dbru_stats.busy == 1 && !writes);

 reset(&p, BIT(1), 0);
 free_buffers = DBRU_FREE_MIN;
 gc1601_gpon_dbru_check(&p);
 assert(p.gpon.dbru_stats.buffer_skips == 1);
 assert(p.gpon.dbru_stats.skipped == BIT(1));
 assert(!p.gpon.dbru_stats.rewrites && !loads && !qmu_calls);
 assert(p.gpon.dbru_stats.passes == 1 && clears == 2 && writes == 2);
 assert(p.gpon.dbru_stats.flag_clears == 1);

 reset(&p, BIT(1), BIT(1));
 registers[TX_QUEUE_LEVEL(1) / 4] = 1200;
 gc1601_gpon_dbru_check(&p);
 assert(p.gpon.dbru_stats.rewrites == 1 && loads == 1 && qmu_calls == 2);
 assert(registers[TX_DBRU_LEVEL / 4] == 300 && writes == 5);
 assert(p.gpon.dbru_stats.rewritten == BIT(1));
 assert(p.gpon.dbru_stats.flag_persists == 1);
 assert(!p.gpon.dbru_stats.flag_clears);

 reset(&p, BIT(1) | BIT(2), BIT(3));
 registers[TX_QUEUE_LEVEL(1) / 4] = 1200;
 registers[TX_QUEUE_LEVEL(2) / 4] = 600;
 gc1601_gpon_dbru_check(&p);
 assert(p.gpon.dbru_stats.rewrites == 2 && loads == 2 && writes == 8);
 assert(registers[TX_DBRU_LEVEL / 4] == 0);
 assert(p.gpon.dbru_stats.after == BIT(3));
 assert(p.gpon.dbru_stats.flag_clears == 1);

 reset(&p, BIT(1), 0);
 qmu_error = 1;
 gc1601_gpon_dbru_check(&p);
 assert(p.gpon.dbru_stats.qmu_errors == 2);
 assert(p.gpon.dbru_stats.rewrites == 1 && writes == 5);
 puts("GC1601 DBRu diagnostics: passed");
}
`);
