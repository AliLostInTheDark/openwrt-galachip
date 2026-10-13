'use strict';
'require view';
'require poll';
'require dom';
'require fs';
'require form';

var states = {
	1: _('O1 initial'), 2: _('O2 standby'), 3: _('O3 serial number'), 4: _('O4 ranging'),
	5: _('O5 operation'), 6: _('O6 popup'), 7: _('O7 emergency stop')
};

/* OPTICAL_FRONTEND_ALARM_* bits */
var alarmNames = [
	_('TX low power'), _('TX high power'), _('TX low bias'), _('TX high bias'),
	_('RX low power (LOS)'), _('RX high power'), _('Low voltage'), _('High voltage'),
	_('Low temperature'), _('High temperature')
];

function text(value) {
	return value == null || value === '' ? '-' : String(value);
}

function scaled(value, divisor, digits, unit) {
	var n = Number(value);
	return value != null && Number.isFinite(n) ? (n / divisor).toFixed(digits) + ' ' + unit : '-';
}

function dbm(nw) {
	var n = Number(nw);
	return Number.isFinite(n) && n > 0 ? (10 * Math.log10(n / 1000000)).toFixed(2) + ' dBm' : '-';
}

function alarms(bits) {
	var names = alarmNames.filter(function(name, i) { return (bits >> i) & 1; });
	return names.length ? names.join(', ') : _('None');
}

var omciAlarmNames = {
	'low-rx-power': _('Low RX power'), 'high-rx-power': _('High RX power'),
	'sf': _('Signal fail'), 'sd': _('Signal degrade'),
	'low-tx-power': _('Low TX power'), 'high-tx-power': _('High TX power'),
	'laser-bias': _('Laser bias'), 'lan-los': _('LAN LOS')
};

/* Alarms the ONU reports to the OLT */
function omciAlarms(list) {
	if (!Array.isArray(list))
		return '-';
	if (!list.length)
		return _('None');

	return list.map(function(a) {
		var me = a['class'] == 263 ? 'ANI-G' : 'UNI %d'.format(parseInt(a.entity, 16) & 0xff);
		var names = (a.alarms || []).map(function(n) { return omciAlarmNames[n] || n; });

		return '%s: %s%s'.format(me, names.join(', '), a.reported ? '' : ' (' + _('held by ARC') + ')');
	}).join('; ');
}

function vlanText(vlan) {
	if (!vlan || !vlan.mode)
		return '-';
	if (vlan.error)
		return '%s: %s (%s)'.format(vlan.mode, vlan.error, text(vlan.port));
	if (vlan.source == 'pending')
		return '%s: %s (%s)'.format(vlan.mode, _('waiting for the OLT VLAN rule'), text(vlan.port));
	if (vlan.vid)
		return '%s: VLAN %d, %s %d (%s)'.format(vlan.mode, vlan.vid, _('priority'), vlan.prio || 0, text(vlan.port));
	return '%s: %s (%s)'.format(vlan.mode, _('untagged'), text(vlan.port));
}

function table(rows) {
	return E('table', { 'class': 'table' }, rows.map(function(row) {
		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '33%' }, row[0]),
			E('td', { 'class': 'td left' }, row[1])
		]);
	}));
}

function statsTable(stats) {
	var metrics = [
		[ 'rx_power', _('RX power (1490 nm)'), 'dBm', 2 ],
		[ 'tx_power', _('TX power (1310 nm)'), 'dBm', 2 ],
		[ 'bias', _('Laser bias'), 'mA', 2 ],
		[ 'voltage', _('Supply voltage'), 'V', 3 ],
		[ 'temperature', _('BOSA temperature'), '°C', 1 ]
	];

	if (!stats || !stats.metrics)
		return E('p', {}, _('No statistics: the omci-optstat service is not running on the ONU.'));

	return E([], [
		E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				_('Metric'), _('Min'), _('Mean'), _('Median'), _('Max'), _('Samples')
			].map(function(t) { return E('th', { 'class': 'th left' }, t); }))
		].concat(metrics.map(function(m) {
			var s = stats.metrics[m[0]] || {};
			var fmt = function(v) { return v == null ? '-' : Number(v).toFixed(m[3]) + ' ' + m[2]; };

			return E('tr', { 'class': 'tr' }, [
				m[1], s.count ? fmt(s.min) : '-', s.count ? fmt(s.mean) : '-',
				s.count ? fmt(s.median) : '-', s.count ? fmt(s.max) : '-', String(s.count || 0)
			].map(function(c) { return E('td', { 'class': 'td left' }, c); }));
		}))),
		E('p', {}, _('Last %d h, one sample every %d s. TX power and bias are only sampled in O5, RX power only with light.')
			.format((stats.window || 0) / 3600, stats.interval || 0))
	]);
}

function live(data) {
	if (!data || !data.status)
		return E('div', { 'class': 'alert-message warning' },
			_('The ONU at %s is not reachable. Set its address below, and give this router an address in its management subnet on the port it is connected to.')
				.format(data && data.host || '-'));

	var s = data.status, id = s.identity || {};

	return E([], [
		E('h3', {}, _('GPON')),
		table([
			[ _('State'), text(states[s.state] || s.state) ],
			[ _('ONU ID / OMCC GEM port'), '%s / %s'.format(text(s.onu_id), text(s.gem_port)) ],
			[ _('OMCI agent'), s.agent_operational ? _('Operational') : _('Not operational') ],
			[ _('OLT profile'), text(s.olt_profile_effective_name) ],
			[ _('MIB data sync / objects'), '%s / %s'.format(text(s.mib_sync), text(s.mib_objects)) ],
			[ _('OMCI frames rx / tx'), '%s / %s'.format(text(s.rx_packets), text(s.tx_packets)) ],
			[ _('FEC downstream / upstream'), '%s / %s'.format(text(s.fec_downstream), text(s.fec_upstream)) ],
			[ _('LAN VLAN mode'), vlanText(data.vlan) ]
		]),
		E('h3', {}, _('Identity')),
		table([
			[ _('Serial number'), text(id.serial) ],
			[ _('Vendor / equipment ID'), '%s / %s'.format(text(id['vendor-id']), text(id['equipment-id'])) ],
			[ _('Hardware version'), text(id['hardware-version']) ],
			[ _('Software versions'), '%s / %s'.format(text(id['software-version-0']), text(id['software-version-1'])) ]
		]),
		E('h3', {}, _('Optics')),
		table([
			[ _('RX power (1490 nm)'), dbm(s.bosa_rx_power_nw) ],
			[ _('TX power (1310 nm)'), dbm(s.bosa_tx_power_nw) ],
			[ _('Laser bias'), scaled(s.bosa_bias_ua, 1000, 2, 'mA') ],
			[ _('Supply voltage'), scaled(s.bosa_voltage_uv, 1000000, 3, 'V') ],
			[ _('BOSA temperature'), scaled(s.bosa_temperature_mc, 1000, 1, '°C') ],
			[ _('Alarms'), alarms(s.bosa_alarms || 0) ],
			[ _('Alarms reported to the OLT'), omciAlarms(s.omci_alarms) ]
		]),
		E('h3', {}, _('Statistics')),
		statsTable(data.stats)
	]);
}

function fetchData() {
	return L.resolveDefault(fs.exec_direct('/usr/bin/omci-stats', [ '--json' ], 'json'), null);
}

return view.extend({
	load: function() {
		return fetchData();
	},

	render: function(data) {
		var m, s, o;
		var root = E('div', { 'id': 'omci-stats-live' }, live(data));

		poll.add(function() {
			return fetchData().then(function(next) {
				dom.content(root, live(next));
			});
		}, 5);

		m = new form.Map('omci-stats', _('GPON ONU'),
			_('Live status of the GPON ONU in front of this router, read from its read-only OMCI status API.'));

		s = m.section(form.NamedSection, 'main', 'omci-stats', _('Connection'));
		s.addremove = false;

		o = s.option(form.Value, 'host', _('ONU address'));
		o.datatype = 'host';
		o.placeholder = '192.168.1.1';

		o = s.option(form.Value, 'timeout', _('Request timeout (s)'));
		o.datatype = 'range(1,30)';
		o.placeholder = '3';

		return m.render().then(function(mapEl) {
			return E([], [ root, mapEl ]);
		});
	}
});
