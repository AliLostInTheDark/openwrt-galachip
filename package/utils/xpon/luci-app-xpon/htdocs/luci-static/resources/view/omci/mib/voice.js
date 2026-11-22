'use strict';
'require view';
'require omci.mib-editor as mib';

return view.extend({
	load: mib.load,
	render: function(data) {
		return mib.render('voice', data, _('Voice'), _('The analogue phone ports and their SIP service: PPTP POTS UNIs, VoIP voice CTPs, SIP agent and user data, media and service profiles, and the line status the ONU reports.'));
	},
	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
