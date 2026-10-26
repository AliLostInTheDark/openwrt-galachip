// SPDX-License-Identifier: GPL-2.0-or-later
// The PON page: what everyone looks at first, why the ONU is not (or not steadily)
// at O5, its editable identity, the far end, the driver's own state by category,
// what the OLT configured, how the ISP serves the internet, and the bundle a new OLT is supported from.
// The same on every ONU family.
'use strict';
'require view';
'require form';
'require uci';
'require fs';
'require rpc';
'require poll';
'require ui';

var callStatus = rpc.declare({ object: 'gpon', method: 'status' });
var callDetail = rpc.declare({ object: 'gpon', method: 'detail' });
var callCapture = rpc.declare({ object: 'gpon', method: 'capture', params: [ 'name' ] });
var callWan = rpc.declare({ object: 'network.interface.wan', method: 'status' });
var callProv = rpc.declare({ object: 'gpon', method: 'provisioning' });
var callDiscover = rpc.declare({ object: 'gpon', method: 'wan_discover', params: [ 'seconds' ] });
var callDiscovered = rpc.declare({ object: 'gpon', method: 'wan_result' });
var callApply = rpc.declare({ object: 'gpon', method: 'apply', params: [ 'action', 'value' ] });
var callBundleMeta = rpc.declare({ object: 'gpon', method: 'bundle_meta',
	params: [ 'isp', 'olt_model', 'olt_version', 'consent' ] });

var CAPTURES = [
	[ 'gpon', _('Driver state (/proc/gpon)') ],
	[ 'gpon_prov', _('What the OLT provisioned (olt1 ledger)') ],
	[ 'gpon_unknown', _('OMCI this ONU did not identify (olt1, replayable)') ],
	[ 'ploam_unknown', _('PLOAM this ONU does not handle (olt1, replayable)') ],
	[ 'oltcap', _('OMCI exchange with the OLT (olt1 ring)') ],
	[ 'ploamcap', _('PLOAM exchange with the OLT (olt1 ring)') ],
	[ 'gpon_log', _('Driver event log') ],
	[ 'gpon_health', _('Health summary') ]
];

/* A /proc/gpon line goes to the first category whose key pattern matches; both
 * families' spellings are listed ("key: value" Luna, "key = value" Cortina). */
var CATEGORIES = [
	[ _('Activation (O-state)'), /^(onu_state|fsm|fiber|onu_id|eqd|min_delay|onu\(state|main\(eqd|reset|version|alarm|los_cfg_sts|cdr_recover|gtc_|gpon_ds\b)/ ],
	[ _('OLT and far end'), /^(omci_farend|omci_olt_vendor|omcc|omci_port)/ ],
	[ _('Identity'), /^(omci_id_|vendor-id|vendor-spec|serial-number|sn-source|sn$|us_onu_id)/ ],
	[ _('Optics'), /^(optic|optics|rgb8|cmu|gbox|bosa|laser|us_laser)/ ],
	[ _('PLOAM'), /^(ds_ploam|us_ploam|ds_cntr|ploam)/ ],
	[ _('OMCI'), /^(omci|ds_omci|vlan_service|data_owner|puc)/ ],
	[ _('Data path (GEM / T-CONT)'), /^(data|gem|gpon_us|gpon_ds_|gpon_fec|gpon_pucif|us_|boh_cfg|tcont)/ ]
];
var OTHER = _('Driver registers and other');

function categorise(text) {
	var out = {}, last = OTHER;
	(text || '').split('\n').forEach(function(line) {
		if (!line.trim())
			return;
		if (!/^\s/.test(line)) {
			var key = line.split(/[:=]/)[0].trim();
			last = OTHER;
			for (var i = 0; i < CATEGORIES.length; i++)
				if (CATEGORIES[i][1].test(key)) { last = CATEGORIES[i][0]; break; }
		}
		(out[last] = out[last] || []).push(line);
	});
	return out;
}

function num(re, text) {
	var m = (text || '').match(re);
	return m ? +m[1] : null;
}

/* The O5-and-no-WAN ladder, in order: driver, light, state, then what O5 still owes. */
function diagnose(st, text, wan) {
	var out = [], s = st.onu_state, rx = parseFloat(st.optic_rx_dbm);

	if (!st.driver_ready)
		out.push([ 'danger', _('The GPON driver is not ready') +
			(st.init_error ? ' (' + _('init error') + ' ' + st.init_error + ')' : '') ]);
	if (st.los === true)
		out.push([ 'danger', _('No light on the fiber (LOS): check the fiber, the connector and the splitter port.') ]);
	else if (!isNaN(rx) && rx < -28)
		out.push([ 'warning', _('Weak signal: RX %s dBm is below the -28 dBm class B+ sensitivity; clean the connectors and check the optical budget.').format(rx) ]);
	if (/ranging not started: no defined serial number/.test(text))
		out.push([ 'danger', _('Ranging has not started: this ONU has no serial number defined.') ]);

	if (s == 1 || s == 2)
		out.push([ 'warning', _('O%d: the ONU waits for the OLT to ask for its serial number. Check that the OLT port is enabled.').format(s) ]);
	else if (s == 3)
		out.push([ 'warning', _('O3: the OLT asks for serial numbers but has not ranged this one: its SN (and PLOAM password) is probably not authorised on the OLT.') ]);
	else if (s == 4)
		out.push([ 'warning', _('O4: ranging started but did not complete.') ]);
	else if (s == 6 || s == 7)
		out.push([ 'danger', _('O%d: the OLT stopped this ONU (POPUP / emergency stop).').format(s) ]);
	else if (s != 5 && st.los !== true)
		out.push([ 'warning', _('The driver reports no activation state.') ]);

	if (s == 5) {
		var rerange = num(/rerange=(\d+)/, text), outage = num(/last_outage=(\d+)ms/, text);
		var omci = num(/omci_queue: queued=(\d+)/, text);
		var fail = num(/omci_resp\s*[=:]?\s*\S+ tx=\d+ fail=(\d+)/, text);
		var luna_data = num(/data_owner: admitted=\d+ installed=(\d+)/, text);
		var cortina_data = (text.match(/^data\s*=\s*(\S+)/m) || [])[1];

		if (omci === null)
			omci = num(/ds_omci_rx\s*=\s*(\d+)/, text);
		if (rerange)
			out.push([ 'warning', _('Unstable: re-ranged %d times since boot (last outage %d ms).').format(rerange, outage || 0) ]);
		if (omci === 0)
			out.push([ 'danger', _('O5 but no OMCI from the OLT: the management channel (OMCC) was not set up.') ]);
		if (fail)
			out.push([ 'warning', _('%d OMCI responses could not be sent to the OLT.').format(fail) ]);
		if (luna_data === 0 || (cortina_data && cortina_data != 'INSTALLED'))
			out.push([ 'danger', _('O5 and OMCI, but no data path (GEM port / T-CONT) was provisioned: check the OLT service profile.') ]);
		if (wan && wan.up === false)
			out.push([ 'warning', _('O5 but the WAN is down: check the WAN VLAN and protocol (DHCP / PPPoE) against the OLT service.') ]);
	}
	if (!out.length)
		out.push([ 'success', _('Operating (O5) and the WAN is up.') ]);
	return out;
}

function wanText(wan) {
	if (!wan || wan.up === undefined)
		return '—';
	var a = (wan['ipv4-address'] || [])[0];
	return wan.up ? _('up') + (a ? ' (' + a.address + ')' : '') : _('down');
}

function table(rows) {
	return E('table', { 'class': 'table' }, rows.map(function(r) {
		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '33%' }, [ r[0] ]),
			E('td', { 'class': 'td left' }, [ r[1] ])
		]);
	}));
}

function summary(st, wan) {
	var state = 'O' + (st.onu_state || '?') +
		(st.onu_state_name && st.onu_state_name != 'unknown' ? ' – ' + st.onu_state_name : '');
	return table([
		[ _('ONU state'), E('span', { 'class': st.online ? 'label success' : 'label warning' }, [ state ]) ],
		[ _('RX power'), (st.optic_rx_dbm || 'n/a') + ' dBm' ],
		[ _('TX power'), (st.optic_tx_dbm || 'n/a') + ' dBm' ],
		[ _('Signal'), st.los === true ? _('LOS (no fiber light)') : st.los === false ? _('OK') : _('unknown') ],
		[ _('OLT mode'), (st.farend || '—') + (uci.get('gpon', 'identity', 'farend') ? ' (' + _('forced') + ')' : ' (' + _('detected') + ')') ],
		[ _('OLT vendor (ME 131)'), st.olt_vendor || '—' ],
		[ _('WAN'), wanText(wan) ]
	]);
}

function findings(list) {
	return E('ul', {}, list.map(function(f) {
		return E('li', {}, [ E('span', { 'class': 'label ' + f[0] }, [ f[1] ]) ]);
	}));
}

/* The "-> ...: gpon-wan-discover --apply-<action> <value>" lines of a discovery. -> [[action, value, why]] */
function suggestions(text) {
	var out = [];
	(text || '').split('\n').forEach(function(line) {
		var m = line.match(/^\s*->\s*(.+?):\s*gpon-wan-discover --apply-(routed|dhcp) (\S+)\s*$/);
		if (m)
			out.push([ m[2], m[3], m[1] ]);
	});
	return out;
}

function stamp(name, ext) {
	return name + '-' + new Date().toISOString().replace(/[:.]/g, '-') + ext;
}

function save(file, blob) {
	var a = E('a', { 'href': URL.createObjectURL(blob), 'download': file });
	document.body.appendChild(a);
	a.click();
	document.body.removeChild(a);
}

function download(name) {
	return L.resolveDefault(callCapture(name), {}).then(function(r) {
		if (!r.available)
			return ui.addNotification(null, E('p', _('/proc/%s is not available on this firmware.').format(name)), 'warning');
		save(stamp(name, '.txt'), new Blob([ r.text || '' ], { 'type': 'text/plain' }));
	});
}

function downloadBundle(isp, model, version, consent) {
	return callBundleMeta(isp.value, model.value, version.value, consent.value).then(function() {
		return fs.exec_direct('/usr/sbin/gpon-bundle', [ '--stdout' ], 'blob');
	}).then(function(blob) {
		save(stamp('gpon-bundle', '.tar'), blob);
	}).catch(function(e) {
		ui.addNotification(null, E('p', _('No bundle: %s').format(e.message)), 'danger');
	});
}

function showDiscovery(out, acts, btn) {
	return L.resolveDefault(callDiscovered(), {}).then(function(r) {
		if (r.running)
			return new Promise(function(next) { window.setTimeout(next, 3000); }).then(function() {
				return showDiscovery(out, acts, btn);
			});
		btn.disabled = false;
		out.textContent = r.text || _('No result.');
		acts.replaceChildren.apply(acts, suggestions(r.text).map(function(s) {
			return E('button', { 'class': 'cbi-button cbi-button-apply', 'click': function() {
				return callApply(s[0], s[1]).then(function(a) {
					ui.addNotification(null, E('pre', {}, [ a.output || '' ]), a.rc ? 'danger' : 'info');
				});
			} }, [ _('Apply: %s').format(s[2]) ]);
		}));
	});
}

function discoveryBox(atBoot) {
	var out = E('pre', { 'style': 'white-space:pre-wrap' }, [ _('Not run yet.') ]), acts = E('div', {});
	var btn = E('button', { 'class': 'cbi-button cbi-button-action', 'click': function() {
		btn.disabled = true;
		out.textContent = _('Listening on the fibre for about a minute: DHCP and PPPoE on each VLAN, and addresses routed to this ONU...');
		return callDiscover(60).then(function() { return showDiscovery(out, acts, btn); });
	} }, [ _('Detect the internet WAN (about 1 minute)') ]);
	return E('div', { 'class': 'cbi-section' }, [ E('h3', {}, [ _('Find the internet WAN') ]),
		E('div', { 'class': 'cbi-section-descr' }, [
			_('For a new ISP: what the fibre offers, per VLAN. Nothing changes until you press an Apply button.') ]),
		E('p', {}, [ E('strong', {}, [ _('At boot: ') ]), atBoot || _('nothing was needed (the WAN had a route) or automatic WAN is off.') ]),
		btn, out, acts ]);
}

return view.extend({
	diagnose: diagnose,
	categorise: categorise,
	suggestions: suggestions,

	load: function() {
		return Promise.all([
			uci.load('gpon'),
			L.resolveDefault(callStatus(), {}),
			L.resolveDefault(callDetail(), {}),
			L.resolveDefault(callWan(), {}),
			fs.exec('/usr/sbin/rtk_factory', [ '-p', 'config', 'sn' ]).catch(function() { return {}; }),
			L.resolveDefault(callProv(), {}),
			L.resolveDefault(callDiscovered(), {})
		]);
	},

	render: function(data) {
		var st = data[1] || {}, detail = data[2] || {}, wan = data[3] || {};
		var factorySn = ((data[4] || {}).stdout || '').trim();
		var m, s, o;

		var sumBox = E('div', {}, [ summary(st, wan) ]);
		var diagBox = E('div', {}, [ findings(diagnose(st, detail.gpon || '', wan)) ]);

		m = new form.Map('gpon', _('PON'),
			_('Fiber state and diagnosis, this unit\'s GPON identity and the far end it is adapted to. ' +
			  'Save &amp; Apply re-provisions live; the ONU re-ranges with the new identity.'));

		s = m.section(form.NamedSection, 'identity', 'gpon', _('Identity'));
		s.addremove = false;
		s.tab('ploam', _('PLOAM (ranging)'));
		s.tab('omci', _('OMCI (what the OLT reads)'));

		o = s.taboption('ploam', form.ListValue, 'source', _('Source'),
			_('Manual overrides field by field; an empty field keeps this unit\'s factory value.'));
		o.value('factory', _('Factory'));
		o.value('manual', _('Manual'));
		o.default = 'factory';
		o = s.taboption('ploam', form.Value, 'sn', _('GPON serial number'), _('e.g. XPON12345678'));
		o.depends('source', 'manual');
		o.placeholder = factorySn;
		o = s.taboption('ploam', form.Value, 'mac', _('MAC address'));
		o.datatype = 'macaddr';
		o.depends('source', 'manual');
		o = s.taboption('ploam', form.Value, 'ploam_passwd', _('PLOAM password'));
		o.password = true;
		o = s.taboption('ploam', form.Value, 'pon_vendor_id', _('PON vendor ID'), _('4 characters, e.g. XPON'));

		o = s.taboption('omci', form.ListValue, 'farend', _('OLT mode'),
			_('Automatic follows a cloned identity\'s vendor, else the OLT once it names itself in ME 131: that OLT\'s MEs at once, its OMCC version at the next MIB reset. Force a mode when the OLT needs it before naming itself, e.g. its OMCC version in the first MIB upload; it applies at the next MIB reset.'));
		o.value('', _('Automatic (now: %s)').format(st.farend || '—'));
		(st.farends || '').split(/\s+/).filter(Boolean).forEach(function(n) { o.value(n, n); });
		[ [ 'vendor_id', _('Vendor ID (ONU-G)') ], [ 'model', _('Equipment ID (model)') ],
		  [ 'hw_ver', _('Hardware version') ], [ 'sw_ver1', _('Software version, bank 0') ],
		  [ 'sw_ver2', _('Software version, bank 1') ], [ 'product_code', _('Product code') ],
		  [ 'omcc_version', _('OMCC version'), _('Empty = the OLT mode\'s own, e.g. 0x80 or 0xa2') ],
		  [ 'loid', _('LOID') ], [ 'loid_passwd', _('LOID password') ] ].forEach(function(f) {
			o = s.taboption('omci', form.Value, f[0], f[1], f[2]);
			if (/passwd/.test(f[0]))
				o.password = true;
		});

		s = m.section(form.NamedSection, 'wan_auto', 'wan_auto', _('Automatic WAN'),
			_('At boot, a WAN with no default route that nobody set by hand takes the one unambiguous answer ' +
			  'of the WAN detector. It never changes a WAN that works or that you set.'));
		o = s.option(form.Flag, 'enabled', _('Enabled'));
		o.default = '1';
		o.rmempty = false;

		var cats = categorise(detail.gpon), names = CATEGORIES.map(function(c) { return c[0]; }).concat([ OTHER ]);
		var detailBox = E('div', { 'class': 'cbi-section' }, [ E('h3', {}, [ _('Driver state by category') ]) ]
			.concat(names.filter(function(n) { return cats[n]; }).map(function(n) {
				return E('details', { 'open': n == OTHER ? null : '' }, [
					E('summary', {}, [ n + ' (' + cats[n].length + ')' ]),
					E('pre', { 'style': 'white-space:pre-wrap' }, [ cats[n].join('\n') ])
				]);
			})));

		var provBox = E('div', { 'class': 'cbi-section' }, [ E('h3', {}, [ _('What the OLT configured') ]),
			E('div', { 'class': 'cbi-section-descr' }, [
				_('Decoded from the OMCI the OLT sent: its identity, the management address, VLANs, the TR-069 server, ' +
				  'GEM ports, what this ONU does not model, and anything it refused.') ]),
			E('pre', { 'style': 'white-space:pre-wrap' }, [ (data[5] || {}).text || _('Not available on this firmware.') ]) ]);
		var unknownBox = E('div', { 'class': 'cbi-section' }, [ E('h3', {}, [ _('What this ONU could not identify') ]),
			E('div', { 'class': 'cbi-section-descr' }, [
				_('Every kind of message from the OLT this firmware did not answer, refused, or holds no model of -- ' +
				  'including what the OLT sends only now and then. The latest of each kind is kept for replay: ' +
				  'download it below and send it with the bundle.') ]),
			E('pre', { 'style': 'white-space:pre-wrap' }, [ (data[5] || {}).unknown || _('Not available on this firmware.') ]) ]);

		var isp = E('input', { 'class': 'cbi-input-text', 'placeholder': _('ISP name') });
		var oltModel = E('input', { 'class': 'cbi-input-text', 'placeholder': _('OLT model, if you know it') });
		var oltVersion = E('input', { 'class': 'cbi-input-text', 'placeholder': _('OLT firmware, if you know it') });
		var consent = E('select', { 'class': 'cbi-input-select' }, [
			E('option', { 'value': 'private' }, [ _('Keep it private') ]),
			E('option', { 'value': 'publish-anonymous' }, [ _('Publish the results, no name') ]),
			E('option', { 'value': 'publish-with-credit' }, [ _('Publish the results with credit') ]) ]);
		var dlBox = E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, [ _('Support for a new OLT') ]),
			E('div', { 'class': 'cbi-section-descr' }, [
				_('One file holds what is needed to make this firmware work with your OLT offline: send it to ' +
				  'contact@herman-brule.com or attach it to an issue at https://github.com/alphaonex86/openwrt/issues. ' +
				  'It holds this unit\'s serial number and MAC addresses; passwords are removed.') ]),
			isp, oltModel, oltVersion, consent,
			E('button', { 'class': 'cbi-button cbi-button-action', 'click': function() {
				return downloadBundle(isp, oltModel, oltVersion, consent);
			} }, [ _('Download the bundle') ]),
			E('div', { 'class': 'cbi-section-descr' }, [ _('Or one part at a time:') ])
		].concat(CAPTURES.map(function(c) {
			return E('button', { 'class': 'cbi-button', 'click': function() { return download(c[0]); } }, [ c[1] ]);
		})));

		return Promise.resolve(m.render()).then(function(form_node) {
			poll.add(function() {
				return Promise.all([ L.resolveDefault(callStatus(), {}), L.resolveDefault(callDetail(), {}),
					L.resolveDefault(callWan(), {}) ]).then(function(r) {
					sumBox.replaceChildren(summary(r[0] || {}, r[2] || {}));
					diagBox.replaceChildren(findings(diagnose(r[0] || {}, (r[1] || {}).gpon || '', r[2] || {})));
				});
			}, 5);
			return E([], [
				E('div', { 'class': 'cbi-section' }, [ E('h3', {}, [ _('Fiber (GPON)') ]), sumBox ]),
				E('div', { 'class': 'cbi-section' }, [ E('h3', {}, [ _('Why not O5, or not steadily') ]), diagBox ]),
				provBox, unknownBox, discoveryBox((data[6] || {}).auto), form_node, detailBox, dlBox
			]);
		});
	}
});
