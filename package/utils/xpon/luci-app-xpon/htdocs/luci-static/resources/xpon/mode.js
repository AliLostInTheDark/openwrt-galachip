'use strict';
'require baseclass';
'require uci';
'require rpc';
'require fs';
'require ui';

var callCommit = rpc.declare({
	object: 'uci',
	method: 'commit',
	params: [ 'config' ]
});

var callReboot = rpc.declare({
	object: 'system',
	method: 'reboot',
	expect: { result: 0 }
});

var names = { gpon: 'GPON', epon: 'EPON' };

/* Answers once rpcd serves the calls the login page makes. */
function rpcdReady() {
	return fetch(L.env.ubuspath, {
		method: 'POST',
		headers: { 'Content-Type': 'application/json' },
		body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'call',
			params: [ '00000000000000000000000000000000', 'luci', 'getFeatures', {} ] })
	}).then(function(res) {
		return res.json();
	}).then(function(reply) {
		return Array.isArray(reply.result) && reply.result[0] == 0;
	}).catch(function() {
		return false;
	});
}

/*
 * xpon-mode-switch restarts uhttpd and rpcd shortly after it returns: wait
 * for them to go down and come back before loading a page, else the login
 * page's first calls fail.
 */
function awaitRestart(url) {
	var went_down = false, start = Date.now();

	(function poll() {
		rpcdReady().then(function(up) {
			went_down = went_down || !up;
			if ((went_down && up) || Date.now() - start > 60000)
				window.location.href = url;
			else
				window.setTimeout(poll, 500);
		});
	})();
}

return baseclass.extend({
	names: names,

	/* Resolves to 'gpon' or 'epon'. */
	current: function() {
		return uci.load('xpon').then(function() {
			return uci.get('xpon', 'main', 'mode') == 'epon' ? 'epon' : 'gpon';
		}, function() {
			return 'gpon';
		});
	},

	/*
	 * Switch the running PON port; xpon-mode-switch stores the mode and
	 * restarts the OMCI and OAM daemons in order, so no reboot is needed.
	 */
	switchLive: function(mode) {
		ui.showModal(_('Switching to %s…').format(names[mode]), [
			E('p', { 'class': 'spinning' },
				_('The PON port restarts in %s mode. The link drops for a few seconds while the ONU ranges again.').format(names[mode]))
		]);

		return fs.exec('/usr/sbin/xpon-mode-switch', [ mode ]).then(function(res) {
			if (res.code)
				throw new Error((res.stderr || '').trim() || _('exit code %d').format(res.code));
			/*
			 * The switch restarts rpcd, which ends this login: the menu
			 * (GPON or EPON entries) is only rebuilt at the next login.
			 */
			ui.menu.flushCache();
			ui.showModal(_('Switched to %s').format(names[mode]), [
				E('p', { 'class': 'spinning' },
					_('Logging out so the menu shows the %s pages. Log in again to continue.').format(names[mode]))
			]);
			awaitRestart(L.url('admin', 'pon', mode == 'epon' ? 'epon' : 'omci', 'status'));
		}).catch(function(e) {
			ui.hideModal();
			ui.addNotification(null, E('p', _('Switching to %s failed: %s').format(names[mode], e.message)), 'error');
		});
	},

	/* Store the mode and reboot: the PON MAC is chosen at boot. */
	switchTo: function(mode) {
		return uci.load('xpon').then(function() {
			uci.set('xpon', 'main', 'mode', mode);
			return uci.save();
		}).then(function() {
			return callCommit('xpon');
		}).then(function() {
			ui.menu.flushCache();
			return callReboot();
		}).then(function() {
			ui.showModal(_('Rebooting…'), [
				E('p', { 'class': 'spinning' },
					_('The device restarts in %s mode. This page reconnects when it is back.').format(names[mode]))
			]);
			ui.awaitReconnect();
		}).catch(function(e) {
			ui.addNotification(null, E('p', _('Switching to %s failed: %s').format(names[mode], e.message)), 'error');
		});
	},

	confirmSwitch: function(mode) {
		ui.showModal(_('Switch to %s?').format(names[mode]), [
			E('p', _('"Switch now" restarts the PON port in the new mode without rebooting; the fibre link drops for a few seconds. "Save and reboot" stores the mode and restarts the device. The ONU only registers on the OLT in the mode that matches the fibre network; in the wrong mode the internet connection stays down until you switch back.')),
			E('div', { 'class': 'right' }, [
				E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')),
				' ',
				E('button', {
					'class': 'btn cbi-button-neutral',
					'click': L.bind(function() { this.switchTo(mode); }, this)
				}, _('Save and reboot')),
				' ',
				E('button', {
					'class': 'btn cbi-button-action important',
					'click': L.bind(function() { this.switchLive(mode); }, this)
				}, _('Switch now'))
			])
		]);
	},

	/* What an inactive mode's pages show instead of their content. */
	inactiveNotice: function(mode) {
		return E([], [
			E('h2', {}, names[mode]),
			E('div', { 'class': 'cbi-map-descr' },
				_('The PON port runs in %s mode, so %s is off.').format(names[mode == 'gpon' ? 'epon' : 'gpon'], names[mode])),
			E('div', { 'class': 'cbi-page-actions' }, [
				E('button', {
					'class': 'btn cbi-button-action important',
					'click': L.bind(function() { this.confirmSwitch(mode); }, this)
				}, _('Switch to %s').format(names[mode]))
			])
		]);
	}
});
