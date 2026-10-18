'use strict';
'require view';
'require poll';
'require dom';
'require fs';
'require uci';
'require ui';
'require xpon.widgets as widgets';

var DEV = '/sys/class/net/pon0/device/';
var ROGUE = '/etc/xpon-rogue';

var authReasons = {
	1: _('LOID not found on the OLT'),
	2: _('Wrong password'),
	3: _('LOID already in use')
};

function text(value) {
	return value == null || value === '' ? '-' : String(value);
}

/* epon_state is "key=value" words over a few lines. */
function parseState(raw) {
	var state = {};

	String(raw || '').split(/\s+/).forEach(function(word) {
		var i = word.indexOf('=');
		if (i > 0)
			state[word.substring(0, i)] = word.substring(i + 1);
	});
	return state;
}

function oamText(s) {
	switch (s.oam) {
	case 'send_any': return _('Up');
	case 'send_local_remote': return _('Discovering');
	default: return s.registered ? _('Waiting for the OLT') : _('Down');
	}
}

function authText(s) {
	switch (s.auth) {
	case 'ok': return _('Accepted');
	case 'sent': return _('Sent, waiting for the OLT');
	case 'failed': return _('Rejected: %s').format(authReasons[s.auth_fail_reason] || _('unknown reason'));
	default: return _('Not requested by the OLT');
	}
}

function vlanText(s) {
	var modes = [ _('Transparent'), _('Tag'), _('Translation'), _('N:1 aggregation'), _('Trunk') ];
	var olt = (modes[s.vlan_mode] || '-') + (s.vlan_mode ? ', VLAN %d'.format(s.vlan_default) : '');

	if (s.vlan_override && s.vlan_override != 'olt')
		return _('%s (OLT asks for: %s)').format(s.vlan_override == 'tag' ? _('Tag, set locally') : _('Transparent, set locally'), olt);
	return olt;
}

/* bosa_ddm: SFF-8472 values in nW, uA, uV and m°C. */
function dbm(nw) {
	return +nw > 0 ? '%.2f dBm'.format(10 * Math.log10(nw / 1e6)) : '-';
}

function opticsText(d) {
	if (d.rx_nw == null)
		return '-';
	return _('RX %s, TX %s, bias %.1f mA, %.1f °C, %.2f V').format(
		dbm(d.rx_nw), dbm(d.tx_nw), d.bias_ua / 1000, d.temp_mc / 1000, d.vcc_uv / 1e6);
}

/* Bring-up order: light, MPCP registration, OAM discovery, LOID check, service. */
var bringUp = [
	[ _('Light'), _('Optical signal') ], [ _('MPCP'), _('Registration') ],
	[ _('OAM'), _('Discovery') ], [ _('Auth'), _('LOID check') ], [ _('Service'), _('Up') ]
];

function stage(s, hw) {
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

function line(s, hw, ddm, limits) {
	var st = stage(s, hw);

	return E('div', { 'class': 'xpon-grid' }, [
		widgets.panel(_('Bring-up'), widgets.stages(bringUp, st[0], st[1], st[2])),
		widgets.panel(null, widgets.gauge({
			title: _('Receive'), value: widgets.dbm(ddm.rx_nw), limits: limits.rx,
			min: -35, max: -5, lo: -27, hi: -3, loName: _('sensitivity'), hiName: _('overload'),
			note: _('PX20+ receive window, -27 to -3 dBm'), empty: _('No light'), emptyBad: true
		})),
		widgets.panel(null, widgets.gauge({
			title: _('Transmit'), value: widgets.dbm(ddm.tx_nw), limits: limits.tx,
			min: -4, max: 6, lo: -1, hi: 4, loName: _('minimum'), hiName: _('maximum'),
			note: _('PX20 launch power, -1 to 4 dBm'), empty: _('Laser off')
		}))
	]);
}

function row(label, value) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td left', 'width': '33%' }, label),
		E('td', { 'class': 'td left' }, value)
	]);
}

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('xpon'),
			L.resolveDefault(fs.read('/var/run/epon-oamd.json'), null),
			L.resolveDefault(fs.read('/sys/class/net/pon0/device/epon_state'), null),
			L.resolveDefault(fs.read('/sys/class/net/pon0/device/bosa_ddm'), null),
			L.resolveDefault(fs.read(DEV + 'allow_tx'), null),
			L.resolveDefault(fs.read(DEV + 'tx_hold'), null),	/* 5 */
			L.resolveDefault(fs.stat(ROGUE), null),
			widgets.limits()
		]);
	},

	/* The OLT's CTC TX power control (0x00A1) can hold the laser off. */
	handleRelease: function(ev) {
		return L.resolveDefault(fs.remove(ROGUE), null).then(function() {
			return fs.write(DEV + 'tx_hold', '0');
		}).then(function() {
			ui.addNotification(null, E('p', _('Transmitter hold released.')), 'info');
		}).catch(function(e) {
			ui.addNotification(null, E('p', _('Could not release the hold: %s').format(e.message)), 'error');
		});
	},

	renderTable: function(data) {
		var mode = uci.get('xpon', 'main', 'mode') || 'gpon';
		var s = {}, hw = parseState(data[2]);

		try { s = JSON.parse(data[1] || '{}'); } catch (e) { s = {}; }

		if (mode != 'epon')
			return E('div', { 'class': 'alert-message warning' },
				_('The PON port runs in GPON mode. Switch it under Network → PON Mode.'));

		var tx;

		if (text(data[5]).trim() == '1')
			tx = E('span', {}, [
				data[6] ? _('Held off for good by the OLT (%s)').format(ROGUE) : _('Held off by the OLT'),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-negative',
					'click': ui.createHandlerFn(this, 'handleRelease')
				}, _('Release'))
			]);
		else
			tx = data[4] == null ? '-' : text(data[4]).trim() == '1' ? _('Allowed') : _('Off');

		return E([], [ line(s, hw, parseState(data[3]), data[7] || {}), E('table', { 'class': 'table' }, [
			row(_('Transmitter'), tx),
			row(_('Holdover'), +hw.holdover_ms > 0 ? _('%d ms').format(hw.holdover_ms) : _('Off')),
			row(_('Silent after a refused registration'), hw.nack_silent == '1' ? _('Yes') : _('No')),
			row(_('Optical signal'), hw.signal == '1' ? _('Present') : _('Lost')),
			row(_('Optics'), opticsText(parseState(data[3]))),
			row(_('Downstream sync'), hw.sync == '1' ? _('Yes') : _('No')),
			row(_('MPCP registration'), (+hw.registered & 1) ? _('Registered') : _('Not registered')),
			row(_('OAM link'), oamText(s)),
			row(_('CTC extended OAM'), s.ctc == 'ok' ? _('Version 0x%02x').format(s.ctc_version) : text(s.ctc)),
			row(_('LOID authentication'), authText(s)),
			row(_('VSOL inter-authentication'), {
				'off': _('Off'), 'not-asked': _('Not requested by the OLT'),
				'ok': _('OLT key accepted'), 'mismatch': _('OLT key mismatch: check the secret and ONU MAC')
			}[s.interauth] || '-'),
			row(_('OLT MAC address'), s.oam == 'send_any' ? text(s.olt_mac) : '-'),
			row(_('ONU MAC address'), text(s.onu_mac || hw.mac)),
			row(_('LAN VLAN'), vlanText(s)),
			row(_('OAM frames received / sent'), '%s / %s'.format(text(s.rx_pdus), text(s.tx_pdus))),
			row(_('Unsupported attributes requested'), text(s.unknown_leaves))
		]) ]);
	},

	render: function(data) {
		widgets.style();
		var node = E('div', { 'class': 'cbi-section' }, this.renderTable(data));

		poll.add(L.bind(function() {
			return this.load().then(L.bind(function(fresh) {
				dom.content(node, this.renderTable(fresh));
			}, this));
		}, this), 10);

		return E([], [
			E('h2', {}, _('EPON status')),
			E('div', { 'class': 'cbi-map-descr' }, _('The EPON MAC registers with the OLT by itself (MPCP); the OAM link and LOID authentication then bring the service up.')),
			node,
			widgets.report()
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
