'use strict';
'require view';
'require ui';
'require omci.layout as layout';
'require omci.rpc as omci';
'require omci.mib-view as mibView';
'require xpon.widgets as widgets';

function searchText(o) {
	return [ o.name, o.class_id, o.entity_id, '0x%04x'.format(o.entity_id), JSON.stringify(o.fields || {}) ]
		.join(' ').toLowerCase();
}

return view.extend({
	load: function() {
		return omci.mibList(0);
	},

	render: function(data) {
		widgets.style();
		var objects = data || [];
		var classFilter = E('select', { 'class': 'cbi-input-select', 'id': 'omci-mib-class-filter' });
		var search = E('input', { 'class': 'cbi-input-text', 'type': 'search', 'id': 'omci-mib-search',
			'placeholder': _('VLAN, name, value…') });
		var count = E('span');
		var root = E('div');

		function byClass() {
			var classes = {};

			objects.forEach(function(o) {
				(classes[o.class_id] = classes[o.class_id] || []).push(o);
			});
			return Object.keys(classes).map(Number).sort(function(a, b) { return a - b; }).map(function(id) {
				return { id: id, name: classes[id][0].name || _('Unknown managed entity'), objects: classes[id] };
			});
		}

		function fillClasses() {
			var selected = classFilter.value;

			classFilter.replaceChildren(E('option', { 'value': '' }, _('All classes')));
			byClass().forEach(function(c) {
				classFilter.appendChild(E('option', { 'value': String(c.id) }, '%d, %s (%d)'.format(c.id, c.name, c.objects.length)));
			});
			classFilter.value = selected;
		}

		function group(c, open) {
			var body = E('div', { 'class': 'xpon-grid' });
			var node = E('details', { 'class': 'cbi-section omci-mib-class' }, [
				E('summary', {}, [
					E('strong', {}, c.name),
					E('span', { 'class': 'omci-mib-class-info' },
						c.objects.length == 1 ? _('class %d').format(c.id) :
						_('class %d, %d objects').format(c.id, c.objects.length))
				]),
				body
			]);

			/* Priority queues alone are about a hundred objects: draw on demand. */
			node.addEventListener('toggle', function() {
				if (node.open && !body.firstChild)
					c.objects.forEach(function(o) { body.appendChild(mibView.object(o)); });
			});
			node.open = open;
			return node;
		}

		function draw() {
			var term = search.value.trim().toLowerCase(), shown = 0;
			var groups = byClass().filter(function(c) {
				return !classFilter.value || c.id == classFilter.value;
			}).map(function(c) {
				return { id: c.id, name: c.name, objects: c.objects.filter(function(o) {
					return !term || searchText(o).indexOf(term) >= 0;
				}) };
			}).filter(function(c) { return c.objects.length; });

			groups.forEach(function(c) { shown += c.objects.length; });
			count.textContent = _('%d object(s)').format(shown);
			root.replaceChildren.apply(root, groups.length ? groups.map(function(c) {
				return group(c, !!classFilter.value || !!term);
			}) : [ E('div', { 'class': 'alert-message notice' }, _('No MIB objects match.')) ]);
		}

		function refresh() {
			return omci.mibList(0).then(function(next) {
				objects = next || [];
				fillClasses();
				draw();
			}).catch(function(error) {
				ui.addNotification(null, E('p', {}, _('MIB read failed: %s').format(error.message || error)), 'error');
			});
		}

		classFilter.addEventListener('change', draw);
		search.addEventListener('input', draw);
		fillClasses();
		draw();

		return layout.apply(E([], [
			E('h2', {}, _('MIB read')),
			E('p', {}, _('Every managed entity in the live MIB, decoded. Open a class to see its objects.')),
			E('div', { 'class': 'cbi-section omci-mib-filter' }, [
				E('label', { 'for': 'omci-mib-class-filter' }, _('Class')), classFilter,
				E('label', { 'for': 'omci-mib-search' }, _('Search')), search,
				count,
				E('button', { 'type': 'button', 'class': 'cbi-button cbi-button-action',
					'click': ui.createHandlerFn(null, refresh) }, _('Refresh'))
			]),
			root
		]));
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
