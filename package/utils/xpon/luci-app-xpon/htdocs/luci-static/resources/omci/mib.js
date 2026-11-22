'use strict';
'require baseclass';

/* What the OLT provisioned, read from the MIB list of the xpon object. */

function hexBytes(hex) {
	var bytes = [];

	for (var i = 0; i + 1 < (hex || '').length; i += 2)
		bytes.push(parseInt(hex.substr(i, 2), 16));
	return bytes;
}

/* Created by the OLT, or created by the ONU and then configured by the OLT. */
function oltProvisioned(o) {
	return o.origin_name == 'olt' || !!o.olt_written;
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

		if (!oltProvisioned(o))
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
		if (!oltProvisioned(o) || o.class_id != 171)
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

/* Alloc-IDs of the T-CONTs the OLT bound; 0x00FF and 0xFFFF mean none (G.988 9.2.2). */
function allocIds(objects) {
	return (objects || []).filter(function(o) {
		return o.class_id == 262;
	}).map(function(o) {
		return be16(hexBytes(o.data), 0);
	}).filter(function(id) {
		return id != 0xff && id != 0xffff;
	}).sort(function(a, b) { return a - b; });
}

/* GEM port network CTPs the OLT provisioned. */
function gemPorts(objects) {
	return (objects || []).filter(function(o) {
		return o.class_id == 268 && oltProvisioned(o);
	}).map(function(o) {
		return be16(hexBytes(o.data), 0);
	}).sort(function(a, b) { return a - b; });
}

function gemDirection(dir) {
	switch (dir) {
	case 1: return _('Upstream');
	case 2: return _('Downstream');
	case 3: return _('Bidirectional');
	default: return dir != null ? String(dir) : '-';
	}
}

/*
 * Full resolved service paths: each OLT-provisioned GEM Port (ME 268) joined
 * with its T-CONT (ME 262), IWTP (ME 266), 802.1p mapper (ME 130), bridge
 * port (ME 47), VLAN filter (ME 84), and Extended VLAN rules (ME 171).
 */
function dataPaths(objects) {
	var me = {}, iwtpByCtp = {}, mapperByIwtp = {}, bridgeByMapper = {}, paths = [];
	var uniLabels = [];

	(objects || []).forEach(function(o) { me[o.class_id + ':' + o.entity_id] = o; });

	function extRulesText(rules) {
		var list = [];

		(rules || []).forEach(function(r) {
			var src, dst;

			if (r.delete || r.tags_to_remove == 3)
				return;
			src = r.filter_inner_vid != null && r.filter_inner_vid != 4096 ?
				_('VID %d').format(r.filter_inner_vid) :
				(r.filter_inner_pbit == 15 ? _('untagged') : _('any tag'));
			dst = r.treat_inner_vid != null && r.treat_inner_vid != 4096 ?
				_('-> VID %d').format(r.treat_inner_vid) :
				(r.tags_to_remove ? _('-> untag') : _('-> keep'));
			if (r.treat_inner_pbit >= 0 && r.treat_inner_pbit <= 7)
				dst += _(' (p%d)').format(r.treat_inner_pbit);
			list.push(src + ' ' + dst);
		});
		return list.filter(function(v, i) { return list.indexOf(v) == i; });
	}

	(objects || []).forEach(function(o) {
		if (!oltProvisioned(o))
			return;
		if (o.class_id == 266) {
			var ctpId = be16(hexBytes(o.data), 0);
			(iwtpByCtp[ctpId] = iwtpByCtp[ctpId] || []).push(o);
		} else if (o.class_id == 130) {
			var b130 = hexBytes(o.data);
			for (var p = 0; p < 8; p++) {
				var iwtpId = be16(b130, 2 + 2 * p);
				if (iwtpId && iwtpId != 0xffff)
					(mapperByIwtp[iwtpId] = mapperByIwtp[iwtpId] || []).push({ mapper: o.entity_id, pbit: p });
			}
		} else if (o.class_id == 47) {
			var b47 = hexBytes(o.data);
			if (b47[3] == 3) {
				var mId = be16(b47, 4);
				(bridgeByMapper[mId] = bridgeByMapper[mId] || []).push(o);
			}
		} else if (o.class_id == 171) {
			var ext = o.extended_vlan || {};
			var rules = extRulesText(ext.rules);
			var target = ext.association_type == 10 ? 'VEIP 0x%04X'.format(ext.associated_me || o.entity_id) :
				ext.association_type == 2 ? 'PPTP UNI 0x%04X'.format(ext.associated_me || o.entity_id) :
				'ME 0x%04X'.format(ext.associated_me || o.entity_id);
			uniLabels.push(rules.length ? '%s: %s'.format(target, rules.join(', ')) :
				'%s (%s)'.format(target, _('pass-through / 0 OLT rules')));
		}
	});

	(objects || []).filter(function(o) {
		return o.class_id == 268 && oltProvisioned(o);
	}).sort(function(a, b) {
		return be16(hexBytes(a.data), 0) - be16(hexBytes(b.data), 0);
	}).forEach(function(o) {
		var b = hexBytes(o.data);
		var gem = be16(b, 0), tcontId = be16(b, 2), dir = b[4];
		var tcont = me['262:' + tcontId];
		var alloc = tcont ? be16(hexBytes(tcont.data), 0) : null;
		var iwtps = iwtpByCtp[o.entity_id] || [];
		var prios = [], mappers = {}, filterVids = [];

		iwtps.forEach(function(iwtp) {
			(mapperByIwtp[iwtp.entity_id] || []).forEach(function(hit) {
				mappers[hit.mapper] = true;
				if (prios.indexOf(hit.pbit) < 0)
					prios.push(hit.pbit);
			});
		});

		Object.keys(mappers).forEach(function(mId) {
			(bridgeByMapper[mId] || []).forEach(function(bp) {
				var vf = me['84:' + bp.entity_id];
				if (!vf)
					return;
				var entries = (vf.vlan_filter || {}).entries;
				if (entries && entries.length) {
					entries.forEach(function(e) {
						if (e.vid >= 1 && e.vid <= 4094 && filterVids.indexOf(e.vid) < 0)
							filterVids.push(e.vid);
					});
				} else {
					var fb = hexBytes(vf.data), fn = Math.min(fb[25] || 0, 12);
					for (var i = 0; i < fn; i++) {
						var vid = be16(fb, 2 * i) & 0xfff;
						if (vid >= 1 && vid <= 4094 && filterVids.indexOf(vid) < 0)
							filterVids.push(vid);
					}
				}
			});
		});

		prios.sort(function(a, b) { return a - b; });
		filterVids.sort(function(a, b) { return a - b; });

		paths.push({
			gem: gem,
			dir: dir,
			direction: gemDirection(dir),
			alloc: alloc != null && alloc != 0xff && alloc != 0xffff ? alloc : null,
			tcontEntity: tcontId,
			mappers: Object.keys(mappers).map(Number).sort(function(a, b) { return a - b; }),
			prios: prios,
			filterVids: filterVids,
			uniRules: uniLabels
		});
	});

	return paths;
}

function linkQos(objects) {
	var anig = null, galPayload = null, schedByTcont = {}, tconts = [];

	(objects || []).forEach(function(o) {
		var f = o.fields || {};

		if (o.class_id == 263 && !anig) {
			anig = {
				sr_indication: !!f.sr_indication,
				tconts: f.tconts,
				gem_block_length: f.gem_block_length,
				sf_threshold: f.sf_threshold,
				sd_threshold: f.sd_threshold,
				response_time: f.response_time
			};
		} else if (o.class_id == 272 && f.max_gem_payload != null) {
			if (galPayload == null || oltProvisioned(o))
				galPayload = f.max_gem_payload;
		} else if (o.class_id == 278 && f.tcont) {
			schedByTcont[parseInt(f.tcont, 16)] = f.policy || '-';
		}
	});

	(objects || []).forEach(function(o) {
		if (o.class_id != 262)
			return;
		var alloc = be16(hexBytes(o.data), 0);
		if (alloc == 0xff || alloc == 0xffff)
			return;
		tconts.push({
			entity: '0x%04X'.format(o.entity_id),
			alloc_id: alloc,
			policy: schedByTcont[o.entity_id] || (o.fields || {}).policy || '-'
		});
	});
	tconts.sort(function(a, b) { return a.alloc_id - b.alloc_id; });

	return {
		anig: anig,
		max_gem_payload: galPayload,
		tconts: tconts
	};
}

function uniPorts(objects) {
	var list = [];

	(objects || []).forEach(function(o) {
		var f = o.fields || {}, details = [];

		if (o.class_id == 11) {
			if (f.max_frame_size)
				details.push(_('Max frame %d B').format(f.max_frame_size));
			if (f.pppoe_filter)
				details.push(_('PPPoE filter on'));
			if (f.bridged_or_ip != null)
				details.push(f.bridged_or_ip == 0 ? _('Bridged') :
					f.bridged_or_ip == 1 ? _('IP router') : _('Depends on parent'));
			list.push({
				kind: 'Ethernet UNI',
				entity_id: o.entity_id,
				entity: '0x%04X'.format(o.entity_id),
				type: f.sensed_type || '-',
				admin: f.admin_state || '-',
				oper: f.oper_state || '-',
				olt_written: !!o.olt_written,
				details: details.join(', ') || '-'
			});
		} else if (o.class_id == 329) {
			if (f.interdomain_name)
				details.push(f.interdomain_name);
			list.push({
				kind: 'VEIP',
				entity_id: o.entity_id,
				entity: '0x%04X'.format(o.entity_id),
				type: _('Virtual Ethernet'),
				admin: f.admin_state || '-',
				oper: f.oper_state || '-',
				olt_written: !!o.olt_written,
				details: details.join(', ') || '-'
			});
		}
	});

	list.sort(function(a, b) {
		return (a.kind != b.kind ? (a.kind < b.kind ? -1 : 1) : a.entity_id - b.entity_id);
	});
	return list;
}

/* NUL-terminated text out of an attribute's bytes. */
function cstring(bytes, off, len) {
	var out = '';

	for (var i = off; i < off + len && bytes[i]; i++)
		out += bytes[i] >= 32 && bytes[i] < 127 ? String.fromCharCode(bytes[i]) : '?';
	return out;
}

/* ME 157 holds up to 15 parts of 25 bytes after a part count. */
function largeString(me, id) {
	var o = me['157:' + id], b, parts, bytes = [];

	if (!o)
		return null;
	b = hexBytes(o.data);
	parts = Math.min(b[0] || 0, 15);
	for (var i = 0; i < parts; i++)
		bytes = bytes.concat(b.slice(1 + 25 * i, 26 + 25 * i));
	return cstring(bytes, 0, bytes.length);
}

/*
 * The ACS the OLT configured: ME 340 points at a network address (ME 137),
 * which points at the URL (ME 157) and the login (ME 148, username 1 and 2,
 * password, realm). The decoded fields redact the login, the raw bytes do not.
 */
function acsTarget(me, o) {
	var b = hexBytes(o.data), net = me['137:' + be16(b, 1)], nb, auth, ab, url, out = {};

	if (!net)
		return out;
	nb = hexBytes(net.data);
	if (be16(nb, 2) != 0xffff)
		url = largeString(me, be16(nb, 2));
	if (url)
		out.acs_url = url;
	auth = me['148:' + be16(nb, 0)];
	if (be16(nb, 0) != 0xffff && auth) {
		ab = hexBytes(auth.data);
		out.acs_user = cstring(ab, 1, 25) + cstring(ab, 76, 14);
		out.acs_password = cstring(ab, 26, 25);
		out.acs_realm = cstring(ab, 51, 25);
	}
	return out;
}

function oltServices(objects) {
	var me = {}, multicast = [], ipHosts = [], tr069 = [];
	var mcastGems = [];

	(objects || []).forEach(function(o) { me[o.class_id + ':' + o.entity_id] = o; });

	(objects || []).forEach(function(o) {
		var f = o.fields || {};

		if (o.class_id == 281 && oltProvisioned(o)) {
			var ctpId = parseInt(f.gem_port, 16);
			var ctp = me['268:' + ctpId];
			var gem = ctp ? be16(hexBytes(ctp.data), 0) : null;
			if (gem != null && mcastGems.indexOf(gem) < 0)
				mcastGems.push(gem);
		} else if (o.class_id == 309 && oltProvisioned(o)) {
			multicast.push({
				entity: '0x%04X'.format(o.entity_id),
				igmp_version: f.igmp_version || '-',
				immediate_leave: !!f.immediate_leave,
				upstream_tci: (f.notes || {}).upstream_igmp_tci || f.upstream_igmp_tci || '-'
			});
		} else if (o.class_id == 134) {
			var ip = f.current_ip && f.current_ip != '0.0.0.0' ? f.current_ip : f.ip;
			var configured = oltProvisioned(o) || (ip && ip != '0.0.0.0');
			ipHosts.push({
				entity: '0x%04X'.format(o.entity_id),
				configured: configured,
				ip: ip || '0.0.0.0',
				gateway: (f.current_gateway && f.current_gateway != '0.0.0.0') ? f.current_gateway : (f.gateway || '0.0.0.0'),
				dns: (f.current_primary_dns && f.current_primary_dns != '0.0.0.0') ? f.current_primary_dns : (f.primary_dns || '0.0.0.0')
			});
		} else if (o.class_id == 340) {
			var acs = f.acs_address && f.acs_address != '0xffff' ? f.acs_address : null;
			tr069.push(Object.assign({
				entity: '0x%04X'.format(o.entity_id),
				admin: f.admin_state || '-',
				acs: acs
			}, acsTarget(me, o)));
		}
	});

	mcastGems.sort(function(a, b) { return a - b; });
	return {
		multicast: multicast,
		multicast_gems: mcastGems,
		ip_hosts: ipHosts,
		tr069: tr069
	};
}

function softwareImages(objects) {
	return (objects || []).filter(function(o) {
		return o.class_id == 7;
	}).sort(function(a, b) {
		return a.entity_id - b.entity_id;
	}).map(function(o) {
		var f = o.fields || {};
		return {
			bank: o.entity_id,
			version: f.version || '-',
			active: !!f.active,
			committed: !!f.committed,
			valid: !!f.valid
		};
	});
}

return baseclass.extend({
	hexBytes: hexBytes,
	be16: be16,
	provisionedVlans: provisionedVlans,
	routeText: routeText,
	oltPriority: oltPriority,
	mapperPriority: mapperPriority,
	allocIds: allocIds,
	gemPorts: gemPorts,
	dataPaths: dataPaths,
	linkQos: linkQos,
	uniPorts: uniPorts,
	oltServices: oltServices,
	softwareImages: softwareImages,
	oltProvisioned: oltProvisioned
});
