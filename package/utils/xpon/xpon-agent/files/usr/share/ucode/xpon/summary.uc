// SPDX-License-Identifier: GPL-2.0-only
/*
 * The xpon summary method: the PON mode, the optics with the module's own
 * power thresholds and, in EPON mode, the MAC and OAM state. Open to the
 * unauthenticated session for the router behind the ONU (luci-app-xpon-stats)
 * and the LuCI gauges, so nothing here names a LOID, password or key.
 */

import { readfile } from 'fs';
import { xpon_get } from 'omci';

/* Fields of /var/run/epon-oamd.json that are status, not credentials. */
const oam_fields = [
	'generation', 'registered', 'oam', 'ctc', 'ctc_version', 'auth',
	'auth_fail_reason', 'olt_mac', 'onu_mac', 'vlan_mode', 'vlan_default',
	'vlan_override', 'silent', 'tx_off', 'rx_pdus', 'tx_pdus',
	'unknown_leaves', 'interauth'
];

function read(path) {
	return trim(readfile(path) ?? '');
}

/* LLID 0's MAC-level state, under the names LuCI reads. */
function epon(x) {
	let oam = {};
	let out = {
		mac: {
			signal: x.signal_detect ? 1 : 0,
			sync: x.epon_sync ? 1 : 0,
			registered: x.epon_registered ?? 0,
			nack_silent: x.epon_nack_silent ? 1 : 0,
			holdover_ms: x.epon_holdover_ms ?? 0,
			mac: x.epon_mac
		},
		oam: {}
	};

	/* epon-oamd renames a finished file into place; absent until it runs. */
	try { oam = json(readfile('/var/run/epon-oamd.json') ?? '{}') ?? {}; } catch (e) {}
	for (let field in oam_fields)
		if (exists(oam, field))
			out.oam[field] = oam[field];
	out.allow_tx = x.tx_allow ?? false;
	out.tx_hold = x.tx_hold ?? false;
	return out;
}

export function summary() {
	let mode = read('/sys/class/xpon/pon0/mode_current') || null;
	let x = xpon_get('pon0') ?? {};
	let out = {
		mode,
		ddm: x.ddm ?? {},
		/* The module's alarm and warning levels, in nW. */
		thresholds: x.thresholds ?? {}
	};

	if (mode == 'epon')
		out.epon = epon(x);
	return out;
};
