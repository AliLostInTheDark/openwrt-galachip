'use strict';

/*
 * Readable fields for MIB objects. The agent returns each object's attribute
 * data as hex in G.988 attribute order, so a field is a name, a byte offset,
 * a length and a format. Classes without a table keep only their hex.
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

/* [ name, offset, length, format, names ] */
const layouts = {
	'2': [ [ 'mib_data_sync', 0, 1 ] ],
	'5': [
		[ 'actual_type', 0, 1 ], [ 'expected_type', 1, 1 ],
		[ 'expected_port_count', 2, 1 ],
		[ 'expected_equipment_id', 3, 20, 'str' ],
		[ 'actual_equipment_id', 23, 20, 'str' ],
		[ 'protection_profile', 43, 2, 'ptr' ]
	],
	'6': [
		[ 'type', 0, 1 ], [ 'ports', 1, 1 ], [ 'serial', 2, 8, 'serial' ],
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
		[ 'expected_type', 0, 1 ], [ 'sensed_type', 1, 1 ],
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
		[ 'forward_operation', 24, 1, 'hex' ],
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
		[ 'max_table_size', 1, 2 ], [ 'input_tpid', 3, 2, 'ptr' ],
		[ 'output_tpid', 5, 2, 'ptr' ], [ 'downstream_mode', 7, 1 ],
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
		[ 'equipment_id', 0, 20, 'str' ], [ 'omcc_version', 20, 1, 'hex' ],
		[ 'vendor_product_code', 21, 2 ], [ 'security_capability', 23, 1 ],
		[ 'security_mode', 24, 1 ], [ 'priority_queues', 25, 2 ],
		[ 'traffic_schedulers', 27, 1 ], [ 'gem_ports', 29, 2 ]
	],
	'262': [
		[ 'alloc_id', 0, 2, 'ptr' ],
		[ 'policy', 3, 1, 'enum', policies ]
	],
	'263': [
		[ 'sr_indication', 0, 1 ], [ 'tconts', 1, 2 ],
		[ 'gem_block_length', 3, 2 ], [ 'sf_threshold', 7, 1 ],
		[ 'sd_threshold', 8, 1 ], [ 'optical_signal_level', 11, 2, 'ptr' ],
		[ 'lower_optical_threshold', 13, 1 ],
		[ 'upper_optical_threshold', 14, 1 ],
		[ 'response_time', 15, 2 ], [ 'transmit_level', 17, 2, 'ptr' ]
	],
	'264': [
		[ 'admin_state', 2, 1, 'enum', admin_state ],
		[ 'management_capability', 3, 1 ],
		[ 'non_omci_management', 4, 2, 'ptr' ]
	],
	'266': [
		[ 'gem_port', 0, 2, 'ptr' ],
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
		[ 'allocated_size', 3, 2 ], [ 'related_port', 9, 4, 'ptr' ],
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

function text(data, offset, len) {
	let out = '';

	for (let i = 0; i < len; i++) {
		let c = byte(data, offset + i);

		if (!c)
			break;
		out += (c >= 0x20 && c < 0x7f) ? chr(c) : '.';
	}
	return trim(out);
}

function field(data, spec) {
	let offset = spec[1], len = spec[2], format = spec[3], names = spec[4];

	switch (format) {
	case 'str':
		return text(data, offset, len);
	case 'ptr':
		return sprintf(`0x%0${len * 2}x`, number(data, offset, len));
	case 'raw':
		return substr(data, offset * 2, len * 2);
	case 'hex':
		return sprintf('0x%02x', number(data, offset, len));
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

/* Priority 8 in a filter is "don't care", 14 is "default rule", 15 "no tag". */
function tag(pbit, vid, tpid, treatment) {
	if (pbit == 15)
		return treatment ? 'no tag added' : 'no tag';
	if (!treatment && pbit == 14)
		return 'default';

	let p = (pbit == 8) ? (treatment ? 'copy inner' : 'any') :
		(pbit == 9) ? 'copy outer' : (pbit == 10) ? 'from DSCP' : pbit;
	let v = (vid == 4096) ? (treatment ? 'copy inner' : 'any') :
		(vid == 4097) ? 'copy outer' : vid;

	return `vid ${v}, pbit ${p}, tpid ${tpid}`;
}

function rule_text(r) {
	return sprintf('filter outer [%s] inner [%s] ethertype %d -> remove %d, outer [%s] inner [%s]',
		tag(r.filter_outer_pbit, r.filter_outer_vid, r.filter_outer_tpid_dei),
		tag(r.filter_inner_pbit, r.filter_inner_vid, r.filter_inner_tpid_dei),
		r.filter_ethertype, r.tags_to_remove,
		tag(r.treat_outer_pbit, r.treat_outer_vid, r.treat_outer_tpid_dei, true),
		tag(r.treat_inner_pbit, r.treat_inner_vid, r.treat_inner_tpid_dei, true));
}

export function decode(item) {
	let layout = layouts[item.class_id];
	let data = item.data;
	let fields = {};

	if (!layout || type(data) != 'string')
		return null;
	for (let spec in layout)
		fields[spec[0]] = field(data, spec);

	if (item.class_id == 84) {
		fields.filters = [];
		for (let i = 0; i < fields.entries && i < 12; i++) {
			let tci = number(data, i * 2, 2);

			push(fields.filters, `vid ${tci & 0xfff}, pbit ${tci >> 13}, dei ${(tci >> 12) & 1}`);
		}
	}
	else if (item.class_id == 171 && item.extended_vlan)
		fields.rules = map(item.extended_vlan.rules || [], rule_text);
	return fields;
}
