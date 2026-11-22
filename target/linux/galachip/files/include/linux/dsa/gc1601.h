/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_DSA_TAG_GC1601_H
#define _NET_DSA_TAG_GC1601_H

/*
 * The GalaChip GC1601 packet engine takes the egress port from the TX
 * descriptor, not from the frame. The tagger appends one byte, a bitmask
 * of the destination port, which the conduit driver removes and turns into
 * descriptor bits. Received frames carry their ingress port as a
 * METADATA_HW_PORT_MUX dst, so they have no tag.
 */
#define GC1601_TAG_LEN		1

#endif
