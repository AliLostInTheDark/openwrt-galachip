// SPDX-License-Identifier: GPL-2.0-only
/*
 * OMCI multicast provisioning (G.988 9.3.27 and 9.3.28) for the router's IGMP
 * snooping and proxy.
 *
 * settings() turns the multicast operations profile the OLT attached to a
 * subscriber (through the multicast subscriber config info) into what the
 * LAN bridge, its ports and the firewall can enforce.
 */

export const CLASS = { profile: 309, subscriber: 310 };

const NULL_POINTER = 0xffff;

/* G.988 9.3.27 attribute numbers of the access control list tables */
const ACL_DYNAMIC = 7, ACL_STATIC = 8;

function bytes(data) {
	let out = [];

	for (let i = 0; i + 1 < length(data ?? ''); i += 2)
		push(out, hex(substr(data, i, 2)));
	return out;
}

function u8(b, at) { return b[at] ?? 0; }
function u16(b, at) { return (u8(b, at) << 8) | u8(b, at + 1); }
function u32(b, at) { return (u16(b, at) << 16) | u16(b, at + 2); }

/*
 * The group ranges, as numbers, of both access control lists. Each table
 * row is the attribute number, its length and the 24-byte row: table
 * control, GEM port, VLAN, source address, first and last destination
 * address, bandwidth.
 */
function acl(item) {
	let raw = bytes(item.mcast_tables), ranges = [];

	for (let at = 0; at + 26 <= length(raw); at += 26) {
		let attr = raw[at], row = slice(raw, at + 2, at + 26);

		if (attr != ACL_DYNAMIC && attr != ACL_STATIC)
			continue;
		let first = u32(row, 10), last = u32(row, 14);

		if (first || last)
			push(ranges, [ first, last || first ]);
	}
	return ranges;
}

/* The subscriber config info that names a profile; the VEIP's one first. */
function subscriber(objects) {
	let found = null;

	for (let o in objects) {
		if (o.class_id != CLASS.subscriber)
			continue;
		let b = bytes(o.data);

		if (u16(b, 1) == NULL_POINTER || u16(b, 1) == 0)
			continue;
		if (!found || (o.entity_id >> 8) == 0x06)
			found = { entity_id: o.entity_id, data: b };
	}
	return found;
}

/*
 * What to enforce, or null when no profile is provisioned. Zero values mean
 * "not set": the local default stays.
 */
export function settings(objects) {
	let sub = subscriber(objects);

	if (!sub)
		return null;

	let profile = null;
	for (let o in objects)
		if (o.class_id == CLASS.profile && o.entity_id == u16(sub.data, 1))
			profile = o;
	if (!profile)
		return null;

	let b = bytes(profile.data), version = u8(b, 0);
	let tag_control = u8(b, 5);

	return {
		profile: profile.entity_id,
		/* 2 and 3 are IGMPv2/v3, 16 and 17 MLDv1/v2 */
		igmp_version: version == 2 || version == 3 ? version : null,
		mld_version: version == 16 || version == 17 ? version - 15 : null,
		fast_leave: u8(b, 2) == 1,
		/* tag control 0 sends upstream IGMP as it is */
		upstream_pcp: tag_control ? (u16(b, 3) >> 13) : null,
		upstream_rate: u32(b, 6),
		robustness: u8(b, 10),
		query_interval_s: u32(b, 15),
		query_response_ds: u32(b, 19),
		last_member_ds: u32(b, 23),
		max_groups: u16(sub.data, 3),
		max_bandwidth: u32(sub.data, 5),
		acl: acl(profile),
	};
}
