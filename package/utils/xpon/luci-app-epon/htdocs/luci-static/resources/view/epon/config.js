'use strict';
'require view';
'require form';
'require uci';
'require xpon.widgets as widgets';

/* Field limits follow epon-oamd's buffers (src/epon-oamd.c, struct oam). */
function limitLength(o, max) {
	var renderWidget = o.renderWidget;

	o.renderWidget = function() {
		var node = renderWidget.apply(this, arguments);
		var input = node.querySelector ? node.querySelector('input') : null;

		if (input)
			input.setAttribute('maxlength', max);
		return node;
	};
}

function asciiField(o, max) {
	limitLength(o, max);
	o.validate = function(section_id, value) {
		if (!/^[\x20-\x7e]*$/.test(value))
			return _('Only printable ASCII characters are allowed');
		if (value.length > max)
			return _('At most %d characters').format(max);
		return true;
	};
}

function hexField(o, digits) {
	limitLength(o, digits);
	o.validate = function(section_id, value) {
		return !value || new RegExp('^[0-9a-fA-F]{' + digits + '}$').test(value) ||
			_('%d hexadecimal digits').format(digits);
	};
}

return view.extend({
	load: function() {
		return uci.load('epon');
	},

	render: function() {
		var m, s, o;

		m = new form.Map('epon', _('EPON configuration'),
			_('Each setting shows when a saved change reaches the OLT.'));
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
		limitLength(o, 17);
		o = s.option(form.Value, 'loid', _('LOID'),
			_('Logical ONU ID provisioned on the OLT. Up to 24 characters.'));
		asciiField(o, 24);
		o = s.option(form.Value, 'loid_password', _('LOID password'),
			_('Up to 12 characters.'));
		o.password = true;
		asciiField(o, 12);

		s = m.section(form.NamedSection, 'main', 'epon', _('LAN VLAN'));
		o = s.option(form.ListValue, 'vlan_mode', _('VLAN mode'));
		o.value('olt', _('Set by the OLT (applies the CTC Port VLAN mode the OLT sends)'));
		o.value('transparent', _('Transparent (frames pass unchanged; the router tags the VLAN)'));
		o.value('tag', _('Tagged (adds the VLAN below upstream, removes it towards the LAN)'));
		o.default = 'olt';
		o = s.option(form.Value, 'vlan_id', _('VLAN ID'));
		o.datatype = 'range(1,4094)';
		o.depends('vlan_mode', 'tag');

		s = m.section(form.NamedSection, 'main', 'epon', _('ONU identity'),
			_('Reported to the OLT as the ONU serial number (CTC). Some OLTs accept only known vendors. Empty fields send the built-in defaults.'));
		o = s.option(form.Value, 'vendor_id', _('Vendor ID'), _('Up to 4 characters.'));
		asciiField(o, 4);
		o = s.option(form.Value, 'model', _('Model'), _('Up to 4 characters.'));
		asciiField(o, 4);
		o = s.option(form.Value, 'hardware_version', _('Hardware version'), _('Up to 8 characters.'));
		asciiField(o, 8);
		o = s.option(form.Value, 'software_version', _('Software version'), _('Up to 16 characters.'));
		asciiField(o, 16);
		o = s.option(form.Value, 'extended_model', _('Extended model'),
			_('Sent with the serial number on CTC 3.0 links. Up to 16 characters.'));
		asciiField(o, 16);

		s = m.section(form.NamedSection, 'main', 'epon', _('Replaced ONU identity'),
			_('For OLTs that check more than the serial number: copy these from the ONU this one replaces (its OAM capture or status). Empty keeps this ONU\'s own. This changes what the OLT sees, not what the device is.'));
		o = s.option(form.Value, 'oam_oui', _('OAM OUI'), _('6 hexadecimal digits, e.g. b48618.'));
		hexField(o, 6);
		o = s.option(form.Value, 'oam_vendor_info', _('OAM vendor specific'), _('8 hexadecimal digits.'));
		hexField(o, 8);
		o = s.option(form.Value, 'chipset_id', _('CTC chipset ID'), _('16 hexadecimal digits.'));
		hexField(o, 16);

		s = m.section(form.NamedSection, 'main', 'epon', _('VSOL inter-authentication'),
			_('VSOL OLTs (for example Excitel\'s V1600D) exchange a key derived from the ONU MAC and a shared secret before provisioning. Leave empty unless your OLT asks for it; the Status page shows whether it did.'));
		o = s.option(form.Value, 'interauth_secret', _('Shared secret'), _('Up to 64 characters.'));
		o.password = true;
		asciiField(o, 64);

		s = m.section(form.NamedSection, 'main', 'epon', _('Logging'));
		o = s.option(form.ListValue, 'debug', _('Log level'));
		o.value('0', _('0 (Normal: state changes and errors, also in the kernel log)'));
		o.value('1', _('1 (Debug: also unsupported or ignored OLT requests, key changes and VLAN updates)'));

		/*
		 * procd restarts epon-oamd when its command line changes, which every
		 * option but the ONU MAC is on; init writes that one to the MAC.
		 */
		m.children.forEach(function(section) {
			section.children.forEach(function(option) {
				widgets.annotate(option, option.option == 'onu_mac' ? 'next' : 'drop');
			});
		});

		return m.render().then(function(node) {
			var descr = node.querySelector('.cbi-map-descr');

			widgets.style();
			node.insertBefore(widgets.legend([ 'next', 'drop' ]),
					  descr ? descr.nextSibling : node.firstChild);
			return node;
		});
	}
});
