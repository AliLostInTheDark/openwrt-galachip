'use strict';
'require view';
'require omci.mib-editor as mib';

return view.extend({
	load: mib.load,
	render: function(data) {
		return mib.render('zte', data, _('ZTE'), _('ZTE vendor-specific managed entities (242, 243, 247, 65294, 65305, 65314, 65315). Active when the ZTE OLT profile is in use.'));
	},
	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
