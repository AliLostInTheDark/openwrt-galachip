'use strict';
'require baseclass';
'require rpc';
'require fs';
'require poll';
'require ui';
'require xpon.widgets as widgets';

/*
 * The PON line state in the header on every page: the activation state and
 * the receive power, linking to the status page. The theme loads this when
 * the menu has the PON pages; the menu also says which mode runs.
 */

var DEV = '/sys/class/net/pon0/device/';

var callStatus = rpc.declare({
	object: 'omci.agent',
	method: 'status_live',
	params: [ 'device' ],
	expect: { status: {} }
});

function parse(raw) {
	var out = {};

	String(raw || '').split(/\s+/).forEach(function(word) {
		var i = word.indexOf('=');
		if (i > 0)
			out[word.substring(0, i)] = word.substring(i + 1);
	});
	return out;
}

function power(nw) {
	var dbm = widgets.dbm(nw);
	return dbm == null ? _('no light') : '%.1f dBm'.format(dbm);
}

function gpon() {
	return L.resolveDefault(callStatus(0), {}).then(function(s) {
		var up = s.state == 5 && s.agent_operational;

		if (s.state == null)
			return [ _('GPON down'), false ];
		return [ 'GPON O%d · %s'.format(s.state, power(s.bosa_rx_power_nw)), up ];
	});
}

function epon() {
	return Promise.all([
		L.resolveDefault(fs.read(DEV + 'epon_state'), null),
		L.resolveDefault(fs.read(DEV + 'bosa_ddm'), null),
		L.resolveDefault(fs.read('/var/run/epon-oamd.json'), null)
	]).then(function(data) {
		var hw = parse(data[0]), ddm = parse(data[1]), oam = {}, stage;

		try { oam = JSON.parse(data[2] || '{}'); } catch (e) {}
		if (hw.signal != '1')
			stage = _('no light');
		else if (!(+hw.registered & 1))
			stage = _('registering');
		else if (oam.oam != 'send_any')
			stage = _('OAM down');
		else if (oam.auth == 'failed' || oam.auth == 'sent')
			stage = _('LOID check');
		else
			return [ 'EPON · %s'.format(power(ddm.rx_nw)), true ];

		return [ 'EPON %s'.format(stage), false ];
	});
}

return baseclass.extend({
	__init__: function() {
		return ui.menu.load().then(L.bind(function(tree) {
			var net = (((tree.children || {}).admin || {}).children || {}).network || {};
			var nodes = net.children || {};

			if (nodes.omci)
				return this.start(gpon, 'admin/network/omci/status');
			if (nodes.epon)
				return this.start(epon, 'admin/network/epon/status');
		}, this));
	},

	start: function(read, page) {
		var open = function() { location.href = L.url(page); };
		var update = function() {
			return read().then(function(state) {
				ui.showIndicator('xpon', state[0], open, state[1] ? 'active' : 'inactive');
			});
		};

		poll.add(update, 30);
		return update();
	}
});
