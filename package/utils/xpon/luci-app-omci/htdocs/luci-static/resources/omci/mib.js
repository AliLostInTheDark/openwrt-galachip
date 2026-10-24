'use strict';
'require baseclass';

/* What the OLT provisioned, read from the MIB list of omci.agent. */

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

/* 0,4,5,6,7 -> '0, 4-7' */
function ranges(numbers) {
	var out = [];

	numbers.map(Number).sort(function(a, b) { return a - b; }).forEach(function(n) {
		var last = out[out.length - 1];

		if (last && n == last[1] + 1)
			last[1] = n;
		else if (!last || n != last[1])
			out.push([ n, n ]);
	});

	return out.map(function(r) { return r[0] == r[1] ? String(r[0]) : r[0] + '-' + r[1]; }).join(', ');
}

/* One line per GEM port, then the T-CONT Alloc-IDs. */
function routeLines(route) {
	if (!route)
		return [ _('no GEM port found') ];

	return Object.keys(route.gems).map(function(gem) {
		return _('GEM %s: priority %s').format(gem, ranges(route.gems[gem]));
	}).concat(route.allocs.length ? [ _('Alloc-ID %s').format(ranges(route.allocs)) ] : []);
}

function routeText(route) {
	return routeLines(route).join(', ');
}

/*
 * What the OLT's Extended VLAN rules do with this VLAN's priority. A rule
 * covers it when it filters single-tagged frames on this VID or on any VID
 * (4096), or tags untagged frames with it. Filter P-bits 8 is any, 14 the
 * default rule; treatment P-bits 0-7 set, 8 copy inner, 9 copy outer,
 * 10 from DSCP, 15 add no tag (the received tag is kept unless removed).
 */
function oltPriority(objects, vid) {
	return oltPriorityLines(objects, vid).join('; ');
}

/* As oltPriority, one line each; rules that accept one priority and keep it are merged. */
function oltPriorityLines(objects, vid) {
	var texts = [], kept = [];

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

			if (tagged && r.filter_inner_pbit <= 7 && r.treat_inner_pbit == r.filter_inner_pbit) {
				if (kept.indexOf(r.filter_inner_pbit) < 0)
					kept.push(r.filter_inner_pbit);
				return;
			}

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

	if (kept.length)
		texts.unshift(kept.length == 1 ? _('OLT: accepts priority %s only, keeps it').format(kept[0]) :
			_('OLT: accepts priority %s, keeps it').format(ranges(kept)));

	return texts.filter(function(t, i) { return texts.indexOf(t) == i; });
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

/* GEM port network CTPs the OLT created. */
function gemPorts(objects) {
	return (objects || []).filter(function(o) {
		return o.class_id == 268 && o.origin_name == 'olt';
	}).map(function(o) {
		return be16(hexBytes(o.data), 0);
	}).sort(function(a, b) { return a - b; });
}

return baseclass.extend({
	hexBytes: hexBytes,
	be16: be16,
	provisionedVlans: provisionedVlans,
	ranges: ranges,
	routeLines: routeLines,
	routeText: routeText,
	oltPriority: oltPriority,
	oltPriorityLines: oltPriorityLines,
	mapperPriority: mapperPriority,
	allocIds: allocIds,
	gemPorts: gemPorts
});
