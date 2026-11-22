// SPDX-License-Identifier: GPL-2.0-only
/*
 * The xpon vlan_status method: the LAN VLAN mode omci-vlan last applied,
 * from its state file.
 */

import { readfile } from 'fs';

export const STATE_FILE = '/var/run/omci-vlan.json';

export function vlan_status() {
	let state = {};

	try { state = json(readfile(STATE_FILE) ?? '{}') ?? {}; } catch (e) {}
	return state;
};
