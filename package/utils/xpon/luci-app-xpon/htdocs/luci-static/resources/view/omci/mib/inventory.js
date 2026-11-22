'use strict';
'require view';
'require omci.mib-editor as mib';

return view.extend({
	load: mib.load,
	render: function(data) {
		return mib.render('inventory', data, _('Equipment'), _('ONU identity, software images, cardholders, circuit packs and the other equipment and management entities.'));
	},
	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
