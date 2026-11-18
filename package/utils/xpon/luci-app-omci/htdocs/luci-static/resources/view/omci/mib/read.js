'use strict';
'require view';
'require ui';
'require omci.layout as layout';
'require omci.rpc as omci';
'require omci.mib-view as mibView';
'require xpon.widgets as widgets';

function searchText(o) {
	return [ o.name, o.class_id, mibView.entityText(o.entity_id), JSON.stringify(o.fields || {}) ]
		.join(' ').toLowerCase();
}

return view.extend({
	load: function() {
		return omci.mibList(0);
	},

	render: function(data) {
		widgets.style();
		var objects = data || [];
		var sectionFilter = E('select', { 'class': 'cbi-input-select', 'id': 'omci-mib-section-filter' });
		var classFilter = E('select', { 'class': 'cbi-input-select', 'id': 'omci-mib-class-filter' });
		var search = E('input', { 'class': 'cbi-input-text', 'type': 'search', 'id': 'omci-mib-search',
			'placeholder': _('VLAN, name, value…') });
		var count = E('span');
		var root = E('div');
		var expandAll = false;

		function bySectionAndClass() {
			var sections = {};

			objects.forEach(function(o) {
				var sec = mibView.classifySection(o);
				if (!sections[sec])
					sections[sec] = {};
				(sections[sec][o.class_id] = sections[sec][o.class_id] || []).push(o);
			});
			return mibView.sectionOrder.filter(function(sec) {
				return !!sections[sec];
			}).map(function(sec) {
				var byId = sections[sec];
				var classes = Object.keys(byId).map(Number).sort(function(a, b) { return a - b; }).map(function(id) {
					return { id: id, section: sec, name: byId[id][0].name || _('Unknown managed entity'), objects: byId[id] };
				});
				var total = classes.reduce(function(acc, c) { return acc + c.objects.length; }, 0);
				return { key: sec, title: mibView.sectionLabels[sec] || sec, classes: classes, total: total };
			});
		}

		function fillFilters() {
			var selSection = sectionFilter.value;
			var selClass = classFilter.value;
			var sections = bySectionAndClass();

			sectionFilter.replaceChildren(E('option', { 'value': '' }, _('All categories & vendors')));
			sections.forEach(function(s) {
				sectionFilter.appendChild(E('option', { 'value': s.key }, '%s (%d)'.format(s.title, s.total)));
			});
			sectionFilter.value = selSection;

			classFilter.replaceChildren(E('option', { 'value': '' }, _('All classes')));
			sections.forEach(function(s) {
				if (sectionFilter.value && s.key !== sectionFilter.value)
					return;
				s.classes.forEach(function(c) {
					classFilter.appendChild(E('option', { 'value': String(c.id) },
						'%d, %s (%d)'.format(c.id, c.name, c.objects.length)));
				});
			});
			classFilter.value = selClass;
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

			function populate() {
				if (!body.firstChild)
					c.objects.forEach(function(o) { body.appendChild(mibView.object(o)); });
			}

			node.addEventListener('toggle', function() {
				if (node.open)
					populate();
			});
			node.open = open;
			if (open)
				populate();
			return node;
		}

		function draw() {
			var term = search.value.trim().toLowerCase(), shown = 0;
			var sections = bySectionAndClass().filter(function(s) {
				return !sectionFilter.value || s.key === sectionFilter.value;
			}).map(function(s) {
				var classes = s.classes.filter(function(c) {
					return !classFilter.value || c.id == classFilter.value;
				}).map(function(c) {
					return { id: c.id, section: c.section, name: c.name, objects: c.objects.filter(function(o) {
						return !term || searchText(o).indexOf(term) >= 0;
					}) };
				}).filter(function(c) { return c.objects.length; });
				var total = classes.reduce(function(acc, c) { return acc + c.objects.length; }, 0);
				return { key: s.key, title: s.title, classes: classes, total: total };
			}).filter(function(s) { return s.classes.length; });

			sections.forEach(function(s) { shown += s.total; });
			count.textContent = _('%d object(s)').format(shown);
			root.replaceChildren.apply(root, sections.length ? sections.map(function(s) {
				var forceOpen = expandAll || !!sectionFilter.value || !!classFilter.value || !!term;
				return E('div', { 'class': 'cbi-section' }, [
					E('h3', {}, '%s (%d)'.format(s.title, s.total))
				].concat(s.classes.map(function(c) {
					return group(c, forceOpen);
				})));
			}) : [ E('div', { 'class': 'alert-message notice' }, _('No MIB objects match.')) ]);
		}

		function refresh() {
			return omci.mibList(0).then(function(next) {
				objects = next || [];
				fillFilters();
				draw();
			}).catch(function(error) {
				ui.addNotification(null, E('p', {}, _('MIB read failed: %s').format(error.message || error)), 'error');
			});
		}

		sectionFilter.addEventListener('change', function() {
			classFilter.value = '';
			fillFilters();
			draw();
		});
		classFilter.addEventListener('change', draw);
		search.addEventListener('input', draw);
		fillFilters();
		draw();

		return layout.apply(E([], [
			E('h2', {}, _('OMCI MIB Browser')),
			E('p', {}, _('Live Management Information Base (ITU-T G.988), grouped by functional category and vendor. All decoded attributes are displayed.')),
			E('div', { 'class': 'cbi-section omci-mib-filter' }, [
				E('label', { 'for': 'omci-mib-section-filter' }, _('Group')), sectionFilter,
				E('label', { 'for': 'omci-mib-class-filter' }, _('Class')), classFilter,
				E('label', { 'for': 'omci-mib-search' }, _('Search')), search,
				count,
				E('button', { 'type': 'button', 'class': 'cbi-button',
					'click': function() { expandAll = !expandAll; draw(); } }, _('Toggle expand all')),
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
