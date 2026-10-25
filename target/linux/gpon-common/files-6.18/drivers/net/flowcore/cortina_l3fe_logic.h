/* SPDX-License-Identifier: GPL-2.0-only */
/* cortina_l3fe_logic.h -- logic hoisted out of cortina-l3fe.c
 * dev/MEASURED-cortina_l3fe_logic.h.md sec 1. */
#ifndef _CORTINA_L3FE_LOGIC_H
#define _CORTINA_L3FE_LOGIC_H

#include <linux/types.h>

void l3fe_wan_mac_derive(const u8 *lan_mac, u8 *wan_mac);

#endif /* _CORTINA_L3FE_LOGIC_H */
