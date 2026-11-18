'use strict';
'require view';
'require omci.mib-editor as mib';

return view.extend({
	load: mib.load,
	render: function(data) {
		return mib.render('multicast', data, _('Multicast'), _('Multicast operations profiles, subscriber settings and the other IGMP and MLD entities.'));
	},
	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
