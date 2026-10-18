'use strict';
'require view';
'require xpon.mode as ponmode';

/* The page of the inactive mode: only a way to switch to it. */
return view.extend({
	load: function() {
		return ponmode.current();
	},

	render: function(current) {
		return ponmode.inactiveNotice(current == 'gpon' ? 'epon' : 'gpon');
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
