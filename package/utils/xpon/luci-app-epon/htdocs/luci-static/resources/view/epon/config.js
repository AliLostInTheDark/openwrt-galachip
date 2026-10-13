'use strict';
'require view';
'require form';
'require uci';

return view.extend({
	load: function() {
		return uci.load('epon');
	},

	render: function() {
		var m, s, o;

		m = new form.Map('epon', _('EPON configuration'));
		s = m.section(form.NamedSection, 'main', 'epon', _('Authentication'));
		o = s.option(form.Flag, 'enabled', _('Answer the OLT\'s OAM'),
			_('Without OAM the OLT registers the ONU but does not bring the service up.'));
		o.default = '1';
		o = s.option(form.Flag, 'active_discovery', _('Active OAM discovery'),
			_('Start 802.3ah discovery instead of waiting for the OLT. Leave off unless the status page shows the OAM link stuck waiting while MPCP is registered.'));
		o.default = '0';
		o = s.option(form.Value, 'onu_mac', _('ONU MAC address'),
			_('The MAC the OLT registers. Many EPON OLTs authorise the ONU by it, so replacing an ISP ONT usually means entering its MAC here. Empty keeps the board\'s own; a change applies on the next registration.'));
		o.datatype = 'macaddr';
		o = s.option(form.Value, 'loid', _('LOID'),
			_('Logical ONU ID provisioned on the OLT; up to 24 characters.'));
		o.datatype = 'maxlength(24)';
		o = s.option(form.Value, 'loid_password', _('LOID password'),
			_('Up to 12 characters.'));
		o.datatype = 'maxlength(12)';
		o.password = true;

		s = m.section(form.NamedSection, 'main', 'epon', _('LAN VLAN'));
		o = s.option(form.ListValue, 'vlan_mode', _('VLAN mode'),
			_('<em>Set by the OLT</em> applies the VLAN mode the OLT sends (CTC Port VLAN). <em>Transparent</em> passes frames unchanged; <em>Tagged</em> adds the VLAN below upstream and removes it towards the LAN.'));
		o.value('olt', _('Set by the OLT'));
		o.value('transparent', _('Transparent'));
		o.value('tag', _('Tagged'));
		o.default = 'olt';
		o = s.option(form.Value, 'vlan_id', _('VLAN ID'));
		o.datatype = 'range(1,4094)';
		o.depends('vlan_mode', 'tag');

		s = m.section(form.NamedSection, 'main', 'epon', _('ONU identity'),
			_('Reported to the OLT as the ONU serial number (CTC). Some OLTs accept only known vendors.'));
		o = s.option(form.Value, 'vendor_id', _('Vendor ID'), _('Four characters.'));
		o.datatype = 'maxlength(4)';
		o = s.option(form.Value, 'model', _('Model'), _('Four characters.'));
		o.datatype = 'maxlength(4)';
		o = s.option(form.Value, 'hardware_version', _('Hardware version'));
		o.datatype = 'maxlength(8)';
		o = s.option(form.Value, 'software_version', _('Software version'));
		o.datatype = 'maxlength(16)';
		o = s.option(form.Value, 'extended_model', _('Extended model'),
			_('Sent with the serial number on CTC 3.0 links.'));
		o.datatype = 'maxlength(16)';
		o.placeholder = 'XZ000-G7';

		s = m.section(form.NamedSection, 'main', 'epon', _('Replaced ONU identity'),
			_('For OLTs that check more than the serial number: copy these from the ONU this one replaces (its OAM capture or status). Empty keeps the G7\'s own. This changes what the OLT sees, not what the device is.'));
		o = s.option(form.Value, 'oam_oui', _('OAM OUI'), _('6 hex digits, e.g. b48618.'));
		o.validate = function(id, v) { return !v || /^[0-9a-fA-F]{6}$/.test(v) || _('6 hex digits'); };
		o = s.option(form.Value, 'oam_vendor_info', _('OAM vendor specific'), _('8 hex digits.'));
		o.validate = function(id, v) { return !v || /^[0-9a-fA-F]{8}$/.test(v) || _('8 hex digits'); };
		o = s.option(form.Value, 'chipset_id', _('CTC chipset ID'), _('16 hex digits.'));
		o.validate = function(id, v) { return !v || /^[0-9a-fA-F]{16}$/.test(v) || _('16 hex digits'); };

		s = m.section(form.NamedSection, 'main', 'epon', _('VSOL inter-authentication'),
			_('VSOL OLTs (for example Excitel\'s V1600D) exchange a key derived from the ONU MAC and a shared secret before provisioning. Leave empty unless your OLT asks for it; the Status page shows whether it did.'));
		o = s.option(form.Value, 'interauth_secret', _('Shared secret'));
		o.password = true;
		o.datatype = 'maxlength(64)';

		s = m.section(form.NamedSection, 'main', 'epon', _('Logging'));
		o = s.option(form.ListValue, 'debug', _('Log level'));
		o.value('0', _('Normal'));
		o.value('1', _('Debug'));

		return m.render();
	}
});
