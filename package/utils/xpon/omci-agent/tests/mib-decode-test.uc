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
check('171 rule', f.rules[0], 'filter outer [vid 660, pbit any, tpid 0] inner [vid any, pbit any, tpid 0] ethertype 0 -> remove 1, outer [no tag added] inner [vid 660, pbit copy outer, tpid 3]');

f = decode({ class_id: 84, data: data([ '0294', '00000000000000000000000000000000000000000000', '10', '01' ]) });
check('84 filter', f.filters[0], 'vid 660, pbit 0, dei 0');
check('84 entries', length(f.filters), 1);

f = decode({ class_id: 47, data: data([ '0001', '01', '0b', '0301', '00000000000000', '001122334455' ]) });
check('47 tp type', f.tp_type, '11 (VEIP)');
check('47 tp', f.tp, '0x0301');
check('47 mac', f.mac, '00:11:22:33:44:55');

f = decode({ class_id: 256, data: data([ '5a594f54', '56312e30000000000000000000', '00', '5a594f5412345678' ]) });
check('256 vendor', f.vendor_id, 'ZYOT');
check('256 version', f.version, 'V1.0');
check('256 serial', f.serial, 'ZYOT12345678');

f = decode({ class_id: 65305, data: '01aabbccddeeff' });
check('vendor ME', f.attr_2, 'aabbccddeeff');
check('unknown class', decode({ class_id: 9999, data: '00' }), null);

if (fails)
	exit(1);
print('MIB decode checks passed\n');
