'use strict';
'require view';
'require form';
'require uci';
'require fs';
'require ui';
'require dom';
'require omci.rpc as omci';
'require omci.mib as mib';

var perPort = false, vlanMap;

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
	if (perPort)
		return;

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
			E('td', { 'class': 'td left' }, answers.length ?
				_('Internet candidate: %s').format(answerText(answers)) :
				scanned ? answerText(answers) : _('Provisioned; not probed yet')),
			E('td', { 'class': 'td right' }, answers.length ? E('button', {
				'class': 'cbi-button cbi-button-apply',
				'data-vlan-apply': '',
				'disabled': perPort,
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

function portSpec(ports, name) {
	return (ports || []).find(function(p) { return p.split(':')[0] == name; });
}

return view.extend({
	load: function() {
		return Promise.all([ uci.load('omci'), L.resolveDefault(uci.load('network'), null),
			L.resolveDefault(omci.vlanStatus(), {}) ]);
	},

	render: function(data) {
		var status = data[2], ports = status.port_tagging_ports || [];
		var bridge = uci.sections('network', 'device').find(function(s) { return s.name == 'br-lan'; });
		var supported = !!(bridge && status.port_vlan && ports.length);
		var reserved = status.reserved_vids || [];
		var m = new form.Map('network', _('LAN VLANs'),
			_('Choose one VLAN policy here. The service VLAN policy uses the global mode; the per-port table controls VLAN membership and tagging for each LAN port.'));
		var s, o, root;

		vlanMap = m;
		m.chain('omci');
		perPort = supported && uci.get('network', bridge['.name'], 'vlan_filtering') == '1';

		function policySection(section, advanced) {
			var render = section.render, parse = section.parse;
			section.render = function() {
				return Promise.resolve(render.apply(this, arguments)).then(function(node) {
					node.setAttribute('data-vlan-policy', advanced ? 'ports' : 'global');
					node.hidden = perPort != advanced;
					return node;
				});
			};
			section.parse = function() {
				return perPort == advanced ? parse.apply(this, arguments) : Promise.resolve();
			};
			return section;
		}

		if (supported) {
			s = m.section(form.NamedSection, bridge['.name'], 'device', _('VLAN policy'));
			s.uciconfig = 'network';
			s.addremove = false;
			o = s.option(form.ListValue, 'vlan_filtering', _('Control mode'));
			o.value('0', _('Service VLAN (simple)'));
			o.value('1', _('Per-port VLAN table'));
			o.default = '0';
			o.rmempty = false;
			o.forcewrite = true;
			o.cfgvalue = function() { return perPort ? '1' : '0'; };
			o.description = _('Only the selected policy applies. Include your current LAN port and the ONU in the management VLAN before applying per-port control.');
			o.onchange = function(ev, section_id, value) {
				perPort = value == '1';
				root.querySelectorAll('[data-vlan-policy]').forEach(function(node) {
					node.hidden = perPort != (node.getAttribute('data-vlan-policy') == 'ports');
				});
				root.querySelectorAll('[data-vlan-apply]').forEach(function(button) { button.disabled = perPort; });
			};
			ports.forEach(function(port) {
				o = s.option(form.ListValue, 'omci_pcp_' + port, _('%s upstream priority').format(port.toUpperCase()));
				o.value('-1', _('Preserve packet priority'));
				for (var p = 0; p < 8; p++)
					o.value(String(p), String(p));
				o.default = '-1';
				o.rmempty = false;
				o.retain = true;
				o.depends('vlan_filtering', '1');
				o.description = _('Optionally replace the 802.1p priority sent towards the OLT. Downstream priority is preserved.');
			});
		}

		s = policySection(m.section(form.NamedSection, 'vlan', 'vlan', _('Service VLAN')), false);
		s.uciconfig = 'omci';
		s.addremove = false;
		o = s.option(form.ListValue, 'mode', _('Mode'));
		o.value('transparent', _('Transparent'));
		o.value('tagged', _('Tagged'));
		o.value('olt', _('Control by OLT'));
		o.default = 'transparent';
		o.rmempty = false;
		o.description = _('<em>Transparent</em> passes frames unchanged; the router tags the VLAN itself. <em>Tagged</em> adds the VLAN below towards the OLT and removes it towards the LAN, so the router sends untagged frames. <em>Control by OLT</em> does the same with the VLAN that the OLT assigns to untagged traffic. The service VLAN is inserted in hardware when switch offload is active.');

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

		ports.forEach(function(port) {
			o = s.option(form.ListValue, port + '_tagged', _('%s service VLAN').format(port.toUpperCase()));
			o.value('0', _('Follow LAN VLAN mode'));
			o.value('1', _('Keep service VLAN tagged'));
			o.depends('mode', 'tagged');
			o.depends('mode', 'olt');
			o.retain = true;
			o.default = '0';
			o.rmempty = false;
			o.description = _('Choose independently for each port. Keep tagged retains the VLAN that the LAN VLAN mode or OLT otherwise removes; the connected router must tag that VLAN. Other tagged services pass unchanged. This requires one native service VLAN.');
		});

		if (supported) {
			if (uci.get('network', 'lan', '.type') == 'interface') {
				s = policySection(m.section(form.NamedSection, 'lan', 'interface', _('ONU management')), true);
				s.uciconfig = 'network';
				o = s.option(form.Value, 'device', _('Management device'));
				o.rmempty = false;
				o.placeholder = 'br-lan.1';
				o.forcewrite = true;
				o.description = _('Management VLAN with ONU (CPU) membership, for example br-lan.1. Optional untagged access below works independently of each port’s service VLAN.');
				o.cfgvalue = function() {
					var device = uci.get('network', 'lan', 'device') == 'br-onu'
						? (uci.get('network', 'omci_management', 'ports') || [])[0]
						: uci.get('network', 'lan', 'device');
					return device == 'br-lan' ? 'br-lan.1' : device;
				};
				o.write = function(id, value) {
					if (uci.get('network', 'lan', 'device') == 'br-onu')
						uci.set('network', 'omci_management', 'ports', [ value ]);
					else
						uci.set('network', 'lan', 'device', value);
				};
				o.validate = function(id, value) {
					var match = /^br-lan\.([1-9][0-9]{0,3})$/.exec(value);
					return match && Number(match[1]) <= 4094 &&
						reserved.indexOf(Number(match[1])) < 0 ? true : _('Use br-lan.VID with ONU (CPU) membership (1–4094). The service bridge br-lan cannot be a port of br-onu.');
				};
				o = s.option(form.MultiValue, 'omci_management_ports', _('Untagged management access'));
				o.optional = true;
				o.rmempty = true;
				ports.forEach(function(port) { o.value(port, port.toUpperCase()); });
				o.description = _('Selected ports accept IPv4 traffic to the ONU’s management address without a VLAN tag, even when their native VLAN carries Internet or another service. Service VLANs remain isolated. Uses a Linux bridge and traffic classifiers; ordinary service traffic stays on its existing offload path.');
				o.validate = function(id, value) {
					var existing = uci.get('network', 'omci_management');
					var collision = uci.sections('network', 'device').some(function(dev) {
						return dev.name == 'br-onu' && dev['.name'] != 'omci_management';
					});
					return !L.toArray(value).length || (!collision && (!existing ||
						(existing['.type'] == 'device' && existing.name == 'br-onu' && existing.type == 'bridge')))
						? true : _('The network section omci_management is already used by another device.');
				};
				o.write = function(id, value) {
					var selected = L.toArray(value);
					var lower = uci.get('network', 'lan', 'device');
					if (selected.length) {
						if (lower == 'br-lan')
							lower = 'br-lan.1';
						if (lower != 'br-onu') {
							uci.add('network', 'device', 'omci_management');
							uci.set('network', 'omci_management', 'type', 'bridge');
							uci.set('network', 'omci_management', 'name', 'br-onu');
							uci.set('network', 'omci_management', 'ports', [ lower ]);
							if (bridge.macaddr)
								uci.set('network', 'omci_management', 'macaddr', bridge.macaddr);
							uci.set('network', 'lan', 'device', 'br-onu');
						}
						uci.set('network', id, this.option, selected);
					} else {
						if (lower == 'br-onu') {
							uci.set('network', 'lan', 'device',
								(uci.get('network', 'omci_management', 'ports') || [])[0]);
							uci.remove('network', 'omci_management');
						}
						uci.unset('network', id, this.option);
					}
				};
				o.remove = function(id) { return this.write(id, []); };
			}

			s = policySection(m.section(form.GridSection, 'bridge-vlan', _('VLAN membership')), true);
			s.uciconfig = 'network';
			s.description = _('ONU (CPU) membership enables local access on that VLAN. Untagged management access above can also provide local access independently of the native service VLAN.');
			s.anonymous = true;
			s.addremove = true;
			s.sortable = true;
			s.filter = function(id) { return uci.get('network', id, 'device') == 'br-lan'; };
			s.handleAdd = function(ev, name) {
				var id = this.map.data.add('network', 'bridge-vlan', name);
				uci.set('network', id, 'device', 'br-lan');
				uci.set('network', id, 'ports', [ 'pon0:t' ]);
				this.map.addedSection = id;
				return this.renderMoreOptionsModal(id, ev);
			};

			o = s.option(form.Value, 'vlan', _('VLAN ID'));
			o.datatype = 'range(1,4094)';
			o.editable = true;
			o.rmempty = false;
			o.validate = function(id, value) {
				var vid = Number(value);
				if (reserved.indexOf(vid) >= 0)
					return _('VLAN %d is reserved by this device.').format(vid);
				var duplicate = uci.sections('network', 'bridge-vlan').some(function(row) {
					return row['.name'] != id && row.device == 'br-lan' &&
						Number(this.formvalue(row['.name'])) == vid;
				}, this);
				return duplicate ? _('Each VLAN may appear only once.') : true;
			};

			o = s.option(form.Flag, 'local', _('ONU (CPU)'));
			o.editable = true;
			o.default = '1';
			o.rmempty = false;

			ports.concat([ 'pon0' ]).forEach(function(port) {
				var option = s.option(form.ListValue, '_port_' + port,
					port == 'pon0' ? _('PON') : port.toUpperCase());
				option.value('-', _('Not a member'));
				option.value('t', _('Tagged'));
				if (port != 'pon0') {
					option.value('u', _('Untagged'));
					option.value('t*', _('Tagged, native'));
					option.value('u*', _('Untagged, native'));
				}
				option.rmempty = false;
				option.editable = true;
				option.cfgvalue = function(id) {
					var spec = portSpec(uci.get('network', id, 'ports'), port);
					return spec ? (spec.split(':')[1] || 'u*') : '-';
				};
				option.write = function(id, value) {
					var list = (uci.get('network', id, 'ports') || []).filter(function(p) {
						return p.split(':')[0] != port;
					});
					if (value != '-')
						list.push(port + ':' + value);
					uci.set('network', id, 'ports', list);
				};
				option.validate = function(id, value) {
					if (value.indexOf('*') < 0)
						return true;
					var duplicate = uci.sections('network', 'bridge-vlan').some(function(row) {
						return row['.name'] != id && row.device == 'br-lan' &&
							(this.formvalue(row['.name']) || '').indexOf('*') >= 0;
					}, this);
					return duplicate ? _('Each LAN port may have only one native VLAN.') : true;
				};
			});

		}

		s = m.section(form.NamedSection, 'main', 'agent', _('VLAN scan'));
		s.uciconfig = 'omci';
		s.addremove = false;
		s.description = _('Scan results can set the simple service VLAN policy. With per-port control, configure the discovered VLAN in the membership table.');
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
			return runScan('vlan');
		};

		o = s.option(form.DummyValue, '_scan_results', ' ');
		o.rawhtml = true;
		o.cfgvalue = function() {
			return '<div id="omci-vlanscan-results"></div>';
		};

		s.children.forEach(function(option) { option.ucisection = 'vlan'; });

		return m.render().then(function(node) {
			root = node;
			node.classList.add('omci-vlan-page');
			node.prepend(E('style', {}, `
.omci-vlan-page .cbi-section-table { table-layout: auto; min-width: 62rem; width: 100%; }
.omci-vlan-page .cbi-section-table { display: table; }
.omci-vlan-page .cbi-section-table thead { display: table-header-group; }
.omci-vlan-page .cbi-section-table tbody { display: table-row-group; }
.omci-vlan-page .cbi-section-table tr { display: table-row; }
.omci-vlan-page .cbi-section-table td,
.omci-vlan-page .cbi-section-table th { display: table-cell; vertical-align: middle; }
.omci-vlan-page .cbi-section-table td::before { display: none; }
.omci-vlan-page .cbi-section-table .cbi-section-actions { white-space: nowrap; }
.omci-vlan-page .cbi-section-table .cbi-section-actions > div { display: flex; flex-wrap: nowrap; align-items: center; justify-content: flex-end; gap: .375rem; }
.omci-vlan-page .cbi-section-table .cbi-section-actions button { flex: 0 0 auto; width: auto; min-width: max-content; max-width: none; white-space: nowrap; overflow-wrap: normal; }
.omci-vlan-page .cbi-section-table tbody td { padding-top: .5rem; padding-bottom: .5rem; }
.omci-vlan-page .cbi-section-table th:nth-child(2),
.omci-vlan-page .cbi-section-table td:nth-child(2) { text-align: center; }
.omci-vlan-page .cbi-section-table select { text-overflow: clip; }
.omci-vlan-page .cbi-section-table .cbi-input-select,
.omci-vlan-page .cbi-section-table select { min-width: 11rem; width: 100%; max-width: none; }
.omci-vlan-page .cbi-section-table input[type="text"] { min-width: 5rem; width: 6rem; }
.omci-vlan-page .cbi-section-table .cbi-value-description { white-space: normal; overflow-wrap: anywhere; }
.omci-vlan-page .cbi-value-field { min-width: 0; }
.omci-vlan-page .cbi-value-field > select { max-width: 100%; }
.omci-vlan-page .cbi-value-description { overflow-wrap: anywhere; }
.omci-vlan-scroll { width: 100%; max-width: 100%; overflow-x: auto; padding-bottom: .75rem; }
@media (max-width: 600px) {
 .omci-vlan-page .cbi-value { display: block; }
 .omci-vlan-page .cbi-value label.cbi-value-title { display: block; float: none; width: auto; text-align: left; padding: 0 0 .5rem; }
 .omci-vlan-page .cbi-value > .cbi-value-field { width: 100%; margin: 0; padding: 0; }
 .omci-vlan-page .cbi-value > .cbi-value-field select,
 .omci-vlan-page .cbi-value > .cbi-value-field input[type="text"],
 .omci-vlan-page .cbi-value > .cbi-value-field .cbi-dropdown { width: 100%; min-width: 0; max-width: 100%; box-sizing: border-box; }
}
`));
			node.querySelectorAll('.cbi-section-table').forEach(function(table) {
				var scroll = E('div', { 'class': 'omci-vlan-scroll', 'tabindex': '0',
					'role': 'region', 'aria-label': _('VLAN membership') });
				table.parentNode.insertBefore(scroll, table);
				scroll.appendChild(table);
			});
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
