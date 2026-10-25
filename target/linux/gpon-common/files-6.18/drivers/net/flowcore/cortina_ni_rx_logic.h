/* SPDX-License-Identifier: GPL-2.0-only */
/* cortina_ni_rx_logic.h -- logic hoisted out of ...
 * dev/MEASURED-cortina_ni_rx_logic.h.md sec 1. */
#ifndef _CORTINA_NI_RX_LOGIC_H
#define _CORTINA_NI_RX_LOGIC_H

#include <linux/types.h>

u32 rx_hdri_get(const u32 *w, unsigned int bit, unsigned int width);

/* Pack a MAC into the three L2FE FDB key words (aal __aal_mac_2_fdb_data;
 * vid/scind/dot1p = 0) - the append and the lookup-only path in
 * cortina-ni-rx.c share this so both hash to the same bucket. */
void cortina_ni_l2fe_fdb_key(const u8 *mac, u32 *d3, u32 *d2, u32 *d1);

/* THE BACKOFF LADDER for the decoupled LAN bring-up. The ...
 * dev/MEASURED-cortina_ni_rx_logic.h.md sec 2. */
#define CA_NI_RX_BRINGUP_FAST_TICKS	30u	/* 1/s for the first 30 s */
#define CA_NI_RX_BRINGUP_MID_TICKS	300u	/* then 1/8 s out to 5 min */
#define CA_NI_RX_BRINGUP_MID_PERIOD	8u
#define CA_NI_RX_BRINGUP_SLOW_PERIOD	60u	/* then once a minute, forever */

unsigned int cortina_ni_rx_bringup_period(u64 ticks);
bool cortina_ni_rx_bringup_due(u64 ticks);

/* Round two (2026-09-02): the L2FE bring-up's DECISIONS move ...
 * dev/MEASURED-cortina_ni_rx_logic.h.md sec 3. */
#define CA_NI_LSPID_PON			0x07
#define CA_NI_LSPID_L3_WAN		0x18
#define CA_NI_L2FE_LPORT_COUNT		64

/* Which WAN delivery class a HEADER_A lspid selects. Both WAN ...
 * dev/MEASURED-cortina_ni_rx_logic.h.md sec 4. */
enum ca_ni_rx_wan_class {
	CA_NI_RX_WAN_NONE = 0,	/* not a WAN lspid: fall through to eth0 */
	CA_NI_RX_WAN_PON,	/* de-encapsulated data-GEM frame, lspid PON */
	CA_NI_RX_WAN_L3,	/* L3-WAN lspid: L3FE punt, IF the HW-L3 path is armed */
};

enum ca_ni_rx_wan_class cortina_ni_rx_wan_class(u32 lspid);

/* L2FE FDB action/status codec. The engine protocol ...
 * dev/MEASURED-cortina_ni_rx_logic.h.md sec 5. */
u32 cortina_ni_l2fe_fdb_action(u32 ldpid);
int cortina_ni_l2fe_fdb_cmd_status_idx(u32 cmd_return);
bool cortina_ni_l2fe_fdb_action_da(u32 action, u32 *ldpid);

/* One lport's L2FE profile: every VALUE the per-port bring-up ...
 * dev/MEASURED-cortina_ni_rx_logic.h.md sec 6. */
struct ca_ni_lport_profile {
	u32 ilpb_d4;		/* WAN flag (GEM range) */
	u32 ilpb_d3;
	u32 ilpb_d2;		/* class: MC vs physical-port vs GEM */
	u32 ilpb_d1;		/* source-movement policy */
	u32 ilpb_d0;
	u32 mmshp_d1;		/* all-but-self isolation bitmap, hi word */
	u32 mmshp_d0;		/* lo word */
	u32 elpb_d0;		/* egress WAN/LAN choice */
	u8 chkid;		/* VLAN membership check-id (stock table) */
};

void cortina_ni_rx_lport_profile(unsigned int lport,
				 struct ca_ni_lport_profile *p);

#endif /* _CORTINA_NI_RX_LOGIC_H */
