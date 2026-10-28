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
	[ 0, _('TX power low') ], [ 1, _('TX power high') ],
	[ 2, _('Bias low') ], [ 3, _('Bias high') ],
	[ 4, _('RX power low') ], [ 5, _('RX power high') ],
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

	var mode = { transparent: _('Transparent'), tagged: _('Tagged'), olt: _('Control by OLT') }[vlan.mode] || vlan.mode;
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
		dom.content(node, vlanText(vlan));
}

function row(label, value, id) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td left', 'width': '42%' }, label),
		E('td', { 'class': 'td left', 'id': 'omci-status-' + id }, text(value))
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
			title: _('Receive'), value: widgets.dbm(status.bosa_rx_power_nw), limits: limits.rx,
			min: -35, max: -5, lo: -27, hi: -8, loName: _('sensitivity'), hiName: _('overload'),
			note: _('Class B+ receive window, -27 to -8 dBm'), empty: _('No light'), emptyBad: true
		})),
		widgets.panel(null, widgets.gauge({
			title: _('Transmit'), value: widgets.dbm(status.bosa_tx_power_nw), limits: limits.tx,
			min: -2, max: 8, lo: 0.5, hi: 5, loName: _('minimum'), hiName: _('maximum'),
			note: _('Class B+ launch power, 0.5 to 5 dBm'), empty: _('Laser off')
		}))
	]);
}

/* What the OLT provisioned, in sentences, from the MIB objects it created. */
function provisioned(objects, vlan) {
	var allocs = mib.allocIds(objects), gems = mib.gemPorts(objects);
	var vlans = mib.provisionedVlans(objects), lead = [];
	var vids = Object.keys(vlans).map(Number).sort(function(a, b) { return a - b; });

	if (!(objects || []).some(function(o) { return o.origin_name == 'olt'; }))
		return E('p', { 'class': 'xpon-lead' },
			_('The OLT has not provisioned anything yet. Its services appear here once the ONU is registered and the OLT has sent its configuration.'));

	lead.push(allocs.length ?
		_('The OLT grants upstream bandwidth on Alloc-ID %s and carries its services on GEM port %s.').format(allocs.join(', '), gems.join(', ') || '-') :
		_('The OLT has not bound a T-CONT yet, so nothing can be sent upstream.'));
	lead.push(vids.length ? _('Its services use VLAN %s.').format(vids.join(', ')) :
		_('It set no VLAN rule: the service runs untagged.'));
	lead.push(_('Towards the LAN: %s.').format(vlanText(vlan)));

	return E([], [
		E('p', { 'class': 'xpon-lead' }, lead.join(' ')),
		vids.length ? E('div', { 'class': 'xpon-cards' }, vids.map(function(vid) {
			var entry = vlans[vid], route = entry.route;

			return widgets.card([
				_('VLAN %d').format(vid),
				route ? mib.routeText(route) : _('No GEM port found for it'),
				mib.oltPriority(objects, vid) || mib.mapperPriority(route) || _('No priority rule'),
				_('From: %s').format(entry.sources.join(', '))
			]);
		})) : ''
	]);
}

function summary(status) {
	var cards = [
		[ _('OMCI agent'), status.agent_operational ? _('Operational') : _('Not operational') ],
		[ _('OLT profile'), text(status.olt_profile_effective_name) ],
		[ _('MIB objects'), text(status.mib_objects, '0') ],
		[ _('OMCI alarms'), omciAlarmCount(status.omci_alarms, status.channel_up) ]
	];

	return E('div', { 'class': 'omci-summary' }, cards.map(function(card) {
		return E('div', { 'class': 'omci-card' }, [
			E('div', { 'class': 'omci-card-label' }, card[0]),
			E('div', { 'class': 'omci-card-value' }, card[1])
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
	'low-rx-power': _('Low RX power'), 'high-rx-power': _('High RX power'),
	'sf': _('Signal fail'), 'sd': _('Signal degrade'),
	'low-tx-power': _('Low TX power'), 'high-tx-power': _('High TX power'),
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
		responder: text(status.responder),
		ifindex: text(status.ifindex),
		onu_id: text(status.onu_id),
		gem_port: status.gem_port == 0xffff ? _('Not assigned') : text(status.gem_port),
		channel_up: yesno(status.channel_up),
		agent_enabled: yesno(status.agent_enabled),
		agent_operational: yesno(status.agent_operational),
		permissive: yesno(status.permissive),
		fake_omci: yesno(status.fake_omci),
		reboot: status.reboot == null ? '-' : yesno(status.reboot),
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
		olt_vendor: text(olt.vendor_id || olt.vendor), olt_equipment: text(olt.equipment_id || olt.model),
		olt_version: text(olt.version),
		fec_downstream: fec(status.fec_downstream), fec_upstream: fec(status.fec_upstream),
		bosa_temperature: temperature(status.bosa_temperature_mc), bosa_voltage: scaled(status.bosa_voltage_uv, 1e6, 2, 'V'),
		bosa_bias: scaled(status.bosa_bias_ua, 1e3, 1, 'mA'),
		bosa_tx_power: power(status.bosa_tx_power_nw), bosa_rx_power: power(status.bosa_rx_power_nw),
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
			dom.content(node, v[key]);
	});

	var summaryNode = document.getElementById('omci-summary-root');
	if (summaryNode)
		dom.content(summaryNode, summary(status));

	var lineNode = document.getElementById('omci-line-root');
	if (lineNode)
		dom.content(lineNode, line(status));
}

/* The MIB list is large: read it after rendering, and again only when the MIB changes. */
function loadProvisioned(vlan) {
	var node = document.getElementById('omci-provisioned-root');

	if (!node)
		return Promise.resolve();
	return L.resolveDefault(omci.mibList(0), []).then(function(objects) {
		dom.content(node, provisioned(objects, vlan));
	});
}

return view.extend({
	load: function() {
		return Promise.all([
			omci.status(0),
			L.resolveDefault(omci.vlanStatus(), null),
			widgets.limits()
		]);
	},

	render: function(data) {
		injectStyle();
		var status = data[0] || {};
		var v;

		limits = data[2] || {};
		v = values(status);
		var mibSync = status.mib_sync, vlan = data[1];

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
			section(_('Channel and agent'), [
				row(_('PON state'), v.state, 'state'), row(_('Interface index'), v.ifindex, 'ifindex'),
				row(_('Responder'), v.responder, 'responder'),
				row(_('ONU ID'), v.onu_id, 'onu_id'), row(_('Default GEM port'), v.gem_port, 'gem_port'),
				row(_('OMCI channel up'), v.channel_up, 'channel_up'), row(_('Agent enabled'), v.agent_enabled, 'agent_enabled'),
				row(_('Agent operational'), v.agent_operational, 'agent_operational'), row(_('Permissive mode'), v.permissive, 'permissive'),
				row(_('Fake OMCI'), v.fake_omci, 'fake_omci'),
				row(_('Allow OLT-requested reboot'), v.reboot, 'reboot'),
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
				row(_('Configured profile'), v.profile_configured, 'profile_configured'),
				row(_('Effective profile'), v.profile_effective, 'profile_effective'),
				row(_('Forced profile'), v.profile_forced, 'profile_forced'),
				row(_('Active quirks'), v.quirks, 'quirks'),
				row(_('OLT vendor ID'), v.olt_vendor, 'olt_vendor'), row(_('OLT equipment ID'), v.olt_equipment, 'olt_equipment'),
				row(_('OLT version'), v.olt_version, 'olt_version')
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
				row(_('BOSA bias current'), v.bosa_bias, 'bosa_bias'), row(_('TX optical power'), v.bosa_tx_power, 'bosa_tx_power'),
				row(_('RX optical power'), v.bosa_rx_power, 'bosa_rx_power'), row(_('BOSA alarms'), v.bosa_alarms, 'bosa_alarms')
			]),
			section(_('OMCI alarms'), omciAlarmRows.map(function(alarm, i) {
				return row(omciAlarmLabel(alarm), v['omci_alarm_' + i], 'omci_alarm_' + i);
			})),
			widgets.report()
		]));
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
