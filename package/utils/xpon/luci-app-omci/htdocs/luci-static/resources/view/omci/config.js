'use strict';
'require view';
'require form';
'require uci';
'require fs';
'require ui';
'require dom';
'require omci.rpc as omci';

/* Untagged, OLT-provisioned VLANs and the extra or full range. */
function scanCandidates(objects, extra, all) {
	var candidates = { '0:0': true }, vlans = provisionedVlans(objects), v;

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

function hexBytes(hex) {
	var bytes = [];

	for (var i = 0; i + 1 < (hex || '').length; i += 2)
		bytes.push(parseInt(hex.substr(i, 2), 16));
	return bytes;
}

function be16(bytes, off) {
	return ((bytes[off] || 0) << 8) | (bytes[off + 1] || 0);
}

/*
 * The VLANs the OLT provisioned and what carries them: VLAN tagging filter
 * (ME 84) -> MAC bridge port (ME 47, same entity) -> 802.1p mapper (ME 130)
 * -> GEM interworking TP (ME 266) -> GEM port network CTP (ME 268) -> T-CONT
 * (ME 262). Extended VLAN rules (ME 171) may name an ingress or treatment
 * VLAN without a resolvable GEM route.
 */
function provisionedVlans(objects) {
	var me = {}, vlans = {};

	(objects || []).forEach(function(o) { me[o.class_id + ':' + o.entity_id] = o; });

	function route(bridgePort) {
		var port = me['47:' + bridgePort], mapper, b, p, gems = {}, allocs = {};

		if (!port)
			return null;
		b = hexBytes(port.data);
		if (b[3] != 3)		/* TP type: 802.1p mapper */
			return null;
		mapper = me['130:' + be16(b, 4)];
		if (!mapper)
			return null;
		b = hexBytes(mapper.data);

		for (p = 0; p < 8; p++) {
			var iwtp = me['266:' + be16(b, 2 + 2 * p)], ctp, cb, gem, tcont;

			ctp = iwtp && me['268:' + be16(hexBytes(iwtp.data), 0)];
			if (!ctp)
				continue;
			cb = hexBytes(ctp.data);
			gem = be16(cb, 0);
			tcont = me['262:' + be16(cb, 2)];
			(gems[gem] = gems[gem] || []).push(p);
			if (tcont)
				allocs[be16(hexBytes(tcont.data), 0)] = true;
		}

		return Object.keys(gems).length ? { gems: gems, allocs: Object.keys(allocs) } : null;
	}

	function remember(vid, source, port, prio) {
		var entry, mapped;

		if (!Number.isInteger(vid) || vid < 1 || vid > 4094)
			return;
		entry = vlans[vid] || (vlans[vid] = { route: null, sources: [], priorities: [] });
		if (entry.sources.indexOf(source) < 0)
			entry.sources.push(source);
		mapped = port != null ? route(port) : null;
		if (mapped)
			entry.route = mapped;
		if (prio >= 0 && prio <= 7 && entry.priorities.indexOf(prio) < 0)
			entry.priorities.push(prio);
	}

	(objects || []).forEach(function(o) {
		var b, n, i, entries;

		if (o.origin_name != 'olt')
			return;

		if (o.class_id == 84) {
			entries = (o.vlan_filter || {}).entries;
			if (entries && entries.length) {
				entries.forEach(function(e) {
					remember(+e.vid, _('VLAN filter (ME 84)'), o.entity_id, +e.pbit);
				});
			} else {
				b = hexBytes(o.data);
				n = Math.min(b[25] || 0, 12);
				for (i = 0; i < n; i++)
					remember(be16(b, 2 * i) & 0xfff, _('VLAN filter (ME 84)'),
						o.entity_id, be16(b, 2 * i) >> 13);
			}
		} else if (o.class_id == 171) {
			((o.extended_vlan || {}).rules || []).forEach(function(r) {
				if (r.delete || r.tags_to_remove == 3)
					return;
				remember(+r.filter_inner_vid, _('Extended VLAN filter (ME 171)'),
					o.extended_vlan.associated_me, +r.filter_inner_pbit);
				remember(+r.filter_outer_vid, _('Extended VLAN filter (ME 171)'),
					o.extended_vlan.associated_me, +r.filter_outer_pbit);
				remember(+r.treat_inner_vid, _('Extended VLAN treatment (ME 171)'),
					o.extended_vlan.associated_me, +r.treat_inner_pbit);
				remember(+r.treat_outer_vid, _('Extended VLAN treatment (ME 171)'),
					o.extended_vlan.associated_me, +r.treat_outer_pbit);
			});
		}
	});

	return vlans;
}

function routeText(route) {
	if (!route)
		return _('no GEM port found');

	return Object.keys(route.gems).map(function(gem) {
		var prios = route.gems[gem];

		return 'GEM %s (%s)'.format(gem, prios.length == 8 ? _('priority 0-7') :
			_('priority %s').format(prios.join(',')));
	}).join(', ') + (route.allocs.length ? ', Alloc-ID ' + route.allocs.join(',') : '');
}

/*
 * What the OLT's Extended VLAN rules do with this VLAN's priority. A rule
 * covers it when it filters single-tagged frames on this VID or on any VID
 * (4096), or tags untagged frames with it. Filter P-bits 8 is any, 14 the
 * default rule; treatment P-bits 0-7 set, 8 copy inner, 9 copy outer,
 * 10 from DSCP, 15 add no tag (the received tag is kept unless removed).
 */
function oltPriority(objects, vid) {
	var texts = [];

	(objects || []).forEach(function(o) {
		if (o.origin_name != 'olt' || o.class_id != 171)
			return;
		((o.extended_vlan || {}).rules || []).forEach(function(r) {
			var tagged = r.filter_inner_pbit != 15, accept, send;

			if (r.delete || r.tags_to_remove == 3 || r.filter_outer_pbit != 15)
				return;
			if (tagged ? r.filter_inner_vid != vid && r.filter_inner_vid != 4096 :
			    r.treat_inner_vid != vid)
				return;

			accept = !tagged ? _('untagged') :
				r.filter_inner_pbit <= 7 ? _('accepts priority %d only').format(r.filter_inner_pbit) : _('accepts priority 0-7');
			send = r.treat_inner_pbit <= 7 ? _('sets it to %d').format(r.treat_inner_pbit) :
				r.treat_inner_pbit == 8 ? _('keeps it') :
				r.treat_inner_pbit == 9 ? _('copies it from the outer tag') :
				r.treat_inner_pbit == 10 ? _('sets it from DSCP') :
				r.tags_to_remove ? _('removes the tag') : _('keeps it');
			texts.push(_('OLT: %s, %s').format(accept, send));
		});
	});

	return texts.filter(function(t, i) { return texts.indexOf(t) == i; }).join('; ');
}

/* Without a VLAN rule for it, the 802.1p mapper still tells which priorities the OLT carries. */
function mapperPriority(route) {
	var prios = [];

	Object.keys((route || {}).gems || {}).forEach(function(gem) {
		prios = prios.concat(route.gems[gem]);
	});
	if (!prios.length)
		return '';

	return _('OLT mapper: %s').format(prios.length == 8 ? '0-7' : prios.sort().join(','));
}

function priorityText(objects, vid, answers, route) {
	var parts = [], olt = vid ? oltPriority(objects, vid) || mapperPriority(route) : '';

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
			E('td', { 'class': 'td left' }, entry ? routeText(route) : '-'),
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
						      provisionedVlans(objects), objects, true));
		});
	}).catch(function(err) {
		dom.content(root, E('p', {}, _('VLAN scan failed: %s').format(err.message || err)));
	});
}

return view.extend({
	load: function() {
		/* The MIB list is slow to read: the VLAN table fills in after rendering. */
		return Promise.all([ uci.load('omci'), [],
				     L.resolveDefault(uci.load('network'), null) ]);
	},

	render: function(data) {
		var m, s, o;

		m = new form.Map('omci', _('OMCI configuration'),
			_('Settings for the OMCI agent in the kernel. Changes take effect as soon as they are saved. Leave an identity field empty to keep the value provided by the device.'));

		s = m.section(form.NamedSection, 'main', 'agent', _('Agent'));
		s.anonymous = true;
		s.addremove = false;

		o = s.option(form.Value, 'device', _('OMCI device'));
		o.datatype = 'uinteger';
		o.default = '0';

		/* omci.agent applies only the options present in UCI, and a flag
		 * left at its default would be removed: write them all. */
		o = s.option(form.Flag, 'enabled', _('Enable OMCI agent'));
		o.default = o.enabled;
		o.rmempty = false;

		o = s.option(form.Flag, 'permissive', _('Permissive mode'));
		o.default = o.enabled;
		o.rmempty = false;
		o.description = _('Accept OLT behavior that departs from ITU-T G.988 where a known workaround exists.');

		o = s.option(form.Flag, 'fake_omci', _('Fake unsupported OMCI responses'));
		o.default = o.disabled;
		o.rmempty = false;

		o = s.option(form.Flag, 'reboot', _('Allow OLT-requested reboot'));
		o.default = o.enabled;
		o.rmempty = false;

		o = s.option(form.DynamicList, 'vlan_translate', _('VLAN translation'));
		o.datatype = 'string';
		o.placeholder = '100:10';
		o.description = _('Pairs of <code>network VLAN:LAN VLAN</code>. The router uses the LAN VLAN, and the ONU replaces it with the network VLAN in both directions. Takes precedence over the VLAN rules sent by the OLT.');

		o = s.option(form.DynamicList, 'vlan_stack', _('VLAN stacking (QinQ)'));
		o.datatype = 'string';
		o.placeholder = '200:30';
		o.description = _('Pairs of <code>S-VLAN:C-VLAN</code>. Upstream, the ONU adds the S-VLAN tag on top of the C-VLAN tag sent by the router; downstream, it removes it again.');

		o = s.option(form.ListValue, 'olt_profile', _('OLT profile'));
		[ ['auto', _('Auto detect')], ['generic', _('Generic')], ['nokia-alcl', _('Nokia / Alcatel-Lucent')],
		  ['dasan', _('DASAN')], ['huawei', _('Huawei')], ['fiberhome', _('FiberHome')], ['zte', _('ZTE')],
		  ['unspecified', _('Unspecified')] ].forEach(function(v) { o.value(v[0], v[1]); });
		o.default = 'auto';

		o = s.option(form.ListValue, 'olt_profile_force', _('Force OLT profile'));
		[ ['unspecified', _('Do not force')], ['generic', _('Generic')], ['nokia-alcl', _('Nokia / Alcatel-Lucent')],
		  ['dasan', _('DASAN')], ['huawei', _('Huawei')], ['fiberhome', _('FiberHome')], ['zte', _('ZTE')] ].forEach(function(v) { o.value(v[0], v[1]); });
		o.default = 'unspecified';

		s = m.section(form.NamedSection, 'main', 'agent', _('ONU identity'));
		s.anonymous = true;
		s.addremove = false;

		o = s.option(form.Value, 'serial', _('GPON serial number'));
		o.datatype = 'maxlength(32)';
		o.placeholder = 'MSTC12345678';
		o.description = _('Serial number reported to the OLT: four vendor characters followed by eight hexadecimal digits.');

		o = s.option(form.Value, 'vendor_id', _('Vendor ID'));
		o.datatype = 'maxlength(16)';
		o.placeholder = 'MSTC';

		o = s.option(form.Value, 'hardware_version', _('Hardware version'));
		o.datatype = 'maxlength(14)';

		o = s.option(form.Value, 'software_version_0', _('Software image 0 version'));
		o.datatype = 'maxlength(14)';

		o = s.option(form.Value, 'software_version_1', _('Software image 1 version'));
		o.datatype = 'maxlength(14)';

		o = s.option(form.Value, 'equipment_id', _('Equipment ID'));
		o.datatype = 'maxlength(20)';

		o = s.option(form.Value, 'password', _('SLID / GPON password'));
		o.password = true;
		o.datatype = 'maxlength(32)';

		o = s.option(form.Value, 'loid', _('LOID'));
		o.datatype = 'maxlength(24)';
		o.description = _('Logical ONU ID, for OLTs that authenticate by LOID.');

		o = s.option(form.Value, 'loid_password', _('LOID password'));
		o.password = true;
		o.datatype = 'maxlength(12)';

		o = s.option(form.Value, 'omcc_version', _('OMCC version'));
		o.placeholder = '0xA1';
		o.description = _('Decimal or hexadecimal value, for example <code>0xA1</code>. Leave empty to keep the current value.');

		o = s.option(form.Value, 'traffic_management_option', _('Traffic management option'));
		o.datatype = 'range(0,255)';

		o = s.option(form.ListValue, 'onu_type', _('ONU type'));
		o.value('', _('Keep kernel value'));
		[ 'Other', 'SFU', 'HGU', 'MDU', 'SBU', 'MTU', 'CBU' ].forEach(function(name, value) {
			o.value(String(value), name);
		});

		o = s.option(form.Value, 'uni_count', _('UNI count'));
		o.datatype = 'range(0,255)';

		s = m.section(form.NamedSection, 'vlan', 'vlan', _('LAN VLAN mode'),
			_('How the bridge carries the service VLAN between the OLT and the LAN port.'));
		s.addremove = false;

		o = s.option(form.ListValue, 'mode', _('Mode'));
		o.value('transparent', _('Transparent'));
		o.value('tagged', _('Tagged'));
		o.value('olt', _('Control by OLT'));
		o.default = 'transparent';
		o.rmempty = false;
		o.description = _('<em>Transparent</em> passes frames unchanged; the router tags the VLAN itself. <em>Tagged</em> adds the VLAN below towards the OLT and removes it towards the LAN, so the router sends untagged frames. <em>Control by OLT</em> does the same with the VLAN that the OLT assigns to untagged traffic. With switch offload, the VLAN is added in hardware and the priority is always 0.');

		o = s.option(form.Value, 'vid', _('VLAN ID'));
		o.datatype = 'range(1,4094)';
		o.depends('mode', 'tagged');
		o.rmempty = false;

		o = s.option(form.ListValue, 'prio', _('VLAN priority'));
		for (var i = 0; i <= 7; i++)
			o.value(String(i));
		o.default = '0';
		o.depends('mode', 'tagged');

		o = s.option(form.Value, 'scan_vids', _('Extra VLAN IDs to scan'));
		o.placeholder = '100 200';
		o.description = _('The scan always tries untagged frames and every VLAN found in the OLT\'s rules, with their priorities.');

		o = s.option(form.Flag, 'scan_all', _('Scan all VLAN IDs (1-4094)'));
		o.description = _('Sends more than 8000 broadcast probes. Only services provisioned for this ONU can reply.');

		o = s.option(form.Button, '_scan', _('VLAN scan'));
		o.inputtitle = _('Start VLAN scan');
		o.inputstyle = 'action';
		o.description = _('Lists every VLAN provisioned by the OLT, including those that do not reply. The scan sends PPPoE PADI and DHCPv4 DISCOVER probes; a silent service may use IPv6, a static address or a fixed client binding. Save extra VLAN IDs before scanning. Switch offload and the LAN VLAN setting are paused during the scan, so untagged probes leave untagged.');
		o.onclick = function(ev, section_id) {
			return runScan(section_id);
		};

		o = s.option(form.DummyValue, '_scan_results', ' ');
		o.rawhtml = true;
		o.cfgvalue = function() {
			return '<div id="omci-vlanscan-results"></div>';
		};

		return m.render().then(function(node) {
			var bridged = [];

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
					dom.content(root, scanResults([], 0, provisionedVlans(objects),
								      objects, false));
				});
			}
			return node;
		});
	}
});
