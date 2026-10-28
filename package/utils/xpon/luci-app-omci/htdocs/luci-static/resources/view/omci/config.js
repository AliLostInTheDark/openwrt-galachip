'use strict';
'require omci.layout as layout';
'require view';
'require form';
'require uci';
'require xpon.widgets as widgets';

/*
 * Field limits follow the agent's identity parser (net/xpon/omci/identity.c):
 * fixed-size OMCI string attributes, ASCII only.
 */
/* @max is a length, or a function of the current value for dual-format fields. */
function limitLength(o, max) {
	var renderWidget = o.renderWidget;

	o.renderWidget = function() {
		var node = renderWidget.apply(this, arguments);
		var input = node.querySelector ? node.querySelector('input') : null;
		var update = function() {
			input.setAttribute('maxlength', typeof(max) == 'function' ? max(input.value) : max);
		};

		if (input) {
			update();
			input.addEventListener('input', update);
		}
		return node;
	};
}

/*
 * Identity fields take ASCII or the same bytes in hexadecimal. Any character
 * that is not a hex digit means ASCII, so the shorter limit applies.
 */
function asciiOrHexLength(ascii, hex) {
	return function(value) {
		return /[^0-9a-f]/i.test(value) ? ascii : hex;
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

function hexDigits(value) {
	return value.replace(/^(hex:|0x)/i, '').replace(/[:\-._\s]/g, '');
}

/*
 * A select for a numeric option. Values are stored in decimal; an existing
 * hexadecimal or unlisted value is shown as-is instead of being replaced.
 */
function numericList(o, keepLabel, choices) {
	o.value('', keepLabel);
	choices.forEach(function(c) { o.value(String(c[0]), c[1]); });

	o.cfgvalue = function(section_id) {
		var value = uci.get('omci', section_id, this.option), n;

		if (value == null || value === '')
			return value;
		n = Number(value);
		if (!Number.isInteger(n))
			return value;
		if (this.keylist.indexOf(String(n)) < 0)
			this.value(String(n), _('%d (custom)').format(n));
		return String(n);
	};
}

function hexLabel(value, text) {
	return '0x%02X / %d (%s)'.format(value, value, text);
}

/*
 * What saving costs. A new serial number or vendor ID ranges the ONU again;
 * the password and every attribute the OLT reads from the MIB count from the
 * next registration; the agent and the PON driver take the rest at once.
 */
var applyCost = {
	enabled: 'drop', serial: 'drop', vendor_id: 'drop',
	permissive: 'live', fake_omci: 'live', reboot: 'live', transmit: 'next',
	vlan_translate: 'live', vlan_stack: 'live', olt_profile: 'live', olt_profile_force: 'live',
	password: 'next', loid: 'next', loid_password: 'next',
	hardware_version: 'next', software_version_0: 'next', software_version_1: 'next',
	equipment_id: 'next', omcc_version: 'next', traffic_management_option: 'next',
	dba_status_reporting: 'next', onu_type: 'next', uni_count: 'next'
};

return view.extend({
	load: function() {
		return uci.load('omci');
	},

	render: function(data) {
		var m, s, o;

		m = new form.Map('omci', _('OMCI configuration'),
			_('Settings for the OMCI agent. Each setting shows when a saved change reaches the OLT. Empty identity fields leave the current values unchanged.'));

		s = m.section(form.NamedSection, 'main', 'agent');
		s.anonymous = true;
		s.addremove = false;
		s.tab('agent', _('Agent'));
		s.tab('identity', _('ONU identity'));
		s.tab('capabilities', _('Advertised capabilities'));
		s.tab('rewrite', _('VLAN rewrite'));

		o = s.taboption('agent', form.Value, 'device', _('OMCI device'));
		o.datatype = 'uinteger';
		o.default = '0';

		/* omci.agent applies only the options present in UCI, and a flag
		 * left at its default would be removed: write them all. */
		o = s.taboption('agent', form.Flag, 'enabled', _('Enable OMCI agent'));
		o.default = o.enabled;
		o.rmempty = false;

		o = s.taboption('agent', form.Flag, 'permissive', _('Permissive mode'));
		o.default = o.enabled;
		o.rmempty = false;
		o.description = _('Accept OLT behavior that departs from ITU-T G.988 where a known workaround exists.');

		o = s.taboption('agent', form.Flag, 'fake_omci', _('Fake unsupported OMCI responses'));
		o.default = o.disabled;
		o.rmempty = false;

		o = s.taboption('agent', form.Flag, 'reboot', _('Allow OLT-requested reboot'));
		o.default = o.disabled;
		o.rmempty = false;

		o = s.taboption('agent', form.Flag, 'transmit', _('Transmitter'));
		o.default = o.enabled;
		o.rmempty = false;
		o.description = _('Off keeps the ONU receive-only from the next boot: it does not range and the laser stays off.');

		o = s.taboption('rewrite', form.DynamicList, 'vlan_translate', _('VLAN translation'));
		o.datatype = 'string';
		o.placeholder = '100:10';
		o.description = _('Pairs of <code>network VLAN:LAN VLAN</code>. The router uses the LAN VLAN, and the ONU replaces it with the network VLAN in both directions. Takes precedence over the VLAN rules sent by the OLT.');

		o = s.taboption('rewrite', form.DynamicList, 'vlan_stack', _('VLAN stacking (QinQ)'));
		o.datatype = 'string';
		o.placeholder = '200:30';
		o.description = _('Pairs of <code>S-VLAN:C-VLAN</code>. Upstream, the ONU adds the S-VLAN tag on top of the C-VLAN tag sent by the router; downstream, it removes it again.');

		o = s.taboption('agent', form.RichListValue, 'olt_profile', _('OLT profile'));
		[ ['auto', _('Auto detect')], ['generic', _('Generic')], ['nokia-alcl', _('Nokia / Alcatel-Lucent')],
		  ['dasan', _('DASAN')], ['huawei', _('Huawei')], ['fiberhome', _('FiberHome')], ['zte', _('ZTE')],
		  ['unspecified', _('Unspecified')] ].forEach(function(v) { o.value(v[0], v[1]); });
		o.default = 'auto';

		o = s.taboption('agent', form.RichListValue, 'olt_profile_force', _('Force OLT profile'));
		[ ['unspecified', _('Do not force')], ['generic', _('Generic')], ['nokia-alcl', _('Nokia / Alcatel-Lucent')],
		  ['dasan', _('DASAN')], ['huawei', _('Huawei')], ['fiberhome', _('FiberHome')], ['zte', _('ZTE')] ].forEach(function(v) { o.value(v[0], v[1]); });
		o.default = 'unspecified';

		o = s.taboption('identity', form.Value, 'serial', _('GPON serial number'));
		o.placeholder = 'ABCD12345678';
		o.description = _('12 characters: 4 vendor characters followed by 8 hexadecimal digits, or all 16 hexadecimal digits. The limit follows the first 4 characters.');
		/* Only the vendor part can hold non-hex characters. */
		limitLength(o, function(value) {
			return /[^0-9a-f]/i.test(value.substr(0, 4)) ? 12 : 16;
		});
		o.validate = function(section_id, value) {
			if (!value || /^[\x21-\x7e]{4}[0-9a-f]{8}$/i.test(value) || /^[0-9a-f]{16}$/i.test(value))
				return true;
			return _('Expecting 4 characters and 8 hexadecimal digits, or 16 hexadecimal digits');
		};

		o = s.taboption('identity', form.Value, 'vendor_id', _('Vendor ID'));
		o.placeholder = 'ABCD';
		o.description = _('4 characters, or 8 hexadecimal digits.');
		limitLength(o, asciiOrHexLength(4, 8));
		o.validate = function(section_id, value) {
			if (!value || /^[\x20-\x7e]{4}$/.test(value) || /^[0-9a-f]{8}$/i.test(value))
				return true;
			return _('Expecting 4 characters or 8 hexadecimal digits');
		};

		o = s.taboption('identity', form.Value, 'hardware_version', _('Hardware version'));
		o.description = _('Up to 14 characters.');
		asciiField(o, 14);

		o = s.taboption('identity', form.Value, 'software_version_0', _('Software image 0 version'));
		o.description = _('Up to 14 characters.');
		asciiField(o, 14);

		o = s.taboption('identity', form.Value, 'software_version_1', _('Software image 1 version'));
		o.description = _('Up to 14 characters.');
		asciiField(o, 14);

		o = s.taboption('identity', form.Value, 'equipment_id', _('Equipment ID'));
		o.description = _('Up to 20 characters.');
		asciiField(o, 20);

		o = s.taboption('identity', form.Value, 'password', _('SLID / GPON password'));
		o.password = true;
		o.description = _('Up to 10 characters, or 20 hexadecimal digits (optionally prefixed with <code>hex:</code>).');
		limitLength(o, function(value) {
			return /^hex:/i.test(value) ? 24 : asciiOrHexLength(10, 20)(value);
		});
		o.validate = function(section_id, value) {
			if (!value || (/^[\x20-\x7e]{1,10}$/.test(value) && !/^hex:/i.test(value)) ||
			    /^[0-9a-f]{20}$/i.test(hexDigits(value)))
				return true;
			return _('Expecting up to 10 characters or 20 hexadecimal digits');
		};

		o = s.taboption('identity', form.Value, 'loid', _('LOID'));
		o.description = _('Logical ONU ID, for OLTs that authenticate by LOID. Up to 24 characters.');
		asciiField(o, 24);

		o = s.taboption('identity', form.Value, 'loid_password', _('LOID password'));
		o.password = true;
		o.description = _('Up to 12 characters.');
		asciiField(o, 12);

		/* ITU-T G.988 9.1.2, ONU2-G OMCC version (Amd. 2, 2025) */
		o = s.taboption('capabilities', form.RichListValue, 'omcc_version', _('OMCC version'));
		numericList(o, _('Keep current value'), [
			[ 0x80, _('G.984.4 (06/04)') ],
			[ 0x81, _('G.984.4 Amd.1 (06/05)') ],
			[ 0x82, _('G.984.4 Amd.2 (03/06)') ],
			[ 0x83, _('G.984.4 Amd.3 (12/06)') ],
			[ 0x84, _('G.984.4 (02/08)') ],
			[ 0x85, _('G.984.4 Amd.1 (06/09)') ],
			[ 0x86, _('G.984.4 Amd.2 (2009), baseline messages only') ],
			[ 0x96, _('G.984.4 Amd.2 (2009), baseline and extended messages') ],
			[ 0xA0, _('G.988 (2010), baseline messages only') ],
			[ 0xA1, _('G.988 Amd.1 (2011), baseline messages only') ],
			[ 0xA2, _('G.988 Amd.2 (2012), baseline messages only') ],
			[ 0xA3, _('G.988 (2012), baseline messages only') ],
			[ 0xA4, _('G.988 Amd.1 (2014), baseline messages only') ],
			[ 0xB0, _('G.988 (2010), baseline and extended messages') ],
			[ 0xB1, _('G.988 Amd.1 (2011), baseline and extended messages') ],
			[ 0xB2, _('G.988 Amd.2 (2012), baseline and extended messages') ],
			[ 0xB3, _('G.988 (2012), baseline and extended messages') ],
			[ 0xB4, _('G.988 Amd.1 (2014), baseline and extended messages') ]
		].map(function(c) { return [ c[0], hexLabel(c[0], c[1]) ]; }));
		o.description = _('OMCI version the ONU reports to the OLT in ONU2-G. The agent default is 0xA0. Some OLTs expect the version of the ONT being replaced.');

		/* ITU-T G.988 9.1.1, ONU-G traffic management option */
		o = s.taboption('capabilities', form.RichListValue, 'traffic_management_option', _('Traffic management option'));
		numericList(o, _('Keep current value'), [
			[ 0, _('0 (Priority controlled: upstream traffic uses the traffic schedulers and priority queues)') ],
			[ 1, _('1 (Rate controlled: each connection\'s maximum upstream rate is enforced by shaping)') ],
			[ 2, _('2 (Priority and rate controlled: queues and schedulers, plus per-connection shaping)') ]
		]);
		o.description = _('Upstream traffic management model advertised in ONU-G; the OLT adapts its provisioning to it. The default is 0. This does not enable hardware traffic shaping.');

		o = s.taboption('capabilities', form.Flag, 'dba_status_reporting', _('Report queue status for DBA'));
		o.default = o.enabled;
		o.rmempty = false;
		o.description = _('ANI-G SR indication. On tells the OLT that the ONU sends valid DBRu queue reports; off makes it use traffic-monitoring DBA. The OLT reads it at its next MIB upload.');

		o = s.taboption('capabilities', form.RichListValue, 'onu_type', _('ONU type'));
		o.value('', _('Keep current value'));
		[
			_('Other'),
			_('SFU (single-family unit: bridged ONU, the router does the routing)'),
			_('HGU (home gateway unit: ONU with a built-in router)'),
			_('MDU (multi-dwelling unit: many subscriber ports in one building)'),
			_('SBU (single-business unit)'),
			_('MTU (multi-tenant unit: many business subscribers)'),
			_('CBU (cellular backhaul unit)')
		].forEach(function(name, value) {
			o.value(String(value), name);
		});

		o = s.taboption('capabilities', form.RichListValue, 'uni_count', _('UNI count'));
		numericList(o, _('Keep current value'), Array.from({ length: 16 }, function(x, i) {
			return [ i + 1, String(i + 1) ];
		}));
		o.description = _('Number of Ethernet UNIs advertised to the OLT. Advertising extra UNIs does not add physical Ethernet ports; match the ONT whose identity is cloned.');

		o = s.taboption('capabilities', form.RichListValue, 'veip_slot', _('VEIP slot'));
		o.value('', _('Learn from the OLT'));
		o.value('3', _('Slot 3 (0x0301)'));
		o.value('6', _('Slot 6 (0x0601)'));
		o.value('10', _('Slot 10 (0x0A01)'));
		o.default = '';
		o.description = _('Slot of the Virtual Ethernet Interface Point (VEIP). The ONU starts on slot 6 under the Nokia/ALCL profile and slot 10 otherwise, then moves the VEIP to the slot the OLT provisions. Nokia ISAM OLTs mostly use slot 6; some, such as Airtel India, use slot 3. Fix the slot only when the OLT rejects the uploaded one before provisioning.');

		m.children.forEach(function(section) {
			section.children.forEach(function(option) {
				/* Empty choices clear an override: keep current or learn from the OLT. */
				if (option.keylist && option.keylist.indexOf('') >= 0) {
					option.optional = true;
					option.rmempty = true;
				}
				if (applyCost[option.option])
					widgets.annotate(option, applyCost[option.option]);
			});
		});

		return m.render().then(function(node) {
			var descr = node.querySelector('.cbi-map-descr');

			widgets.style();
			node.insertBefore(widgets.legend([ 'live', 'next', 'drop' ]),
					  descr ? descr.nextSibling : node.firstChild);

			return layout.apply(node);
		});
	}
});
