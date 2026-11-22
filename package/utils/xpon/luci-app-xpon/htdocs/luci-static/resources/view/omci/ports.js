'use strict';
'require view';
'require form';
'require uci';
'require fs';
'require ui';
'require dom';
'require omci.rpc as omci';
'require omci.mib as mib';

var vlanMap;

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
	return vlanMap.save().then(function() {
		uci.set('omci', 'vlan', 'mode', result.vid ? 'tagged' : 'transparent');
		if (result.vid) {
			uci.set('omci', 'vlan', 'vid', String(result.vid));
			uci.set('omci', 'vlan', 'prio', String(result.prio || 0));
		}
		return uci.save();
	}).then(function() {
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
			/* A reply's text comes from whichever server answered the probe. */
			E('td', { 'class': 'td left' }, [ answers.length ?
				_('Internet candidate: %s').format(answerText(answers)) :
				scanned ? answerText(answers) : _('Provisioned; not probed yet') ]),
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
		dom.content(root, E('p', {}, [ _('VLAN scan failed: %s').format(err.message || err) ]));
	});
}

return view.extend({
	load: function() {
		return Promise.all([ uci.load('omci'), L.resolveDefault(uci.load('network'), null) ]);
	},

	render: function() {
		var m = new form.Map('network', _('LAN VLANs'),
			_('<em>Transparent</em>: the ONU passes the VLAN tag unchanged, so the router must tag this VLAN. <em>Tagged</em>: the ONU adds the VLAN towards the OLT and removes it towards the LAN, so the router uses no VLAN.'));
		var s, o;

		vlanMap = m;
		m.chain('omci');

		s = m.section(form.NamedSection, 'vlan', 'vlan', _('Service VLAN'));
		s.uciconfig = 'omci';
		s.addremove = false;
		o = s.option(form.ListValue, 'mode', _('Mode'));
		o.value('', _('Automatic'));
		o.value('routed', _('Routed (the PON is the WAN)'));
		o.value('transparent', _('Transparent (router tags)'));
		o.value('tagged', _('Tagged (router untagged)'));
		o.value('olt', _('Control by OLT'));
		o.description = _('<em>Automatic</em> is Routed when a network interface uses the PON, as on a home gateway, and Transparent otherwise. <em>Control by OLT</em> works like Tagged, using the VLAN that the OLT assigns to untagged traffic.');

		o = s.option(form.Value, 'vid', _('VLAN ID'));
		o.datatype = 'range(1,4094)';
		o.depends('mode', 'tagged');
		o.rmempty = false;

		/* IEEE 802.1Q traffic types */
		o = s.option(form.ListValue, 'prio', _('VLAN priority'));
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

		s = m.section(form.NamedSection, 'main', 'agent', _('VLAN scan'));
		s.uciconfig = 'omci';
		s.addremove = false;
		s.description = _('Results can set the service VLAN directly.');
		o = s.option(form.Value, 'scan_vids', _('Extra VLAN IDs to scan'));
		o.placeholder = '100 200';
		o.description = _('The scan always tries untagged frames and every VLAN found in the OLT\'s rules, with their priorities.');

		o = s.option(form.Flag, 'scan_all', _('Scan all VLAN IDs (1-4094)'));
		o.description = _('Sends more than 8000 broadcast probes. Only services provisioned for this ONU can reply.');

		o = s.option(form.Button, '_scan', _('Scan'));
		o.inputtitle = _('Start VLAN scan');
		o.inputstyle = 'action';
		o.description = _('Sends PPPoE and DHCPv4 probes and lists every VLAN the OLT provisioned, including those that do not reply. A silent VLAN may still be in use, for example with IPv6 or a static address. Save extra VLAN IDs first. LAN traffic may be interrupted during the scan.');
		o.onclick = function(ev, section_id) {
			return runScan('vlan');
		};

		o = s.option(form.DummyValue, '_scan_results', ' ');
		o.rawhtml = true;
		o.cfgvalue = function() {
			return '<div id="omci-vlanscan-results"></div>';
		};

		s.children.forEach(function(option) { option.ucisection = 'vlan'; });

		return m.render().then(function(node) {
			node.classList.add('omci-vlan-page');
			node.prepend(E('style', {}, `
.omci-vlan-page .cbi-value-field { min-width: 0; }
.omci-vlan-page .cbi-value-field > select { max-width: 100%; }
.omci-vlan-page .cbi-value-description { overflow-wrap: anywhere; }
@media (max-width: 600px) {
 .omci-vlan-page .cbi-value:not(.hidden) { display: block; }
 .omci-vlan-page .cbi-value label.cbi-value-title { display: block; float: none; width: auto; text-align: left; padding: 0 0 .5rem; }
 .omci-vlan-page .cbi-value > .cbi-value-field { width: 100%; margin: 0; padding: 0; }
 .omci-vlan-page .cbi-value > .cbi-value-field select,
 .omci-vlan-page .cbi-value > .cbi-value-field input[type="text"],
 .omci-vlan-page .cbi-value > .cbi-value-field .cbi-dropdown { width: 100%; min-width: 0; max-width: 100%; box-sizing: border-box; }
}
`));
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
					E('p', {}, _('The bridge port %h tags traffic that the ONU already tags, so frames leave with two VLAN tags and the service stops working. Use <em>pon0</em> as bridge port and configure its service VLAN on this page instead.').format(bridged.join(', ')))
				]), node.firstChild);

			var scanRoot = node.querySelector('#omci-vlanscan-results');

			if (scanRoot) {
				dom.content(scanRoot, E('em', {}, _('Reading the VLANs provisioned by the OLT…')));
				L.resolveDefault(omci.mibList(0), []).then(function(objects) {
					dom.content(scanRoot, scanResults([], 0, mib.provisionedVlans(objects),
								      objects, false));
				});
			}
			return node;
		});
	}
});
