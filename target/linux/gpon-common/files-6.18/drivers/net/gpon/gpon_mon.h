/* SPDX-License-Identifier: GPL-2.0-or-later */
/* TIER: CORE, header-only and pure: no MMIO, no allocator, no clock.
 *
 * One PLOAM or OMCI PDU as an Ethernet frame, so a monitor netdev hands the GPON
 * control plane to packet sockets: tcpdump and Wireshark instead of printk. The
 * direction is in the addresses (the OLT is 02:00:00:00:00:01, the ONU
 * 02:00:00:00:00:02); OMCI rides 0x88b5, the local experimental ethertype the
 * omci.lua Wireshark plugin registers (Wireshark ships no OMCI dissector), PLOAM
 * rides 0x88b6.
 */
#ifndef GPON_MON_H
#define GPON_MON_H

#include <linux/types.h>

#define GPON_MON_HDR		14
#define GPON_MON_ETH_OMCI	0x88b5
#define GPON_MON_ETH_PLOAM	0x88b6

enum gpon_mon_proto {
	GPON_MON_PLOAM = 0,
	GPON_MON_OMCI = 1,
};

/* Write the frame into @out (GPON_MON_HDR + @len bytes). -> frame length. */
static inline unsigned int gpon_mon_encap(u8 *out, enum gpon_mon_proto proto, int upstream,
					  const u8 *pdu, unsigned int len)
{
	static const u8 olt[6] = { 0x02, 0, 0, 0, 0, 0x01 };
	static const u8 onu[6] = { 0x02, 0, 0, 0, 0, 0x02 };
	const u8 *dst = upstream ? olt : onu;
	const u8 *src = upstream ? onu : olt;
	unsigned int type = proto == GPON_MON_OMCI ? GPON_MON_ETH_OMCI : GPON_MON_ETH_PLOAM;
	unsigned int i;

	for (i = 0; i < 6; i++) {
		out[i] = dst[i];
		out[6 + i] = src[i];
	}
	out[12] = (u8)(type >> 8);
	out[13] = (u8)type;
	for (i = 0; i < len; i++)
		out[GPON_MON_HDR + i] = pdu[i];
	return GPON_MON_HDR + len;
}

#endif /* GPON_MON_H */
