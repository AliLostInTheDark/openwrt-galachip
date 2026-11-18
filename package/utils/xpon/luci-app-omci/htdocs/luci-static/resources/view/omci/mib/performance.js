'use strict';
'require view';
'require omci.mib-editor as mib';

return view.extend({
	load: mib.load,
	render: function(data) {
		return mib.render('performance', data, _('Performance'), _('PM history data the OLT reads every 15 minutes, and the threshold data it compares them with. Counters this ONU does not keep read zero.'));
	},
	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
