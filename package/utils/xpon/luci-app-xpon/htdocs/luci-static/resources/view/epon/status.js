'use strict';
'require view';
'require poll';
'require dom';
'require fs';
'require uci';
'require ui';
'require rpc';
'require xpon.widgets as widgets';

var ROGUE = '/etc/xpon-rogue';

var callSummary = rpc.declare({ object: 'xpon', method: 'summary' });
var callTxRelease = rpc.declare({ object: 'xpon', method: 'tx_release' });

var authReasons = {
	1: _('LOID not found on the OLT'),
	2: _('Wrong password'),
	3: _('LOID already in use')
};

function text(value) {
	return value == null || value === '' ? '-' : String(value);
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

/* xpon summary ddm: SFF-8472 values in nW, uA, uV and m°C. */
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
	if (s.interauth == 'asked')
		return [ 3, true, _('The OLT requested VSOL inter-authentication on leaf 0x2040; set the shared secret under EPON Configuration.') ];
	if (s.interauth == 'mismatch')
		return [ 3, true, _('VSOL inter-authentication key mismatch; check the shared secret under EPON Configuration.') ];
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
		E('td', { 'class': 'td left' }, [ value ])
	]);
}

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('xpon'),
			L.resolveDefault(fs.read('/var/run/epon-oamd.json'), null),
			L.resolveDefault(callSummary(), {}),	/* 2: MAC state, transmitter, optics */
			L.resolveDefault(fs.stat(ROGUE), null),
			widgets.limits()
		]);
	},

	/* The OLT's CTC TX power control (0x00A1) can hold the laser off. */
	handleRelease: function(ev) {
		return L.resolveDefault(fs.remove(ROGUE), null).then(function() {
			return callTxRelease();
		}).then(function() {
			ui.addNotification(null, E('p', _('Transmitter hold released.')), 'info');
		}).catch(function(e) {
			ui.addNotification(null, E('p', _('Could not release the hold: %s').format(e.message)), 'error');
		});
	},

	renderTable: function(data) {
		var mode = uci.get('xpon', 'main', 'mode') || 'gpon';
		var summary = data[2] || {};
		var s = {}, hw = (summary.epon && summary.epon.mac) || {}, ddm = summary.ddm || {};

		try { s = JSON.parse(data[1] || '{}'); } catch (e) { s = {}; }

		if (mode != 'epon')
			return E('div', { 'class': 'alert-message warning' },
				_('The PON interface is currently operating in GPON mode. Switch modes under PON → PON Mode.'));

		var transmitter = summary.epon || {};
		var tx;

		if (transmitter.tx_hold)
			tx = E('span', {}, [
				data[3] ? _('Permanently disabled by the OLT (%s)').format(ROGUE) : _('Temporarily disabled by the OLT'),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-negative',
					'click': ui.createHandlerFn(this, 'handleRelease')
				}, _('Release'))
			]);
		else
			tx = transmitter.allow_tx == null ? '-' : transmitter.allow_tx ? _('Enabled') : _('Off');

		return E([], [ line(s, hw, ddm, data[4] || {}), E('table', { 'class': 'table' }, [
			row(_('Optical Transmitter'), tx),
			row(_('Optical Holdover'), +hw.holdover_ms > 0 ? _('%d ms').format(hw.holdover_ms) : _('Off')),
			row(_('Quiet-Zone Backoff (MPCP NACK)'), hw.nack_silent == '1' ? _('Active') : _('Inactive')),
			row(_('Optical Signal'), hw.signal == '1' ? _('Present') : _('LOS')),
			row(_('Optical Telemetry'), opticsText(ddm)),
			row(_('Downstream Sync'), hw.sync == '1' ? _('Locked') : _('Unlocked')),
			row(_('MPCP Registration'), (+hw.registered & 1) ? _('Registered') : _('Unregistered')),
			row(_('IEEE 802.3ah OAM Link'), oamText(s)),
			row(_('CTC Extended OAM'), s.ctc == 'ok' ? _('Version 0x%02X').format(s.ctc_version) : text(s.ctc)),
			row(_('LOID Authentication'), authText(s)),
			row(_('VSOL Inter-Authentication'), {
				'off': _('Off'), 'not-asked': _('Not requested by the OLT'),
				'asked': _('Requested by the OLT — set the shared secret under EPON Configuration'),
				'ok': _('Accepted'), 'mismatch': _('Key mismatch — verify shared secret and ONU MAC address')
			}[s.interauth] || '-'),
			s.interauth_olt ? row(_('VSOL OLT Key (0x2040)'), E('code', {}, s.interauth_olt)) : '',
			row(_('OLT MAC Address'), s.oam == 'send_any' ? text(s.olt_mac) : '-'),
			row(_('ONU MAC Address'), text(s.onu_mac || hw.mac)),
			row(_('LAN VLAN'), vlanText(s)),
			row(_('OAMPDUs Received / Transmitted'), '%s / %s'.format(text(s.rx_pdus), text(s.tx_pdus))),
			row(_('Unsupported Attributes Requested'), text(s.unknown_leaves))
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
			E('h2', {}, _('EPON Status')),
			E('div', { 'class': 'cbi-map-descr' }, _('IEEE 802.3ah MPCP registration runs in hardware; OAM discovery and CTC LOID authentication then bring up the data service.')),
			node,
			widgets.report()
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
