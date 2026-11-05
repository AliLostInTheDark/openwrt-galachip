'use strict';
'require view';
'require form';
'require uci';
'require ui';
'require xpon.mode as ponmode';
'require xpon.widgets as widgets';

return view.extend({
	load: function() {
		return uci.load('xpon');
	},

	render: function() {
		widgets.style();
		var m, s, o;

		m = new form.Map('xpon', _('PON mode'),
			_('The PON port runs one MAC at a time and can be switched dynamically or at boot. The ONU only registers on the OLT in the mode that matches the fibre network: GPON (ITU-T G.984, configured by OMCI) or EPON (IEEE 802.3ah, configured by OAM).'));
		s = m.section(form.NamedSection, 'main', 'xpon');
		o = s.option(form.ListValue, 'mode', _('Mode'),
			_('Select the desired operating mode. Use "Switch live" to reconfigure the transceiver immediately without rebooting, or "Save and reboot" for a full system restart.'));
		o.value('gpon', _('GPON'));
		o.value('epon', _('EPON'));
		o.default = 'gpon';
		this.map = m;
		return m.render();
	},

	handleSave: null,
	handleReset: null,

	handleSwitchLive: function() {
		var map = this.map;

		return map.parse().then(function() {
			var mode = map.data.get('xpon', 'main', 'mode') == 'epon' ? 'epon' : 'gpon';

			ponmode.confirmSwitch(mode, true);
		});
	},

	handleSaveReboot: function() {
		var map = this.map;

		return map.parse().then(function() {
			var mode = map.data.get('xpon', 'main', 'mode') == 'epon' ? 'epon' : 'gpon';

			ponmode.confirmSwitch(mode, false);
		});
	},

	addFooter: function() {
		return E('div', { 'class': 'cbi-page-actions' }, [
			E('button', {
				'class': 'btn cbi-button-action important',
				'click': ui.createHandlerFn(this, 'handleSwitchLive')
			}, _('Switch live')),
			' ',
			E('button', {
				'class': 'btn cbi-button-neutral',
				'click': ui.createHandlerFn(this, 'handleSaveReboot')
			}, _('Save and reboot'))
		]);
	}
});
