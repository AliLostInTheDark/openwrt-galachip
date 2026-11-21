'use strict';
'require omci.layout as layout';
'require view';
'require poll';
'require dom';
'require omci.rpc as omci';
'require omci.mib as mib';
'require xpon.widgets as widgets';

function text(value, fallback) {
	return value == null || value === '' ? (fallback || '-') : String(value);
}

function yesno(value) {
	return value ? _('Yes') : _('No');
}

function hex(value, width) {
	if (value == null)
		return '-';
	return '0x' + Number(value).toString(16).padStart(width || 2, '0');
}

/* enum omci_fec_status; absent when the PON driver does not report it. */
function fec(value) {
	switch (value) {
	case 1: return _('Off');
	case 2: return _('On');
	case null: case undefined: return _('Not reported');
	default: return _('Unknown');
	}
}

/* OPTICAL_FRONTEND_ALARM_* bits */
var opticalAlarms = [
	[ 0, _('1310nm (TX) power low') ], [ 1, _('1310nm (TX) power high') ],
	[ 2, _('Bias low') ], [ 3, _('Bias high') ],
	[ 4, _('1490nm (RX) power low') ], [ 5, _('1490nm (RX) power high') ],
	[ 6, _('Voltage low') ], [ 7, _('Voltage high') ],
	[ 8, _('Temperature low') ], [ 9, _('Temperature high') ]
];

function alarms(value) {
	if (value == null)
		return '-';
	var set = opticalAlarms.filter(function(a) { return value & (1 << a[0]); });
	return set.length ? set.map(function(a) { return a[1]; }).join(', ') : _('None');
}

function scaled(value, div, digits, unit) {
	return value == null ? '-' : '%s %s'.format((Number(value) / div).toFixed(digits), unit);
}

function state(value) {
	return value == null ? '-' : 'O' + value;
}

function power(value) {
	var n = Number(value);
	if (!Number.isFinite(n) || n <= 0)
		return '-';
	return '%s dBm / %s mW'.format((10 * Math.log10(n / 1e6)).toFixed(2), (n / 1e6).toFixed(4));
}

function temperature(value) {
	if (value == null || value === '')
		return '-';
	var n = Number(value);
	return Number.isFinite(n) ? '%s °C / %s °F'.format((n / 1000).toFixed(1), (n * 9 / 5000 + 32).toFixed(1)) : '-';
}

function vlanText(vlan) {
	if (!vlan || !vlan.mode)
		return _('The omci-vlan service is not running.');

	if (vlan.mode == 'bridge')
		return _('Set per port in the VLAN table (%s)').format(vlan.port || '-');

	var mode = { routed: _('Routed'), transparent: _('Transparent'), tagged: _('Tagged'), olt: _('Control by OLT') }[vlan.mode] || vlan.mode;
	var detail;

	if (vlan.error)
		detail = _('error: %s').format(vlan.error);
	else if (vlan.source == 'pending')
		detail = _('waiting for the OLT VLAN rule');
	else if (vlan.vid)
		detail = _('VLAN %d, priority %d').format(vlan.vid, vlan.prio || 0);
	else
		detail = _('untagged');

	return '%s: %s (%s)'.format(mode, detail, vlan.port || '-');
}

function updateVlan(vlan) {
	var node = document.getElementById('omci-status-lan_vlan');
	if (node)
		dom.content(node, [ vlanText(vlan) ]);
}

/* Identity and OLT text can come from the OLT: a string given to E() or
 * dom.content() alone would be parsed as HTML, one in an array stays text. */
function row(label, value, id) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td left', 'width': '42%' }, label),
		E('td', { 'class': 'td left', 'id': 'omci-status-' + id }, [ text(value) ])
	]);
}

function section(title, rows) {
	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, title),
		E('table', { 'class': 'table' }, rows)
	]);
}

/* G.984.3 activation states. */
var activation = [
	[ 'O1', _('Initial') ], [ 'O2', _('Standby') ], [ 'O3', _('Serial number') ],
	[ 'O4', _('Ranging') ], [ 'O5', _('Operation') ]
];

function stateText(status) {
	var id = text(status.onu_id);

	switch (status.state) {
	case 1: return _('Waiting for light and downstream frame sync from the OLT.');
	case 2: return _('In sync with the OLT; waiting for its upstream overhead message.');
	case 3: return _('Answering the OLT\'s serial number requests. It has not assigned an ONU ID yet: check that the OLT has this serial number authorised.');
	case 4: return _('The OLT assigned ONU ID %s and is measuring the fibre distance.').format(id);
	case 5: return status.agent_operational ?
		_('Registered as ONU %s and provisioned: the line carries traffic.').format(id) :
		_('Registered as ONU %s; the OLT has not finished OMCI provisioning.').format(id);
	case 6: return _('The downstream signal was lost; waiting for it to return before starting over.');
	case 7: return _('The OLT switched this ONU off (Disable_serial_number). It stays silent until the OLT enables it again.');
	default: return _('The PON port is not running in GPON mode.');
	}
}

/* The optical module's warning and alarm levels, read once per page. */
var limits = {};

function line(status) {
	var items = activation.slice(), current = status.state >= 1 && status.state <= 5 ? status.state - 1 : -1;

	if (status.state == 6 || status.state == 7) {
		items.push([ 'O' + status.state, status.state == 6 ? _('Signal lost') : _('Emergency stop') ]);
		current = items.length - 1;
	}

	return E('div', { 'class': 'xpon-grid' }, [
		widgets.panel(_('Activation'), widgets.stages(items, current, status.state >= 6, stateText(status))),
		widgets.panel(null, widgets.gauge({
			title: _('1490nm (RX)'), value: widgets.dbm(status.bosa_rx_power_nw), limits: limits.rx,
			min: -35, max: -5, lo: -27, hi: -8, loName: _('sensitivity'), hiName: _('overload'),
			note: _('Class B+ receive window, -27 to -8 dBm'), empty: _('No light'), emptyBad: true
		})),
		widgets.panel(null, widgets.gauge({
			title: _('1310nm (TX)'), value: widgets.dbm(status.bosa_tx_power_nw), limits: limits.tx,
			min: -2, max: 8, lo: 0.5, hi: 5, loName: _('minimum'), hiName: _('maximum'),
			note: _('Class B+ launch power, 0.5 to 5 dBm'), empty: _('Laser off')
		}))
	]);
}

/* What the OLT provisioned, in sentences, from the MIB objects it created or configured. */
function provisioned(objects, vlan) {
	var allocs = mib.allocIds(objects), gems = mib.gemPorts(objects);
	var vlans = mib.provisionedVlans(objects), paths = mib.dataPaths(objects), lead = [];
	var vids = Object.keys(vlans).map(Number).sort(function(a, b) { return a - b; });
	var pathTable = null;

	if (!(objects || []).some(mib.oltProvisioned))
		return E('p', { 'class': 'xpon-lead' },
			_('The OLT has not provisioned anything yet. Its services appear here once the ONU is registered and the OLT has sent its configuration.'));

	lead.push(allocs.length ?
		_('The OLT grants upstream bandwidth on Alloc-ID %s and carries its services on GEM port %s.').format(allocs.join(', '), gems.join(', ') || '-') :
		_('The OLT has not bound a T-CONT yet, so nothing can be sent upstream.'));
	lead.push(vids.length ? _('Its services use VLAN %s.').format(vids.join(', ')) :
		_('It set no VLAN rule: the service runs untagged.'));
	lead.push(_('Towards the LAN: %s.').format(vlanText(vlan)));

	if (paths.length) {
		var titles = [
			_('GEM Port'),
			_('Direction'),
			_('Alloc-ID (T-CONT)'),
			_('802.1p Mapper / Priorities'),
			_('VLAN Filter (ME 84)'),
			_('UNI / Extended VLAN (ME 171)')
		];
		pathTable = E('table', { 'class': 'table', 'style': 'margin-top:12px' }, [
			E('tr', { 'class': 'tr table-titles' }, titles.map(function(t) {
				return E('th', { 'class': 'th left' }, t);
			}))
		].concat(paths.map(function(p) {
			var pbits = p.prios.length == 8 ? '0-7' : (p.prios.length ? p.prios.join(', ') : '-');
			var mapperTxt = p.mappers.length ? '%s (p %s)'.format(p.mappers.join(', '), pbits) : '-';

			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'data-title': titles[0] }, String(p.gem)),
				E('td', { 'class': 'td left', 'data-title': titles[1] }, p.direction),
				E('td', { 'class': 'td left', 'data-title': titles[2] }, p.alloc != null ? String(p.alloc) : '-'),
				E('td', { 'class': 'td left', 'data-title': titles[3] }, mapperTxt),
				E('td', { 'class': 'td left', 'data-title': titles[4] }, p.filterVids.length ? p.filterVids.join(', ') : '-'),
				E('td', { 'class': 'td left', 'data-title': titles[5] }, p.uniRules.length ? p.uniRules.join('; ') : '-')
			]);
		})));
	}

	return E([], [
		E('p', { 'class': 'xpon-lead' }, [ lead.join(' ') ]),
		vids.length ? E('div', { 'class': 'xpon-cards' }, vids.map(function(vid) {
			var entry = vlans[vid], route = entry.route;

			return widgets.card([
				_('VLAN %d').format(vid),
				route ? mib.routeText(route) : _('No GEM port found for it'),
				mib.oltPriority(objects, vid) || mib.mapperPriority(route) || _('No priority rule'),
				_('From: %s').format(entry.sources.join(', '))
			]);
		})) : '',
		pathTable || ''
	]);
}

function oltVendorText(olt) {
	var id = olt && (olt.vendor_id || olt.vendor);
	if (olt && olt.vendor_name && id)
		return '%s (%s)'.format(olt.vendor_name, id);
	return text(id);
}

function summary(status) {
	var cards = [
		[ _('OMCI agent'), status.agent_operational ? _('Operational') : _('Not operational') ],
		[ _('OLT vendor'), oltVendorText(status.olt) ],
		[ _('MIB objects'), text(status.mib_objects, '0') ],
		[ _('OMCI alarms'), omciAlarmCount(status.omci_alarms, status.channel_up) ]
	];

	return E('div', { 'class': 'omci-summary' }, cards.map(function(card) {
		return E('div', { 'class': 'omci-card' }, [
			E('div', { 'class': 'omci-card-label' }, card[0]),
			E('div', { 'class': 'omci-card-value' }, [ card[1] ])
		]);
	}));
}

function injectStyle() {
	widgets.style();
	if (document.getElementById('omci-style'))
		return;

	document.head.appendChild(E('style', { 'id': 'omci-style' }, [
		'.omci-summary{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px;margin:0 0 18px}',
		'.omci-card{border:1px solid var(--border-color-medium,#ccc);border-radius:6px;padding:12px 14px;background:var(--background-color-high,#fff)}',
		'.omci-card-label{font-size:12px;opacity:.72;margin-bottom:5px}',
		'.omci-card-value{font-size:18px;font-weight:600;overflow-wrap:anywhere}',
		'.omci-quirks{font-family:monospace;font-size:12px;overflow-wrap:anywhere}'
	].join('')));
}

/* Alarm names from omcid's status */
var omciAlarmNames = {
	'low-rx-power': _('Low 1490nm (RX) power'), 'high-rx-power': _('High 1490nm (RX) power'),
	'sf': _('Signal fail'), 'sd': _('Signal degrade'),
	'low-tx-power': _('Low 1310nm (TX) power'), 'high-tx-power': _('High 1310nm (TX) power'),
	'laser-bias': _('Laser bias'), 'lan-los': _('LAN LOS')
};

/* Every alarm the agent can raise: ANI-G (class 263) and PPTP Ethernet UNI 1 (class 11) */
var omciAlarmRows = [
	[ 263, 'low-rx-power' ], [ 263, 'high-rx-power' ], [ 263, 'sf' ], [ 263, 'sd' ],
	[ 263, 'low-tx-power' ], [ 263, 'high-tx-power' ], [ 263, 'laser-bias' ],
	[ 11, 'lan-los' ]
];

function omciAlarmLabel(row) {
	return '%s: %s'.format(row[0] == 263 ? 'ANI-G' : 'UNI 1', omciAlarmNames[row[1]]);
}

function omciAlarmState(list, row, channelUp) {
	var entry;

	if (channelUp != null && !channelUp)
		return _('Unavailable (channel down)');
	if (!Array.isArray(list))
		return '-';
	entry = list.filter(function(a) {
		return a['class'] == row[0] && (row[0] == 263 || (parseInt(a.entity, 16) & 0xff) == 1) &&
		       (a.alarms || []).indexOf(row[1]) >= 0;
	})[0];
	if (!entry)
		return _('Clear');

	return entry.reported ? _('Active, reported') : _('Active, held by ARC');
}

function omciAlarmCount(list, channelUp) {
	var count = 0;

	if (channelUp != null && !channelUp)
		return _('Unavailable');
	if (!Array.isArray(list))
		return '-';
	list.forEach(function(a) { count += (a.alarms || []).length; });

	return count ? _('%d active').format(count) : _('None');
}

function values(status) {
	var identity = status.identity || {};
	var olt = status.olt || {};
	var v = {
		state: state(status.state),
		ifindex: text(status.ifindex),
		onu_id: text(status.onu_id),
		gem_port: status.gem_port == 0xffff ? _('Not assigned') : text(status.gem_port),
		channel_up: yesno(status.channel_up),
		agent_enabled: yesno(status.agent_enabled),
		agent_operational: yesno(status.agent_operational),
		permissive: yesno(status.permissive),
		fake_omci: yesno(status.fake_omci),
		reboot: status.reboot == null ? '-' : yesno(status.reboot),
		responder: text(status.responder),
		onu_type: text(status.onu_type_name),
		uni_count: text(status.uni_count),
		traffic_management_option: text(status.traffic_management_option),
		uapi: '%s / client %s (%s)'.format(text(status.uapi_version), text(status.client_uapi_version), status.uapi_compatible ? _('compatible') : _('mismatch')),
		capabilities: hex(status.capabilities, 8),
		profile_configured: text(status.olt_profile_configured_name),
		profile_effective: text(status.olt_profile_effective_name),
		profile_forced: text(status.olt_profile_forced_name),
		quirks: (status.olt_profile_quirk_names || []).join(', ') || _('None'),
		mib_sync: text(status.mib_sync),
		mib_objects: text(status.mib_objects),
		rx_packets: text(status.rx_packets), rx_bytes: text(status.rx_bytes), rx_dropped: text(status.rx_dropped),
		tx_packets: text(status.tx_packets), tx_bytes: text(status.tx_bytes), tx_errors: text(status.tx_errors),
		responses: text(status.responses), duplicates: text(status.duplicates), unsupported: text(status.unsupported),
		fake_responses: text(status.fake_responses),
		serial: text(identity.serial), vendor_id: text(identity['vendor-id']),
		hardware_version: text(identity['hardware-version']), software_version_0: text(identity['software-version-0']),
		software_version_1: text(identity['software-version-1']), equipment_id: text(identity['equipment-id']),
		loid_status: text(status.loid_status_name),
		omcc_version: identity['omcc-version'] == null ? '-' : '%s (%s)'.format(hex(identity['omcc-version'], 2), identity['omcc-version']),
		olt_vendor: oltVendorText(olt), olt_equipment: text(olt.equipment_id || olt.model),
		olt_version: text(olt.version),
		fec_downstream: fec(status.fec_downstream), fec_upstream: fec(status.fec_upstream),
		bosa_temperature: temperature(status.bosa_temperature_mc),
		bosa_voltage: scaled(status.bosa_voltage_uv, 1e6, 2, 'V'),
		bosa_bias: scaled(status.bosa_bias_ua, 1e3, 1, 'mA'),
		bosa_tx_power: power(status.bosa_tx_power_nw),
		bosa_rx_power: power(status.bosa_rx_power_nw),
		bosa_alarms: alarms(status.bosa_alarms)
	};

	omciAlarmRows.forEach(function(row, i) {
		v['omci_alarm_' + i] = omciAlarmState(status.omci_alarms, row, status.channel_up);
	});

	return v;
}

function update(status) {
	var v = values(status);
	Object.keys(v).forEach(function(key) {
		var node = document.getElementById('omci-status-' + key);
		if (node)
			dom.content(node, [ v[key] ]);
	});

	var summaryNode = document.getElementById('omci-summary-root');
	if (summaryNode)
		dom.content(summaryNode, summary(status));

	var lineNode = document.getElementById('omci-line-root');
	if (lineNode)
		dom.content(lineNode, line(status));
}

/* The MIB list is large: read it after rendering, and again only when the MIB changes. */
var cachedObjects = [], cachedEvents = {}, showSecrets = false;

function secretText(v) {
	return v ? (showSecrets ? v : '\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022') : '-';
}

function secretsToggle() {
	return E('label', { 'style': 'display:inline-flex;align-items:center;gap:6px;margin:6px 0' }, [
		E('input', {
			'type': 'checkbox',
			'checked': showSecrets ? 'checked' : null,
			'change': function(ev) {
				var node = document.getElementById('omci-olt-details-root');

				showSecrets = ev.target.checked;
				if (node)
					dom.content(node, renderOltDetails(cachedObjects));
			}
		}),
		_('Show passwords the OLT configured')
	]);
}

function renderEvents(data) {
	var events = (data && data.events) || [];
	var rows = [];

	if (!events.length) {
		rows.push(E('tr', { 'class': 'tr placeholder' }, [
			E('td', { 'class': 'td left', 'colspan': '3' }, E('em', {}, _('No OMCI or PLOAM events recorded yet.')))
		]));
	} else {
		rows = events.slice(0, 30).map(function(ev) {
			var ts = ev.time ? new Date(ev.time * 1000).toLocaleTimeString() : '-';
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'style': 'white-space:nowrap' }, [ '%s (%s)'.format(ts, ev.uptime || '-') ]),
				E('td', { 'class': 'td left', 'style': 'white-space:nowrap' }, [ String(ev.kind || '-') ]),
				E('td', { 'class': 'td left' }, [ String(ev.message || '-') ])
			]);
		});
	}

	return E('table', { 'class': 'table' }, [
		E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th left', 'width': '20%' }, _('Time (Uptime)')),
			E('th', { 'class': 'th left', 'width': '12%' }, _('Type')),
			E('th', { 'class': 'th left' }, _('Event'))
		])
	].concat(rows));
}

function fmtEnum(val) {
	if (val == null || val === '')
		return '-';
	return String(val).replace(/^(\d+)\s*\((.+)\)$/, '$1, $2');
}

function simpleTable(titles, rows) {
	return E('table', { 'class': 'table' }, [
		E('tr', { 'class': 'tr table-titles' }, titles.map(function(t) {
			return E('th', { 'class': 'th left' }, t);
		}))
	].concat(rows.map(function(cols) {
		return E('tr', { 'class': 'tr' }, cols.map(function(c, i) {
			return E('td', { 'class': 'td left', 'data-title': titles[i] }, [ text(c) ]);
		}));
	})));
}

function renderOltDetails(objects) {
	var sections = [];
	var qos = mib.linkQos(objects);
	var ports = mib.uniPorts(objects);
	var svcs = mib.oltServices(objects);
	var imgs = mib.softwareImages(objects);

	if (qos.anig || qos.max_gem_payload != null || qos.tconts.length) {
		var qosRows = [];
		if (qos.anig) {
			qosRows.push([ _('SR-DBA indication'), yesno(qos.anig.sr_indication) ]);
			if (qos.anig.sf_threshold != null || qos.anig.sd_threshold != null)
				qosRows.push([ _('Signal fail / degrade BER'), '1e-%s / 1e-%s'.format(
					text(qos.anig.sf_threshold), text(qos.anig.sd_threshold)) ]);
			if (qos.anig.gem_block_length != null)
				qosRows.push([ _('GEM block length'), '%d B'.format(qos.anig.gem_block_length) ]);
			if (qos.anig.response_time != null)
				qosRows.push([ _('ONU response time'), '%d ns'.format(qos.anig.response_time) ]);
			if (qos.anig.tconts != null)
				qosRows.push([ _('Total T-CONTs supported'), String(qos.anig.tconts) ]);
		}
		if (qos.max_gem_payload != null)
			qosRows.push([ _('Max GEM payload (GAL ME 272)'), '%d B'.format(qos.max_gem_payload) ]);
		qos.tconts.forEach(function(t) {
			qosRows.push([ _('Bound T-CONT %s').format(t.entity),
				_('Alloc-ID %d · Policy %s').format(t.alloc_id, fmtEnum(t.policy)) ]);
		});
		sections.push(section(_('OLT-to-ONT link & QoS parameters'), qosRows.map(function(r, i) {
			return row(r[0], r[1], 'qos_' + i);
		})));
	}

	if (ports.length) {
		sections.push(E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Physical UNI & VEIP ports (OLT view)')),
			simpleTable(
				[ _('Port'), _('Entity'), _('Type'), _('Admin state'), _('Oper state'), _('OLT written'), _('Details') ],
				ports.map(function(p) {
					return [ p.kind, p.entity, fmtEnum(p.type), fmtEnum(p.admin), fmtEnum(p.oper), yesno(p.olt_written), p.details ];
				})
			)
		]));
	}

	if (svcs.multicast.length || svcs.multicast_gems.length || svcs.ip_hosts.length || svcs.tr069.length) {
		var svcRows = [];
		if (svcs.multicast_gems.length)
			svcRows.push([ _('Multicast GEM port (ME 281)'), svcs.multicast_gems.join(', ') ]);
		svcs.multicast.forEach(function(m) {
			svcRows.push([ _('Multicast profile %s (ME 309)').format(m.entity),
				_('%s · Immediate leave: %s · Upstream IGMP: %s').format(
					fmtEnum(m.igmp_version), yesno(m.immediate_leave), m.upstream_tci) ]);
		});
		svcs.ip_hosts.forEach(function(h) {
			svcRows.push([ _('IP host %s (ME 134)').format(h.entity),
				h.configured ?
					_('IP %s · GW %s · DNS %s').format(h.ip, h.gateway, h.dns) :
					_('Not configured by OLT') ]);
		});
		svcs.tr069.forEach(function(t) {
			svcRows.push([ _('TR-069 server %s (ME 340)').format(t.entity),
				t.acs ? _('ACS %s (%s)').format(t.acs_url || t.acs, fmtEnum(t.admin)) : _('Not configured (%s)').format(fmtEnum(t.admin)) ]);
			if (t.acs_user || t.acs_password)
				svcRows.push([ _('ACS login (ME 148)'),
					_('User %s · Password %s%s').format(t.acs_user || '-', secretText(t.acs_password),
						t.acs_realm ? _(' · Realm %s').format(t.acs_realm) : '') ]);
		});
		if (svcs.tr069.some(function(t) { return t.acs_user || t.acs_password; }))
			sections.push(secretsToggle());
		sections.push(section(_('OLT-provisioned multicast & remote management'), svcRows.map(function(r, i) {
			return row(r[0], r[1], 'oltsvc_' + i);
		})));
	}

	if (imgs.length) {
		sections.push(E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Dual-bank software images (OLT view)')),
			simpleTable(
				[ _('Bank'), _('Version'), _('Active'), _('Committed'), _('Valid') ],
				imgs.map(function(im) {
					return [ _('Image %d').format(im.bank), im.version, yesno(im.active), yesno(im.committed), yesno(im.valid) ];
				})
			)
		]));
	}

	return E([], sections);
}

/* The JSON snapshot: identifiers and the ACS login are masked unless asked for. */
function snapshot(status, vlan, events, withSecrets) {
	var copy = JSON.parse(JSON.stringify(status || {}));
	var services = JSON.parse(JSON.stringify(mib.oltServices(cachedObjects)));

	if (!withSecrets) {
		if (copy.serial) copy.serial = '[masked]';
		if (copy.identity) {
			if (copy.identity.serial) copy.identity.serial = '[masked]';
			if (copy.identity.password) copy.identity.password = '[masked]';
			if (copy.identity.loid) copy.identity.loid = '[masked]';
		}
		services.tr069.forEach(function(t) {
			if (t.acs_user) t.acs_user = '[masked]';
			if (t.acs_password) t.acs_password = '[masked]';
		});
	}
	return {
		generated_at: new Date().toISOString(),
		masked: !withSecrets,
		status: copy,
		vlan: vlan || null,
		data_paths: mib.dataPaths(cachedObjects),
		provisioned_vlans: mib.provisionedVlans(cachedObjects),
		link_qos: mib.linkQos(cachedObjects),
		uni_ports: mib.uniPorts(cachedObjects),
		olt_services: services,
		software_images: mib.softwareImages(cachedObjects),
		events: (events && events.events) || []
	};
}

function loadProvisioned(vlan) {
	var node = document.getElementById('omci-provisioned-root');
	var detailsNode = document.getElementById('omci-olt-details-root');

	if (!node)
		return Promise.resolve();
	return L.resolveDefault(omci.mibList(0), []).then(function(objects) {
		cachedObjects = objects || [];
		dom.content(node, provisioned(objects, vlan));
		if (detailsNode)
			dom.content(detailsNode, renderOltDetails(cachedObjects));
	});
}

function eventKey(s) {
	return [ s.state, s.onu_id, s.channel_up, s.agent_operational, s.mib_sync, s.bosa_alarms,
		JSON.stringify(s.omci_alarms), s.alarms_text ].join('|');
}

function loadEvents() {
	var node = document.getElementById('omci-events-root');

	if (!node)
		return Promise.resolve();
	return L.resolveDefault(omci.events(0), { events: [] }).then(function(data) {
		cachedEvents = data || { events: [] };
		dom.content(node, renderEvents(cachedEvents));
	});
}

return view.extend({
	load: function() {
		return Promise.all([
			omci.status(0),
			L.resolveDefault(omci.vlanStatus(), null),
			widgets.limits(),
			L.resolveDefault(omci.events(0), { events: [] })
		]);
	},

	render: function(data) {
		injectStyle();
		var status = data[0] || {};
		var v;

		limits = data[2] || {};
		cachedEvents = data[3] || { events: [] };
		v = values(status);
		var mibSync = status.mib_sync, vlan = data[1], seen = eventKey(status);

		poll.add(function() {
			/* A load during an apply finds no configuration, and OLT-G
			 * comes with provisioning: read it all until both are there. */
			return Promise.all([
				status.identity && status.olt ? omci.liveStatus(0) : omci.status(0),
				L.resolveDefault(omci.vlanStatus(), null)
			]).then(function(next) {
				if (next[0]) {
					Object.assign(status, next[0]);
					update(status);
				}
				vlan = next[1];
				updateVlan(vlan);
				/* The event log only changes with the status: read it then, not every poll. */
				if (eventKey(status) != seen) {
					seen = eventKey(status);
					loadEvents();
				}
				if (status.mib_sync != mibSync) {
					mibSync = status.mib_sync;
					return loadProvisioned(vlan);
				}
			});
		}, 10);
		window.setTimeout(function() { loadProvisioned(vlan); }, 0);

		return layout.apply(E([], [
			E('h2', {}, _('OMCI status')),
			E('p', {}, _('Configured identity and agent settings, with live readings refreshed every 10 seconds.')),
			E('div', { 'id': 'omci-line-root' }, line(status)),
			E('div', { 'id': 'omci-summary-root' }, summary(status)),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('What the OLT provisioned')),
				E('div', { 'id': 'omci-provisioned-root' }, E('em', {}, _('Reading the MIB…')))
			]),
			E('div', { 'id': 'omci-olt-details-root' }),
			section(_('Channel and agent'), [
				row(_('PON state'), v.state, 'state'), row(_('Interface index'), v.ifindex, 'ifindex'),
				row(_('ONU ID'), v.onu_id, 'onu_id'), row(_('Default GEM port'), v.gem_port, 'gem_port'),
				row(_('OMCI channel up'), v.channel_up, 'channel_up'), row(_('Agent enabled'), v.agent_enabled, 'agent_enabled'),
				row(_('Agent operational'), v.agent_operational, 'agent_operational'), row(_('Permissive mode'), v.permissive, 'permissive'),
				row(_('Fake OMCI'), v.fake_omci, 'fake_omci'),
				row(_('Allow OLT-requested reboot'), v.reboot, 'reboot'),
				row(_('Responder'), v.responder, 'responder'),
				row(_('UAPI'), v.uapi, 'uapi'), row(_('Capabilities'), v.capabilities, 'capabilities'),
				row(_('LAN VLAN mode'), vlanText(data[1]), 'lan_vlan')
			]),
			section(_('ONU identity'), [
				row(_('ONU type'), v.onu_type, 'onu_type'),
				row(_('UNI ports'), v.uni_count, 'uni_count'),
				row(_('Traffic management option'), v.traffic_management_option, 'traffic_management_option'),
				row(_('Serial number'), v.serial, 'serial'), row(_('Vendor ID'), v.vendor_id, 'vendor_id'),
				row(_('Hardware version'), v.hardware_version, 'hardware_version'),
				row(_('Software image 0 version'), v.software_version_0, 'software_version_0'),
				row(_('Software image 1 version'), v.software_version_1, 'software_version_1'),
				row(_('Equipment ID'), v.equipment_id, 'equipment_id'), row(_('OMCC version'), v.omcc_version, 'omcc_version'),
				row(_('LOID authentication'), v.loid_status, 'loid_status')
			]),
			section(_('OLT interoperability'), [
				row(_('OLT vendor'), v.olt_vendor, 'olt_vendor'), row(_('OLT equipment ID'), v.olt_equipment, 'olt_equipment'),
				row(_('OLT version'), v.olt_version, 'olt_version'),
				row(_('Configured quirk profile'), v.profile_configured, 'profile_configured'),
				row(_('Effective quirk profile'), v.profile_effective, 'profile_effective'),
				row(_('Forced quirk profile'), v.profile_forced, 'profile_forced'),
				row(_('Applied quirks'), v.quirks, 'quirks')
			]),
			section(_('MIB and OMCI traffic'), [
				row(_('MIB data sync'), v.mib_sync, 'mib_sync'), row(_('MIB objects'), v.mib_objects, 'mib_objects'),
				row(_('RX packets'), v.rx_packets, 'rx_packets'), row(_('RX bytes'), v.rx_bytes, 'rx_bytes'),
				row(_('RX dropped'), v.rx_dropped, 'rx_dropped'), row(_('TX packets'), v.tx_packets, 'tx_packets'),
				row(_('TX bytes'), v.tx_bytes, 'tx_bytes'), row(_('TX errors'), v.tx_errors, 'tx_errors'),
				row(_('Agent responses'), v.responses, 'responses'), row(_('Duplicates'), v.duplicates, 'duplicates'),
				row(_('Unsupported requests'), v.unsupported, 'unsupported'), row(_('Fake responses'), v.fake_responses, 'fake_responses')
			]),
			section(_('Optical telemetry'), [
				row(_('Downstream FEC'), v.fec_downstream, 'fec_downstream'), row(_('Upstream FEC'), v.fec_upstream, 'fec_upstream'),
				row(_('BOSA temperature'), v.bosa_temperature, 'bosa_temperature'), row(_('BOSA voltage'), v.bosa_voltage, 'bosa_voltage'),
				row(_('BOSA bias current'), v.bosa_bias, 'bosa_bias'), row(_('1310nm (TX) optical power'), v.bosa_tx_power, 'bosa_tx_power'),
				row(_('1490nm (RX) optical power'), v.bosa_rx_power, 'bosa_rx_power'), row(_('BOSA alarms'), v.bosa_alarms, 'bosa_alarms')
			]),
			section(_('OMCI alarms'), omciAlarmRows.map(function(alarm, i) {
				return row(omciAlarmLabel(alarm), v['omci_alarm_' + i], 'omci_alarm_' + i);
			})),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, _('Recent OMCI & PLOAM events')),
				E('div', { 'id': 'omci-events-root' }, renderEvents(cachedEvents))
			]),
			widgets.report({
				onSnapshot: function(withSecrets) {
					return snapshot(status, vlan, cachedEvents, withSecrets);
				}
			})
		]));
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
