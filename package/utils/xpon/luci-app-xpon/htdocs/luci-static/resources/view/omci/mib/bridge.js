'use strict';
'require view';
'require omci.mib-editor as mib';

return view.extend({
	load: mib.load,
	render: function(data) {
		return mib.render('bridge', data, _('UNI and layer 2'), _('The subscriber side: Ethernet and virtual Ethernet UNIs, MAC bridges, 802.1p mappers and VLAN filtering and tagging.'));
	},
	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
