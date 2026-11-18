'use strict';
'require view';
'require omci.mib-editor as mib';

return view.extend({
	load: mib.load,
	render: function(data) {
		return mib.render('ctc', data, _('CTC / Stock G7'), _('China Telecom (CTC) and stock XZ000-G7 vendor-specific managed entities (250, 65282, 65320–65321, 65380–65386, 65523, 65528–65531).'));
	},
	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
