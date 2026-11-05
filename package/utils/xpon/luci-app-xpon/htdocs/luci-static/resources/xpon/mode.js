'use strict';
'require baseclass';
'require uci';
'require rpc';
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

	/* Live-switch without rebooting: applies UCI changes through procd and reloads daemons. */
	switchLive: function(mode) {
		ui.showIndicator('mode_switching', _('Switching PON port to %s mode live…').format(names[mode]));
		return uci.load('xpon').then(function() {
			uci.set('xpon', 'main', 'mode', mode);
			return uci.save();
		}).then(function() {
			return ui.changes.apply(true);
		}).then(function() {
			ui.hideIndicator('mode_switching');
			ui.addNotification(null, E('p', _('PON mode switched to %s live. The optical transceiver has been reconfigured and protocol daemons reloaded.').format(names[mode])), 'info');
			window.setTimeout(function() {
				window.location.reload();
			}, 1500);
		}).catch(function(e) {
			ui.hideIndicator('mode_switching');
			ui.addNotification(null, E('p', _('Live switch to %s failed: %s').format(names[mode], e.message || e)), 'error');
		});
	},

	/* Store the mode and reboot. */
	switchTo: function(mode) {
		return uci.load('xpon').then(function() {
			uci.set('xpon', 'main', 'mode', mode);
			return uci.save();
		}).then(function() {
			return callCommit('xpon');
		}).then(function() {
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

	confirmSwitch: function(mode, live) {
		if (live) {
			ui.showModal(_('Switch to %s live?').format(names[mode]), [
				E('p', _('The device reconfigures the optical SerDes and protocol stack immediately without a full system reboot. The network link will briefly drop while the transceiver re-ranges with the OLT.')),
				E('div', { 'class': 'right' }, [
					E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')),
					' ',
					E('button', {
						'class': 'btn cbi-button-action important',
						'click': L.bind(function() {
							ui.hideModal();
							this.switchLive(mode);
						}, this)
					}, _('Switch live'))
				])
			]);
			return;
		}

		ui.showModal(_('Switch to %s and reboot?').format(names[mode]), [
			E('p', _('The device saves the PON mode and restarts the entire system. The ONU only registers on the OLT in the mode that matches the fibre network.')),
			E('div', { 'class': 'right' }, [
				E('button', { 'class': 'btn', 'click': ui.hideModal }, _('Cancel')),
				' ',
				E('button', {
					'class': 'btn cbi-button-action important',
					'click': L.bind(function() { this.switchTo(mode); }, this)
				}, _('Save and reboot'))
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
					'click': L.bind(function() { this.confirmSwitch(mode, true); }, this)
				}, _('Switch to %s live').format(names[mode])),
				' ',
				E('button', {
					'class': 'btn cbi-button-neutral',
					'click': L.bind(function() { this.confirmSwitch(mode, false); }, this)
				}, _('Save and reboot'))
			])
		]);
	}
});
