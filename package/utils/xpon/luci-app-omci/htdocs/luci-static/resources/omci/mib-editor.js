'use strict';
'require omci.layout as layout';
'require baseclass';
'require ui';
'require omci.rpc as omci';
'require xpon.widgets as widgets';

var groups = {
	inventory: function(c) { return c.category_name === 'equipment' || c.category_name === 'management'; },
	gpon: function(c) { return c.category_name === 'ani'; },
	bridge: function(c) { return c.category_name === 'uni' || c.category_name === 'layer2' || !!(c.flags & (1 << 6)); },
	multicast: function(c) { return c.category_name === 'multicast'; },
	other: function(c) {
		return c.category_name !== 'equipment' && c.category_name !== 'management' &&
		       c.category_name !== 'ani' && c.category_name !== 'uni' &&
		       c.category_name !== 'layer2' && c.category_name !== 'multicast' &&
		       !(c.flags & (1 << 6));
	}
};

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

function load() {
	return Promise.all([ omci.classList(0), omci.mibList(0) ]);
}

function render(group, data, title, description) {
	widgets.style();
	var classes = data[0] || [], objects = data[1] || [];
	var writable = L.hasViewPermission();
	var predicate = groups[group] || groups.other;
	var byId, groupClasses, createButton;
	var originLabels = {
		'default': _('Default / seed'), 'local': _('Local'),
		'olt': _('Provisioned by OLT'), 'hardware': _('Hardware')
	};
	var originFilter = E('select', { 'class': 'cbi-input-select', 'id': 'omci-mib-origin-filter' }, [
		E('option', { 'value': 'all' }, _('All origins')),
		E('option', { 'value': 'default' }, _('Default / seed')),
		E('option', { 'value': 'olt' }, _('Provisioned by OLT')),
		E('option', { 'value': 'local' }, _('Local edits')),
		E('option', { 'value': 'hardware' }, _('Hardware'))
	]);
	var tableRoot = E('div', { 'class': 'omci-table-scroll', 'tabindex': '0',
		'role': 'region', 'aria-label': _('MIB objects') });
	var count = E('span');

	function updateCatalog() {
		byId = {};
		classes.forEach(function(c) { byId[c.class_id] = c; });
		groupClasses = classes.filter(predicate).sort(function(a, b) { return a.class_id - b.class_id; });
	}

	function reloadObjects() {
		return Promise.all([ omci.classList(0), omci.mibList(0) ]).then(function(next) {
			classes = next[0] || [];
			objects = next[1] || [];
			updateCatalog();
			renderTable();
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
		var choices = {};
		groupClasses.forEach(function(c) {
			choices[String(c.class_id)] = '%d — %s'.format(c.class_id, c.name);
		});
		var classWidget = item ? null : new ui.Select(
			groupClasses.length ? String(groupClasses[0].class_id) : null, choices,
			{ optional: false, sort: groupClasses.map(function(c) { return String(c.class_id); }) });
		var classNode = item ? E('input', { 'class': 'cbi-input-text', 'readonly': true,
			'value': String(item.class_id) }) : classWidget.render();
		var entityNode = E('input', { 'class': 'cbi-input-text', 'value': item ? hex(item.entity_id, 4) : '',
			'placeholder': '0x0000', 'readonly': item ? true : null });
		var maskNode = E('input', { 'class': 'cbi-input-text', 'value': item ? hex(item.attr_mask, 4) : '0xffff' });
		var dataNode = E('textarea', { 'class': 'cbi-input-text omci-mib-mono', 'rows': 5,
			'style': 'width:100%;font-family:monospace', 'placeholder': _('Hex bytes without separators (maximum 64 bytes)') }, item ? item.data || '' : '');

		ui.showModal(item ? _('Edit MIB object') : _('Create MIB object'), [
			E('p', {}, _('Changes update the live MIB immediately and are recorded as local. The OLT may replace them during provisioning.')),
			field(_('Class'), classNode), field(_('Entity ID'), entityNode),
			field(_('Attribute mask'), maskNode), field(_('Attribute data'), dataNode),
			E('div', { 'class': 'omci-mib-actions' }, [
				modalButton(_('Cancel'), 'reset', ui.hideModal),
				modalButton(item ? _('Save') : _('Create'), 'positive', function() {
					var classId = parseUint(item ? classNode.value : classWidget.getValue()), entityId = parseUint(entityNode.value);
					var mask = parseUint(maskNode.value), bytes = normalizeHex(dataNode.value);
					if (classId == null || entityId == null || mask == null || bytes == null) {
						ui.addNotification(null, E('p', {}, _('Class, entity and mask must be between 0 and 65535. Data must be at most 64 bytes of hexadecimal pairs.')), 'error');
						return;
					}
					return complete(omci.mibSet(0, classId, entityId, mask, bytes));
				})
			])
		], 'omci-mib-editor');
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

	function renderTable() {
		var filtered = objects.filter(function(o) {
			var c = byId[o.class_id] || { category_name: 'unknown', flags: 0 };
			return predicate(c) && (originFilter.value == 'all' || o.origin_name == originFilter.value);
		});
		count.textContent = _('%d object(s)').format(filtered.length);
		if (createButton)
			createButton.disabled = !groupClasses.length;
		if (!filtered.length) {
			tableRoot.replaceChildren(E('div', { 'class': 'alert-message notice' },
				_('No MIB objects match this category and origin filter.')));
			return;
		}

		var headings = [ _('Class'), _('Entity'), _('Managed entity'), _('Origin'), _('Mask'), _('Data') ];
		if (writable)
			headings.push(_('Actions'));
		var table = E('table', { 'class': 'table omci-mib-table' }, [
			E('tr', { 'class': 'tr table-titles' }, headings.map(function(label) {
				return E('th', { 'class': 'th' }, label);
			}))
		]);
		filtered.forEach(function(o) {
			var c = byId[o.class_id] || {};
			var cells = [
				E('td', { 'class': 'td nowrap' }, String(o.class_id)),
				E('td', { 'class': 'td nowrap' }, hex(o.entity_id, 4)),
				E('td', { 'class': 'td' }, o.name || c.name || '-'),
				E('td', { 'class': 'td' }, originLabels[o.origin_name] || o.origin_name || '-'),
				E('td', { 'class': 'td nowrap' }, hex(o.attr_mask, 4)),
				E('td', { 'class': 'td', 'style': 'font-family:monospace;overflow-wrap:anywhere;max-width:32em' }, o.data || '')
			];
			if (writable)
				cells.push(E('td', { 'class': 'td nowrap' }, [
					modalButton(_('Edit'), 'action', function() {
						return omci.mibGet(0, o.class_id, o.entity_id).then(showEditor).catch(notifyError);
					}), ' ', modalButton(_('Delete'), 'negative', function() { showDelete(o); })
				]));
			table.appendChild(E('tr', { 'class': 'tr' }, cells));
		});
		tableRoot.replaceChildren(table);
	}

	updateCatalog();
	if (writable)
		createButton = modalButton(_('Create MIB object'), 'action', function() { showEditor(null); });
	originFilter.addEventListener('change', renderTable);
	renderTable();

	return layout.apply(E([], [
		E('style', {}, [
			'.omci-mib-actions{display:flex;justify-content:flex-end;flex-wrap:wrap;gap:8px;margin:12px 0}',
			'.omci-mib-filter{display:flex;align-items:center;flex-wrap:wrap;gap:8px}',
			'.omci-mib-table{min-width:760px}',
			'.omci-mib-table th{white-space:nowrap}'
		].join('')),
		E('h2', {}, title), E('p', {}, description),
		E('div', { 'class': 'cbi-section omci-mib-filter' }, [
			E('label', { 'for': 'omci-mib-origin-filter' }, _('Origin filter')), originFilter, count
		]),
		tableRoot,
		E('div', { 'class': 'cbi-page-actions omci-mib-actions' }, [
			createButton || '',
			modalButton(_('Refresh'), 'action', function() { return reloadObjects().catch(notifyError); }),
			writable ? modalButton(_('Reset MIB'), 'negative', showReset) : ''
		])
	]));
}

return baseclass.extend({
	load: load,
	render: render
});
