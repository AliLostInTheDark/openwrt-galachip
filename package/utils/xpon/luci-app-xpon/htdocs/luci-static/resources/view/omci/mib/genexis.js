'use strict';
'require view';
'require omci.mib-editor as mib';

return view.extend({
	load: mib.load,
	render: function(data) {
		return mib.render('genexis', data, _('Genexis'), _('Genexis vendor-specific managed entities (255 Ethernet PM history data 4, 65305 IP flow filter). Active when the Genexis OLT profile is in use.'));
	},
	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
