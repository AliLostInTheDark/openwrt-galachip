// SPDX-License-Identifier: GPL-2.0-only
/* See cortina_l3fe_logic.h. Moved verbatim; no line was rewritten.
 */
#include <linux/types.h>

#include "cortina_l3fe_logic.h"

/* WAN MAC = LAN/base MAC + 1 (per-board rule, stock-verified). */
void l3fe_wan_mac_derive(const u8 *lan_mac, u8 *wan_mac)
{
	u64 v = ((u64)lan_mac[0] << 40) | ((u64)lan_mac[1] << 32) |
		((u64)lan_mac[2] << 24) | ((u64)lan_mac[3] << 16) |
		((u64)lan_mac[4] << 8) | lan_mac[5];

	v++;
	wan_mac[0] = v >> 40;
	wan_mac[1] = v >> 32;
	wan_mac[2] = v >> 24;
	wan_mac[3] = v >> 16;
	wan_mac[4] = v >> 8;
	wan_mac[5] = v;
}
