'use strict';
'require view';
'require xpon.mode as ponmode';
'require xpon.widgets as widgets';

/* The page of the inactive mode: only a way to switch to it. */
return view.extend({
	load: function() {
		return ponmode.current();
	},

	render: function(current) {
		widgets.style();
		return ponmode.inactiveNotice(current == 'gpon' ? 'epon' : 'gpon');
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
