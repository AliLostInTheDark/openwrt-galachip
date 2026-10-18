'use strict';
'require view';
'require dom';
'require ui';
'require omci.rpc as omci';

/*
 * The agent's MIB, one table per object with the fields omci.agent decodes;
 * objects of a class it has no layout for show their attribute data as hex.
 */

function hex(n, width) {
	return '0x' + ('0000' + n.toString(16)).slice(-width);
}

function fieldRows(item) {
	var fields = item.fields;

	if (!fields)
		return [ E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '33%' }, _('Data')),
			E('td', { 'class': 'td left omci-mib-mono' }, item.data || '')
		]) ];

	return Object.keys(fields).map(function(name) {
		var value = fields[name];

		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '33%' }, name),
			E('td', { 'class': 'td left omci-mib-mono' }, Array.isArray(value) ?
				(value.length ? E('ol', { 'start': 0 }, value.map(function(line) {
					return E('li', {}, line);
				})) : '-') : String(value))
		]);
	});
}

function objectBlock(item) {
	return E('details', { 'class': 'omci-mib-object' }, [
		E('summary', {}, [
			E('strong', {}, hex(item.entity_id, 4)), ' ',
			E('span', { 'class': 'omci-mib-origin' }, item.origin_name || '')
		]),
		E('table', { 'class': 'table' }, fieldRows(item))
	]);
}

function render(objects, filter) {
	var classes = {};

	filter = (filter || '').trim().toLowerCase();
	(objects || []).forEach(function(item) {
		var name = item.name || _('Unknown ME');

		if (filter && name.toLowerCase().indexOf(filter) < 0 &&
		    String(item.class_id) != filter && hex(item.entity_id, 4) != filter)
			return;
		(classes[item.class_id] = classes[item.class_id] || { name: name, items: [] }).items.push(item);
	});

	var ids = Object.keys(classes).map(Number).sort(function(a, b) { return a - b; });

	if (!ids.length)
		return E('p', { 'class': 'cbi-section-descr' }, _('No MIB objects match.'));

	return E([], ids.map(function(id) {
		var cls = classes[id];

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, '%s (%d) – %d'.format(cls.name, id, cls.items.length)),
			E([], cls.items.map(objectBlock))
		]);
	}));
}

return view.extend({
	load: function() {
		return L.resolveDefault(omci.mibList(0), []);
	},

	render: function(objects) {
		var root = E('div', {}, render(objects));
		var search = E('input', {
			'class': 'cbi-input-text',
			'type': 'search',
			'placeholder': _('Filter by class name, class ID or entity (0x0301)'),
			'input': function() { dom.content(root, render(objects, search.value)); }
		});

		return E([], [
			E('style', {}, [
				'.omci-mib-object{margin:0 0 6px}',
				'.omci-mib-object summary{cursor:pointer;padding:4px 0}',
				'.omci-mib-origin{opacity:.7;font-size:12px}',
				'.omci-mib-mono{font-family:monospace;overflow-wrap:anywhere}',
				'.omci-mib-mono ol{margin:0;padding-left:2.5em}'
			].join('')),
			E('h2', {}, _('OMCI MIB')),
			E('p', { 'class': 'cbi-section-descr' },
				_('Managed entities the ONU reports and the OLT provisioned, decoded per G.988. Origin "olt" marks what the OLT created.')),
			E('div', { 'class': 'cbi-section' }, search),
			root
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
