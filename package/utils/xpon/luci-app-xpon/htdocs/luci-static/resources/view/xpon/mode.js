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
			_('The PON port runs one MAC at a time and picks it at boot. The ONU only registers on the OLT in the mode that matches the fibre network: GPON (ITU-T G.984, configured by OMCI) or EPON (IEEE 802.3ah, configured by OAM).'));
		s = m.section(form.NamedSection, 'main', 'xpon');
		o = s.option(form.ListValue, 'mode', _('Mode'),
			_('Save and reboot applies a new mode.'));
		o.value('gpon', _('GPON'));
		o.value('epon', _('EPON'));
		o.default = 'gpon';
		this.map = m;
		return m.render();
	},

	handleSave: null,
	handleReset: null,

	handleSaveApply: function() {
		var map = this.map;

		return map.parse().then(function() {
			var mode = map.data.get('xpon', 'main', 'mode') == 'epon' ? 'epon' : 'gpon';

			ponmode.confirmSwitch(mode);
		});
	},

	addFooter: function() {
		/* One action: store the mode and reboot. */
		return E('div', { 'class': 'cbi-page-actions' }, [
			E('button', {
				'class': 'btn cbi-button-apply',
				'click': ui.createHandlerFn(this, 'handleSaveApply')
			}, _('Save and reboot'))
		]);
	}
});
