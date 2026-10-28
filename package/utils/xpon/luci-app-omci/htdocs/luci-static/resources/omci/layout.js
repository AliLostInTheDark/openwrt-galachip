'use strict';
'require baseclass';

return baseclass.extend({
	apply: function(node) {
		if (!document.getElementById('omci-layout-style'))
			document.head.appendChild(E('link', {
				'id': 'omci-layout-style', 'rel': 'stylesheet',
				'href': L.resource('omci/layout.css')
			}));

		if (node.nodeType == 11)
			node = E('div', {}, node);
		node.classList.add('omci-page');
		return node;
	}
});
