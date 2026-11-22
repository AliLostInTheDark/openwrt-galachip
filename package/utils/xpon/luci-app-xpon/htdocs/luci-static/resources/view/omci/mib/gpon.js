'use strict';
'require view';
'require omci.mib-editor as mib';

return view.extend({
	load: mib.load,
	render: function(data) {
		return mib.render('gpon', data, _('ANI'), _('The PON side of the ONU: ANI-G, T-CONTs, GEM ports and their traffic management entities.'));
	},
	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
