'use strict';

/*
 * Offline decoding of the OLT's conversation: OMCI PDUs (G.988 baseline and
 * extended) and GPON PLOAM messages (G.984.3), as omcid's trace and the PON
 * driver's /proc/ploamcap record them. Only the attribute layouts below are
 * needed to split a Create, Set, Get response or MIB upload record into
 * named values; an ME missing here is still shown, as raw hex.
 *
 * An attribute is [ name, size, flags, format ]: flags 'c' set-by-create,
 * 't' a table (size is one row); format 'str', 'mac', 'sn', 'ip', 'hex' or
 * an enum object. Adding an OLT's vendor ME is one more entry.
 */

const admin = { '0': 'unlocked', '1': 'locked' };
const oper = { '0': 'enabled', '1': 'disabled' };
const tp_types = {
	'1': 'PPTP Eth UNI', '2': 'IW VCC', '3': '802.1p mapper', '4': 'IP host',
	'5': 'GEM IWTP', '6': 'mcast GEM IWTP', '7': 'PPTP xDSL', '8': 'PPTP VDSL',
	'9': 'Eth flow TP', '11': 'VEIP', '12': 'PPTP MoCA'
};
const assoc_types = {
	'0': 'MAC bridge port', '1': '802.1p mapper', '2': 'PPTP Eth UNI',
	'3': 'IP host', '4': 'PPTP xDSL', '5': 'GEM IWTP', '6': 'mcast GEM IWTP',
	'7': 'PPTP MoCA', '9': 'Eth flow TP', '10': 'VEIP', '11': 'MPLS PW TP',
	'12': 'EFM bonding group'
};
const iw_options = {
	'0': 'unstructured TDM', '1': 'MAC bridged LAN', '3': 'IP data',
	'4': 'video return', '5': '802.1p mapper', '6': 'ds broadcast',
	'7': 'MPLS PW TDM'
};
const directions = { '1': 'UNI->ANI', '2': 'ANI->UNI', '3': 'bidirectional' };
const policies = { '0': 'null', '1': 'strict priority', '2': 'WRR' };
/* G.988 9.9.3 SIP status, 9.9.11 VoIP voice server status and line state */
const sip_status = {
	'0': 'ok/initial', '1': 'connected', '2': 'failed (ICMP)',
	'3': 'failed (malformed response)', '4': 'failed (inadequate info)',
	'5': 'failed (timeout)', '6': 'redundant, offline'
};
const voice_server_status = {
	'0': 'none/initial', '1': 'registered', '2': 'in session',
	'3': 'failed registration (ICMP)', '4': 'failed registration (TCP)',
	'5': 'failed registration (authentication)', '6': 'failed registration (timeout)',
	'7': 'failed registration (server error)', '8': 'failed invite (ICMP)',
	'9': 'failed invite (TCP)', '10': 'failed invite (authentication)',
	'11': 'failed invite (timeout)', '12': 'failed invite (server error)',
	'13': 'port not configured', '14': 'configuration done'
};
const voip_line_state = {
	'0': 'idle, on hook', '1': 'off hook, dial tone', '2': 'dialling',
	'3': 'ringing', '4': 'audible ringback', '5': 'connecting', '6': 'connected',
	'7': 'disconnecting', '8': 'receiver off hook, no tone',
	'9': 'receiver off hook with tone', '10': 'unknown or error'
};

function counters(names, size) {
	return map(names, (n) => [ n, size ]);
}

function pm(rest) {
	return [ [ 'interval_end_time', 1 ], [ 'threshold_data', 2, 'c', 'hex' ], ...rest ];
}

const eth_frame_counters = counters([
	'drop_events', 'octets', 'packets', 'broadcast', 'multicast',
	'crc_errored', 'undersize', 'oversize', 'p64', 'p65_127', 'p128_255',
	'p256_511', 'p512_1023', 'p1024_1518'
], 4);

/* Vendor MEs have no published names: attr_N with the sizes me.c uses. */
function numbered(sizes) {
	let out = [];

	for (let i, n in sizes)
		push(out, [ `attr_${i + 1}`, n, 'c' ]);
	return out;
}

export const classes = {
	'2': [ 'ONU data', [ [ 'mib_data_sync', 1 ] ] ],
	'5': [ 'Cardholder', [
		[ 'actual_type', 1 ], [ 'expected_type', 1 ],
		[ 'expected_port_count', 1 ], [ 'expected_equipment_id', 20, '', 'str' ],
		[ 'actual_equipment_id', 20, '', 'str' ], [ 'protection_profile', 1 ],
		[ 'invoke_protection_switch', 1 ], [ 'arc', 1 ], [ 'arc_interval', 1 ]
	] ],
	'6': [ 'Circuit pack', [
		[ 'type', 1, 'c' ], [ 'ports', 1 ], [ 'serial', 8, '', 'sn' ],
		[ 'version', 14, '', 'str' ], [ 'vendor_id', 4, '', 'str' ],
		[ 'admin_state', 1, 'c', admin ], [ 'oper_state', 1, '', oper ],
		[ 'bridged_or_ip', 1 ], [ 'equipment_id', 20, '', 'str' ],
		[ 'card_configuration', 1, 'c' ], [ 'tcont_buffers', 1 ],
		[ 'priority_queues', 1 ], [ 'traffic_schedulers', 1 ],
		[ 'power_shed_override', 4, '', 'hex' ]
	] ],
	'7': [ 'Software image', [
		[ 'version', 14, '', 'str' ], [ 'committed', 1 ], [ 'active', 1 ],
		[ 'valid', 1 ], [ 'product_code', 25, '', 'str' ], [ 'image_hash', 16 ]
	] ],
	'11': [ 'PPTP Ethernet UNI', [
		[ 'expected_type', 1 ], [ 'sensed_type', 1 ], [ 'auto_detection', 1 ],
		[ 'loopback', 1 ], [ 'admin_state', 1, '', admin ],
		[ 'oper_state', 1, '', oper ], [ 'config_ind', 1, '', 'hex' ],
		[ 'max_frame_size', 2 ], [ 'dte_or_dce', 1 ], [ 'pause_time', 2 ],
		[ 'bridged_or_ip', 1 ], [ 'arc', 1 ], [ 'arc_interval', 1 ],
		[ 'pppoe_filter', 1 ], [ 'power_control', 1 ]
	] ],
	'24': [ 'Ethernet PM history data', pm(counters([
		'fcs_errors', 'excessive_collisions', 'late_collisions',
		'frames_too_long', 'rx_buffer_overflows', 'tx_buffer_overflows',
		'single_collisions', 'multiple_collisions', 'sqe_test',
		'deferred_tx', 'tx_mac_errors', 'carrier_sense_errors',
		'alignment_errors', 'rx_mac_errors'
	], 4)) ],
	'45': [ 'MAC bridge service profile', [
		[ 'spanning_tree', 1, 'c' ], [ 'learning', 1, 'c' ],
		[ 'port_bridging', 1, 'c' ], [ 'priority', 2, 'c' ],
		[ 'max_age', 2, 'c' ], [ 'hello_time', 2, 'c' ],
		[ 'forward_delay', 2, 'c' ], [ 'unknown_mac_discard', 1, 'c' ],
		[ 'mac_learning_depth', 1, 'c' ], [ 'ageing_time', 4, 'c' ]
	] ],
	'46': [ 'MAC bridge configuration data', [
		[ 'bridge_mac', 6, '', 'mac' ], [ 'priority', 2 ],
		[ 'designated_root', 8 ], [ 'root_path_cost', 4 ],
		[ 'bridge_ports', 1 ], [ 'root_port', 2 ], [ 'hello_time', 2 ],
		[ 'forward_delay', 2 ]
	] ],
	'47': [ 'MAC bridge port configuration data', [
		[ 'bridge', 2, 'c', 'hex' ], [ 'port_number', 1, 'c' ],
		[ 'tp_type', 1, 'c', tp_types ], [ 'tp', 2, 'c', 'hex' ],
		[ 'priority', 2, 'c' ], [ 'path_cost', 2, 'c' ],
		[ 'spanning_tree', 1, 'c' ], [ 'encapsulation', 1, 'c' ],
		[ 'lan_fcs', 1, 'c' ], [ 'mac', 6, '', 'mac' ],
		[ 'outbound_td', 2, '', 'hex' ], [ 'inbound_td', 2, '', 'hex' ],
		[ 'mac_learning_depth', 1, 'c' ], [ 'lasp', 2, '', 'hex' ]
	] ],
	'48': [ 'MAC bridge port designation data', [
		[ 'designated_root_cost_port', 24 ], [ 'port_state', 1 ]
	] ],
	'49': [ 'MAC bridge port filter table data', [ [ 'mac_filter_table', 8, 't' ] ] ],
	'50': [ 'MAC bridge port bridge table data', [ [ 'bridge_table', 8, 't' ] ] ],
	'51': [ 'MAC bridge PM history data', pm([ [ 'learning_discards', 4 ] ]) ],
	'52': [ 'MAC bridge port PM history data', pm(counters([
		'forwarded', 'delay_exceeded_discards', 'mtu_exceeded_discards',
		'received', 'received_discarded'
	], 4)) ],
	'78': [ 'VLAN tagging operation configuration data', [
		[ 'us_mode', 1, 'c' ], [ 'us_tci', 2, 'c', 'hex' ], [ 'ds_mode', 1, 'c' ],
		[ 'association_type', 1, 'c' ], [ 'associated_me', 2, 'c', 'hex' ]
	] ],
	'79': [ 'MAC bridge port filter preassign table', counters([
		'ipv4_mcast', 'ipv6_mcast', 'ipv4_bcast', 'rarp', 'ipx', 'netbeui',
		'appletalk', 'bpdu', 'arp', 'pppoe_bcast'
	], 1) ],
	'84': [ 'VLAN tagging filter data', [
		[ 'filter_list', 24, 'c' ], [ 'forward_operation', 1, 'c', 'hex' ],
		[ 'entries', 1, 'c' ]
	] ],
	'89': [ 'Ethernet PM history data 2', pm([ [ 'pppoe_filtered', 4 ] ]) ],
	'130': [ '802.1p mapper service profile', [
		[ 'tp', 2, 'c', 'hex' ],
		...map([ 0, 1, 2, 3, 4, 5, 6, 7 ], (p) => [ `pbit${p}_iwtp`, 2, 'c', 'hex' ]),
		[ 'unmarked_frame_option', 1, 'c' ], [ 'dscp_to_pbit', 24 ],
		[ 'default_pbit', 1, 'c' ], [ 'tp_type', 1, 'c' ]
	] ],
	'131': [ 'OLT-G', [
		[ 'vendor_id', 4, '', 'str' ], [ 'equipment_id', 20, '', 'str' ],
		[ 'version', 14, '', 'str' ], [ 'time_of_day', 14 ]
	] ],
	'133': [ 'ONU power shedding', counters([
		'restore_power_interval', 'data_interval', 'voice_interval',
		'video_overlay_interval', 'video_return_interval', 'dsl_interval',
		'atm_interval', 'ces_interval', 'frame_interval', 'sdh_interval',
		'shedding_status'
	], 2) ],
	'134': [ 'IP host config data', [
		[ 'ip_options', 1, '', 'hex' ], [ 'mac', 6, '', 'mac' ],
		[ 'onu_identifier', 25, '', 'str' ], [ 'ip', 4, '', 'ip' ],
		[ 'mask', 4, '', 'ip' ], [ 'gateway', 4, '', 'ip' ],
		[ 'primary_dns', 4, '', 'ip' ], [ 'secondary_dns', 4, '', 'ip' ],
		[ 'current_ip', 4, '', 'ip' ], [ 'current_mask', 4, '', 'ip' ],
		[ 'current_gateway', 4, '', 'ip' ], [ 'current_primary_dns', 4, '', 'ip' ],
		[ 'current_secondary_dns', 4, '', 'ip' ], [ 'domain_name', 25, '', 'str' ],
		[ 'host_name', 25, '', 'str' ], [ 'relay_agent_options', 2 ]
	] ],
	'136': [ 'TCP/UDP config data', [
		[ 'port', 2, 'c' ], [ 'protocol', 1, 'c' ], [ 'tos', 1, 'c' ],
		[ 'ip_host', 2, 'c', 'hex' ]
	] ],
	'137': [ 'Network address', [
		[ 'security', 2, 'c', 'hex' ], [ 'address', 2, 'c', 'hex' ]
	] ],
	'148': [ 'Authentication security method', [
		[ 'validation_scheme', 1 ], [ 'username1', 25, '', 'secret' ],
		[ 'password', 25, '', 'secret' ], [ 'realm', 25, '', 'str' ],
		[ 'username2', 14, '', 'secret' ]
	] ],
	'157': [ 'Large string', [
		[ 'parts', 1 ],
		...map([ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 ],
		       (n) => [ `part${n}`, 25, '', 'str' ])
	] ],
	'158': [ 'ONU remote debug', [
		[ 'command_format', 1 ], [ 'command', 25, '', 'str' ],
		[ 'reply_table', 25, 't' ]
	] ],
	'171': [ 'Extended VLAN tagging operation configuration data', [
		[ 'association_type', 1, 'c', assoc_types ], [ 'max_table_size', 2 ],
		[ 'input_tpid', 2, '', 'hex' ], [ 'output_tpid', 2, '', 'hex' ],
		[ 'downstream_mode', 1 ], [ 'rule', 16, 't' ],
		[ 'associated_me', 2, 'c', 'hex' ], [ 'dscp_to_pbit', 24 ]
	] ],
	'256': [ 'ONU-G', [
		[ 'vendor_id', 4, '', 'str' ], [ 'version', 14, '', 'str' ],
		[ 'serial', 8, '', 'sn' ], [ 'traffic_management', 1 ],
		[ 'deprecated', 1 ], [ 'battery_backup', 1 ],
		[ 'admin_state', 1, '', admin ], [ 'oper_state', 1, '', oper ],
		[ 'survival_time', 1 ], [ 'logical_onu_id', 24, '', 'secret' ],
		[ 'logical_password', 12, '', 'secret' ], [ 'credentials_status', 1 ],
		[ 'extended_tc_options', 2, '', 'hex' ]
	] ],
	'257': [ 'ONU2-G', [
		[ 'equipment_id', 20, '', 'str' ], [ 'omcc_version', 1, '', 'hex' ],
		[ 'vendor_product_code', 2 ], [ 'security_capability', 1 ],
		[ 'security_mode', 1 ], [ 'priority_queues', 2 ],
		[ 'traffic_schedulers', 1 ], [ 'deprecated', 1 ], [ 'gem_ports', 2 ],
		[ 'sys_up_time', 4 ], [ 'connectivity_capability', 2, '', 'hex' ],
		[ 'connectivity_mode', 1 ], [ 'qos_flexibility', 2, '', 'hex' ],
		[ 'pq_scale_factor', 2 ]
	] ],
	'262': [ 'T-CONT', [
		[ 'alloc_id', 2 ], [ 'deprecated', 1 ], [ 'policy', 1, '', policies ]
	] ],
	'263': [ 'ANI-G', [
		[ 'sr_indication', 1 ], [ 'tconts', 2 ], [ 'gem_block_length', 2 ],
		[ 'piggyback_dba', 1 ], [ 'deprecated', 1 ], [ 'sf_threshold', 1 ],
		[ 'sd_threshold', 1 ], [ 'arc', 1 ], [ 'arc_interval', 1 ],
		[ 'rx_level', 2, '', 'hex' ], [ 'lower_rx_threshold', 1 ],
		[ 'upper_rx_threshold', 1 ], [ 'response_time', 2 ],
		[ 'tx_level', 2, '', 'hex' ], [ 'lower_tx_threshold', 1 ],
		[ 'upper_tx_threshold', 1 ]
	] ],
	'264': [ 'UNI-G', [
		[ 'deprecated', 2 ], [ 'admin_state', 1, '', admin ],
		[ 'management_capability', 1 ], [ 'non_omci_management', 2, '', 'hex' ],
		[ 'relay_agent_options', 2, '', 'hex' ]
	] ],
	'266': [ 'GEM interworking termination point', [
		[ 'gem_ctp', 2, 'c', 'hex' ], [ 'interworking', 1, 'c', iw_options ],
		[ 'service_profile', 2, 'c', 'hex' ], [ 'interworking_tp', 2, 'c', 'hex' ],
		[ 'pptp_counter', 1 ], [ 'oper_state', 1, '', oper ],
		[ 'gal_profile', 2, 'c', 'hex' ], [ 'gal_loopback', 1 ]
	] ],
	'268': [ 'GEM port network CTP', [
		[ 'port_id', 2, 'c' ], [ 'tcont', 2, 'c', 'hex' ],
		[ 'direction', 1, 'c', directions ], [ 'us_queue', 2, 'c', 'hex' ],
		[ 'us_td', 2, 'c', 'hex' ], [ 'uni_counter', 1 ],
		[ 'ds_queue', 2, 'c', 'hex' ], [ 'encryption', 1 ],
		[ 'ds_td', 2, 'c', 'hex' ], [ 'key_ring', 1, 'c' ]
	] ],
	'272': [ 'GAL Ethernet profile', [ [ 'max_gem_payload', 2, 'c' ] ] ],
	'273': [ 'Threshold data 1', counters([ 't1', 't2', 't3', 't4', 't5', 't6', 't7' ], 4) ],
	'274': [ 'Threshold data 2', counters([ 't8', 't9', 't10', 't11', 't12', 't13', 't14' ], 4) ],
	'277': [ 'Priority queue', [
		[ 'queue_configuration', 1 ], [ 'max_size', 2 ], [ 'allocated_size', 2 ],
		[ 'discard_reset_interval', 2 ], [ 'discard_threshold', 2 ],
		[ 'related_port', 4, '', 'hex' ], [ 'scheduler', 2, '', 'hex' ],
		[ 'weight', 1 ], [ 'back_pressure', 2 ], [ 'back_pressure_time', 4 ],
		[ 'back_pressure_occur', 2 ], [ 'back_pressure_clear', 2 ],
		[ 'drop_thresholds', 8, '', 'hex' ], [ 'drop_max_p', 2 ],
		[ 'drop_w_q', 1 ], [ 'colour_marking', 1 ]
	] ],
	'278': [ 'Traffic scheduler', [
		[ 'tcont', 2, '', 'hex' ], [ 'parent', 2, '', 'hex' ],
		[ 'policy', 1, '', policies ], [ 'priority_weight', 1 ]
	] ],
	'280': [ 'Traffic descriptor', [
		[ 'cir', 4, 'c' ], [ 'pir', 4, 'c' ], [ 'cbs', 4, 'c' ], [ 'pbs', 4, 'c' ],
		[ 'colour_mode', 1, 'c' ], [ 'ingress_colour', 1, 'c' ],
		[ 'egress_colour', 1, 'c' ], [ 'meter_type', 1, 'c' ]
	] ],
	'281': [ 'Multicast GEM interworking termination point', [
		[ 'gem_ctp', 2, 'c', 'hex' ], [ 'interworking', 1, 'c', iw_options ],
		[ 'service_profile', 2, 'c', 'hex' ], [ 'not_used', 2, 'c' ],
		[ 'pptp_counter', 1 ], [ 'oper_state', 1, '', oper ],
		[ 'gal_profile', 2, 'c', 'hex' ], [ 'not_used2', 1, 'c' ],
		[ 'ipv4_table', 12, 't' ], [ 'ipv6_table', 24, 't' ]
	] ],
	'287': [ 'OMCI', [ [ 'me_types', 2, 't' ], [ 'message_types', 1, 't' ] ] ],
	'296': [ 'Ethernet PM history data 3', pm(counters([
		'drop_events', 'octets', 'packets', 'broadcast', 'multicast',
		'undersize', 'fragments', 'jabbers', 'p64', 'p65_127', 'p128_255',
		'p256_511', 'p512_1023', 'p1024_1518'
	], 4)) ],
	'309': [ 'Multicast operations profile', [
		[ 'igmp_version', 1, 'c' ], [ 'igmp_function', 1, 'c' ],
		[ 'immediate_leave', 1, 'c' ], [ 'us_igmp_tci', 2, 'c', 'hex' ],
		[ 'us_igmp_tag_control', 1, 'c' ], [ 'us_igmp_rate', 4, 'c' ],
		[ 'dynamic_acl', 24, 't' ], [ 'static_acl', 24, 't' ],
		[ 'lost_groups', 10, 't' ], [ 'robustness', 1, 'c' ],
		[ 'querier_ip', 4, 'c', 'ip' ], [ 'query_interval', 4, 'c' ],
		[ 'query_max_response', 4, 'c' ], [ 'last_member_query', 4 ],
		[ 'unauthorized_join', 1 ], [ 'ds_igmp_tci', 3, 'c', 'hex' ]
	] ],
	'310': [ 'Multicast subscriber config info', [
		[ 'me_type', 1, 'c' ], [ 'operations_profile', 2, 'c', 'hex' ],
		[ 'max_groups', 2, 'c' ], [ 'max_bandwidth', 4, 'c' ],
		[ 'bandwidth_enforcement', 1, 'c' ], [ 'service_packages', 20, 't' ],
		[ 'preview_groups', 22, 't' ]
	] ],
	'311': [ 'Multicast subscriber monitor', [
		[ 'me_type', 1, 'c' ], [ 'current_bandwidth', 4 ], [ 'joins', 4 ],
		[ 'bandwidth_exceeded', 4 ], [ 'ipv4_groups', 24, 't' ],
		[ 'ipv6_groups', 58, 't' ]
	] ],
	'312': [ 'FEC PM history data', pm([
		[ 'corrected_bytes', 4 ], [ 'corrected_codewords', 4 ],
		[ 'uncorrectable_codewords', 4 ], [ 'total_codewords', 4 ],
		[ 'fec_seconds', 2 ]
	]) ],
	'321': [ 'Ethernet frame PM history data downstream', pm(eth_frame_counters) ],
	'322': [ 'Ethernet frame PM history data upstream', pm(eth_frame_counters) ],
	'329': [ 'Virtual Ethernet interface point', [
		[ 'admin_state', 1, '', admin ], [ 'oper_state', 1, '', oper ],
		[ 'interdomain_name', 25, '', 'str' ], [ 'tcp_udp', 2, '', 'hex' ],
		[ 'iana_port', 2 ]
	] ],
	'334': [ 'Ethernet frame extended PM', [
		[ 'interval_end_time', 1 ], [ 'control_block', 16, 'c', 'hex' ],
		...eth_frame_counters
	] ],
	'340': [ 'BBF TR-069 management server', [
		[ 'admin_state', 1, '', admin ], [ 'acs_address', 2, '', 'hex' ],
		[ 'associated_tag', 2, '', 'hex' ]
	] ],
	'341': [ 'GEM port network CTP PM history data', pm([
		[ 'tx_frames', 4 ], [ 'rx_frames', 4 ], [ 'rx_bytes', 8 ],
		[ 'tx_bytes', 8 ], [ 'key_errors', 4 ]
	]) ],
	'425': [ 'Ethernet frame extended PM 64-bit', [
		[ 'interval_end_time', 1 ], [ 'control_block', 16, 'c', 'hex' ],
		...map(eth_frame_counters, (a) => [ a[0], 8 ])
	] ],

	/* Huawei */
	'53': [ 'Physical path termination point POTS UNI', [
		[ 'administrative_state', 1, '', admin ],
		[ 'interworking_tp_pointer', 2, '', 'hex' ],
		[ 'arc', 1 ],
		[ 'arc_interval', 1 ],
		[ 'impedance', 1 ],
		[ 'transmission_path', 1 ],
		[ 'rx_gain', 1 ],
		[ 'tx_gain', 1 ],
		[ 'operational_state', 1, '', oper ],
		[ 'hook_state', 1, '', { '0': 'on hook', '1': 'off hook' } ],
		[ 'pots_holdover_time', 2 ],
		[ 'nominal_feed_voltage', 1 ],
		[ 'loss_of_softswitch', 1 ]
	] ],
	'58': [ 'Voice service profile', [
		[ 'announcement_type', 1, 'c' ],
		[ 'jitter_target', 2, 'c' ],
		[ 'jitter_buffer_max', 2, 'c' ],
		[ 'echo_cancel_ind', 1, 'c' ],
		[ 'pstn_protocol_variant', 2, 'c' ],
		[ 'dtmf_digit_levels', 2, 'c' ],
		[ 'dtmf_digit_duration', 2, 'c' ],
		[ 'hook_flash_minimum_time', 2, 'c' ],
		[ 'hook_flash_maximum_time', 2, 'c' ],
		[ 'tone_pattern_table', 20, 't' ],
		[ 'tone_event_table', 7, 't' ],
		[ 'ringing_pattern_table', 5, 't' ],
		[ 'ringing_event_table', 7, 't' ],
		[ 'network_specific_extensions_pointer', 2, 'c', 'hex' ]
	] ],
	'138': [ 'VoIP config data', [
		[ 'available_signalling_protocols', 1 ],
		[ 'signalling_protocol_used', 1, '', { '0': 'none', '1': 'SIP', '2': 'H.248', '3': 'MGCP', '255': 'not used' } ],
		[ 'available_voip_configuration_methods', 4 ],
		[ 'voip_configuration_method_used', 1 ],
		[ 'voip_configuration_address_pointer', 2, '', 'hex' ],
		[ 'voip_configuration_state', 1, '', { '0': 'inactive', '1': 'active', '2': 'initializing', '3': 'fault' } ],
		[ 'retrieve_profile', 1 ],
		[ 'profile_version', 25, '', 'str' ]
	] ],
	'139': [ 'VoIP voice CTP', [
		[ 'user_protocol_pointer', 2, 'c', 'hex' ],
		[ 'pptp_pointer', 2, 'c', 'hex' ],
		[ 'voip_media_profile_pointer', 2, 'c', 'hex' ],
		[ 'signalling_code', 1, 'c' ]
	] ],
	'140': [ 'Call control performance monitoring history data', pm([
		[ 'call_setup_failures', 4 ],
		[ 'call_setup_timer', 4 ],
		[ 'call_terminate_failures', 4 ],
		[ 'analog_port_releases', 4 ],
		[ 'analog_port_off_hook_timer', 4 ]
	]) ],
	'141': [ 'VoIP line status', [
		[ 'voip_codec_used', 2 ],
		[ 'voip_voice_server_status', 1, '', voice_server_status ],
		[ 'voip_port_session_type', 1 ],
		[ 'voip_call_1_packet_period', 2 ],
		[ 'voip_call_2_packet_period', 2 ],
		[ 'voip_call_1_dest_addr', 25, '', 'str' ],
		[ 'voip_call_2_dest_addr', 25, '', 'str' ],
		[ 'voip_line_state', 1, '', voip_line_state ],
		[ 'emergency_call_status', 1 ]
	] ],
	'142': [ 'VoIP media profile', [
		[ 'fax_mode', 1, 'c' ],
		[ 'voice_service_profile_pointer', 2, 'c', 'hex' ],
		[ 'codec_selection_1st_order', 1, 'c' ],
		[ 'packet_period_selection_1st_order', 1, 'c' ],
		[ 'silence_suppression_1st_order', 1, 'c' ],
		[ 'codec_selection_2nd_order', 1, 'c' ],
		[ 'packet_period_selection_2nd_order', 1, 'c' ],
		[ 'silence_suppression_2nd_order', 1, 'c' ],
		[ 'codec_selection_3rd_order', 1, 'c' ],
		[ 'packet_period_selection_3rd_order', 1, 'c' ],
		[ 'silence_suppression_3rd_order', 1, 'c' ],
		[ 'codec_selection_4th_order', 1, 'c' ],
		[ 'packet_period_selection_4th_order', 1, 'c' ],
		[ 'silence_suppression_4th_order', 1, 'c' ],
		[ 'oob_dtmf', 1, 'c' ],
		[ 'rtp_profile_pointer', 2, 'c', 'hex' ]
	] ],
	'143': [ 'RTP profile data', [
		[ 'local_port_min', 2, 'c' ],
		[ 'local_port_max', 2, 'c' ],
		[ 'dscp_mark', 1, 'c' ],
		[ 'piggyback_events', 1, 'c' ],
		[ 'tone_events', 1, 'c' ],
		[ 'dtmf_events', 1, 'c' ],
		[ 'cas_events', 1, 'c' ],
		[ 'ip_host_config_pointer', 2, '', 'hex' ]
	] ],
	'144': [ 'RTP performance monitoring history data', pm([
		[ 'rtp_errors', 4 ],
		[ 'packet_loss', 4 ],
		[ 'maximum_jitter', 4 ],
		[ 'maximum_time_between_rtcp_packets', 4 ],
		[ 'buffer_underflows', 4 ],
		[ 'buffer_overflows', 4 ]
	]) ],
	'145': [ 'Network dial plan table', [
		[ 'dial_plan_number', 2 ],
		[ 'dial_plan_table_max_size', 2, 'c' ],
		[ 'critical_dial_timeout', 2, 'c' ],
		[ 'partial_dial_timeout', 2, 'c' ],
		[ 'dial_plan_format', 1, 'c' ],
		[ 'dial_plan_table', 30, 't' ]
	] ],
	'146': [ 'VoIP application service profile', [
		[ 'cid_features', 1, 'c' ],
		[ 'call_waiting_features', 1, 'c' ],
		[ 'call_progress_or_transfer_features', 2, 'c' ],
		[ 'call_presentation_features', 2, 'c' ],
		[ 'direct_connect_feature', 1, 'c' ],
		[ 'direct_connect_uri_pointer', 2, 'c', 'hex' ],
		[ 'bridged_line_agent_uri_pointer', 2, 'c', 'hex' ],
		[ 'conference_factory_uri_pointer', 2, 'c', 'hex' ],
		[ 'dial_tone_feature_delay_warmline_timer', 2 ]
	] ],
	'147': [ 'VoIP feature access codes', [
		[ 'cancel_call_waiting', 5 ],
		[ 'call_hold', 5 ],
		[ 'call_park', 5 ],
		[ 'caller_id_activate', 5 ],
		[ 'caller_id_deactivate', 5 ],
		[ 'do_not_disturb_activation', 5 ],
		[ 'do_not_disturb_deactivation', 5 ],
		[ 'do_not_disturb_pin_change', 5 ],
		[ 'emergency_service_number', 5 ],
		[ 'intercom_service', 5 ],
		[ 'unattended_blind_call_transfer', 5 ],
		[ 'attended_call_transfer', 5 ]
	] ],
	'150': [ 'SIP agent config data', [
		[ 'proxy_server_address_pointer', 2, 'c', 'hex' ],
		[ 'outbound_proxy_address_pointer', 2, 'c', 'hex' ],
		[ 'primary_sip_dns', 4, 'c' ],
		[ 'secondary_sip_dns', 4, 'c' ],
		[ 'tcp_udp_pointer', 2, '', 'hex' ],
		[ 'sip_reg_exp_time', 4 ],
		[ 'sip_rereg_head_start_time', 4 ],
		[ 'host_part_uri', 2, 'c' ],
		[ 'sip_status', 1, '', sip_status ],
		[ 'sip_registrar', 2, 'c' ],
		[ 'softswitch', 4, 'c' ],
		[ 'sip_response_table', 5, 't' ],
		[ 'sip_option_transmit_control', 1, 'c' ],
		[ 'sip_uri_format', 1 ],
		[ 'redundant_sip_agent_pointer', 2, 'c', 'hex' ]
	] ],
	'151': [ 'SIP agent performance monitoring history data', pm([
		[ 'transactions', 4 ],
		[ 'rx_invite_reqs', 4 ],
		[ 'rx_invite_retrans', 4 ],
		[ 'rx_noninvite_reqs', 4 ],
		[ 'rx_noninvite_retrans', 4 ],
		[ 'rx_response', 4 ],
		[ 'rx_response_retransmissions', 4 ],
		[ 'tx_invite_reqs', 4 ],
		[ 'tx_invite_retrans', 4 ],
		[ 'tx_noninvite_reqs', 4 ],
		[ 'tx_noninvite_retrans', 4 ],
		[ 'tx_response', 4 ],
		[ 'tx_response_retransmissions', 4 ]
	]) ],
	'152': [ 'SIP call initiation performance monitoring history data', pm([
		[ 'failed_to_connect_counter', 4 ],
		[ 'failed_to_validate_counter', 4 ],
		[ 'timeout_counter', 4 ],
		[ 'failure_received_counter', 4 ],
		[ 'failed_to_authenticate_counter', 4 ]
	]) ],
	'153': [ 'SIP user data', [
		[ 'sip_agent_pointer', 2, 'c', 'hex' ],
		[ 'user_part_aor', 2, 'c' ],
		[ 'sip_display_name', 25, '', 'str' ],
		[ 'username_password', 2, 'c' ],
		[ 'voicemail_server_sip_uri', 2, 'c' ],
		[ 'voicemail_subscription_expiration_time', 4, 'c' ],
		[ 'network_dial_plan_pointer', 2, 'c', 'hex' ],
		[ 'application_services_profile_pointer', 2, 'c', 'hex' ],
		[ 'feature_code_pointer', 2, 'c', 'hex' ],
		[ 'pptp_pointer', 2, 'c', 'hex' ],
		[ 'release_timer', 1 ],
		[ 'roh_timer', 1 ]
	] ],
	'276': [ 'GAL Ethernet performance monitoring history data', pm([
		[ 'discarded_frames', 4 ]
	]) ],
	'298': [ 'Dot1 rate limiter', [
		[ 'parent_me_pointer', 2, 'c', 'hex' ],
		[ 'tp_type', 1, 'c' ],
		[ 'upstream_unicast_flood_rate_pointer', 2, 'c', 'hex' ],
		[ 'upstream_broadcast_rate_pointer', 2, 'c', 'hex' ],
		[ 'upstream_multicast_payload_rate_pointer', 2, 'c', 'hex' ],
		[ 'downstream_unicast_flood_rate_pointer', 2, '', 'hex' ],
		[ 'downstream_broadcast_rate_pointer', 2, '', 'hex' ],
		[ 'downstream_multicast_payload_rate_pointer', 2, '', 'hex' ]
	] ],
	'336': [ 'ONU dynamic power management control', [
		[ 'power_reduction_management_capability', 1 ],
		[ 'power_reduction_management_mode', 1 ],
		[ 'itransinit', 2 ],
		[ 'itxinit', 2 ],
		[ 'maximum_sleep_interval', 4 ],
		[ 'maximum_receiver_off_interval', 4 ],
		[ 'minimum_aware_interval', 4 ],
		[ 'minimum_active_held_interval', 2 ],
		[ 'maximum_sleep_interval_extension', 8 ],
		[ 'ethernet_passive_optical_network_capability_extension', 1 ],
		[ 'ethernet_passive_optical_network_setup_extension', 1 ],
		[ 'missing_consecutive_bursts_threshold', 4 ]
	] ],
	'350': [ 'Huawei flow mapping', numbered([ 1, 1, 1, 1, 2, 4, 16, 4, 6 ]) ],
	'352': [ 'Huawei VLAN mapping', numbered([ 2, 2, 2, 1, 4, 4 ]) ],
	'353': [ 'Huawei software image extension', numbered([ 14, 1, 1, 1, 4 ]) ],

	/* Nokia/ALCL */
	'65295': [ 'Nokia optical supervision', numbered([ 1, 2, 1, 2, 1, 2, 1, 2, 1, 2 ]) ],
	'65296': [ 'Nokia ONT generic V2', numbered([ 1, 1, 4, 1, 1, 1, 4, 1, 1, 12, 4, 25 ]) ],
	'65297': [ 'Nokia UNI supplemental V2', numbered([ 1, 1, 2, 1, 1, 1, 1 ]) ],
	'65305': [ 'Nokia VLAN downstream supplemental', numbered([ 1, 6 ]) ],
	'65306': [ 'Nokia VLAN upstream policer', numbered([ 19 ]) ]
};

for (let id in [ 367, 370, 373, 65408, 65414, 65425 ])
	classes[id] = [ `Huawei multicast ${id}`, numbered([ 1, 2, 2, 2, 4 ]) ];

export const actions = {
	'4': 'Create', '6': 'Delete', '8': 'Set', '9': 'Get',
	'11': 'Get all alarms', '12': 'Get all alarms next', '13': 'MIB upload',
	'14': 'MIB upload next', '15': 'MIB reset', '16': 'Alarm', '17': 'AVC',
	'18': 'Test', '19': 'Start SW download', '20': 'Download section',
	'21': 'End SW download', '22': 'Activate image', '23': 'Commit image',
	'24': 'Synchronize time', '25': 'Reboot', '26': 'Get next',
	'27': 'Test result', '28': 'Get current data', '29': 'Set table'
};

export const results = {
	'0': 'success', '1': 'processing error', '2': 'not supported',
	'3': 'parameter error', '4': 'unknown ME', '5': 'unknown instance',
	'6': 'device busy', '7': 'instance exists', '9': 'attribute(s) failed'
};

export function vendor_class(class_id) {
	return (class_id >= 240 && class_id <= 255) || class_id >= 65280;
}

export function class_name(class_id) {
	let c = classes[class_id];

	return c ? c[0] : (vendor_class(class_id) ? `vendor ME ${class_id}` : `ME ${class_id}`);
}

export function bytes(hexstr) {
	let out = [];

	for (let i = 0; i + 1 < length(hexstr); i += 2)
		push(out, hex(substr(hexstr, i, 2)));
	return out;
}

function be(b, at, len) {
	let n = 0;

	for (let i = 0; i < len; i++)
		n = n * 256 + (b[at + i] ?? 0);
	return n;
}

function hexstr(b, at, len) {
	let s = '';

	for (let i = 0; i < len; i++)
		s += sprintf('%02x', b[at + i] ?? 0);
	return s;
}

function text(b, at, len) {
	let s = '';

	for (let i = 0; i < len; i++) {
		let c = b[at + i] ?? 0;

		if (!c)
			break;
		s += (c >= 0x20 && c < 0x7f) ? chr(c) : '.';
	}
	return trim(s);
}

export function serial(b, at) {
	return text(b, at, 4) + uc(hexstr(b, at + 4, 4));
}

/* Priority 8 in a filter is "don't care", 14 a default rule, 15 no tag. */
function tag(pbit, vid, tpid, treatment) {
	if (pbit == 15)
		return treatment ? '-' : 'untagged';
	if (!treatment && pbit == 14)
		return 'default';

	let p = (pbit == 8) ? (treatment ? 'inner' : '*') :
		(pbit == 9) ? 'outer' : (pbit == 10) ? 'dscp' : pbit;
	let v = (vid == 4096) ? (treatment ? 'inner' : '*') :
		(vid == 4097) ? 'outer' : vid;

	return `${v}/p${p}` + (tpid ? `/t${tpid}` : '');
}

/* G.988 9.3.13 received frame VLAN tagging operation table row. */
export function ext_vlan_rule(b, at) {
	let w = map([ 0, 4, 8, 12 ], (o) => be(b, at + o, 4));
	let ethertypes = [ '*', 'IPoE', 'PPPoE', 'ARP', 'IPv6' ];

	return sprintf('[%s %s %s] -> pop %d push [%s %s]',
		tag(w[0] >> 28, (w[0] >> 15) & 0x1fff, (w[0] >> 12) & 7),
		tag(w[1] >> 28, (w[1] >> 15) & 0x1fff, (w[1] >> 12) & 7),
		ethertypes[w[1] & 15] ?? `eth${w[1] & 15}`, w[2] >> 30,
		tag((w[2] >> 16) & 15, (w[2] >> 3) & 0x1fff, w[2] & 7, true),
		tag((w[3] >> 16) & 15, (w[3] >> 3) & 0x1fff, w[3] & 7, true));
}

/* G.988: a row whose treatment words are all ones deletes its filter. */
export function ext_vlan_rule_deleted(b, at) {
	for (let i = 8; i < 16; i++)
		if (b[at + i] != 0xff)
			return false;
	return true;
}

function value(class_id, spec, b, at) {
	let size = spec[1], format = spec[3];

	if (class_id == 171 && spec[0] == 'rule')
		return ext_vlan_rule(b, at);
	if (class_id == 84 && spec[0] == 'filter_list') {
		let vids = [];

		for (let i = 0; i < 12; i++) {
			let tci = be(b, at + 2 * i, 2);

			if (tci)
				push(vids, `${tci & 0xfff}/p${tci >> 13}`);
		}
		return length(vids) ? join(',', vids) : '-';
	}
	switch (format) {
	case 'str':
		return `"${text(b, at, size)}"`;
	case 'secret':
		return text(b, at, size) == '' ? '""' : '<redacted>';
	case 'sn':
		return serial(b, at);
	case 'mac':
		return join(':', map([ 0, 1, 2, 3, 4, 5 ], (i) => hexstr(b, at + i, 1)));
	case 'ip':
		return join('.', map([ 0, 1, 2, 3 ], (i) => b[at + i] ?? 0));
	case 'hex':
		return '0x' + hexstr(b, at, size);
	default:
		if (type(format) == 'object') {
			let n = be(b, at, size);

			return format[n] != null ? `${n}(${format[n]})` : n;
		}
		return size > 4 ? '0x' + hexstr(b, at, size) : be(b, at, size);
	}
}

/*
 * Split attribute values packed in mask order (attribute 1 is bit 15).
 * A table attribute carries one row in a Set, and its 4-byte size in a Get
 * response (get_response). Returns { attrs, complete }.
 */
export function attributes(class_id, mask, b, at, end, get_response) {
	let c = classes[class_id], out = [];

	if (!mask)
		return { attrs: out, complete: true };
	if (!c) {
		push(out, { name: sprintf('mask 0x%04x', mask), value: '0x' + hexstr(b, at, end - at) });
		return { attrs: out, complete: false };
	}
	for (let i = 0; i < 16; i++) {
		if (!(mask & (0x8000 >> i)))
			continue;

		let spec = c[1][i];

		if (!spec) {
			push(out, { name: `attr_${i + 1}`, value: '0x' + hexstr(b, at, end - at) });
			return { attrs: out, complete: false };
		}
		let size = (get_response && index(spec[2] ?? '', 't') >= 0) ? 4 : spec[1];

		if (at + size > end) {
			push(out, { name: spec[0], value: '(truncated)' });
			return { attrs: out, complete: false };
		}
		push(out, { name: spec[0], index: i + 1, value: (size == spec[1]) ?
			    value(class_id, spec, b, at) : `table ${be(b, at, size)} bytes`,
			    at, size });
		at += size;
	}
	return { attrs: out, complete: true };
}

/* The attribute mask of a Create: every set-by-create attribute. */
export function create_mask(class_id) {
	let c = classes[class_id], mask = 0;

	if (!c)
		return null;
	for (let i, spec in c[1])
		if (index(spec[2] ?? '', 'c') >= 0)
			mask |= 0x8000 >> i;
	return mask;
}

export function mask_names(class_id, mask) {
	let c = classes[class_id], names = [];

	for (let i = 0; i < 16; i++)
		if (mask & (0x8000 >> i))
			push(names, c?.[1]?.[i]?.[0] ?? `attr_${i + 1}`);
	return join(',', names);
}

/*
 * One OMCI PDU as omcid traced it: baseline (device id 0x0a, 32 bytes of
 * contents) or extended (0x0b, explicit length). The trailer is ignored.
 */
export function omci_pdu(hexdata) {
	let b = bytes(hexdata);

	if (length(b) < 8)
		return null;

	let mt = b[2], extended = b[3] == 0x0b;
	let p = {
		tci: be(b, 0, 2), mt, db: !!(mt & 0x80), ar: !!(mt & 0x40),
		ak: !!(mt & 0x20), action: mt & 0x1f, extended,
		class_id: be(b, 4, 2), entity: be(b, 6, 2), b
	};

	p.at = extended ? 10 : 8;
	p.end = extended ? min(10 + be(b, 8, 2), length(b)) : min(40, length(b));
	return p;
}

export function action_name(action) {
	return actions[action] ?? `action ${action}`;
}

export function result_name(r) {
	return results[r] ?? `result ${r}`;
}

/* ---- PLOAM, G.984.3 ---- */

const ploam_down = {
	'1': 'Upstream_Overhead', '2': 'Serial_Number_Mask', '3': 'Assign_ONU-ID',
	'4': 'Ranging_Time', '5': 'Deactivate_ONU-ID', '6': 'Disable_Serial_Number',
	'8': 'Encrypted_Port-ID', '9': 'Request_Password', '10': 'Assign_Alloc-ID',
	'11': 'No_Message', '12': 'POPUP', '13': 'Request_Key',
	'14': 'Configure_Port-ID', '15': 'PEE', '16': 'Change_Power_Level',
	'17': 'PST', '18': 'BER_Interval', '19': 'Key_Switching_Time',
	'20': 'Extended_Burst_Length', '21': 'PON-ID', '22': 'Swift_POPUP',
	'23': 'Ranging_Adjustment', '24': 'Sleep_Allow'
};

const ploam_up = {
	'1': 'Serial_Number_ONU', '2': 'Password', '3': 'Dying_Gasp',
	'4': 'No_Message', '5': 'Encryption_Key', '6': 'PEE', '7': 'PST',
	'8': 'REI', '9': 'Acknowledge', '10': 'Sleep_Request'
};

export function ploam_name(down, id) {
	return (down ? ploam_down : ploam_up)[id] ?? sprintf('unknown 0x%02x', id);
}

function port_id(d, at) {
	return (d[at] << 4) | (d[at + 1] >> 4);
}

/* A record's PLOAM: ONU-ID, message ID, 10 data bytes (and CRC). */
export function ploam(down, hexdata) {
	let b = bytes(hexdata);

	/* An all-zero record is a ring slot that was never filled. */
	if (length(b) < 12 || !length(filter(b, (x) => x)))
		return null;

	let onu = b[0], id = b[1], d = slice(b, 2, 12), info = '';

	if (down) {
		switch (id) {
		case 3: info = `onu-id ${d[0]} sn ${serial(d, 1)}`; break;
		case 4: info = `EqD ${be(d, 1, 4)} bits${d[0] & 1 ? ' (protection path)' : ''}`; break;
		case 6: info = `${d[0] == 0xff ? 'disable' : d[0] == 0x0f ? 'enable all' : 'enable'} sn ${serial(d, 1)}`; break;
		case 8: info = `port ${port_id(d, 1)} ${d[0] & 1 ? 'encrypted' : 'clear'}`; break;
		case 10: info = `alloc ${port_id(d, 0)} type ${d[2] == 255 ? 'deallocate' : d[2] == 1 ? 'GEM' : d[2]}`; break;
		case 14: info = `port ${port_id(d, 1)} ${d[0] & 1 ? 'activate' : 'deactivate'}`; break;
		case 18: info = `${be(d, 0, 4)} frames`; break;
		case 19: info = `superframe ${be(d, 0, 4)}`; break;
		case 21: info = `type ${d[0]} pon-id 0x${hexstr(d, 1, 7)}`; break;
		}
	} else {
		switch (id) {
		case 1: info = `sn ${serial(d, 0)}`; break;
		case 2: info = '<redacted>'; break;
		case 5: info = `key index ${d[0]} fragment ${d[1]}`; break;
		case 8: info = `${be(d, 0, 4)} BIP errors, seq ${d[4] >> 4}`; break;
		case 9: info = `acks ${ploam_name(true, d[0])}`; break;
		}
	}
	return { onu, id, name: ploam_name(down, id), info };
}
