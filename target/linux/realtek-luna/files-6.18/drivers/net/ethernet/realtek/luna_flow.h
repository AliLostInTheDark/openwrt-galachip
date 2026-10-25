/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LUNA_FLOW_H
#define _LUNA_FLOW_H

#include "luna_flow_mac.h"

struct luna_flow_layout {
	const struct luna_l34_acc *acc;
	const struct luna_flow_mac_layout *mac;
	u32 window_size;
	u8 sram_bits, port_bits, external_port_bits;
};

static const struct luna_flow_layout luna_flow_rtl9603cvd = {
	.acc = &luna_l34_acc_rtl9603cvd,
	.mac = &luna_flow_mac_rtl9603cvd,
	.window_size = 0x802000,
	.sram_bits = 10, .port_bits = 6, .external_port_bits = 6,
};

#endif
