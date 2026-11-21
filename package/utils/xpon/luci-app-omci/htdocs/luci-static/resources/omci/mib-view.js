'use strict';
'require baseclass';

var words = {
	id: 'ID', mac: 'MAC', tp: 'TP', td: 'TD', gem: 'GEM', iwtp: 'IWTP', omcc: 'OMCC',
	tcont: 'T-CONT', tconts: 'T-CONTs', sr: 'SR', sf: 'SF', sd: 'SD', me: 'ME',
	tpid: 'TPID', dscp: 'DSCP', igmp: 'IGMP', tci: 'TCI', arc: 'ARC', pppoe: 'PPPoE',
	dte: 'DTE', dce: 'DCE', tcp: 'TCP', udp: 'UDP', iana: 'IANA', ip: 'IP', lan: 'LAN',
	fcs: 'FCS', tm: 'TM', dba: 'DBA', mib: 'MIB', cir: 'CIR', pir: 'PIR', cbs: 'CBS',
	pbs: 'PBS', fec: 'FEC'
};

/*
 * Where a MIB instance came from. G.988 has the ONU create some instances
 * autonomously at start-up and the OLT create the rest; the OLT may also Set
 * attributes of an ONU-created instance. Ordered as the origin filter lists them.
 */
var origins = [
	[ 'olt', _('OLT-provisioned'),
	  _('Created by the OLT with an OMCI Create. It lasts until the OLT deletes it or resets the MIB.') ],
	[ 'olt_written', _('ONU-created, OLT-configured'),
	  _('Created by the ONU at start-up; the OLT has since changed its attributes with an OMCI Set.') ],
	[ 'default', _('ONU-created'),
	  _('Created by the ONU at start-up with its own values; the OLT has not changed it.') ],
	[ 'local', _('Local override'),
	  _('Changed on this device in LuCI or with omcictl, not by the OLT.') ],
	[ 'hardware', _('Hardware-reported'),
	  _('Filled in from the hardware by the ONU.') ]
];

function label(name) {
	var attr = /^attr_(\d+)$/.exec(name), pbit = /^pbit(\d)_(.+)$/.exec(name);

	if (attr)
		return _('Attribute %s').format(attr[1]);
	if (pbit)
		name = 'priority_' + pbit[1] + '_' + pbit[2];

	var parts = name.split('_').map(function(word) { return words[word] || word; });

	parts[0] = parts[0].charAt(0).toUpperCase() + parts[0].slice(1);
	return parts.join(' ');
}

function value(v) {
	return v == null || v === '' ? '-' : String(v);
}

function entityText(id) {
	return '0x%04x (%d)'.format(id, id);
}

function originKey(o) {
	return o.origin_name == 'default' && o.olt_written ? 'olt_written' : o.origin_name;
}

function originEntry(o) {
	var key = originKey(o);

	return origins.filter(function(e) { return e[0] == key; })[0];
}

function originText(o) {
	var e = originEntry(o);

	return e ? e[1] : (o.origin_name || '-');
}

function originHelp(o) {
	var e = originEntry(o);

	return e ? e[2] : null;
}

function printable(code) {
	return code >= 0x20 && code < 0x7f ? String.fromCharCode(code) : '.';
}

/* Offset, hex and ASCII, without the zero padding after the last used row. */
function hexDump(data) {
	var bytes = (data || '').match(/../g) || [], last = bytes.length, lines = [], i;

	while (last > 0 && bytes[last - 1] == '00')
		last--;
	for (i = 0; i < last; i += 16) {
		var row = bytes.slice(i, i + 16);

		lines.push('%04x  %s  %s'.format(i, row.join(' ').padEnd(47),
			row.map(function(b) { return printable(parseInt(b, 16)); }).join('')));
	}
	return lines.length ? lines.join('\n') : _('All zero');
}

/* MIB text can come from the OLT: cells take it in an array, so E() adds it as
 * a text node and not as HTML. */
function rows(fields) {
	var out = [];

	Object.keys(fields || {}).forEach(function(key) {
		if (key == 'rules' || key == 'notes')
			return;
		if (Array.isArray(fields[key]))
			fields[key].forEach(function(line, i) {
				out.push([ _('%s %d').format(label(key).replace(/s$/, ''), i + 1), line ]);
			});
		else
			out.push([ label(key), value(fields[key]), (fields.notes || {})[key] ]);
	});
	return out;
}

function fieldTable(fields) {
	return E('table', { 'class': 'table' }, rows(fields).map(function(row) {
		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '50%' }, [ row[0] ]),
			E('td', { 'class': 'td left' }, [
				row[1],
				row[2] ? E('div', { 'class': 'omci-mib-note' }, [ row[2] ]) : null
			])
		]);
	}));
}

function rulesTable(rules) {
	var heads = [ _('Rule'), _('Filter outer'), _('Filter inner'), _('EtherType'), _('Action'),
		_('Add outer'), _('Add inner') ];

	return E('div', { 'class': 'omci-table-scroll' }, E('table', { 'class': 'table omci-mib-table omci-mib-rules' }, [
		E('tr', { 'class': 'tr table-titles' }, heads.map(function(head) {
			return E('th', { 'class': 'th' }, head);
		}))
	].concat(rules.map(function(r) {
		var s = r.summary;

		return E('tr', { 'class': 'tr' }, [ r.index, s.filter_outer, s.filter_inner, s.ethertype,
			s.action, s.add_outer, s.add_inner ].map(function(cell) {
			return E('td', { 'class': 'td' }, [ String(cell) ]);
		}));
	}))));
}

function object(o) {
	var rules = ((o.extended_vlan || {}).rules || []).filter(function(r) { return r.summary; });

	return E('div', { 'class': 'xpon-panel omci-mib-object' + (rules.length ? ' omci-mib-wide' : '') }, [
		E('div', { 'class': 'omci-mib-object-head' }, [
			E('strong', {}, _('Entity %s').format(entityText(o.entity_id))),
			E('span', { 'class': 'omci-mib-origin', 'title': originHelp(o) }, originText(o))
		]),
		o.fields ? fieldTable(o.fields) : E('p', {}, _('No field names are known for this class; its data is shown below.')),
		rules.length ? E('h5', {}, _('VLAN rules')) : '',
		rules.length ? rulesTable(rules) : '',
		E('details', { 'class': 'omci-mib-raw', 'open': o.fields ? null : true }, [
			E('summary', {}, _('Raw data, attribute mask %s').format('0x%04x'.format(o.attr_mask || 0))),
			E('pre', {}, [ hexDump(o.data) ])
		])
	]);
}

function summary(o) {
	var parts = rows(o.fields).map(function(row) {
		return '%s: %s'.format(row[0], row[1]);
	});

	return parts.length ? parts.join(', ') : _('No field names known');
}

var sectionOrder = [
	'equipment', 'ani', 'uni', 'multicast', 'voice', 'performance', 'management', 'standard_other',
	'vendor_nokia', 'vendor_huawei', 'vendor_zte', 'vendor_fiberhome', 'vendor_ctc', 'vendor_genexis', 'vendor_other'
];

var sectionLabels = {
	equipment: _('Standard — Equipment'),
	ani: _('Standard — ANI & GPON'),
	uni: _('Standard — UNI & Layer 2 / VLAN'),
	multicast: _('Standard — Multicast'),
	voice: _('Standard — Voice & VoIP'),
	performance: _('Standard — Performance (PM)'),
	management: _('Standard — Management'),
	standard_other: _('Standard — Other'),
	vendor_nokia: _('Vendor — Nokia / ALCL'),
	vendor_huawei: _('Vendor — Huawei'),
	vendor_zte: _('Vendor — ZTE'),
	vendor_fiberhome: _('Vendor — FiberHome'),
	vendor_ctc: _('Vendor — CTC'),
	vendor_genexis: _('Vendor — Genexis'),
	vendor_other: _('Vendor — Other')
};

function classifySection(o) {
	var id = Number(o.class_id || 0);
	var name = String(o.name || '').toLowerCase();

	/* the agent names the vendor of a class two OLTs share, by the OLT profile */
	if (o.vendor)
		return 'vendor_' + o.vendor;
	if (name.indexOf('genexis') >= 0)
		return 'vendor_genexis';
	if (name.indexOf('nokia') >= 0 || name.indexOf('alcl') >= 0)
		return 'vendor_nokia';
	if (name.indexOf('huawei') >= 0)
		return 'vendor_huawei';
	if (name.indexOf('zte') >= 0)
		return 'vendor_zte';
	if (name.indexOf('fiberhome') >= 0 || name.indexOf('fhtt') >= 0)
		return 'vendor_fiberhome';
	if (name.indexOf('ctc') >= 0)
		return 'vendor_ctc';

	if ((id >= 350 && id <= 373) || id == 65398 || (id >= 65414 && id <= 65454))
		return 'vendor_huawei';
	if (id >= 65295 && id <= 65306)
		return 'vendor_nokia';
	if (id == 255)
		return 'vendor_genexis';
	if (id == 242 || id == 243 || id == 247 || id == 65294 || id == 65314 || id == 65315)
		return 'vendor_zte';
	if ((id >= 65322 && id <= 65338) || id == 65363 || id == 65411)
		return 'vendor_fiberhome';
	if (id == 250 || id == 65282 || id == 65320 || id == 65321 || id == 65408 || id == 65527 || (id >= 65380 && id <= 65386) ||
	    id == 65523 || (id >= 65528 && id <= 65531))
		return 'vendor_ctc';
	if ((id >= 240 && id <= 255) || (id >= 350 && id <= 399) || id >= 65280)
		return 'vendor_other';

	if (id == 2 || id == 5 || id == 6 || id == 7 || id == 133 || id == 159 || id == 160 ||
	    id == 256 || id == 257 || id == 297 || id == 331 || id == 336 || id == 441)
		return 'equipment';
	if (id == 281 || id == 309 || id == 310 || id == 311)
		return 'multicast';
	if (id == 53 || id == 58 || (id >= 138 && id <= 147) || (id >= 150 && id <= 153))
		return 'voice';
	if (id == 24 || id == 51 || id == 52 || id == 89 || id == 273 || id == 274 || id == 276 ||
	    id == 296 || id == 312 || id == 334 || id == 341 || id == 425 ||
	    name.indexOf('pm ') >= 0 || name.indexOf('performance') >= 0 || name.indexOf('threshold') >= 0)
		return 'performance';
	if (id == 11 || (id >= 45 && id <= 50) || id == 79 || id == 84 || id == 130 || id == 171 ||
	    id == 264 || id == 298 || id == 329)
		return 'uni';
	if (id == 262 || id == 263 || id == 266 || id == 268 || id == 272 || id == 277 || id == 278 || id == 280)
		return 'ani';
	if (id == 131 || id == 134 || id == 136 || id == 137 || id == 148 || id == 157 || id == 158 ||
	    id == 287 || id == 288 || id == 289 || id == 340)
		return 'management';
	return 'standard_other';
}

return baseclass.extend({
	entityText: entityText,
	origins: origins,
	originKey: originKey,
	originText: originText,
	originHelp: originHelp,
	object: object,
	summary: summary,
	sectionOrder: sectionOrder,
	sectionLabels: sectionLabels,
	classifySection: classifySection
});
