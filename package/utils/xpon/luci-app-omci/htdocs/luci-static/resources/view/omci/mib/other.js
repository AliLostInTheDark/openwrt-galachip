'use strict';
'require view';
'require omci.mib-editor as mib';

return view.extend({
	load: mib.load,
	render: function(data) {
		return mib.render('other', data, _('Other'), _('Managed entities outside the equipment, ANI, UNI and layer 2, and multicast groups.'));
	},
	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
