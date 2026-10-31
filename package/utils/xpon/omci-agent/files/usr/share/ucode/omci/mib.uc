'use strict';

/*
 * Readable fields for MIB objects. The agent returns each object's attribute
 * data as hex in G.988 attribute order, so a field is a name, a byte offset,
 * a length and a format. Text reads as ASCII and counts as decimal. Pointers,
 * TPIDs and version codes stay in hex as G.988 writes them, each with a plain
 * explanation in fields.notes. Object data that is not text says so, and the
 * raw bytes stay in item.data. Classes without a table keep only those bytes.
 */

const admin_state = [ 'unlocked', 'locked' ];
const oper_state = [ 'enabled', 'disabled' ];

const tp_types = {
	'1': 'PPTP Ethernet UNI', '2': 'interworking VCC', '3': '802.1p mapper',
	'4': 'IP host', '5': 'GEM IWTP', '6': 'multicast GEM IWTP', '7': 'PPTP xDSL',
	'8': 'PPTP VDSL', '9': 'Ethernet flow TP', '11': 'VEIP', '12': 'PPTP MoCA'
};

const association_types = {
	'0': 'MAC bridge port', '1': '802.1p mapper', '2': 'PPTP Ethernet UNI',
	'3': 'IP host', '4': 'PPTP xDSL', '5': 'GEM IWTP', '6': 'multicast GEM IWTP',
	'7': 'PPTP MoCA', '9': 'Ethernet flow TP', '10': 'VEIP',
	'11': 'MPLS pseudowire TP', '12': 'EFM bonding group'
};

const interwork_options = {
	'0': 'unstructured TDM', '1': 'MAC bridged LAN', '3': 'IP data service',
	'4': 'video return path', '5': '802.1p mapper', '6': 'downstream broadcast',
	'7': 'MPLS pseudowire TDM'
};

const directions = { '1': 'UNI to ANI', '2': 'ANI to UNI', '3': 'bidirectional' };
const policies = { '0': 'null', '1': 'strict priority', '2': 'WRR' };
const tpids = { '33024': '802.1Q', '34984': '802.1ad' };
const omcc_versions = {
	'128': 'G.984.4 (06/04)', '129': 'G.984.4 Amd.1 (06/05)', '130': 'G.984.4 Amd.2 (03/06)',
	'131': 'G.984.4 Amd.3 (12/06)', '132': 'G.984.4 (02/08)', '133': 'G.984.4 Amd.1 (06/09)',
	'134': 'G.984.4 Amd.2 (2009), baseline', '150': 'G.984.4 Amd.2 (2009), baseline and extended',
	'160': 'G.988 (2010), baseline', '161': 'G.988 Amd.1 (2011), baseline',
	'162': 'G.988 Amd.2 (2012), baseline', '163': 'G.988 (2012), baseline',
	'164': 'G.988 Amd.1 (2014), baseline', '176': 'G.988 (2010), baseline and extended',
	'177': 'G.988 Amd.1 (2011), baseline and extended',
	'178': 'G.988 Amd.2 (2012), baseline and extended',
	'179': 'G.988 (2012), baseline and extended',
	'180': 'G.988 Amd.1 (2014), baseline and extended'
};
const forward_operations = { '16': 'forward by VLAN list' };
const port_types = { '47': '10/100/1000BASE-T' };
const igmp_versions = { '1': 'IGMPv1', '2': 'IGMPv2', '3': 'IGMPv3', '16': 'MLDv1', '17': 'MLDv2' };

/* G.988 9.3.13, Extended VLAN tagging operation rule fields */
const ethertypes = {
	'1': 'IPv4 (0x0800)', '2': 'PPPoE (0x8863/0x8864)', '3': 'ARP (0x0806)',
	'4': 'IPv6 (0x86dd)'
};
const filter_tpids = {
	'4': 'TPID 0x8100', '5': 'input TPID', '6': 'input TPID, DEI 0',
	'7': 'input TPID, DEI 1'
};
const treat_tpids = {
	'0': 'TPID and DEI from inner', '1': 'TPID and DEI from outer',
	'2': 'output TPID, DEI from inner', '3': 'output TPID, DEI from outer',
	'4': 'TPID 0x8100', '6': 'output TPID, DEI 0', '7': 'output TPID, DEI 1'
};

/* [ name, offset, length, format, names ] */
const layouts = {
	'2': [ [ 'mib_data_sync', 0, 1 ] ],
	'5': [
		[ 'actual_type', 0, 1, 'enum', port_types ],
		[ 'expected_type', 1, 1, 'enum', port_types ],
		[ 'expected_port_count', 2, 1 ],
		[ 'expected_equipment_id', 3, 20, 'str' ],
		[ 'actual_equipment_id', 23, 20, 'str' ],
		[ 'protection_profile', 43, 2, 'ptr' ]
	],
	'6': [
		[ 'type', 0, 1, 'enum', port_types ], [ 'ports', 1, 1 ],
		[ 'serial', 2, 8, 'serial' ],
		[ 'version', 10, 14, 'str' ], [ 'vendor_id', 24, 4, 'str' ],
		[ 'admin_state', 28, 1, 'enum', admin_state ],
		[ 'oper_state', 29, 1, 'enum', oper_state ],
		[ 'bridged_or_ip', 30, 1 ], [ 'equipment_id', 31, 20, 'str' ],
		[ 'card_configuration', 51, 1 ], [ 'tcont_buffers', 52, 1 ],
		[ 'priority_queues', 53, 1 ], [ 'traffic_schedulers', 54, 1 ]
	],
	'7': [
		[ 'version', 0, 14, 'str' ], [ 'committed', 14, 1 ],
		[ 'active', 15, 1 ], [ 'valid', 16, 1 ]
	],
	'11': [
		[ 'expected_type', 0, 1, 'enum', port_types ],
		[ 'sensed_type', 1, 1, 'enum', port_types ],
		[ 'auto_detect', 2, 1 ], [ 'loopback', 3, 1 ],
		[ 'admin_state', 4, 1, 'enum', admin_state ],
		[ 'oper_state', 5, 1, 'enum', oper_state ],
		[ 'duplex', 6, 1 ], [ 'max_frame_size', 7, 2 ],
		[ 'dte_or_dce', 9, 1 ], [ 'pause_time', 10, 2 ],
		[ 'bridged_or_ip', 12, 1 ], [ 'arc', 13, 1 ],
		[ 'arc_interval', 14, 1 ], [ 'pppoe_filter', 15, 1 ],
		[ 'power_control', 16, 1 ]
	],
	'46': [
		[ 'bridge_mac', 0, 6, 'mac' ], [ 'priority', 6, 2 ],
		[ 'designated_root', 8, 8, 'raw' ], [ 'root_path_cost', 16, 4 ],
		[ 'bridge_ports', 20, 1 ], [ 'root_port', 21, 2 ],
		[ 'hello_time', 23, 2 ], [ 'forward_delay', 25, 2 ]
	],
	'45': [
		[ 'spanning_tree', 0, 1 ], [ 'learning', 1, 1 ],
		[ 'port_bridging', 2, 1 ], [ 'priority', 3, 2 ],
		[ 'max_age', 5, 2 ], [ 'hello_time', 7, 2 ],
		[ 'forward_delay', 9, 2 ], [ 'unknown_mac_discard', 11, 1 ],
		[ 'mac_learning_depth', 12, 1 ], [ 'ageing_time', 13, 4 ]
	],
	'47': [
		[ 'bridge', 0, 2, 'ptr' ], [ 'port_number', 2, 1 ],
		[ 'tp_type', 3, 1, 'enum', tp_types ], [ 'tp', 4, 2, 'ptr' ],
		[ 'priority', 6, 2 ], [ 'path_cost', 8, 2 ],
		[ 'spanning_tree', 10, 1 ], [ 'encapsulation', 11, 1 ],
		[ 'lan_fcs', 12, 1 ], [ 'mac', 13, 6, 'mac' ],
		[ 'outbound_td', 19, 2, 'ptr' ], [ 'inbound_td', 21, 2, 'ptr' ],
		[ 'mac_learning_depth', 23, 1 ]
	],
	'84': [
		[ 'forward_operation', 24, 1, 'code', forward_operations ],
		[ 'entries', 25, 1 ]
	],
	'130': [
		[ 'tp', 0, 2, 'ptr' ],
		[ 'pbit0_iwtp', 2, 2, 'ptr' ], [ 'pbit1_iwtp', 4, 2, 'ptr' ],
		[ 'pbit2_iwtp', 6, 2, 'ptr' ], [ 'pbit3_iwtp', 8, 2, 'ptr' ],
		[ 'pbit4_iwtp', 10, 2, 'ptr' ], [ 'pbit5_iwtp', 12, 2, 'ptr' ],
		[ 'pbit6_iwtp', 14, 2, 'ptr' ], [ 'pbit7_iwtp', 16, 2, 'ptr' ],
		[ 'unmarked_frame_option', 18, 1 ], [ 'default_pbit', 43, 1 ],
		[ 'tp_type', 44, 1 ]
	],
	'131': [
		[ 'vendor_id', 0, 4, 'str' ], [ 'equipment_id', 4, 20, 'str' ],
		[ 'version', 24, 14, 'str' ], [ 'time_of_day', 38, 14, 'raw' ]
	],
	'171': [
		[ 'association_type', 0, 1, 'enum', association_types ],
		[ 'max_table_size', 1, 2 ], [ 'input_tpid', 3, 2, 'code', tpids ],
		[ 'output_tpid', 5, 2, 'code', tpids ], [ 'downstream_mode', 7, 1 ],
		[ 'associated_me', 24, 2, 'ptr' ]
	],
	'256': [
		[ 'vendor_id', 0, 4, 'str' ], [ 'version', 4, 14, 'str' ],
		[ 'serial', 18, 8, 'serial' ], [ 'traffic_management', 26, 1 ],
		[ 'battery_backup', 28, 1 ],
		[ 'admin_state', 29, 1, 'enum', admin_state ],
		[ 'oper_state', 30, 1, 'enum', oper_state ]
	],
	'257': [
		[ 'equipment_id', 0, 20, 'str' ], [ 'omcc_version', 20, 1, 'code', omcc_versions ],
		[ 'vendor_product_code', 21, 2 ], [ 'security_capability', 23, 1 ],
		[ 'security_mode', 24, 1 ], [ 'priority_queues', 25, 2 ],
		[ 'traffic_schedulers', 27, 1 ], [ 'gem_ports', 29, 2 ]
	],
	'262': [
		[ 'alloc_id', 0, 2, 'alloc' ],
		[ 'policy', 3, 1, 'enum', policies ]
	],
	'263': [
		[ 'sr_indication', 0, 1 ], [ 'tconts', 1, 2 ],
		[ 'gem_block_length', 3, 2 ], [ 'sf_threshold', 7, 1 ],
		[ 'sd_threshold', 8, 1 ], [ 'optical_signal_level', 11, 2 ],
		[ 'lower_optical_threshold', 13, 1 ],
		[ 'upper_optical_threshold', 14, 1 ],
		[ 'response_time', 15, 2 ], [ 'transmit_level', 17, 2 ]
	],
	'264': [
		[ 'admin_state', 2, 1, 'enum', admin_state ],
		[ 'management_capability', 3, 1 ],
		[ 'non_omci_management', 4, 2, 'ptr' ]
	],
	'266': [
		[ 'gem_port', 0, 2 ],
		[ 'interworking', 2, 1, 'enum', interwork_options ],
		[ 'service_profile', 3, 2, 'ptr' ], [ 'interworking_tp', 5, 2, 'ptr' ],
		[ 'oper_state', 8, 1, 'enum', oper_state ],
		[ 'gal_profile', 9, 2, 'ptr' ]
	],
	'268': [
		[ 'port_id', 0, 2 ], [ 'tcont', 2, 2, 'ptr' ],
		[ 'direction', 4, 1, 'enum', directions ],
		[ 'upstream_tm', 5, 2, 'ptr' ], [ 'upstream_td', 7, 2, 'ptr' ],
		[ 'downstream_queue', 10, 2, 'ptr' ], [ 'encryption', 12, 1 ],
		[ 'downstream_td', 13, 2, 'ptr' ]
	],
	'272': [ [ 'max_gem_payload', 0, 2 ] ],
	'277': [
		[ 'queue_configuration', 0, 1 ], [ 'max_size', 1, 2 ],
		[ 'allocated_size', 3, 2 ], [ 'related_port', 9, 4, 'port' ],
		[ 'scheduler', 13, 2, 'ptr' ], [ 'weight', 15, 1 ]
	],
	'278': [
		[ 'tcont', 0, 2, 'ptr' ], [ 'parent', 2, 2, 'ptr' ],
		[ 'policy', 4, 1, 'enum', policies ], [ 'priority_weight', 5, 1 ]
	],
	'280': [
		[ 'cir', 0, 4 ], [ 'pir', 4, 4 ], [ 'cbs', 8, 4 ], [ 'pbs', 12, 4 ],
		[ 'colour_mode', 16, 1 ], [ 'ingress_colour', 17, 1 ],
		[ 'egress_colour', 18, 1 ], [ 'meter_type', 19, 1 ]
	],
	'281': [
		[ 'gem_port', 0, 2, 'ptr' ],
		[ 'interworking', 2, 1, 'enum', interwork_options ],
		[ 'service_profile', 3, 2, 'ptr' ],
		[ 'oper_state', 8, 1, 'enum', oper_state ],
		[ 'gal_profile', 9, 2, 'ptr' ]
	],
	'287': [
		[ 'me_type_table_size', 0, 4 ], [ 'message_type_table_size', 4, 4 ]
	],
	'288': [
		[ 'name', 0, 25, 'str' ], [ 'attributes_table_size', 25, 4 ],
		[ 'access', 29, 1 ], [ 'alarms_table_size', 30, 4 ],
		[ 'avcs_table_size', 34, 4 ], [ 'actions', 38, 4, 'ptr' ],
		[ 'instances_table_size', 42, 4 ], [ 'support', 46, 1 ]
	],
	'289': [
		[ 'name', 0, 25, 'str' ], [ 'size', 25, 2 ], [ 'access', 27, 1 ],
		[ 'format', 28, 1 ], [ 'lower_limit', 29, 4 ],
		[ 'upper_limit', 33, 4 ], [ 'bit_field', 37, 4, 'ptr' ],
		[ 'code_points_table_size', 41, 4 ], [ 'support', 45, 1 ]
	],
	'309': [
		[ 'igmp_version', 0, 1, 'enum', igmp_versions ],
		[ 'igmp_function', 1, 1 ], [ 'immediate_leave', 2, 1 ],
		[ 'upstream_igmp_tci', 3, 2, 'tci' ],
		[ 'upstream_igmp_tag_control', 5, 1 ],
		[ 'upstream_igmp_rate', 6, 4 ]
	],
	'310': [
		[ 'me_type', 0, 1 ], [ 'multicast_operations_profile', 1, 2, 'ptr' ],
		[ 'max_simultaneous_groups', 3, 2 ], [ 'max_multicast_bandwidth', 5, 4 ],
		[ 'bandwidth_enforcement', 9, 1 ]
	],
	'312': [
		[ 'interval_end_time', 0, 1 ], [ 'threshold_data', 1, 2, 'ptr' ],
		[ 'corrected_bytes', 3, 4 ], [ 'corrected_code_words', 7, 4 ],
		[ 'uncorrectable_code_words', 11, 4 ], [ 'total_code_words', 15, 4 ],
		[ 'fec_seconds', 19, 2 ]
	],
	'329': [
		[ 'admin_state', 0, 1, 'enum', admin_state ],
		[ 'oper_state', 1, 1, 'enum', oper_state ],
		[ 'interdomain_name', 2, 25, 'str' ],
		[ 'tcp_udp', 27, 2, 'ptr' ], [ 'iana_port', 29, 2 ]
	]
};

/*
 * Vendor MEs have no published attribute names: number them, with the
 * lengths libomci-protocol's me.c parses them with.
 */
function numbered(lengths) {
	let specs = [], offset = 0;

	for (let i, len in lengths) {
		push(specs, [ `attr_${i + 1}`, offset, len, len > 4 ? 'raw' : null ]);
		offset += len;
	}
	return specs;
}

const huawei_multicast = numbered([ 1, 2, 2, 2, 4 ]);

layouts['350'] = numbered([ 1, 1, 1, 1, 2, 4, 16, 4, 6 ]);
layouts['352'] = numbered([ 2, 2, 2, 1, 4, 4 ]);
layouts['353'] = numbered([ 14, 1, 1, 1, 4 ]);
for (let id in [ 367, 370, 373, 65408, 65414, 65425 ])
	layouts[id] = huawei_multicast;
layouts['65295'] = numbered([ 1, 2, 1, 2, 1, 2, 1, 2, 1, 2 ]);
layouts['65296'] = numbered([ 1, 1, 4, 1, 1, 1, 4, 1, 1, 12, 4, 25 ]);
layouts['65296'][9][3] = 'vendor_serial';
layouts['65297'] = numbered([ 1, 1, 2, 1, 1, 1, 1 ]);
layouts['65305'] = numbered([ 1, 6 ]);
layouts['65306'] = numbered([ 19 ]);

function byte(data, offset) {
	return hex(substr(data, offset * 2, 2)) ?? 0;
}

function number(data, offset, len) {
	let n = 0;

	for (let i = 0; i < len; i++)
		n = n * 256 + byte(data, offset + i);
	return n;
}

/* ASCII up to the NUL padding. Bytes that are not text stay in item.data. */
function text(data, offset, len) {
	let out = '', end = len;

	while (end > 0 && !byte(data, offset + end - 1))
		end--;
	for (let i = 0; i < end; i++) {
		let c = byte(data, offset + i);

		if (c < 0x20 || c >= 0x7f)
			return `not text (${end} byte${end == 1 ? '' : 's'})`;
		out += chr(c);
	}
	return trim(out);
}

function field(data, spec) {
	let offset = spec[1], len = spec[2], format = spec[3], names = spec[4];

	switch (format) {
	case 'str':
		return text(data, offset, len);
	case 'alloc': {
		let n = number(data, offset, len);
		return n == 0xffff ? 'none' : n;
	}
	case 'raw':
		return text(data, offset, len);
	case 'ptr':
	case 'code':
	case 'port':
	case 'tci':
		return sprintf(`0x%0${len * 2}x`, number(data, offset, len));
	case 'vendor_serial':
		return text(data, offset, 4) + ' ' + text(data, offset + 4, 4) +
			uc(substr(data, (offset + 8) * 2, 8));
	case 'mac': {
		let bytes = [];

		for (let i = 0; i < len; i++)
			push(bytes, substr(data, (offset + i) * 2, 2));
		return join(':', bytes);
	}
	case 'serial':
		return text(data, offset, 4) + uc(substr(data, (offset + 4) * 2, 8));
	case 'enum': {
		let n = number(data, offset, len);
		return names[n] != null ? `${n} (${names[n]})` : n;
	}
	default:
		return number(data, offset, len);
	}
}

const td = 'traffic descriptor';

/* What each pointer names: [ object, whether zero means no object ]. */
const pointers = {
	'5': { protection_profile: [ 'protection profile', true ] },
	'47': {
		bridge: [ 'MAC bridge service profile' ],
		tp: [ (data) => tp_types[byte(data, 3)] ],
		outbound_td: [ td, true ], inbound_td: [ td, true ]
	},
	'130': { tp: [ 'termination point', true ] },
	'171': { associated_me: [ (data) => association_types[byte(data, 0)] ] },
	'264': { non_omci_management: [ 'management object', true ] },
	'266': {
		gem_port: [ 'GEM port network CTP' ],
		service_profile: [ (data) => interwork_options[byte(data, 2)] ],
		interworking_tp: [ 'interworking TP', true ],
		gal_profile: [ 'GAL Ethernet profile' ]
	},
	'268': {
		tcont: [ 'T-CONT' ], upstream_tm: [ 'priority queue', true ],
		upstream_td: [ td, true ], downstream_queue: [ 'priority queue', true ],
		downstream_td: [ td, true ]
	},
	'277': { scheduler: [ 'traffic scheduler', true ] },
	'278': { tcont: [ 'T-CONT' ], parent: [ 'traffic scheduler', true ] },
	'281': {
		gem_port: [ 'GEM port network CTP' ],
		service_profile: [ 'service profile' ], gal_profile: [ 'GAL Ethernet profile' ]
	},
	'310': { multicast_operations_profile: [ 'multicast operations profile' ] },
	'329': { tcp_udp: [ 'TCP/UDP config', true ] }
};

for (let i = 0; i < 8; i++)
	pointers['130'][`pbit${i}_iwtp`] = [ 'GEM interworking TP', true ];

/* The plain explanation of a value kept in hex, or null. */
function note(class_id, spec, data) {
	let offset = spec[1], len = spec[2], format = spec[3], n = number(data, offset, len);

	switch (format) {
	case 'code':
		return spec[4][n];
	case 'port':
		return sprintf('port %d, priority %d', number(data, offset, 2), number(data, offset + 2, 2));
	case 'tci':
		return sprintf('VLAN %d, priority %d', n & 0xfff, n >> 13);
	case 'ptr': {
		let target = pointers[class_id]?.[spec[0]];

		if (n == 0xffff || (!n && target?.[1]))
			return 'none';
		let name = type(target?.[0]) == 'function' ? target[0](data) : target?.[0];
		return name ? `${name} ${n}` : `${n}`;
	}
	}
	return null;
}

/* Priority 8 in a filter is "don't care", 14 is "default rule", 15 "no tag". */
function filter_tag(pbit, vid, tpid) {
	if (pbit == 15)
		return 'no tag';
	if (pbit == 14)
		return 'default rule';

	let out = sprintf('VLAN %s, priority %s', vid == 4096 ? 'any' : vid,
			  pbit == 8 ? 'any' : pbit);
	return filter_tpids[tpid] ? `${out}, ${filter_tpids[tpid]}` : out;
}

function treat_tag(pbit, vid, tpid) {
	if (pbit == 15)
		return 'none';

	let p = (pbit == 8) ? 'from inner' : (pbit == 9) ? 'from outer' :
		(pbit == 10) ? 'from DSCP' : pbit;
	let v = (vid == 4096) ? 'from inner' : (vid == 4097) ? 'from outer' : vid;
	let out = `VLAN ${v}, priority ${p}`;

	return treat_tpids[tpid] ? `${out}, ${treat_tpids[tpid]}` : out;
}

function rule_summary(r) {
	let remove = r.tags_to_remove;

	return {
		filter_outer: filter_tag(r.filter_outer_pbit, r.filter_outer_vid, r.filter_outer_tpid_dei),
		filter_inner: filter_tag(r.filter_inner_pbit, r.filter_inner_vid, r.filter_inner_tpid_dei),
		ethertype: r.filter_ethertype ? (ethertypes[r.filter_ethertype] ?? `${r.filter_ethertype}`) : 'any',
		action: remove == 3 ? 'drop frame' : remove ? `remove ${remove} tag${remove > 1 ? 's' : ''}` : 'keep tags',
		add_outer: treat_tag(r.treat_outer_pbit, r.treat_outer_vid, r.treat_outer_tpid_dei),
		add_inner: treat_tag(r.treat_inner_pbit, r.treat_inner_vid, r.treat_inner_tpid_dei)
	};
}

function rule_text(s) {
	return sprintf('filter outer [%s] inner [%s] ethertype %s -> %s, add outer [%s] inner [%s]',
		s.filter_outer, s.filter_inner, s.ethertype, s.action, s.add_outer, s.add_inner);
}

/* 64 code points of 3 bits each, most significant first. */
function dscp_text(map) {
	if (type(map) != 'string' || match(map, /^0*$/))
		return 'all DSCP to priority 0';

	let out = [];

	for (let dscp = 0; dscp < 64; dscp++) {
		let bit = dscp * 3, n = 0;

		for (let i = 0; i < 3; i++, bit++)
			n = n * 2 + ((byte(map, bit >> 3) >> (7 - (bit & 7))) & 1);
		push(out, n);
	}
	return join(' ', out);
}

export function decode(item) {
	let layout = layouts[item.class_id];
	let data = item.data;
	let fields = {}, notes = {};

	if (!layout || type(data) != 'string')
		return null;
	for (let spec in layout) {
		let explained = note(item.class_id, spec, data);

		fields[spec[0]] = field(data, spec);
		if (explained)
			notes[spec[0]] = explained;
	}
	if (length(notes))
		fields.notes = notes;

	if (item.class_id == 84) {
		fields.filters = [];
		for (let i = 0; i < fields.entries && i < 12; i++) {
			let tci = number(data, i * 2, 2);

			push(fields.filters, `VLAN ${tci & 0xfff}, priority ${tci >> 13}, DEI ${(tci >> 12) & 1}`);
		}
	}
	else if (item.class_id == 171 && item.extended_vlan) {
		let vlan = item.extended_vlan;

		if (vlan.max_table_size != null)
			fields.max_table_size = vlan.max_table_size;
		fields.dscp_to_priority = dscp_text(vlan.dscp_to_pbit);
		fields.rules = [];
		for (let r in vlan.rules ?? []) {
			if (r.delete)
				continue;
			r.summary = rule_summary(r);
			push(fields.rules, rule_text(r.summary));
		}
	}
	return fields;
}
