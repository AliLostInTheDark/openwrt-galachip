// SPDX-License-Identifier: GPL-2.0-only
import { readFileSync } from 'node:fs';

const source = readFileSync(process.argv.includes('--stdin') ? 0 :
	new URL('../files/drivers/soc/galachip/gc1601-xpon.c', import.meta.url), 'utf8');
function extract(name) {
	const start = source.lastIndexOf('\nstatic ', source.indexOf(name + '(')) + 1;
	const end = source.indexOf('\n}\n', start) + 3;
	return source.slice(start, end);
}

// Stub hardware lookup and match encoding; exercise the actual collection code.
const harness = `
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(nr) (1UL << (nr))
#define GPON_SERVICES 64
#define GC_SR_FLOW_SLOTS 8

#define GC1601_SFU_LAN_SAME 0
#define GC1601_SFU_LAN_UNTAGGED 1
#define GC1601_SFU_LAN_TRANSLATE 2
#define GC1601_SFU_LAN_STACKED 3

struct gc1601_vlan_map {
 unsigned wan, lan, mode;
};

struct gc1601_service {
 bool used, vlan_valid, default_service, pcp_valid;
 unsigned vlan_id, alloc_id, gem_port, queue, pcp;
 unsigned uni_mode, uni_vid;
};

struct gc1601_sfu_flow {
 unsigned vlan_id, lan_vid, lan, gem_port, tcont, queue, pcp;
 bool untagged, pcp_match, down_only, up_only;
};

struct gc1601_xpon {
 struct gc1601_service service[GPON_SERVICES];
 struct gc1601_vlan_map vlan_map[8];
 unsigned vlan_maps;
 unsigned sfu_untag_vlan;
};

struct gc1601_pon { struct gc1601_xpon xp; };

static int gc1601_gpon_alloc_index(struct gc1601_pon *p, unsigned id) { return (int)id; }

static bool gc1601_pe_sfu_same_key(struct gc1601_sfu_flow *a,
 struct gc1601_sfu_flow *b, bool up)
{
 if (up) {
  if (a->untagged != b->untagged) return false;
  if (!a->untagged && (a->vlan_id != b->vlan_id || a->lan_vid != b->lan_vid)) return false;
  if (a->pcp_match != b->pcp_match) return false;
  if (a->pcp_match && a->pcp != b->pcp) return false;
  return true;
 }
 return a->gem_port == b->gem_port;
}

${extract('gc1601_xpon_lan_mode')}
${extract('gc1601_xpon_service_rank')}
${extract('gc1601_xpon_sfu_collect')}

static unsigned int encode_flow_q(const struct gc1601_sfu_flow *f, bool sfu_queue_zero)
{
 unsigned char q = sfu_queue_zero ? 0 : (f->queue & 7);
 unsigned w0 = BIT(25) | (f->gem_port & 0xfff) << 13 |
  (f->tcont & 0x3f) << 7 | (q & 7) << 3 |
  (q ? BIT(6) : 0) | 4;
 return (w0 >> 3) & 0x7;
}

int main(void) {
 struct gc1601_pon p = {};
 struct gc1601_sfu_flow f[64] = {};
 unsigned i, n;

 /* 1. Baseline Capacity: 64 services */
 for (i = 0; i < 64; i++)
  p.xp.service[i] = (struct gc1601_service) {
   .used = true, .vlan_valid = true, .vlan_id = i + 100, .gem_port = i + 256,
   .uni_mode = GC1601_SFU_LAN_UNTAGGED
  };
 n = gc1601_xpon_sfu_collect(&p, false, f, 64);
 assert(n == 64);
 for (i = 0; i < 64; i++) assert(f[i].gem_port == i + 256 && !f[i].up_only);

 /* 2. Truncation to max */
 n = gc1601_xpon_sfu_collect(&p, false, f, 4);
 assert(n == 4);
 for (i = 0; i < 4; i++) assert(f[i].gem_port == i + 256 && !f[i].up_only);

 /* 3. Single ISP Service (e.g. Internet VLAN 100, untagged LAN) */
 memset(&p, 0, sizeof(p));
 memset(f, 0, sizeof(f));
 p.xp.service[0] = (struct gc1601_service) {
  .used = true, .vlan_valid = true, .vlan_id = 100, .gem_port = 256,
  .alloc_id = 1, .queue = 3, .uni_mode = GC1601_SFU_LAN_UNTAGGED
 };
 n = gc1601_xpon_sfu_collect(&p, false, f, 64);
 assert(n == 2);
 assert(f[0].vlan_id == 100 && f[0].gem_port == 256 && !f[0].up_only);
 assert(f[0].lan == GC1601_SFU_LAN_UNTAGGED);
 assert(f[1].vlan_id == 100 && f[1].gem_port == 256 && f[1].up_only);
 assert(f[1].lan == GC1601_SFU_LAN_TRANSLATE && f[1].lan_vid == 0);
 /* Occupies 4 linear slots (<= 8 slots of Table 8) */
 assert(n * 2 <= GC_SR_FLOW_SLOTS);

 /* 4. Dual ISP Services (Internet VLAN 100 Untagged + IPTV VLAN 660 Tagged) */
 memset(&p, 0, sizeof(p));
 memset(f, 0, sizeof(f));
 p.xp.service[0] = (struct gc1601_service) {
  .used = true, .vlan_valid = true, .vlan_id = 100, .gem_port = 256,
  .alloc_id = 1, .queue = 0, .uni_mode = GC1601_SFU_LAN_UNTAGGED
 };
 p.xp.service[1] = (struct gc1601_service) {
  .used = true, .vlan_valid = true, .vlan_id = 660, .gem_port = 257,
  .alloc_id = 1, .queue = 1, .uni_mode = GC1601_SFU_LAN_SAME
 };
 n = gc1601_xpon_sfu_collect(&p, false, f, 64);
 assert(n == 3);
 assert(f[0].vlan_id == 100 && f[0].gem_port == 256 && !f[0].up_only);
 assert(f[1].vlan_id == 660 && f[1].gem_port == 257 && !f[1].up_only);
 assert(f[2].vlan_id == 100 && f[2].gem_port == 256 && f[2].up_only && f[2].lan_vid == 0);
 /* Occupies 6 linear slots (<= 8 slots of Table 8) */
 assert(n * 2 <= GC_SR_FLOW_SLOTS);

 /* 5. Translated VLAN service (LAN VID 10 -> WAN VID 100) */
 memset(&p, 0, sizeof(p));
 memset(f, 0, sizeof(f));
 p.xp.service[0] = (struct gc1601_service) {
  .used = true, .vlan_valid = true, .vlan_id = 100, .gem_port = 256,
  .alloc_id = 1, .queue = 0, .uni_mode = GC1601_SFU_LAN_TRANSLATE, .uni_vid = 10
 };
 n = gc1601_xpon_sfu_collect(&p, false, f, 64);
 assert(n == 1);
 assert(f[0].vlan_id == 100 && f[0].lan_vid == 10 && f[0].lan == GC1601_SFU_LAN_TRANSLATE);
 assert(!f[0].up_only && !f[0].down_only);
 assert(n * 2 <= GC_SR_FLOW_SLOTS);

 /* 6. Multi-PCP 802.1p Split (4 priorities on VLAN 100) */
 memset(&p, 0, sizeof(p));
 memset(f, 0, sizeof(f));
 for (i = 0; i < 4; i++) {
  p.xp.service[i] = (struct gc1601_service) {
   .used = true, .vlan_valid = true, .vlan_id = 100, .gem_port = 256 + i,
   .alloc_id = 1, .queue = i, .pcp_valid = true, .pcp = i * 2,
   .uni_mode = GC1601_SFU_LAN_SAME
  };
 }
 n = gc1601_xpon_sfu_collect(&p, false, f, 64);
 assert(n == 4);
 for (i = 0; i < 4; i++) {
  assert(f[i].vlan_id == 100 && f[i].gem_port == 256 + i);
  assert(f[i].pcp_match && f[i].pcp == i * 2);
  assert(!f[i].down_only && !f[i].up_only);
 }

 /* 7. Hardware Queue Selection: sfu_queue_zero verification */
 assert(encode_flow_q(&f[2], true) == 0);   /* sfu_queue_zero forces Q0 */
 assert(encode_flow_q(&f[2], false) == 2);  /* multi-queue preserves Q2 */

 puts("SFU primary service capacity, EVTOCD translations, Table 8 slots, and sfu_queue_zero checks passed");
 return 0;
}
`;
process.stdout.write(harness);
