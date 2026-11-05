'use strict';
'require view';
'require form';
'require fs';
'require uci';
'require ui';
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

/* Parse key=value pairs from epon_state */
function parseState(raw) {
	var state = {};

	String(raw || '').split(/\s+/).forEach(function(word) {
		var i = word.indexOf('=');
		if (i > 0)
			state[word.substring(0, i)] = word.substring(i + 1);
	});
	return state;
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

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('epon'),
			L.resolveDefault(fs.read('/var/run/epon-oamd.json'), null),
			L.resolveDefault(fs.read('/sys/class/net/pon0/device/epon_state'), null)
		]);
	},

	render: function(data) {
		var m, s, o;
		var oamJson = {};
		try { oamJson = JSON.parse(data[1] || '{}'); } catch (e) { oamJson = {}; }
		var hwState = parseState(data[2]);

		var ponMac = oamJson.onu_mac || hwState.mac || '';
		var macHex12 = ponMac.replace(/[^0-9a-fA-F]/g, '').toUpperCase();
		var liveOltKey = oamJson.interauth_olt || '';
		var liveOnuKey = oamJson.interauth_onu || '';

		m = new form.Map('epon', _('EPON configuration'),
			_('Configuration for the IEEE 802.3ah / China Telecom (CTC) EPON OAM daemon (epon-oamd). Each setting indicates when a saved change takes effect on the optical link. Empty identity fields preserve the built-in device defaults.'));

		s = m.section(form.NamedSection, 'main', 'epon', _('OAM & LOID authentication'));
		o = s.option(form.Flag, 'enabled', _('Enable EPON OAM daemon'),
			_('Respond to IEEE 802.3ah Clause 57 OAM discovery and China Telecom (CTC) extended OAM provisioning on pon0. Without OAM, the hardware completes MPCP registration (LLID 0) but the OLT will not enable subscriber traffic.'));
		o.default = '1';

		o = s.option(form.Flag, 'active_discovery', _('Active 802.3ah OAM discovery'),
			_('Initiate IEEE 802.3ah Clause 57 OAM discovery from the ONU instead of waiting for the OLT\'s first Information OAMPDU. Leave disabled unless the EPON Status page shows MPCP registered while the OAM link remains stuck waiting for the OLT.'));
		o.default = '0';

		o = s.option(form.Value, 'loid', _('LOID (Logical ONU ID)'),
			_('Subscriber Logical ONU ID sent in CTC extended opcode 0x03 (Auth_Response, auth type 0x01). Up to 24 printable ASCII characters.'));
		asciiField(o, 24);

		o = s.option(form.Value, 'loid_password', _('LOID password'),
			_('Password paired with the LOID in CTC extended opcode 0x03. Leave empty if the OLT authenticates by LOID alone. Up to 12 printable ASCII characters.'));
		o.password = true;
		asciiField(o, 12);

		s = m.section(form.NamedSection, 'main', 'epon', _('Subscriber LAN VLAN'),
			_('Controls how subscriber Ethernet frames are tagged between the LAN bridge and the PON interface. By default, the ONU applies the CTC Port VLAN rules (branch 0xC7, leaf 0x0021) provisioned by the OLT.'));
		o = s.option(form.ListValue, 'vlan_mode', _('VLAN operating mode'));
		o.value('olt', _('Provisioned by the OLT — apply the CTC Port VLAN mode (transparent, tag, 1:1 translation, N:1 aggregation, or trunk) sent by the OLT'));
		o.value('transparent', _('Transparent override — pass all Ethernet frames unchanged so the downstream router handles VLAN tagging'));
		o.value('tag', _('Tagged override — tag untagged LAN frames with the VLAN ID below upstream and strip the tag downstream'));
		o.default = 'olt';

		o = s.option(form.Value, 'vlan_id', _('Override VLAN ID'),
			_('IEEE 802.1Q VLAN ID (1–4094) applied when VLAN operating mode is set to Tagged override.'));
		o.datatype = 'range(1,4094)';
		o.depends('vlan_mode', 'tag');

		s = m.section(form.NamedSection, 'main', 'epon', _('Advertised ONU identity (CTC ONU SN)'),
			_('Reported to the OLT in CTC Extended Attribute leaf 0x0001 (ONU SN). Some OLTs restrict provisioning to known equipment vendors or firmware strings. Empty fields report the built-in defaults.'));
		o = s.option(form.Value, 'vendor_id', _('Vendor ID'),
			_('4-character ASCII vendor code (bytes 0–3 of CTC ONU SN). Default: <code>TPLG</code>.'));
		asciiField(o, 4);

		o = s.option(form.Value, 'model', _('Model'),
			_('4-character ASCII equipment model code (bytes 4–7 of CTC ONU SN; bytes 8–13 carry the board\'s 6-byte PON MAC address). Default: <code>XZG7</code>.'));
		asciiField(o, 4);

		o = s.option(form.Value, 'hardware_version', _('Hardware version'),
			_('Hardware revision string reported in bytes 14–21 of CTC ONU SN. Up to 8 printable ASCII characters. Default: <code>V1.0</code>.'));
		asciiField(o, 8);

		o = s.option(form.Value, 'software_version', _('Software version'),
			_('Software revision string reported in bytes 22–37 of CTC ONU SN. Up to 16 printable ASCII characters. Default: <code>OpenWrt</code>.'));
		asciiField(o, 16);

		o = s.option(form.Value, 'extended_model', _('Extended model'),
			_('Extended equipment model string appended in bytes 38–53 of CTC ONU SN on CTC 3.0 (0x30) links. Up to 16 printable ASCII characters. Default: <code>XZ000-G7</code>.'));
		asciiField(o, 16);

		s = m.section(form.NamedSection, 'main', 'epon', _('Cloned OAM & chipset identity'),
			_('For OLTs that fingerprint the ONU beyond the CTC serial number during discovery. Copy these hexadecimal values from an OAM packet capture or status dump of the ISP ONT being replaced. Leave empty to report this ONU\'s native Galachip GC1601 identity.'));
		o = s.option(form.Value, 'oam_oui', _('802.3ah Information TLV OUI'),
			_('3-byte IEEE OUI advertised in the Local Information OAMPDU TLV. 6 hexadecimal digits without separators (for example, <code>b48618</code> or <code>0011f5</code>).'));
		hexField(o, 6);

		o = s.option(form.Value, 'oam_vendor_info', _('802.3ah vendor-specific info'),
			_('4-byte vendor-specific field in the Local Information OAMPDU TLV. 8 hexadecimal digits.'));
		hexField(o, 8);

		o = s.option(form.Value, 'chipset_id', _('CTC chipset ID (leaf 0x0003)'),
			_('8-byte PON chipset identifier reported in CTC Extended Attribute leaf 0x0003 (2-byte vendor ID, 2-byte chip model, 1-byte revision, 3-byte manufacturing date). 16 hexadecimal digits; empty reports <code>474316010f140614</code> (Galachip GC1601).'));
		hexField(o, 16);

		s = m.section(form.NamedSection, 'main', 'epon', _('VSOL / Excitel OLT inter-authentication (leaf 0x2040)'),
			_('VSOL EPON OLTs (such as the V1600D series deployed by Excitel) perform a mutual challenge-response on CTC extended attribute 0xC7 / 0x2040 before provisioning subscriber VLANs. Both sides derive a 32-character hexadecimal MD5 hash from the ONU PON MAC (12 uppercase hex characters), the shared secret, and the role ("Olt" or "Onu"). Extract the secret from the replaced ONT\'s /bin/eponoamd binary or use the built-in verifier below to test candidate secrets against the OLT\'s challenge key.'));
		o = s.option(form.Value, 'interauth_secret', _('Shared secret'),
			_('ASCII shared secret (up to 64 characters). Empty disables inter-authentication responses.'));
		o.password = true;
		asciiField(o, 64);

		/* Built-in Two-Sided Calculator & Verifier Widget */
		o = s.option(form.DummyValue, '_interauth_calc', _('Inter-authentication calculator & verifier'));
		o.rawhtml = true;
		o.cfgvalue = function() {
			var statePill = '';
			switch (oamJson.interauth) {
			case 'ok':
				statePill = '<span class="xpon-pill xpon-ok">' + _('Accepted by OLT') + '</span>';
				break;
			case 'asked':
				statePill = '<span class="xpon-pill xpon-warn">' + _('Requested by OLT — secret required') + '</span>';
				break;
			case 'mismatch':
				statePill = '<span class="xpon-pill xpon-bad">' + _('Key mismatch') + '</span>';
				break;
			case 'not-asked':
				statePill = '<span class="xpon-pill">' + _('Armed, not requested by OLT') + '</span>';
				break;
			default:
				statePill = '<span class="xpon-pill">' + _('Off (not requested)') + '</span>';
				break;
			}

			return E('div', { 'class': 'xpon-grid', 'style': 'margin-top:12px' }, [
				/* Left Card: Read-Only OLT Side */
				E('div', { 'class': 'xpon-panel' }, [
					E('h4', {}, _('Live OLT Challenge (Read-Only)')),
					E('table', { 'class': 'table', 'style': 'margin:0' }, [
						E('tr', { 'class': 'tr' }, [
							E('td', { 'class': 'td left', 'style': 'width:40%' }, _('Board PON MAC:')),
							E('td', { 'class': 'td left' }, [
								E('strong', {}, ponMac || '-'),
								macHex12 ? E('span', { 'class': 'cbi-value-helpicon', 'style': 'margin-left:6px;font-family:monospace' }, ' [' + macHex12 + ']') : ''
							])
						]),
						E('tr', { 'class': 'tr' }, [
							E('td', { 'class': 'td left' }, _('Challenge status:')),
							E('td', { 'class': 'td left' }, [ statePill ? E('span', { 'innerHTML': statePill }) : '-' ])
						]),
						E('tr', { 'class': 'tr' }, [
							E('td', { 'class': 'td left' }, _('OLT key received (0x2040):')),
							E('td', { 'class': 'td left' }, [
								liveOltKey ? E('code', { 'style': 'font-size:12px;word-break:break-all' }, liveOltKey) :
								             E('em', { 'style': 'color:var(--text-3, #888)' }, _('Waiting for OLT Set_Request on 0x2040…'))
							])
						]),
						E('tr', { 'class': 'tr' }, [
							E('td', { 'class': 'td left' }, _('ONU response key served:')),
							E('td', { 'class': 'td left' }, [
								liveOnuKey ? E('code', { 'style': 'font-size:12px;word-break:break-all' }, liveOnuKey) :
								             E('em', { 'style': 'color:var(--text-3, #888)' }, _('None (secret not set or not queried)'))
							])
						])
					])
				]),

				/* Right Card: Editable ONU Calculator & Verifier Side */
				E('div', { 'class': 'xpon-panel' }, [
					E('h4', {}, _('ONU Key Calculator & Verifier')),
					E('div', { 'style': 'display:flex;flex-direction:column;gap:10px' }, [
						E('div', {}, [
							E('label', { 'style': 'display:block;font-size:12px;font-weight:600;margin-bottom:4px' }, _('Candidate shared secret:')),
							E('input', {
								'type': 'text',
								'id': 'vsol-calc-secret',
								'class': 'cbi-input-text',
								'style': 'width:100%;font-family:monospace',
								'placeholder': _('Enter secret to calculate or test…'),
								'value': uci.get('epon', 'main', 'interauth_secret') || ''
							})
						]),
						E('div', {}, [
							E('label', { 'style': 'display:block;font-size:12px;font-weight:600;margin-bottom:4px' }, _('Optional target key override (pasted from ISP ONT capture):')),
							E('input', {
								'type': 'text',
								'id': 'vsol-calc-target',
								'class': 'cbi-input-text',
								'style': 'width:100%;font-family:monospace',
								'placeholder': liveOltKey || _('32-character hexadecimal key…')
							})
						]),
						E('div', { 'style': 'display:grid;grid-template-columns:1fr;gap:6px;background:var(--surface-2, #f6f7f9);padding:10px;border-radius:8px' }, [
							E('div', {}, [
								E('span', { 'style': 'font-size:11px;font-weight:600;color:var(--text-2, #555)' }, _('Calculated OLT Key (Role "Olt"):')),
								E('div', { 'id': 'vsol-calc-olt-result', 'style': 'font-family:monospace;font-size:12px;word-break:break-all;color:var(--text, #111)' }, '-')
							]),
							E('div', {}, [
								E('span', { 'style': 'font-size:11px;font-weight:600;color:var(--text-2, #555)' }, _('Calculated ONU Key (Role "Onu"):')),
								E('div', { 'id': 'vsol-calc-onu-result', 'style': 'font-family:monospace;font-size:12px;word-break:break-all;color:var(--text, #111)' }, '-')
							])
						]),
						E('div', { 'id': 'vsol-calc-badge', 'style': 'min-height:24px' }),
						E('div', { 'style': 'display:flex;gap:8px;flex-wrap:wrap;margin-top:4px' }, [
							E('button', {
								'type': 'button',
								'class': 'btn cbi-button cbi-button-action',
								'id': 'vsol-btn-check'
							}, _('Check Key')),
							E('button', {
								'type': 'button',
								'class': 'btn cbi-button cbi-button-save',
								'id': 'vsol-btn-apply'
							}, _('Save & Apply (Re-register)'))
						])
					])
				])
			]);
		};

		s = m.section(form.NamedSection, 'main', 'epon', _('Diagnostics & logging'));
		o = s.option(form.ListValue, 'debug', _('OAM log verbosity'));
		o.value('0', _('0 — Normal (link transitions, authentication results, and errors in system and kernel logs)'));
		o.value('1', _('1 — Verbose (also log unsupported OLT attributes, churning key rotations, and VLAN updates)'));
		o.default = '0';

		/* Annotate all options: procd restarts epon-oamd on configuration changes */
		m.children.forEach(function(section) {
			section.children.forEach(function(option) {
				if (option.option !== '_interauth_calc')
					widgets.annotate(option, 'drop');
			});
		});

		return m.render().then(function(node) {
			var descr = node.querySelector('.cbi-map-descr');

			widgets.style();
			node.insertBefore(widgets.legend([ 'drop' ]),
					  descr ? descr.nextSibling : node.firstChild);

			/* Wire up interactive calculator events */
			var secretInput = node.querySelector('#vsol-calc-secret');
			var targetInput = node.querySelector('#vsol-calc-target');
			var calcOltEl = node.querySelector('#vsol-calc-olt-result');
			var calcOnuEl = node.querySelector('#vsol-calc-onu-result');
			var badgeEl = node.querySelector('#vsol-calc-badge');
			var btnCheck = node.querySelector('#vsol-btn-check');
			var btnApply = node.querySelector('#vsol-btn-apply');
			var formSecretInput = node.querySelector('input[name="cbid.epon.main.interauth_secret"]');

			function updateCalc() {
				var sec = secretInput ? secretInput.value.trim() : '';
				if (formSecretInput && secretInput && document.activeElement === secretInput)
					formSecretInput.value = sec;

				if (!sec || !macHex12) {
					if (calcOltEl) calcOltEl.textContent = '-';
					if (calcOnuEl) calcOnuEl.textContent = '-';
					if (badgeEl) badgeEl.innerHTML = '';
					return;
				}

				var oltHash = interauthKey(macHex12, sec, 'Olt');
				var onuHash = interauthKey(macHex12, sec, 'Onu');
				if (calcOltEl) calcOltEl.textContent = oltHash;
				if (calcOnuEl) calcOnuEl.textContent = onuHash;

				var compareTarget = (targetInput && targetInput.value.trim()) || liveOltKey;
				if (compareTarget && badgeEl) {
					var tgt = compareTarget.toLowerCase();
					if (oltHash === tgt) {
						badgeEl.innerHTML = '<span class="xpon-pill xpon-ok">' +
							_('Match — candidate secret generates the exact OLT challenge key on 0x2040.') + '</span>';
					} else if (onuHash === tgt) {
						badgeEl.innerHTML = '<span class="xpon-pill xpon-ok">' +
							_('Match — candidate secret generates the exact ONU response key on 0x2040.') + '</span>';
					} else {
						badgeEl.innerHTML = '<span class="xpon-pill xpon-bad">' +
							_('Mismatch — calculated OLT key does not match target key.') + '</span>';
					}
				} else if (badgeEl) {
					badgeEl.innerHTML = '<span class="xpon-pill">' +
						_('Keys calculated from board PON MAC [%s]').format(macHex12) + '</span>';
				}
			}

			if (secretInput) {
				secretInput.addEventListener('input', updateCalc);
			}
			if (targetInput) {
				targetInput.addEventListener('input', updateCalc);
			}
			if (formSecretInput && secretInput) {
				formSecretInput.addEventListener('input', function() {
					secretInput.value = formSecretInput.value;
					updateCalc();
				});
			}
			if (btnCheck) {
				btnCheck.addEventListener('click', function(ev) {
					ev.preventDefault();
					updateCalc();
				});
			}
			if (btnApply) {
				btnApply.addEventListener('click', function(ev) {
					ev.preventDefault();
					var sec = secretInput ? secretInput.value.trim() : '';
					if (formSecretInput)
						formSecretInput.value = sec;
					uci.set('epon', 'main', 'interauth_secret', sec);
					ui.showIndicator('saving', _('Saving and applying EPON inter-authentication…'));
					return uci.save().then(function() {
						return ui.changes.apply(true);
					}).then(function() {
						ui.hideIndicator('saving');
						ui.addNotification(null, E('p', _('Shared secret saved and epon-oamd restarted for re-registration.')), 'info');
					}).catch(function(e) {
						ui.hideIndicator('saving');
						ui.addNotification(null, E('p', _('Failed to apply configuration: %s').format(e.message || e)), 'error');
					});
				});
			}

			/* Initial run if secret or target already present */
			updateCalc();

			return node;
		});
	}
});
