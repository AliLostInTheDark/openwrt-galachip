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

/* G.988 terms: an ME is created autonomously by the ONU or by the OLT. */
var originLabels = {
	'default': _('ONU-created'), 'local': _('Local override'),
	'olt': _('OLT-created'), 'hardware': _('Hardware')
};

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

function originText(o) {
	if (o.origin_name == 'default' && o.olt_written)
		return _('ONU-created, OLT-configured');
	return originLabels[o.origin_name] || o.origin_name || '-';
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
			E('span', { 'class': 'omci-mib-origin' }, originText(o))
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
	var parts = rows(o.fields).slice(0, 4).map(function(row) {
		return '%s: %s'.format(row[0], row[1]);
	});

	return parts.length ? parts.join(', ') : _('No field names known');
}

return baseclass.extend({
	entityText: entityText,
	originLabels: originLabels,
	originText: originText,
	object: object,
	summary: summary
});
