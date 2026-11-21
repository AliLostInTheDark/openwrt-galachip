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
	/* Huawei */
	'350': [ 'Huawei ONU management', numbered([ 4, 2, 2, 2, 2, 2, 1, 4, 3, 1, 1, 1, 1, 4, 6, 2 ]) ],
	'351': [ 'Huawei extended configuration 351', numbered([ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 ]) ],
	'352': [ 'Huawei VLAN mapping', numbered([ 2, 2, 2, 1, 4, 4 ]) ],
	'353': [ 'Huawei VoIP image', [
		[ 'version', 14, '', 'str' ], [ 'committed', 1 ],
		[ 'active', 1 ], [ 'valid', 1 ], [ 'crc32', 4, '', 'hex' ]
	] ],
	'370': [ 'Huawei UNI QoS', [
		[ 'queue_weight_cfg', 2, '', 'hex' ], [ 'mode', 4, '', 'hex' ], [ 'flags', 4, '', 'hex' ]
	] ],
	'373': [ 'Huawei product identification', [
		[ 'part_count', 1 ],
		[ 'part_1', 25, '', 'str' ],
		[ 'part_2', 25, '', 'str' ],
		[ 'part_3', 25, '', 'str' ],
		[ 'part_4', 25, '', 'str' ],
		[ 'part_5', 25, '', 'str' ],
		[ 'part_6', 25, '', 'str' ],
		[ 'part_7', 25, '', 'str' ],
		[ 'part_8', 25, '', 'str' ],
		[ 'part_9', 25, '', 'str' ],
		[ 'part_10', 25, '', 'str' ],
		[ 'part_11', 25, '', 'str' ],
		[ 'part_12', 25, '', 'str' ],
		[ 'part_13', 25, '', 'str' ],
		[ 'part_14', 25, '', 'str' ],
		[ 'part_15', 25, '', 'str' ]
	] ],
	'65398': [ 'Huawei extended ONT-G2', [
		...numbered([ 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1 ]),
		[ 'vendor_info', 25, '', 'str' ], [ 'attr_15', 1 ], [ 'attr_16', 1 ]
	] ],
	'65408': [ 'CTC extended ONU-G', [ [ 'reset_default', 1 ] ] ],
	'65414': [ 'Huawei optical data', [
		[ 'attr_1', 1 ], [ 'attr_2', 1 ], [ 'attr_3', 1 ], [ 'attr_4', 1 ], [ 'attr_5', 1 ],
		[ 'transceiver_info', 16, '', 'str' ], [ 'attr_7', 2 ], [ 'attr_8', 2 ],
		[ 'attr_9', 4 ], [ 'attr_10', 4 ], [ 'attr_11', 2 ], [ 'attr_12', 2 ], [ 'attr_13', 8, '', 'hex' ]
	] ],
	'65415': [ 'Huawei extended WAN info', numbered([ 1, 1, 1 ]) ],
	'65417': [ 'Huawei extended PPP', [
		[ 'flags', 2, '', 'hex' ], [ 'username', 25, '', 'str' ],
		[ 'attr_4', 2 ], [ 'attr_5', 2 ], [ 'password', 25, '', 'secret' ],
		[ 'attr_7', 2 ], [ 'attr_8', 25, '', 'hex' ], [ 'attr_9', 1 ]
	] ],
	'65423': [ 'Huawei extended ONT3 ability', [
		[ 'attr_1', 1 ], [ 'attr_2', 1 ], [ 'attr_3', 1 ], [ 'attr_4', 1 ],
		[ 'attr_5', 4, '', 'hex' ], [ 'attr_6', 8, '', 'hex' ], [ 'attr_7', 1 ],
		[ 'capability_str', 25, '', 'str' ], [ 'attr_9', 6, '', 'hex' ],
		[ 'attr_10', 4, '', 'hex' ], [ 'attr_11', 1 ], [ 'attr_12', 1 ],
		[ 'attr_13', 1 ], [ 'attr_14', 1 ], [ 'attr_15', 1 ], [ 'attr_16', 2 ]
	] ],
	'65425': [ 'Huawei extended CPU', numbered([ 1, 4, 4, 4, 4, 2, 1 ]) ],
	'65426': [ 'Huawei maintenance', numbered([ 2, 2 ]) ],
	'65427': [ 'Huawei ME 65427', [ [ 'attr_1', 2 ], [ 'attr_2', 24, '', 'hex' ] ] ],
	'65430': [ 'Huawei extended type', numbered([ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 ]) ],
	'65431': [ 'Huawei extended OLT', [ [ 'flags', 4, '', 'hex' ] ] ],
	'65444': [ 'Huawei extended 802.11', [ [ 'config', 8, '', 'hex' ] ] ],
	'65447': [ 'Huawei extended MAC', [ [ 'mode', 1 ] ] ],
	'65450': [ 'Huawei extended 802.11 interface', numbered([ 1, 1, 1, 1, 1 ]) ],
	'65451': [ 'Huawei extended WAN config', numbered([ 1, 1, 2 ]) ],
	'65453': [ 'Huawei extended WAN data 3', [
		[ 'attr_1', 1, 'c' ], [ 'attr_2', 1, 'c' ], [ 'data', 12, 'c', 'hex' ]
	] ],
	'65454': [ 'Huawei extended 802.11 management', [
		[ 'attr_1', 2 ], [ 'attr_2', 2 ], [ 'attr_3', 2 ], [ 'attr_4', 4 ],
		[ 'attr_5', 8, '', 'hex' ], [ 'attr_6', 6, '', 'hex' ], [ 'attr_7', 6, '', 'hex' ]
	] ],

	/* ZTE */
	'242': [ 'ZTE PPTP schedule pointer', [
		[ 'policy', 1, 'c', policies ], [ 'sched_ptr', 2, 'c', 'hex' ]
	] ],
	'243': [ 'ZTE UNI schedule configuration', [
		[ 'num_queues', 1, 'c' ], [ 'queue_map', 8, 'c', 'hex' ], [ 'queue_weight', 8, 'c', 'hex' ]
	] ],
	'247': [ 'ZTE extended ONU-G', [
		[ 'attr_1', 2 ], [ 'attr_2', 2 ], [ 'version', 14, '', 'str' ],
		[ 'attr_4', 2 ], [ 'attr_5', 2 ], [ 'attr_6', 2 ], [ 'attr_7', 2 ],
		[ 'attr_8', 2 ], [ 'attr_9', 2 ], [ 'attr_10', 2 ], [ 'attr_11', 2 ],
		[ 'attr_12', 2 ], [ 'attr_13', 2 ], [ 'attr_14', 2 ], [ 'attr_15', 2 ],
		[ 'reset_default', 1 ]
	] ],
	'65294': [ 'ZTE SNTP configuration', [
		[ 'time_zone', 2 ], [ 'master_server', 4, '', 'ip' ],
		[ 'slave_server', 4, '', 'ip' ], [ 'interval', 4 ],
		[ 'attr_5', 1 ], [ 'attr_6', 1 ]
	] ],
	'65314': [ 'ZTE SSID control', [
		[ 'bssid_mac', 6, '', 'mac' ], [ 'ssid_name_ptr', 2, '', 'hex' ],
		[ 'ssid_hide', 1 ], [ 'user_isolation', 1 ], [ 'max_users', 2 ],
		[ 'current_users', 2 ], [ 'acl', 16, '', 'hex' ], [ 'assoc_devices', 16, '', 'hex' ]
	] ],
	'65315': [ 'ZTE SSID authentication and encryption', [
		[ 'auth_algo', 1 ], [ 'wep_encry_level', 1 ], [ 'wep_default_key_id', 1 ],
		[ 'key_value_ptr', 2, '', 'hex' ], [ 'wpa_rekey_interval', 2 ], [ 'wpa_encry_algo', 1 ]
	] ],

	/* FiberHome */
	'65322': [ 'FiberHome WAN configuration', [
		[ 'wan_index', 1, 'c' ], [ 'wan_name_1', 16, 'c', 'str' ],
		[ 'wan_name_2', 16, '', 'str' ], [ 'wan_name_3', 16, '', 'str' ],
		[ 'wan_name_4', 16, '', 'str' ], [ 'connection_mode', 1, 'c' ],
		[ 'connection_type', 1, 'c' ], [ 'vlan_id', 2, 'c' ], [ 'priority', 2, 'c' ],
		[ 'nat_enable', 1, 'c' ], [ 'address_mode', 1, 'c' ], [ 'qos_enable', 1 ],
		[ 'connection_status', 1 ], [ 'gem_tp_ptr', 2, 'c', 'hex' ],
		[ 'dhcp_remote_id', 10, '', 'str' ]
	] ],
	'65323': [ 'FiberHome PPPoE configuration', [
		[ 'pppoe_proxy_enable', 1, 'c' ], [ 'username_1', 16, 'c', 'str' ],
		[ 'username_2', 16, '', 'str' ], [ 'password_1', 16, 'c', 'secret' ],
		[ 'password_2', 16, '', 'secret' ], [ 'service_name_1', 16, '', 'str' ],
		[ 'service_name_2', 16, '', 'str' ], [ 'dial_pattern', 1 ]
	] ],
	'65324': [ 'FiberHome WAN port binding', [
		[ 'fe_bind_mask', 1, 'c', 'hex' ], [ 'ssid_bind_mask', 1, 'c', 'hex' ]
	] ],
	'65326': [ 'FiberHome WiFi station management 1', [
		[ 'ssid_enable', 1 ], [ 'wifi_standard', 1 ], [ 'ssid_hide', 1 ],
		[ 'auth_mode', 1 ], [ 'encry_type', 1 ], [ 'psk_1', 16, '', 'secret' ],
		[ 'psk_2', 16, '', 'secret' ], [ 'psk_3', 16, '', 'secret' ],
		[ 'psk_4', 16, '', 'secret' ], [ 'wpa_rekey_interval', 4 ],
		[ 'ssid_index', 2 ], [ 'ssid_1', 16, '', 'str' ],
		[ 'ssid_2', 16, '', 'str' ], [ 'wifi_switch', 1 ], [ 'ssid_count', 2 ]
	] ],
	'65327': [ 'FiberHome WiFi station management 2', [
		[ 'wep_encry_level', 1 ], [ 'wep_key1_1', 16, '', 'secret' ],
		[ 'wep_key1_2', 16, '', 'secret' ], [ 'wep_key2_1', 16, '', 'secret' ],
		[ 'wep_key2_2', 16, '', 'secret' ], [ 'wep_key3_1', 16, '', 'secret' ],
		[ 'wep_key3_2', 16, '', 'secret' ], [ 'wep_key4_1', 16, '', 'secret' ],
		[ 'wep_key4_2', 16, '', 'secret' ], [ 'wep_key_index', 1 ],
		[ 'channel', 1 ], [ 'power', 1 ], [ 'country', 1 ],
		[ 'frequency', 2 ], [ 'sta_num', 2 ], [ 'bandwidth', 2 ]
	] ],
	'65328': [ 'FiberHome WiFi station management 3', [
		[ 'radius_service_len', 2 ], [ 'rad_addr_1', 16, '', 'str' ],
		[ 'rad_addr_2', 16, '', 'str' ], [ 'rad_addr_3', 16, '', 'str' ],
		[ 'rad_addr_4', 16, '', 'str' ], [ 'rad_addr_5', 16, '', 'str' ],
		[ 'rad_addr_6', 16, '', 'str' ], [ 'rad_addr_7', 16, '', 'str' ],
		[ 'rad_addr_8', 16, '', 'str' ], [ 'radius_port', 2 ],
		[ 'radius_pwd_1', 16, '', 'secret' ], [ 'radius_pwd_2', 16, '', 'secret' ],
		[ 'wapi_as_ip', 4, '', 'ip' ], [ 'wapi_as_port', 2 ]
	] ],
	'65329': [ 'FiberHome WAN IP host configuration', [
		[ 'ip_address', 4, 'c', 'ip' ], [ 'netmask', 4, 'c', 'ip' ],
		[ 'gateway', 4, 'c', 'ip' ], [ 'primary_dns', 4, 'c', 'ip' ],
		[ 'secondary_dns', 4, 'c', 'ip' ], [ 'static_ipv6_addr', 19, '', 'str' ],
		[ 'ipv6_gateway', 19, '', 'str' ], [ 'ipv6_primary_dns', 19, '', 'str' ]
	] ],
	'65333': [ 'FiberHome PON switchport rate limit', [
		[ 'us_limit_speed', 4 ], [ 'ds_limit_speed', 4 ]
	] ],
	'65338': [ 'FiberHome WAN extended VLAN', [
		[ 'vlan_model', 1, 'c' ], [ 'translation_enable', 1, 'c' ],
		[ 'translation_vid', 2, 'c' ], [ 'cos', 2, 'c' ],
		[ 'qinq_enable', 1 ], [ 'tpid', 2, '', 'hex' ],
		[ 'svlan_id', 2 ], [ 'svlan_cos', 2 ]
	] ],
	'65363': [ 'FiberHome OMCI mode', [
		[ 'standard_omci_flag', 1 ], [ 'cfg_file_ver', 12, '', 'str' ],
		[ 'olt_no', 2 ], [ 'pon_no', 2 ], [ 'onu_no', 2 ]
	] ],
	'65411': [ 'FiberHome WiFi status', [
		[ 'conversation', 2 ], [ 'overlap', 1 ], [ 'wireless_enable', 1 ]
	] ],

	/* CTC / stock G7 */
	'250': [ 'CTC PPPoE WAN configuration', [
		[ 'nat_enable', 1, 'c' ], [ 'pppoe_mode', 1, 'c' ],
		[ 'conn_trigger', 1, 'c' ], [ 'release_timer', 2, 'c' ],
		[ 'username', 25, '', 'str' ], [ 'password', 25, '', 'secret' ],
		[ 'status', 1 ], [ 'online_duration', 4 ],
		[ 'proxy', 2, '', 'hex' ], [ 'username_2_ptr', 2, '', 'hex' ]
	] ],
	'65282': [ 'CTC extended VLAN translation', [
		[ 'tag_control_mode', 1, 'c' ], [ 'entries_num', 1, 'c' ],
		[ 'translation_vlan', 24, 'c', 'hex' ]
	] ],
	'65320': [ 'CTC optical transceiver info', [
		[ 'identifier', 1 ], [ 'vendor_name', 16, '', 'str' ],
		[ 'vendor_oui', 3, '', 'hex' ], [ 'vendor_pn', 16, '', 'str' ],
		[ 'vendor_rev', 4, '', 'str' ], [ 'vendor_sn', 16, '', 'str' ],
		[ 'date_code', 8, '', 'str' ], [ 'diagnostic_type', 1, '', 'hex' ]
	] ],
	'65321': [ 'CTC WAN service binding', [
		[ 'service', 1, 'c' ], [ 'ip_host_instance', 2, 'c', 'hex' ],
		[ 'lan_port_mask', 2, 'c', 'hex' ], [ 'attr_4', 2 ],
		[ 'mvlan', 2, 'c' ], [ 'mtu', 2, 'c' ]
	] ],
	'65380': [ 'CTC WAN connection configuration', [
		[ 'connect_mode', 1, 'c' ], [ 'connect_type', 1, 'c' ],
		[ 'vlan', 2, 'c' ], [ 'priority', 1, 'c' ],
		[ 'vlan_mode', 1, 'c' ], [ 'igmp_proxy', 1, 'c' ], [ 'mvlan', 2, 'c' ]
	] ],
	'65381': [ 'CTC WAN IP/PPPoE profile', [
		[ 'wan_dsp_mode', 1, 'c' ], [ 'pppoe_username', 24, 'c', 'str' ],
		[ 'pppoe_password', 24, 'c', 'secret' ], [ 'bind_lan_mask', 1, 'c', 'hex' ],
		[ 'bind_ssid_mask', 1, 'c', 'hex' ], [ 'bind_ssid5g_mask', 1, 'c', 'hex' ],
		[ 'static_ip', 4, 'c', 'ip' ], [ 'static_netmask', 4, 'c', 'ip' ],
		[ 'static_gateway', 4, 'c', 'ip' ], [ 'static_dns', 4, 'c', 'ip' ],
		[ 'static_dns2', 4, 'c', 'ip' ], [ 'bridge_mode', 1, 'c' ], [ 'enable', 1, 'c' ]
	] ],
	'65385': [ 'CTC WiFi interface configuration', [
		[ 'wifi_mode', 1, 'c' ], [ 'channel', 1, 'c' ],
		[ 'ssid_name', 24, 'c', 'str' ], [ 'bandwidth', 1, 'c' ],
		[ 'rate', 1, 'c' ], [ 'guard_interval', 2, 'c' ],
		[ 'beacon_interval', 2, 'c' ], [ 'dtim_interval', 2, 'c' ],
		[ 'ap_isolation', 1, 'c' ], [ 'broadcast_cancel', 1, 'c' ],
		[ 'ssid_enable', 1, 'c' ], [ 'wifi_enable', 1, 'c' ]
	] ],
	'65386': [ 'CTC WiFi security configuration', [
		[ 'security_mode', 1, 'c' ], [ 'share_key', 24, 'c', 'secret' ],
		[ 'wep_auth', 1, 'c' ], [ 'wep_key_bit', 1, 'c' ],
		[ 'key_index', 1, 'c' ], [ 'key_1', 26, 'c', 'secret' ],
		[ 'key_2', 26, 'c', 'secret' ], [ 'key_3', 26, 'c', 'secret' ],
		[ 'key_4', 26, 'c', 'secret' ], [ 'wpa_encrypt', 1, 'c' ]
	] ],
	'65523': [ 'CTC loop detection control', [
		[ 'enable', 1 ], [ 'loop_detection', 1 ]
	] ],
	'65527': [ 'CTC OLT location configuration data', [
		[ 'horizontal_error', 2 ], [ 'altitude_error', 2 ],
		[ 'area_code', 8, '', 'hex' ], [ 'timestamp', 4 ],
		[ 'gis_digest', 8, '', 'hex' ]
	] ],
	'65528': [ 'CTC loop detection', [
		[ 'operator_id', 4, '', 'str' ], [ 'loop_detect_mgmt', 2 ],
		[ 'loop_port_down', 2 ], [ 'detect_msg_freq', 2 ],
		[ 'recovery_interval', 2 ], [ 'port_vlan_table', 0, 't' ]
	] ],
	'65529': [ 'CTC ONU capability', [
		[ 'operator_id', 4, '', 'str' ], [ 'ctc_spec_version', 1 ],
		[ 'onu_type', 1 ], [ 'tx_power_supply_ctrl', 1 ],
		[ 'customized_onu_type', 1 ]
	] ],
	'65530': [ 'CTC LOID authentication', [
		[ 'operator_id', 4, '', 'str' ], [ 'loid', 24, '', 'secret' ],
		[ 'password', 12, '', 'secret' ], [ 'auth_status', 1 ]
	] ],
	'65531': [ 'CTC extended multicast operations profile', [
		[ 'igmp_version', 1, 'c' ], [ 'igmp_function', 1, 'c' ],
		[ 'immediate_leave', 1, 'c' ], [ 'upstream_igmp_tci', 2, 'c', 'hex' ],
		[ 'upstream_igmp_tag_ctrl', 1, 'c' ], [ 'upstream_igmp_rate', 4, 'c' ],
		[ 'ext_dynamic_acl', 0, 't' ], [ 'ext_static_acl', 0, 't' ],
		[ 'ext_lost_groups', 0, 't' ], [ 'robustness', 1, 'c' ],
		[ 'querier_ip', 16, '', 'hex' ], [ 'query_interval', 4, 'c' ],
		[ 'query_max_resp_time', 4, 'c' ], [ 'last_member_query_interval', 4 ],
		[ 'unauthorized_join_behaviour', 1 ], [ 'ds_igmp_mcast_tci', 3, 'c', 'hex' ]
	] ],

	/* Nokia/ALCL */
	'65295': [ 'Nokia optical supervision', numbered([ 1, 2, 1, 2, 1, 2, 1, 2, 1, 2 ]) ],
	'65296': [ 'Nokia ONT generic V2', numbered([ 1, 1, 4, 1, 1, 1, 4, 1, 1, 12, 4, 25 ]) ],
	'65297': [ 'Nokia UNI supplemental V2', numbered([ 1, 1, 2, 1, 1, 1, 1 ]) ],
	'65305': [ 'Nokia VLAN downstream supplemental', numbered([ 1, 6 ]) ],
	'65306': [ 'Nokia VLAN upstream policer', numbered([ 19 ]) ]
};

/*
 * Classes the OLT vendors share by number. The layout follows the OLT profile
 * (omci.agent: 3 nokia-alcl, 5 huawei, 6 fiberhome, 7 zte, 8 genexis); a
 * variant without profiles is the default. classes above holds the default.
 */
const ip_filter = [
	[ 'state', 1, 'c' ], [ 'mode', 1, 'c' ], [ 'protocol', 1, 'c' ],
	[ 'ingress_type', 2, 'c' ], [ 'ingress_type_value', 2, 'c' ],
	[ 'start_src_ip', 4, 'c', 'ip' ], [ 'end_src_ip', 4, 'c', 'ip' ]
];

export const variants = {
	'255': [
		{ profiles: [ 8 ], vendor: 'genexis', name: 'Genexis Ethernet PM history data 4', attrs: [
			[ 'interval_end_time', 1 ], [ 'threshold_id', 2, 'c', 'hex' ],
			[ 'associate_type', 1, 'c' ], [ 'tx_traffic_kbps', 4 ],
			[ 'rx_traffic_kbps', 4 ], [ 'tx_rate_pps', 4 ], [ 'rx_rate_pps', 4 ],
			[ 'tx_octets', 4 ], [ 'rx_octets', 4 ], [ 'tx_discards', 4 ],
			[ 'rx_discards', 4 ], [ 'tx_errors', 4 ], [ 'rx_errors', 4 ]
		] }
	],
	'65305': [
		{ profiles: [ 7 ], vendor: 'zte', name: 'ZTE IP flow filter', attrs: ip_filter },
		{ profiles: [ 8 ], vendor: 'genexis', name: 'Genexis IP flow filter', attrs: ip_filter },
		{ vendor: 'nokia', name: 'Nokia VLAN downstream supplemental', attrs: numbered([ 1, 6 ]) }
	],
	'65321': [
		{ profiles: [ 6 ], vendor: 'fiberhome', name: 'FiberHome WAN profile', attrs: [
			[ 'attr_1', 2 ], [ 'attr_2', 2 ]
		] },
		{ vendor: 'ctc', name: 'CTC WAN service binding', attrs: [
			[ 'service', 1, 'c' ], [ 'ip_host_instance', 2, 'c', 'hex' ],
			[ 'lan_port_mask', 2, 'c', 'hex' ], [ 'attr_4', 2 ],
			[ 'mvlan', 2, 'c' ], [ 'mtu', 2, 'c' ]
		] }
	]
};

/* the variant of a shared class for @profile, or null for a class without any */
export function variant_for(class_id, profile) {
	let list = variants[class_id];

	if (!list)
		return null;
	for (let v in list)
		if (v.profiles && profile in v.profiles)
			return v;
	for (let v in list)
		if (!v.profiles)
			return v;
	return null;
}

/* the vendor a shared class belongs to under @profile, or null */
export function vendor_of(class_id, profile) {
	return variant_for(class_id, profile)?.vendor;
}

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
