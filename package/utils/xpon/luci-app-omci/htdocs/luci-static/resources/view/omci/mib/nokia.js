'use strict';
'require view';
'require omci.mib-editor as mib';

return view.extend({
	load: mib.load,
	render: function(data) {
		return mib.render('nokia', data, _('Nokia / ALCL'), _('Nokia / Alcatel-Lucent vendor-specific managed entities (65295–65306). Active when the Nokia / ALCL OLT profile is in use.'));
	},
	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
