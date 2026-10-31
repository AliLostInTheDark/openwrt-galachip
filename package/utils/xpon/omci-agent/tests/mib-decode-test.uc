// Run: ucode -L "$PWD/files/usr/share/ucode/*.uc" tests/mib-decode-test.uc
import { decode } from 'omci.mib';

let fails = 0;

function check(what, got, want) {
	if (got !== want) {
		warn(`${what}: got ${got}, want ${want}\n`);
		fails++;
	}
}

function data(hexes) {
	let out = join('', hexes);

	while (length(out) < 128)
		out += '00';
	return out;
}

/* Airtel's class 171 on the slot 3 VEIP, as a VSOL omcicli dump showed it. */
let f = decode({
	class_id: 171,
	data: data([ '0a', '0010', '8100', '8100', '00', '00000000000000000000000000000000', '0301' ]),
	extended_vlan: { rules: [ {
		filter_outer_pbit: 8, filter_outer_vid: 660, filter_outer_tpid_dei: 0,
		filter_inner_pbit: 8, filter_inner_vid: 4096, filter_inner_tpid_dei: 0,
		filter_ethertype: 0, tags_to_remove: 1,
		treat_outer_pbit: 15, treat_outer_vid: 0, treat_outer_tpid_dei: 0,
		treat_inner_pbit: 9, treat_inner_vid: 660, treat_inner_tpid_dei: 3
	} ] }
});
check('171 association', f.association_type, '10 (VEIP)');
check('171 pointer', f.associated_me, '0x0301');
check('171 pointer note', f.notes.associated_me, 'VEIP 769');
check('171 tpid', f.input_tpid, '0x8100');
check('171 tpid note', f.notes.input_tpid, '802.1Q');
check('171 rule', f.rules[0], 'filter outer [VLAN 660, priority any] inner [VLAN any, priority any] ethertype any -> remove 1 tag, add outer [none] inner [VLAN 660, priority from outer, output TPID, DEI from outer]');
check('171 dscp', f.dscp_to_priority, 'all DSCP to priority 0');

f = decode({
	class_id: 171,
	data: data([ '0a' ]),
	extended_vlan: { max_table_size: 32, rules: [
		{ filter_outer_pbit: 15, filter_inner_pbit: 15, filter_ethertype: 1, tags_to_remove: 3,
		  treat_outer_pbit: 15, treat_inner_pbit: 15 },
		{ delete: 1 }
	] }
});
check('171 table size', f.max_table_size, 32);
check('171 drop', f.rules[0], 'filter outer [no tag] inner [no tag] ethertype IPv4 (0x0800) -> drop frame, add outer [none] inner [none]');
check('171 deleted rule', length(f.rules), 1);

f = decode({ class_id: 84, data: data([ '0294', '00000000000000000000000000000000000000000000', '10', '01' ]) });
check('84 filter', f.filters[0], 'VLAN 660, priority 0, DEI 0');
check('84 entries', length(f.filters), 1);

f = decode({ class_id: 47, data: data([ '0001', '01', '0b', '0301', '00000000000000', '001122334455' ]) });
check('47 tp type', f.tp_type, '11 (VEIP)');
check('47 tp', f.tp, '0x0301');
check('47 tp note', f.notes.tp, 'VEIP 769');
check('47 mac', f.mac, '00:11:22:33:44:55');

f = decode({ class_id: 256, data: data([ '5a594f54', '56312e30000000000000000000', '00', '5a594f5412345678' ]) });
check('256 vendor', f.vendor_id, 'ZYOT');
check('256 version', f.version, 'V1.0');
check('256 serial', f.serial, 'ZYOT12345678');

f = decode({ class_id: 65305, data: '01aabbccddeeff' });
check('vendor ME', f.attr_2, 'not text (6 bytes)');

f = decode({ class_id: 131, data: data([ '414c434c', '060204000002' ]) });
check('131 vendor', f.vendor_id, 'ALCL');
check('131 binary string', f.equipment_id, 'not text (6 bytes)');
check('131 empty', f.version, '');
check('131 zero raw', f.time_of_day, '');

f = decode({ class_id: 65296, data: data([ '000000000000000000000000000000', '5a594f545a594f5420095605' ]) });
check('vendor serial', f.attr_10, 'ZYOT ZYOT20095605');

f = decode({ class_id: 262, data: data([ '0100' ]) });
check('262 alloc', f.alloc_id, 256);

f = decode({ class_id: 310, data: data([ '00', '0601' ]) });
check('310 profile', f.multicast_operations_profile, '0x0601');
check('310 note', f.notes.multicast_operations_profile, 'multicast operations profile 1537');

f = decode({ class_id: 257, data: data([ '0000000000000000000000000000000000000000', 'a0' ]) });
check('257 omcc', f.omcc_version, '0xa0');
check('257 omcc note', f.notes.omcc_version, 'G.988 (2010), baseline');

f = decode({ class_id: 277, data: data([ '00', '0000', '0000', '00000000', '01010002' ]) });
check('277 port', f.related_port, '0x01010002');
check('277 port note', f.notes.related_port, 'port 257, priority 2');

f = decode({ class_id: 130, data: data([ 'ffff' ]) });
check('130 pointer', f.tp, '0xffff');
check('130 null pointer', f.notes.tp, 'none');
check('unknown class', decode({ class_id: 9999, data: '00' }), null);

if (fails)
	exit(1);
print('MIB decode checks passed\n');
