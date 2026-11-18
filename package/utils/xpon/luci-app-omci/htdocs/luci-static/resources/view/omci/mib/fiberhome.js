'use strict';
'require view';
'require omci.mib-editor as mib';

return view.extend({
	load: mib.load,
	render: function(data) {
		return mib.render('fiberhome', data, _('FiberHome'), _('FiberHome vendor-specific managed entities (65321–65338, 65363, 65411). Active when the FiberHome OLT profile is in use.'));
	},
	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
