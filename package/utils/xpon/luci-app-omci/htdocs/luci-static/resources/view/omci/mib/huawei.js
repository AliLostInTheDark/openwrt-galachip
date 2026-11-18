'use strict';
'require view';
'require omci.mib-editor as mib';

return view.extend({
	load: mib.load,
	render: function(data) {
		return mib.render('huawei', data, _('Huawei'), _('Huawei vendor-specific managed entities (350–373, 65398–65454). Active when the Huawei OLT profile is in use.'));
	},
	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
