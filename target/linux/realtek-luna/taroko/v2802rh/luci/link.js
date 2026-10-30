'use strict';
'require baseclass';
'require rpc';

const callStatus = rpc.declare({
	object: 'xpon.public',
	method: 'status',
	expect: { '': {} }
});

let mode = null;

function monitor(dev) {
	return /^gponmon\d+$/.test(dev.getName());
}

function virtual(dev) {
	return monitor(dev) || /^omg[mp]\d+$/.test(dev.getName()) ||
		dev.isBridge() || dev.getType() != 'ethernet';
}

return baseclass.extend({
	load: function() {
		return L.resolveDefault(callStatus(), {}).then(function(status) {
			mode = status.mode;
		});
	},

	monitor: monitor,

	speed: function(dev) {
		return dev.getName() == 'pon0' || virtual(dev) ? null : dev.getSpeed();
	},

	pon: function(dev) {
		if (dev.getName() != 'pon0')
			return null;
		if (!dev.getCarrier())
			return _('no link');
		if (mode != 'gpon')
			return _('Connected');
		/* Optical line rates, not Ethernet negotiation or IP throughput. */
		return E('span', {
			'title': _('GPON nominal optical line rate: downstream / upstream'),
			'style': 'white-space:normal'
		}, [ 'GPON ↓ 2.488 / ↑ 1.244 Gbit/s' ]);
	}
});
