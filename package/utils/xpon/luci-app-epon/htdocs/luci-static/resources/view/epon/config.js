'use strict';
'require view';
'require form';
'require fs';
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

/* RFC 1321 MD5 in pure JavaScript, matching epon-oamd.c byte-for-byte */
function md5Hex(msg) {
	function toBytes(str) {
		var b = [];
		for (var i = 0; i < str.length; i++)
			b.push(str.charCodeAt(i) & 0xff);
		return b;
	}

	var bytes = toBytes(msg);
	var len = bytes.length;
	var bitLen = len * 8;

	bytes.push(0x80);
	while ((bytes.length % 64) !== 56)
		bytes.push(0);

	for (var i = 0; i < 8; i++)
		bytes.push((bitLen / Math.pow(2, 8 * i)) & 0xff);

	var h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
	var k = [
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
		0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
		0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
		0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
		0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
	];
	var r = [ 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 ];

	function add(x, y) { return (x + y) & 0xffffffff; }
	function rotl(x, c) { return (x << c) | (x >>> (32 - c)); }

	for (var chunk = 0; chunk < bytes.length; chunk += 64) {
		var w = [];
		for (var j = 0; j < 16; j++) {
			var p = chunk + j * 4;
			w[j] = bytes[p] | (bytes[p + 1] << 8) | (bytes[p + 2] << 16) | (bytes[p + 3] << 24);
		}

		var a = h0, b = h1, c = h2, d = h3;
		for (var i = 0; i < 64; i++) {
			var f, g;
			if (i < 16) {
				f = (b & c) | ((~b) & d);
				g = i;
			} else if (i < 32) {
				f = (d & b) | ((~d) & c);
				g = (5 * i + 1) % 16;
			} else if (i < 48) {
				f = b ^ c ^ d;
				g = (3 * i + 5) % 16;
			} else {
				f = c ^ (b | (~d));
				g = (7 * i) % 16;
			}

			var temp = d;
			d = c;
			c = b;
			var sum = add(add(a, f), add(k[i], w[g]));
			var shift = r[Math.floor(i / 16) * 4 + (i % 4)];
			b = add(b, rotl(sum, shift));
			a = temp;
		}

		h0 = add(h0, a);
		h1 = add(h1, b);
		h2 = add(h2, c);
		h3 = add(h3, d);
	}

	function hex(val) {
		var s = '';
		for (var i = 0; i < 4; i++) {
			var b = (val >>> (i * 8)) & 0xff;
			s += (b < 16 ? '0' : '') + b.toString(16);
		}
		return s;
	}

	return hex(h0) + hex(h1) + hex(h2) + hex(h3);
}

function interauthKey(macHex12, secret, role) {
	if (!macHex12 || !secret)
		return '';
	return md5Hex(macHex12.toUpperCase() + secret + role);
}


/* Check a candidate secret against the key the OLT sent (leaf 0x2040). */
function calculator(oam, fallbackMac, onSet) {
	var mac = String(oam.onu_mac || fallbackMac || '').replace(/[^0-9a-fA-F]/g, '').toUpperCase();
	var input = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'style': 'width:100%' });
	var olt = E('code'), onu = E('code'), verdict = E('div');
	var use = E('button', { 'class': 'btn cbi-button', 'click': function(ev) {
		ev.preventDefault();
		onSet(input.value);
	} }, _('Use this secret'));

	function pill(cls, msg) {
		return E('span', { 'class': 'xpon-pill ' + cls }, msg);
	}

	function update() {
		var secret = input.value, want = String(oam.interauth_olt || '');

		olt.textContent = secret && mac ? md5Hex(mac + secret + 'Olt') : '-';
		onu.textContent = secret && mac ? md5Hex(mac + secret + 'Onu') : '-';
		use.disabled = !secret;
		verdict.textContent = '';
		if (!secret || !want)
			return;
		verdict.appendChild(olt.textContent == want ?
			pill('xpon-ok', _('Match: this secret produces the key the OLT sent.')) :
			pill('xpon-bad', _('No match: the OLT sent a different key.')));
	}

	input.addEventListener('input', update);
	update();
	return E('div', { 'class': 'cbi-section' }, [
		E('h3', _('Secret check')),
		E('div', { 'class': 'cbi-section-descr' },
			mac ? _('Key the OLT sent on 0x2040: %s. Type a candidate secret to see whether it produces it (MD5 of the ONU MAC %s, the secret and "Olt").')
				.format(oam.interauth_olt || _('none yet'), mac) :
			_('The ONU MAC is not known yet; start the EPON link first.')),
		E('div', { 'class': 'cbi-value' }, [ E('label', { 'class': 'cbi-value-title' }, _('Candidate secret')),
			E('div', { 'class': 'cbi-value-field' }, [ input ]) ]),
		E('div', { 'class': 'cbi-value' }, [ E('label', { 'class': 'cbi-value-title' }, _('"Olt" key')),
			E('div', { 'class': 'cbi-value-field' }, [ olt ]) ]),
		E('div', { 'class': 'cbi-value' }, [ E('label', { 'class': 'cbi-value-title' }, _('"Onu" key')),
			E('div', { 'class': 'cbi-value-field' }, [ onu ]) ]),
		verdict,
		E('div', { 'class': 'cbi-page-actions' }, [ use ])
	]);
}

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('epon'),
			L.resolveDefault(fs.read('/var/run/epon-oamd.json'), null),
			L.resolveDefault(fs.read('/sys/class/net/pon0/device/epon_state'), null)
		]);
	},

	render: function(data) {
		var m, s, o, secretField;
		var oam = {};

		try { oam = JSON.parse(data[1] || '{}'); } catch (e) { oam = {}; }
		var hwMac = (String(data[2] || '').match(/\bmac=(\S+)/) || [])[1];

		m = new form.Map('epon', _('EPON configuration'),
			_('Each setting shows when a saved change reaches the OLT.'));
		s = m.section(form.NamedSection, 'main', 'epon', _('Authentication'));
		o = s.option(form.Flag, 'enabled', _('Answer the OLT\'s OAM'),
			_('Without OAM the OLT registers the ONU but does not bring the service up.'));
		o.default = '1';
		o = s.option(form.Flag, 'active_discovery', _('Active OAM discovery'),
			_('Start 802.3ah discovery instead of waiting for the OLT. Leave off unless the status page shows the OAM link stuck waiting while MPCP is registered.'));
		o.default = '0';
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
		secretField = o;
		o.password = true;
		asciiField(o, 64);

		s = m.section(form.NamedSection, 'main', 'epon', _('Logging'));
		o = s.option(form.ListValue, 'debug', _('Log level'));
		o.value('0', _('0 (Normal: state changes and errors, also in the kernel log)'));
		o.value('1', _('1 (Debug: also unsupported or ignored OLT requests, key changes and VLAN updates)'));

		/* procd restarts epon-oamd when its command line changes. */
		m.children.forEach(function(section) {
			section.children.forEach(function(option) {
				widgets.annotate(option, 'drop');
			});
		});

		return m.render().then(function(node) {
			var descr = node.querySelector('.cbi-map-descr');

			widgets.style();
			node.insertBefore(widgets.legend([ 'drop' ]),
					  descr ? descr.nextSibling : node.firstChild);

			/* The check fills the normal field; saving goes through the usual Save & Apply. */
			node.appendChild(calculator(oam, hwMac, function(secret) {
				var field = node.querySelector('input[name="' + secretField.cbid('main') + '"]');

				if (!field)
					return;
				field.value = secret;
				field.dispatchEvent(new Event('input', { bubbles: true }));
				field.dispatchEvent(new Event('change', { bubbles: true }));
			}));
			return node;
		});
	}
});
