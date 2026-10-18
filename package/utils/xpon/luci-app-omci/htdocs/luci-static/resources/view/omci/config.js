'use strict';
'require view';
'require form';
'require uci';
'require fs';
'require ui';
'require dom';
'require omci.rpc as omci';
'require omci.mib as mib';
'require xpon.widgets as widgets';

/* Untagged, OLT-provisioned VLANs and the extra or full range. */
function scanCandidates(objects, extra, all) {
	var candidates = { '0:0': true }, vlans = mib.provisionedVlans(objects), v;

	if (all)
		for (v = 1; v <= 4094; v++)
			candidates[v + ':0'] = true;

	Object.keys(vlans).forEach(function(id) {
		candidates[id + ':0'] = true;
		vlans[id].priorities.forEach(function(prio) {
			candidates[id + ':' + prio] = true;
		});
	});

	String(extra || '').split(/[\s,]+/).forEach(function(id) {
		if (/^\d+$/.test(id) && +id >= 1 && +id <= 4094)
			candidates[+id + ':0'] = true;
	});

	return Object.keys(candidates).map(function(id) {
		var parts = id.split(':');
		return { vid: +parts[0], prio: +parts[1] };
	}).sort(function(a, b) { return a.vid - b.vid || a.prio - b.prio; });
}

function applyScanResult(result) {
	uci.set('omci', 'vlan', 'mode', result.vid ? 'tagged' : 'transparent');
	if (result.vid) {
		uci.set('omci', 'vlan', 'vid', String(result.vid));
		uci.set('omci', 'vlan', 'prio', String(result.prio || 0));
	}

	return uci.save().then(function() {
		return ui.changes.apply(true);
	});
}

function priorityText(objects, vid, answers, route) {
	var parts = [], olt = vid ? mib.oltPriority(objects, vid) || mib.mapperPriority(route) : '';

	if (olt)
		parts.push(olt);
	answers.forEach(function(r) {
		parts.push(_('%s probe PCP %d, reply PCP %d').format(
			r.type == 'dhcp' ? 'DHCP' : 'BRAS', r.prio, r.reply_prio));
	});

	return parts.length ? parts.join('; ') : '-';
}

function answerText(answers) {
	if (!answers.length)
		return _('No DHCPv4 or PPPoE reply. The service may use IPv6, a static address, VoIP, IPTV or management, or require a bound DHCP client.');

	return answers.map(function(r) {
		return (r.type == 'dhcp' ? _('DHCPv4: ') : _('PPPoE BRAS: ')) + r.detail;
	}).join('; ');
}

function scanResults(results, count, vlans, objects, scanned) {
	var vids = Object.keys(vlans).map(Number);

	results.forEach(function(r) {
		if (vids.indexOf(r.vid) < 0)
			vids.push(r.vid);
	});
	vids.sort(function(a, b) { return a - b; });

	if (!vids.length)
		return E('p', {}, scanned ?
			_('No DHCPv4 or PPPoE reply on %d candidates, and the OLT has not provisioned any VLAN.').format(count) :
			_('The OLT has not provisioned any VLAN yet.'));

	return E('table', { 'class': 'table' }, [
		E('tr', { 'class': 'tr table-titles' }, [
			_('VLAN'), _('OLT rule'), _('GEM route'), _('Priority'), _('Service evidence'), ''
		].map(function(t) { return E('th', { 'class': 'th left' }, t); }))
	].concat(vids.map(function(vid) {
		var answers = results.filter(function(r) { return r.vid == vid; });
		var entry = vlans[vid], route = entry && entry.route;

		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left' }, vid ? String(vid) : _('untagged')),
			E('td', { 'class': 'td left' }, entry ? entry.sources.join(', ') : _('Not found in OMCI')),
			E('td', { 'class': 'td left' }, entry ? mib.routeText(route) : '-'),
			E('td', { 'class': 'td left' }, priorityText(objects, vid, answers, route)),
			E('td', { 'class': 'td left' }, answers.length ?
				_('Internet candidate: %s').format(answerText(answers)) :
				scanned ? answerText(answers) : _('Provisioned; not probed yet')),
			E('td', { 'class': 'td right' }, answers.length ? E('button', {
				'class': 'cbi-button cbi-button-apply',
				'click': ui.createHandlerFn(this, applyScanResult, answers[0])
			}, vid ? _('Use tagged') : _('Use transparent')) : '')
		]);
	})));
}

function runScan(section_id) {
	var root = document.getElementById('omci-vlanscan-results');
	var extra = uci.get('omci', section_id, 'scan_vids');
	var all = uci.get('omci', section_id, 'scan_all') == '1';

	dom.content(root, E('em', { 'class': 'spinning' }, _('Scanning...')));

	return L.resolveDefault(omci.mibList(0), []).then(function(objects) {
		var candidates = scanCandidates(objects, extra, all);

		return fs.exec_direct('/usr/sbin/omci-vlanscan',
			[ '-j', '-w', '4' ].concat(candidates.map(function(c) {
				return c.vid + ':' + c.prio;
			})), 'json').then(function(results) {
			dom.content(root, scanResults(results || [], candidates.length,
						      mib.provisionedVlans(objects), objects, true));
		});
	}).catch(function(err) {
		dom.content(root, E('p', {}, _('VLAN scan failed: %s').format(err.message || err)));
	});
}

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
 * What saving costs. A new serial number or vendor ID ranges the ONU again
 * (gc1601_gpon_set_identity_locked); the password and every attribute the
 * OLT reads from the MIB count from the next registration; the rest the
 * agent and the switch pick up at once.
 */
var applyCost = {
	enabled: 'drop', serial: 'drop', vendor_id: 'drop',
	permissive: 'live', fake_omci: 'live', reboot: 'live',
	vlan_translate: 'live', vlan_stack: 'live', olt_profile: 'live', olt_profile_force: 'live',
	mode: 'live', vid: 'live', prio: 'live',
	password: 'next', loid: 'next', loid_password: 'next',
	hardware_version: 'next', software_version_0: 'next', software_version_1: 'next',
	equipment_id: 'next', omcc_version: 'next', traffic_management_option: 'next',
	dba_status_reporting: 'next', onu_type: 'next', uni_count: 'next'
};

return view.extend({
	load: function() {
		/* The MIB list is slow to read: the VLAN table fills in after rendering. */
		return Promise.all([ uci.load('omci'), [],
				     L.resolveDefault(uci.load('network'), null) ]);
	},

	render: function(data) {
		var m, s, o;

		m = new form.Map('omci', _('OMCI configuration'),
			_('Settings for the GC1601 OMCI agent. Each setting shows when a saved change reaches the OLT. Empty identity fields leave the current values unchanged.'));

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

		o = s.taboption('rewrite', form.DynamicList, 'vlan_translate', _('VLAN translation'));
		o.datatype = 'string';
		o.placeholder = '100:10';
		o.description = _('Pairs of <code>network VLAN:LAN VLAN</code>. The router uses the LAN VLAN, and the ONU replaces it with the network VLAN in both directions. Takes precedence over the VLAN rules sent by the OLT.');

		o = s.taboption('rewrite', form.DynamicList, 'vlan_stack', _('VLAN stacking (QinQ)'));
		o.datatype = 'string';
		o.placeholder = '200:30';
		o.description = _('Pairs of <code>S-VLAN:C-VLAN</code>. Upstream, the ONU adds the S-VLAN tag on top of the C-VLAN tag sent by the router; downstream, it removes it again.');

		o = s.taboption('agent', form.ListValue, 'olt_profile', _('OLT profile'));
		[ ['auto', _('Auto detect')], ['generic', _('Generic')], ['nokia-alcl', _('Nokia / Alcatel-Lucent')],
		  ['dasan', _('DASAN')], ['huawei', _('Huawei')], ['fiberhome', _('FiberHome')], ['zte', _('ZTE')],
		  ['unspecified', _('Unspecified')] ].forEach(function(v) { o.value(v[0], v[1]); });
		o.default = 'auto';

		o = s.taboption('agent', form.ListValue, 'olt_profile_force', _('Force OLT profile'));
		[ ['unspecified', _('Do not force')], ['generic', _('Generic')], ['nokia-alcl', _('Nokia / Alcatel-Lucent')],
		  ['dasan', _('DASAN')], ['huawei', _('Huawei')], ['fiberhome', _('FiberHome')], ['zte', _('ZTE')] ].forEach(function(v) { o.value(v[0], v[1]); });
		o.default = 'unspecified';

		o = s.taboption('identity', form.Value, 'serial', _('GPON serial number'));
		o.placeholder = 'TPLG12345678';
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
		o.placeholder = 'TPLG';
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
		o = s.taboption('capabilities', form.ListValue, 'omcc_version', _('OMCC version'));
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
		o = s.taboption('capabilities', form.ListValue, 'traffic_management_option', _('Traffic management option'));
		numericList(o, _('Keep current value'), [
			[ 0, _('0 (Priority controlled: upstream traffic uses the traffic schedulers and priority queues)') ],
			[ 1, _('1 (Rate controlled: each connection\'s maximum upstream rate is enforced by shaping)') ],
			[ 2, _('2 (Priority and rate controlled: queues and schedulers, plus per-connection shaping)') ]
		]);
		o.description = _('Upstream traffic management model advertised in ONU-G; the OLT adapts its provisioning to it. The default is 0. This does not enable hardware traffic shaping.');

		o = s.taboption('capabilities', form.Flag, 'dba_status_reporting', _('Report queue status for DBA'));
		o.default = o.enabled;
		o.rmempty = false;
		o.description = _('ANI-G SR indication. On tells the OLT that the ONU sends valid DBRu queue reports, as the GC1601 does; off makes it use traffic-monitoring DBA. The OLT reads it at its next MIB upload.');

		o = s.taboption('capabilities', form.ListValue, 'onu_type', _('ONU type'));
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

		o = s.taboption('capabilities', form.ListValue, 'uni_count', _('UNI count'));
		numericList(o, _('Keep current value'), Array.from({ length: 16 }, function(x, i) {
			return [ i + 1, String(i + 1) ];
		}));
		o.description = _('Number of Ethernet UNIs advertised to the OLT. Advertising extra UNIs does not add physical Ethernet ports; match the ONT whose identity is cloned.');

		s = m.section(form.NamedSection, 'vlan', 'vlan', _('LAN VLAN mode'),
			_('How the bridge carries the service VLAN between the OLT and the LAN port.'));
		s.addremove = false;
		s.tab('mode', _('LAN VLAN'));
		s.tab('scan', _('VLAN scan'));

		o = s.taboption('mode', form.ListValue, 'mode', _('Mode'));
		o.value('transparent', _('Transparent'));
		o.value('tagged', _('Tagged'));
		o.value('olt', _('Control by OLT'));
		o.default = 'transparent';
		o.rmempty = false;
		o.description = _('<em>Transparent</em> passes frames unchanged; the router tags the VLAN itself. <em>Tagged</em> adds the VLAN below towards the OLT and removes it towards the LAN, so the router sends untagged frames. <em>Control by OLT</em> does the same with the VLAN that the OLT assigns to untagged traffic. With switch offload, the VLAN is added in hardware and the priority is always 0.');

		o = s.taboption('mode', form.Value, 'vid', _('VLAN ID'));
		o.datatype = 'range(1,4094)';
		o.depends('mode', 'tagged');
		o.rmempty = false;

		/* IEEE 802.1Q traffic types */
		o = s.taboption('mode', form.ListValue, 'prio', _('VLAN priority'));
		[
			_('0 (Best effort, the default)'), _('1 (Background, lowest)'),
			_('2 (Excellent effort)'), _('3 (Critical applications)'),
			_('4 (Video)'), _('5 (Voice)'),
			_('6 (Internetwork control)'), _('7 (Network control, highest)')
		].forEach(function(name, value) {
			o.value(String(value), name);
		});
		o.default = '0';
		o.depends('mode', 'tagged');

		o = s.taboption('scan', form.Value, 'scan_vids', _('Extra VLAN IDs to scan'));
		o.placeholder = '100 200';
		o.description = _('The scan always tries untagged frames and every VLAN found in the OLT\'s rules, with their priorities.');

		o = s.taboption('scan', form.Flag, 'scan_all', _('Scan all VLAN IDs (1-4094)'));
		o.description = _('Sends more than 8000 broadcast probes. Only services provisioned for this ONU can reply.');

		o = s.taboption('scan', form.Button, '_scan', _('VLAN scan'));
		o.inputtitle = _('Start VLAN scan');
		o.inputstyle = 'action';
		o.description = _('Lists every VLAN provisioned by the OLT, including those that do not reply. The scan sends PPPoE PADI and DHCPv4 DISCOVER probes; a silent service may use IPv6, a static address or a fixed client binding. Save extra VLAN IDs before scanning. Switch offload and the LAN VLAN setting are paused during the scan, so untagged probes leave untagged.');
		o.onclick = function(ev, section_id) {
			return runScan(section_id);
		};

		o = s.taboption('scan', form.DummyValue, '_scan_results', ' ');
		o.rawhtml = true;
		o.cfgvalue = function() {
			return '<div id="omci-vlanscan-results"></div>';
		};

		m.children.forEach(function(section) {
			section.children.forEach(function(option) {
				if (applyCost[option.option])
					widgets.annotate(option, applyCost[option.option]);
			});
		});

		return m.render().then(function(node) {
			var bridged = [], descr = node.querySelector('.cbi-map-descr');

			widgets.style();
			node.insertBefore(widgets.legend([ 'live', 'next', 'drop' ]),
					  descr ? descr.nextSibling : node.firstChild);

			uci.sections('network', 'device', function(dev) {
				L.toArray(dev.ports).forEach(function(port) {
					if (/^pon0\.[0-9]+$/.test(port))
						bridged.push(port + ' (' + (dev.name || dev['.name']) + ')');
				});
			});
			if (bridged.length)
				node.insertBefore(E('div', { 'class': 'alert-message warning' }, [
					E('h4', {}, _('PON VLAN device used as bridge port')),
					E('p', {}, _('The bridge port %h tags traffic that the ONU already tags, so frames leave with two VLAN tags and the service stops working. Use <em>pon0</em> as bridge port and select the VLAN under <em>LAN VLAN mode</em> instead. This is corrected automatically within a few seconds.').format(bridged.join(', ')))
				]), node.firstChild);

			var root = node.querySelector('#omci-vlanscan-results');

			if (root) {
				dom.content(root, E('em', {}, _('Reading the VLANs provisioned by the OLT…')));
				L.resolveDefault(omci.mibList(0), []).then(function(objects) {
					dom.content(root, scanResults([], 0, mib.provisionedVlans(objects),
								      objects, false));
				});
			}
			return node;
		});
	}
});
