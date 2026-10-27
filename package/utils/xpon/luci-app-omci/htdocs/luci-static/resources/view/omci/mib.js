'use strict';
'require view';
'require dom';
'require ui';
'require omci.rpc as omci';
'require xpon.widgets as widgets';

/*
 * The agent's MIB, one table per object with the fields omci.agent decodes;
 * objects of a class it has no layout for show their attribute data as hex.
 */

function hex(n, width) {
	return '0x' + ('0000' + n.toString(16)).slice(-width);
}

function parseUint(value) {
	value = String(value || '').trim();
	if (!/^(?:0[xX][0-9a-fA-F]+|[0-9]+)$/.test(value))
		return null;
	var number = Number(value);
	return Number.isInteger(number) && number >= 0 && number <= 0xffff ? number : null;
}

function normalizeHex(value) {
	value = String(value || '').trim().replace(/^0[xX]/, '');
	return /^(?:[0-9a-fA-F]{2})*$/.test(value) && value.length <= 128 ? value.toLowerCase() : null;
}

function field(label, node) {
	return E('div', { 'class': 'cbi-value' }, [
		E('label', { 'class': 'cbi-value-title' }, label),
		E('div', { 'class': 'cbi-value-field' }, node)
	]);
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

function objectBlock(item, actions) {
	return E('details', { 'class': 'omci-mib-object' }, [
		E('summary', {}, [
			E('strong', {}, hex(item.entity_id, 4)), ' ',
			E('span', { 'class': 'omci-mib-origin' }, item.origin_name || '')
		]),
		E('table', { 'class': 'table' }, fieldRows(item)),
		actions ? E('div', { 'class': 'omci-mib-actions' }, [
			E('button', { 'type': 'button', 'class': 'cbi-button cbi-button-action',
				'click': ui.createHandlerFn(null, function() { return actions.edit(item); }) }, _('Edit')),
			E('button', { 'type': 'button', 'class': 'cbi-button cbi-button-negative',
				'click': function() { actions.remove(item); } }, _('Delete'))
		]) : ''
	]);
}

function render(objects, filter, actions) {
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
			E([], cls.items.map(function(item) { return objectBlock(item, actions); }))
		]);
	}));
}

return view.extend({
	load: function() {
		return Promise.all([ omci.classList(0), omci.mibList(0) ]);
	},

	render: function(data) {
		widgets.style();
		var classes = data[0] || [], objects = data[1] || [];
		var writable = L.hasViewPermission();
		var actions = writable ? { edit: function(item) {
			return omci.mibGet(0, item.class_id, item.entity_id).then(showEditor).catch(notifyError);
		}, remove: showDelete } : null;
		var root = E('div');
		var createButton;
		var search = E('input', {
			'class': 'cbi-input-text',
			'type': 'search',
			'placeholder': _('Filter by class name, class ID or entity (0x0301)'),
			'input': redraw
		});

		function redraw() {
			dom.content(root, render(objects, search.value, actions));
			if (createButton)
				createButton.disabled = !classes.length;
		}

		function reloadObjects() {
			return Promise.all([ omci.classList(0), omci.mibList(0) ]).then(function(next) {
				classes = next[0] || [];
				objects = next[1] || [];
				redraw();
			});
		}

		function notifyError(error) {
			ui.addNotification(null, E('p', {}, _('MIB operation failed: %s').format(error.message || error)), 'error');
		}

		function complete(operation) {
			return operation.then(function() {
				ui.hideModal();
				return reloadObjects();
			}).catch(notifyError);
		}

		function modalButton(label, kind, handler) {
			return E('button', { 'type': 'button', 'class': 'cbi-button cbi-button-' + kind,
				'click': ui.createHandlerFn(null, handler) }, label);
		}

		function showEditor(item) {
			var classNode = item ? E('input', { 'class': 'cbi-input-text', 'readonly': true,
				'value': String(item.class_id) }) : E('select', { 'class': 'cbi-input-select' },
				classes.slice().sort(function(a, b) { return a.class_id - b.class_id; }).map(function(c) {
					return E('option', { 'value': String(c.class_id) }, '%d — %s'.format(c.class_id, c.name));
				}));
			var entityNode = E('input', { 'class': 'cbi-input-text', 'value': item ? hex(item.entity_id, 4) : '',
				'placeholder': '0x0000', 'readonly': item ? true : null });
			var maskNode = E('input', { 'class': 'cbi-input-text', 'value': item ? hex(item.attr_mask, 4) : '0xffff' });
			var dataNode = E('textarea', { 'class': 'cbi-input-text omci-mib-mono', 'rows': 5,
				'style': 'width:100%', 'placeholder': _('Hex bytes without separators (maximum 64 bytes)') }, item ? item.data || '' : '');

			ui.showModal(item ? _('Edit MIB object') : _('Create MIB object'), [
				E('p', {}, _('Changes update the live MIB immediately and are recorded as local. The OLT may replace them during provisioning.')),
				field(_('Class'), classNode), field(_('Entity ID'), entityNode),
				field(_('Attribute mask'), maskNode), field(_('Attribute data'), dataNode),
				E('div', { 'class': 'omci-mib-actions' }, [
					modalButton(_('Cancel'), 'reset', ui.hideModal),
					modalButton(item ? _('Save') : _('Create'), 'positive', function() {
						var classId = parseUint(classNode.value), entityId = parseUint(entityNode.value);
						var mask = parseUint(maskNode.value), bytes = normalizeHex(dataNode.value);
						if (classId == null || entityId == null || mask == null || bytes == null) {
							ui.addNotification(null, E('p', {}, _('Class, entity and mask must be between 0 and 65535. Data must be at most 64 bytes of hexadecimal pairs.')), 'error');
							return;
						}
						return complete(omci.mibSet(0, classId, entityId, mask, bytes));
					})
				])
			]);
		}

		function showDelete(item) {
			ui.showModal(_('Delete MIB object'), [
				E('p', {}, _('Delete class %d, entity %s from the live MIB?').format(item.class_id, hex(item.entity_id, 4))),
				E('div', { 'class': 'omci-mib-actions' }, [
					modalButton(_('Cancel'), 'reset', ui.hideModal),
					modalButton(_('Delete'), 'negative', function() {
						return complete(omci.mibDelete(0, item.class_id, item.entity_id));
					})
				])
			]);
		}

		function showReset() {
			ui.showModal(_('Reset MIB'), [
				E('p', {}, _('Reset the live MIB? This removes existing service objects and can interrupt traffic.')),
				E('div', { 'class': 'omci-mib-actions' }, [
					modalButton(_('Cancel'), 'reset', ui.hideModal),
					modalButton(_('Reset MIB'), 'negative', function() { return complete(omci.mibReset(0)); })
				])
			]);
		}

		if (writable)
			createButton = modalButton(_('Create MIB object'), 'action', function() { showEditor(null); });
		redraw();

		return E([], [
			E('style', {}, [
				'.omci-mib-object{margin:0 0 6px}',
				'.omci-mib-object summary{cursor:pointer;padding:4px 0}',
				'.omci-mib-origin{opacity:.7;font-size:12px}',
				'.omci-mib-mono{font-family:monospace;overflow-wrap:anywhere}',
				'.omci-mib-actions{display:flex;justify-content:flex-end;flex-wrap:wrap;gap:8px;margin:12px 0}',
				'.omci-mib-mono ol{margin:0;padding-left:2.5em}'
			].join('')),
			E('h2', {}, _('OMCI MIB')),
			E('p', { 'class': 'cbi-section-descr' },
				_('Managed entities the ONU reports and the OLT provisioned, decoded per G.988. Origin "olt" marks what the OLT created.')),
			E('div', { 'class': 'cbi-section' }, search),
			root,
			E('div', { 'class': 'omci-mib-actions' }, [
				createButton || '',
				modalButton(_('Refresh'), 'action', function() { return reloadObjects().catch(notifyError); }),
				writable ? modalButton(_('Reset MIB'), 'negative', showReset) : ''
			])
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
