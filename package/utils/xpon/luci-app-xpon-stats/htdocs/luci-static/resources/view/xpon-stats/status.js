'use strict';
'require view';
'require poll';
'require dom';
'require fs';
'require form';

/*
 * The GPON or EPON ONU in front of this router, as its own status pages show
 * it. Styles use LuCI's generic theme variables or neutral tones, so the page
 * reads in any theme, light or dark.
 */

var CSS = [
	'.xs-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,280px),1fr));gap:14px;margin:0 0 18px}',
	'.xs-panel{min-width:0;padding:14px 16px;border:1px solid var(--border-color-medium,rgba(128,128,128,.3));border-radius:10px;background:var(--background-color-high,transparent)}',
	'.xs-panel>h4{margin:0 0 10px;padding:0;border:0;font-size:13px;font-weight:600;opacity:.75}',
	'.xs-pill{display:inline-flex;align-items:center;gap:6px;padding:1px 9px;border-radius:999px;font-size:12px;font-weight:600;white-space:nowrap;background:rgba(128,128,128,.16)}',
	'.xs-pill::before{content:"";width:6px;height:6px;border-radius:50%;background:currentColor}',
	'.xs-ok{background:rgba(34,197,94,.16);color:#15803d}',
	'.xs-warn{background:rgba(245,158,11,.18);color:#b45309}',
	'.xs-bad{background:rgba(239,68,68,.15);color:#b91c1c}',
	'[data-darkmode="true"] .xs-ok{color:#4ade80}',
	'[data-darkmode="true"] .xs-warn{color:#fbbf24}',
	'[data-darkmode="true"] .xs-bad{color:#f87171}',
	'.xs-head{display:flex;align-items:center;justify-content:space-between;gap:8px;font-weight:600}',
	'.xs-value{margin:6px 0 12px;font-size:30px;font-weight:650;line-height:1.1;font-variant-numeric:tabular-nums}',
	'.xs-value small{margin-left:6px;font-size:14px;font-weight:500;opacity:.6}',
	'.xs-bar{position:relative;height:10px;border-radius:999px;background:rgba(128,128,128,.2)}',
	'.xs-window{position:absolute;top:0;bottom:0;border-radius:999px;background:rgba(34,197,94,.25);box-shadow:inset 0 0 0 1px rgba(34,197,94,.55)}',
	'.xs-alarm{position:absolute;top:-2px;bottom:-2px;width:2px;margin-left:-1px;background:#ef4444}',
	'.xs-mark{position:absolute;top:-4px;width:4px;height:18px;margin-left:-2px;border-radius:2px;background:currentColor}',
	'.xs-scale{display:flex;justify-content:space-between;margin-top:6px;font-size:11px;opacity:.65;font-variant-numeric:tabular-nums}',
	'.xs-note{margin-top:8px;font-size:12px;opacity:.8}',
	'.xs-stages{display:flex;gap:4px;margin:2px 0 12px;container-type:inline-size}',
	'.xs-stage{flex:1 1 0;min-width:0;padding:7px 4px;border-radius:8px;text-align:center;font-size:12px;font-weight:600;line-height:1.25;background:rgba(128,128,128,.14);opacity:.7}',
	'.xs-stage span{display:block;overflow:hidden;font-size:11px;font-weight:500;white-space:nowrap;text-overflow:ellipsis}',
	'.xs-stage.done{opacity:1;background:rgba(34,197,94,.16)}',
	'.xs-stage.current{opacity:1;background:var(--primary-color-medium,#1f6feb);color:var(--on-primary-color,#fff)}',
	'.xs-stage.fail{opacity:1;background:rgba(239,68,68,.15);color:#b91c1c}',
	'[data-darkmode="true"] .xs-stage.fail{color:#f87171}',
	'.xs-text{margin:0;opacity:.85}',
	'.xs-cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,170px),1fr));gap:10px;margin:0 0 18px}',
	'.xs-card{min-width:0;padding:10px 14px;border:1px solid var(--border-color-medium,rgba(128,128,128,.3));border-radius:10px}',
	'.xs-card>div:first-child{font-size:12px;opacity:.7}',
	'.xs-card>div:last-child{font-size:17px;font-weight:600;overflow-wrap:anywhere}',
	'@container (max-width:420px){.xs-stage span{display:none}}',
	'@media (max-width:600px){.xs-value{font-size:26px}}'
].join('');

function text(value) {
	return value == null || value === '' ? '-' : String(value);
}

function yesno(value) {
	return value ? _('Yes') : _('No');
}

function clamp(v, lo, hi) {
	return Math.min(hi, Math.max(lo, v));
}

function dbmOf(nw) {
	var n = Number(nw);
	return Number.isFinite(n) && n > 0 ? 10 * Math.log10(n / 1e6) : null;
}

function dbm(nw) {
	var v = dbmOf(nw);
	return v == null ? '-' : '%.2f dBm'.format(v);
}

function scaled(value, divisor, digits, unit) {
	var n = Number(value);
	return value != null && value !== '' && Number.isFinite(n) ? (n / divisor).toFixed(digits) + ' ' + unit : '-';
}

/* OPTICAL_FRONTEND_ALARM_* bits, as the BOSA reports them */
var alarmNames = [
	_('TX low power'), _('TX high power'), _('TX low bias'), _('TX high bias'),
	_('RX low power (LOS)'), _('RX high power'), _('Low voltage'), _('High voltage'),
	_('Low temperature'), _('High temperature')
];

function alarms(bits) {
	var n = Number(bits) || 0;
	var names = alarmNames.filter(function(name, i) { return (n >> i) & 1; });
	return names.length ? names.join(', ') : _('None');
}

/* bosa_thresholds in nW, from xpon.public, to the module's dBm levels. */
function levels(t, dir) {
	var get = function(key) { return dbmOf((t || {})['%s_%s_nw'.format(dir, key)]); };
	var l = { alarmLo: get('alarm_low'), warnLo: get('warn_low'), warnHi: get('warn_high'), alarmHi: get('alarm_high') };

	/* Only a complete, ordered set describes the module. */
	return l.alarmLo != null && l.alarmLo <= l.warnLo && l.warnLo < l.warnHi && l.warnHi <= l.alarmHi ? l : null;
}

/* --- widgets --- */

function panel(title, children) {
	return E('div', { 'class': 'xs-panel' }, [ title ? E('h4', {}, title) : '' ].concat(children));
}

/* The module's levels (limits) replace the class window when the ONU reports them. */
function gauge(g) {
	var span = g.max - g.min, pct = function(v) { return clamp((v - g.min) / span * 100, 0, 100); };
	var v = g.value, m = g.limits, pill, cls = '', margin = '', ticks = [], note = g.note, step, i;
	var lo = m ? m.warnLo : g.lo, hi = m ? m.warnHi : g.hi;
	var loName = m ? _('the low warning') : g.loName, hiName = m ? _('the high warning') : g.hiName;

	if (v == null) {
		pill = g.empty;
		cls = g.emptyBad ? 'xs-bad' : '';
	} else if (m && (v < m.alarmLo || v > m.alarmHi)) {
		pill = _('Alarm');
		cls = 'xs-bad';
	} else if (v < lo || v > hi) {
		pill = m ? _('Warning') : v < lo ? _('Below window') : _('Above window');
		cls = m ? 'xs-warn' : 'xs-bad';
	} else if (!m && Math.min(v - lo, hi - v) < 3) {
		pill = _('Near limit');
		cls = 'xs-warn';
	} else {
		pill = m ? _('In range') : _('In window');
		cls = 'xs-ok';
	}
	if (v != null)
		margin = (v >= lo ? _('%.1f dB above %s').format(v - lo, loName) : _('%.1f dB below %s').format(lo - v, loName)) +
			' · ' + (v <= hi ? _('%.1f dB below %s').format(hi - v, hiName) : _('%.1f dB above %s').format(v - hi, hiName));
	if (m)
		note = [ _('Module: warning %.1f to %.1f dBm, alarm %.1f / %.1f dBm').format(m.warnLo, m.warnHi, m.alarmLo, m.alarmHi),
			 E('br'), g.note ];

	step = [ 1, 2, 5, 10 ].filter(function(s) { return span / s <= 6; })[0] || 10;
	for (i = g.min; i <= g.max; i += step)
		ticks.push(E('span', {}, String(i)));

	return panel(null, [
		E('div', { 'class': 'xs-head' }, [ E('span', {}, g.title), E('span', { 'class': 'xs-pill ' + cls }, pill) ]),
		E('div', { 'class': 'xs-value' }, v == null ? '–' : [ v.toFixed(2), E('small', {}, 'dBm') ]),
		E('div', { 'class': 'xs-bar', 'role': 'img', 'aria-label': '%s: %s'.format(g.title, v == null ? pill : v.toFixed(2) + ' dBm') }, [
			E('div', { 'class': 'xs-window', 'style': 'left:%.2f%%;width:%.2f%%'.format(pct(lo), pct(hi) - pct(lo)) }),
			m ? E('div', { 'class': 'xs-alarm', 'style': 'left:%.2f%%'.format(pct(m.alarmLo)) }) : '',
			m ? E('div', { 'class': 'xs-alarm', 'style': 'left:%.2f%%'.format(pct(m.alarmHi)) }) : '',
			v == null ? '' : E('div', { 'class': 'xs-mark', 'style': 'left:%.2f%%'.format(pct(v)) })
		]),
		E('div', { 'class': 'xs-scale' }, ticks),
		E('div', { 'class': 'xs-note' }, margin ? [ margin, E('br') ].concat(note) : note)
	]);
}

function stages(title, items, current, failed, sentence) {
	return panel(title, [
		E('div', { 'class': 'xs-stages' }, items.map(function(item, i) {
			var cls = i < current ? ' done' : i == current ? (failed ? ' fail' : ' current') : '';

			return E('div', { 'class': 'xs-stage' + cls, 'title': item[1],
					  'aria-current': i == current ? 'step' : null }, [ item[0], E('span', {}, item[1]) ]);
		})),
		E('p', { 'class': 'xs-text' }, sentence)
	]);
}

function cards(list) {
	return E('div', { 'class': 'xs-cards' }, list.map(function(c) {
		return E('div', { 'class': 'xs-card' }, [ E('div', {}, c[0]), E('div', {}, c[1]) ]);
	}));
}

function section(title, rows) {
	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, title),
		E('table', { 'class': 'table' }, rows.map(function(row) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'width': '40%' }, row[0]),
				E('td', { 'class': 'td left' }, row[1])
			]);
		}))
	]);
}

/* --- GPON --- */

var activation = [
	[ 'O1', _('Initial') ], [ 'O2', _('Standby') ], [ 'O3', _('Serial number') ],
	[ 'O4', _('Ranging') ], [ 'O5', _('Operation') ]
];

function gponText(s) {
	var id = text(s.onu_id);

	switch (s.state) {
	case 1: return _('Waiting for light and downstream frame sync from the OLT.');
	case 2: return _('In sync with the OLT; waiting for its upstream overhead message.');
	case 3: return _('Answering the OLT\'s serial number requests. It has not assigned an ONU ID yet: check that the OLT has this serial number authorised.');
	case 4: return _('The OLT assigned ONU ID %s and is measuring the fibre distance.').format(id);
	case 5: return s.agent_operational ?
		_('Registered as ONU %s and provisioned: the line carries traffic.').format(id) :
		_('Registered as ONU %s; the OLT has not finished OMCI provisioning.').format(id);
	case 6: return _('The downstream signal was lost; waiting for it to return before starting over.');
	case 7: return _('The OLT switched this ONU off (Disable_serial_number). It stays silent until the OLT enables it again.');
	default: return _('The PON port is not running in GPON mode.');
	}
}

/* enum omci_fec_status; upstream FEC is switched per burst by the OLT (G.984.3 13.3.3). */
function fec(value, upstream) {
	switch (value) {
	case 1: return _('Off');
	case 2: return _('On');
	case null: case undefined: return upstream ? _('Not reported (switched by the OLT per burst)') : '-';
	default: return _('Unknown');
	}
}

var omciAlarmNames = {
	'low-rx-power': _('Low RX power'), 'high-rx-power': _('High RX power'),
	'sf': _('Signal fail'), 'sd': _('Signal degrade'),
	'low-tx-power': _('Low TX power'), 'high-tx-power': _('High TX power'),
	'laser-bias': _('Laser bias'), 'lan-los': _('LAN LOS')
};

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

	var mode = { transparent: _('Transparent'), tagged: _('Tagged'), olt: _('Control by OLT') }[vlan.mode] || vlan.mode;

	if (vlan.error)
		return '%s: %s (%s)'.format(mode, vlan.error, text(vlan.port));
	if (vlan.source == 'pending')
		return '%s: %s (%s)'.format(mode, _('waiting for the OLT VLAN rule'), text(vlan.port));
	if (vlan.vid)
		return '%s: %s (%s)'.format(mode, _('VLAN %d, priority %d').format(vlan.vid, vlan.prio || 0), text(vlan.port));
	return '%s: %s (%s)'.format(mode, _('untagged'), text(vlan.port));
}

function gpon(s, vlan, t) {
	var id = s.identity || {}, olt = s.olt || {}, items = activation.slice();
	var current = s.state >= 1 && s.state <= 5 ? s.state - 1 : -1;
	var alarmCount = Array.isArray(s.omci_alarms) ? s.omci_alarms.reduce(function(n, a) { return n + (a.alarms || []).length; }, 0) : null;

	if (s.state == 6 || s.state == 7) {
		items.push([ 'O' + s.state, s.state == 6 ? _('Signal lost') : _('Emergency stop') ]);
		current = items.length - 1;
	}

	return E([], [
		E('div', { 'class': 'xs-grid' }, [
			stages(_('Activation'), items, current, s.state >= 6, gponText(s)),
			gauge({ title: _('Receive'), value: dbmOf(s.bosa_rx_power_nw), limits: levels(t, 'rx'), min: -35, max: -5, lo: -27, hi: -8,
				loName: _('sensitivity'), hiName: _('overload'), note: _('Class B+ receive window, -27 to -8 dBm'),
				empty: _('No light'), emptyBad: true }),
			gauge({ title: _('Transmit'), value: dbmOf(s.bosa_tx_power_nw), limits: levels(t, 'tx'), min: -2, max: 8, lo: 0.5, hi: 5,
				loName: _('minimum'), hiName: _('maximum'), note: _('Class B+ launch power, 0.5 to 5 dBm'),
				empty: _('Laser off') })
		]),
		cards([
			[ _('OMCI agent'), s.agent_operational ? _('Operational') : _('Not operational') ],
			[ _('OLT profile'), text(s.olt_profile_effective_name) ],
			[ _('MIB objects'), text(s.mib_objects) ],
			[ _('OMCI alarms'), alarmCount == null ? '-' : alarmCount ? _('%d active').format(alarmCount) : _('None') ]
		]),
		section(_('Channel and agent'), [
			[ _('PON state'), s.state == null ? '-' : 'O' + s.state ],
			[ _('ONU ID'), text(s.onu_id) ],
			[ _('Default GEM port'), text(s.gem_port) ],
			[ _('OMCI channel up'), yesno(s.channel_up) ],
			[ _('Agent enabled'), yesno(s.agent_enabled) ],
			[ _('Agent operational'), yesno(s.agent_operational) ],
			[ _('Permissive mode'), yesno(s.permissive) ],
			[ _('LAN VLAN mode'), vlanText(vlan) ]
		]),
		section(_('ONU identity'), [
			[ _('ONU type'), text(s.onu_type_name) ],
			[ _('UNI ports'), text(s.uni_count) ],
			[ _('Traffic management option'), text(s.traffic_management_option) ],
			[ _('Serial number'), text(id.serial) ],
			[ _('Vendor ID'), text(id['vendor-id']) ],
			[ _('Hardware version'), text(id['hardware-version']) ],
			[ _('Software image 0 version'), text(id['software-version-0']) ],
			[ _('Software image 1 version'), text(id['software-version-1']) ],
			[ _('Equipment ID'), text(id['equipment-id']) ],
			[ _('OMCC version'), id['omcc-version'] == null ? '-' : '0x%02X / %d'.format(id['omcc-version'], id['omcc-version']) ],
			[ _('LOID authentication'), text(s.loid_status_name) ]
		]),
		section(_('OLT interoperability'), [
			[ _('Configured profile'), text(s.olt_profile_configured_name) ],
			[ _('Effective profile'), text(s.olt_profile_effective_name) ],
			[ _('Active quirks'), (s.olt_profile_quirk_names || []).join(', ') || _('None') ],
			[ _('OLT vendor ID'), text(olt.vendor_id || olt.vendor) ],
			[ _('OLT equipment ID'), text(olt.equipment_id || olt.model) ],
			[ _('OLT version'), text(olt.version) ]
		]),
		section(_('MIB and OMCI traffic'), [
			[ _('MIB data sync'), text(s.mib_sync) ],
			[ _('RX packets / dropped'), '%s / %s'.format(text(s.rx_packets), text(s.rx_dropped)) ],
			[ _('TX packets / errors'), '%s / %s'.format(text(s.tx_packets), text(s.tx_errors)) ],
			[ _('Agent responses'), text(s.responses) ],
			[ _('Duplicates'), text(s.duplicates) ],
			[ _('Unsupported requests'), text(s.unsupported) ]
		]),
		section(_('Optical telemetry'), [
			[ _('Downstream FEC'), fec(s.fec_downstream) ],
			[ _('Upstream FEC'), fec(s.fec_upstream, true) ],
			[ _('BOSA temperature'), scaled(s.bosa_temperature_mc, 1000, 1, '°C') ],
			[ _('Supply voltage'), scaled(s.bosa_voltage_uv, 1e6, 3, 'V') ],
			[ _('Laser bias'), scaled(s.bosa_bias_ua, 1000, 2, 'mA') ],
			[ _('TX optical power'), dbm(s.bosa_tx_power_nw) ],
			[ _('RX optical power'), dbm(s.bosa_rx_power_nw) ],
			[ _('BOSA alarms'), alarms(s.bosa_alarms) ],
			[ _('Alarms reported to the OLT'), omciAlarms(s.omci_alarms) ]
		])
	]);
}

/* --- EPON --- */

var bringUp = [
	[ _('Light'), _('Optical signal') ], [ _('MPCP'), _('Registration') ],
	[ _('OAM'), _('Discovery') ], [ _('Auth'), _('LOID check') ], [ _('Service'), _('Up') ]
];

var authReasons = {
	1: _('LOID not found on the OLT'),
	2: _('Wrong password'),
	3: _('LOID already in use')
};

function eponStage(hw, s) {
	if (hw.signal != '1')
		return [ 0, true, _('No light from the OLT: check the fibre and its connectors.') ];
	if (!(+hw.registered & 1))
		return hw.nack_silent == '1' ?
			[ 1, true, _('The OLT refused the registration; the ONU stays silent for 60 s, then tries again.') ] :
			[ 1, false, _('Light present; registering with the OLT (MPCP).') ];
	if (s.oam != 'send_any')
		return [ 2, false, _('Registered as LLID 0; discovering OAM with the OLT.') ];
	if (s.auth == 'failed')
		return [ 3, true, _('The OLT rejected the LOID: %s.').format(authReasons[s.auth_fail_reason] || _('unknown reason')) ];
	if (s.auth == 'sent')
		return [ 3, false, _('Waiting for the OLT to accept the LOID.') ];
	return [ 4, false, _('OAM link up with OLT %s; the service is up.').format(text(s.olt_mac)) ];
}

function authText(s) {
	switch (s.auth) {
	case 'ok': return _('Accepted');
	case 'sent': return _('Sent, waiting for the OLT');
	case 'failed': return _('Rejected: %s').format(authReasons[s.auth_fail_reason] || _('unknown reason'));
	default: return _('Not requested by the OLT');
	}
}

function oamText(s, hw) {
	switch (s.oam) {
	case 'send_any': return _('Up');
	case 'send_local_remote': return _('Discovering');
	default: return (+hw.registered & 1) ? _('Waiting for the OLT') : _('Down');
	}
}

function eponVlan(s) {
	var modes = [ _('Transparent'), _('Tag'), _('Translation'), _('N:1 aggregation'), _('Trunk') ];
	var olt = (modes[s.vlan_mode] || '-') + (s.vlan_mode && s.vlan_default != null ? ', VLAN %d'.format(s.vlan_default) : '');

	if (s.vlan_override && s.vlan_override != 'olt')
		return _('%s (OLT asks for: %s)').format(s.vlan_override.indexOf('tag') == 0 ? _('Tag, set locally') : _('Transparent, set locally'), olt);
	return olt;
}

function epon(pub) {
	var e = pub.epon || {}, hw = e.mac || {}, s = e.oam || {}, d = pub.ddm || {}, st = eponStage(hw, s);

	return E([], [
		E('div', { 'class': 'xs-grid' }, [
			stages(_('Bring-up'), bringUp, st[0], st[1], st[2]),
			gauge({ title: _('Receive'), value: dbmOf(d.rx_nw), limits: levels(pub.thresholds, 'rx'), min: -35, max: -5, lo: -27, hi: -3,
				loName: _('sensitivity'), hiName: _('overload'), note: _('PX20+ receive window, -27 to -3 dBm'),
				empty: _('No light'), emptyBad: true }),
			gauge({ title: _('Transmit'), value: dbmOf(d.tx_nw), limits: levels(pub.thresholds, 'tx'), min: -4, max: 6, lo: -1, hi: 4,
				loName: _('minimum'), hiName: _('maximum'), note: _('PX20 launch power, -1 to 4 dBm'),
				empty: _('Laser off') })
		]),
		section(_('EPON'), [
			[ _('Transmitter'), e.tx_hold ? _('Held off by the OLT') : e.allow_tx ? _('Allowed') : _('Off') ],
			[ _('Holdover'), +hw.holdover_ms > 0 ? _('%d ms').format(hw.holdover_ms) : _('Off') ],
			[ _('Silent after a refused registration'), yesno(hw.nack_silent == '1') ],
			[ _('Optical signal'), hw.signal == '1' ? _('Present') : _('Lost') ],
			[ _('Downstream sync'), yesno(hw.sync == '1') ],
			[ _('MPCP registration'), (+hw.registered & 1) ? _('Registered') : _('Not registered') ],
			[ _('OAM link'), oamText(s, hw) ],
			[ _('CTC extended OAM'), s.ctc == 'ok' ? _('Version 0x%02x').format(s.ctc_version) : text(s.ctc) ],
			[ _('LOID authentication'), authText(s) ],
			[ _('VSOL inter-authentication'), {
				'off': _('Off'), 'not-asked': _('Not requested by the OLT'),
				'ok': _('OLT key accepted'), 'mismatch': _('OLT key mismatch')
			}[s.interauth] || '-' ],
			[ _('OLT MAC address'), s.oam == 'send_any' ? text(s.olt_mac) : '-' ],
			[ _('ONU MAC address'), text(s.onu_mac || hw.mac) ],
			[ _('LAN VLAN'), eponVlan(s) ],
			[ _('OAM frames received / sent'), '%s / %s'.format(text(s.rx_pdus), text(s.tx_pdus)) ],
			[ _('Unsupported attributes requested'), text(s.unknown_leaves) ]
		]),
		section(_('Optical telemetry'), [
			[ _('BOSA temperature'), scaled(d.temp_mc, 1000, 1, '°C') ],
			[ _('Supply voltage'), scaled(d.vcc_uv, 1e6, 3, 'V') ],
			[ _('Laser bias'), scaled(d.bias_ua, 1000, 2, 'mA') ],
			[ _('TX optical power'), dbm(d.tx_nw) ],
			[ _('RX optical power'), dbm(d.rx_nw) ],
			[ _('BOSA alarms'), d.alarms == null ? '-' : alarms(d.alarms) ]
		])
	]);
}

/* --- page --- */

function live(data) {
	if (!data || data.error || (!data.status && !data.public))
		return E('div', { 'class': 'alert-message warning' },
			_('The ONU at %s is not reachable. Set its address below, and give this router an address in its management subnet on the port it is connected to.')
				.format(data && data.host || '-'));

	if (data.mode == 'epon')
		return epon(data.public);
	if (!data.status)
		return E('div', { 'class': 'alert-message warning' },
			_('The ONU at %s answers, but not with its GPON status.').format(data.host));
	return gpon(data.status, data.vlan, data.public && data.public.thresholds);
}

function fetchData(args) {
	return L.resolveDefault(fs.exec_direct('/usr/bin/xpon-stats', args, 'json'), null);
}

return view.extend({
	load: function() {
		return fetchData([ '--json' ]);
	},

	render: function(data) {
		var m, s, o, status = data && data.status;
		var root = E('div', { 'id': 'xpon-stats-live' }, live(data));

		if (!document.getElementById('xpon-stats-style'))
			document.head.appendChild(E('style', { 'id': 'xpon-stats-style' }, CSS));

		/* Identity and OLT details come from the first full read; polls are lean. */
		poll.add(function() {
			return fetchData([ '--json', '--live' ]).then(function(next) {
				if (next && next.status && status)
					next.status = Object.assign(status, next.status);
				else if (next && next.status)
					status = next.status;
				dom.content(root, live(next));
			});
		}, 10);

		m = new form.Map('xpon-stats', _('PON ONU'),
			_('Live status of the GPON or EPON ONU in front of this router, read from its read-only status API.'));

		s = m.section(form.NamedSection, 'main', 'xpon-stats', _('Connection'));
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
