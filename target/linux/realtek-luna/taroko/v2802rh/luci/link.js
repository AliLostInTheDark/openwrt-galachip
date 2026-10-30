'use strict';
'require baseclass';
'require rpc';
'require network';

const callStatus = rpc.declare({
	object: 'xpon.public',
	method: 'status',
	expect: { '': {} }
});

let mode = null;

function monitor(dev) {
	return /^gponmon\d+$/.test(dev.getName());
}

let management = {};

function speed(dev, seen) {
	const name = dev.getName();
	if (!dev.getCarrier() || monitor(dev) || name == 'pon0' || seen.has(name))
		return null;
	seen.add(name);
	let parents = [];
	if (management[name])
		parents = [ network.instantiateDevice(management[name]) ];
	else if (/^omg[mp]\d+$/.test(name))
		return null;
	else if (dev.isBridge())
		parents = dev.getPorts() || [];
	else if (dev.getType() == 'vlan') {
		const parent = dev.getParent();
		if (parent)
			parents = [ parent ];
	}
	else if (dev.getType() == 'ethernet')
		return dev.getSpeed() > 0 ? dev.getSpeed() : null;
	const rates = parents.map(function(parent) { return speed(parent, new Set(seen)); });
	return Math.max(0, ...rates) || null;
}

return baseclass.extend({
	load: function() {
		return L.resolveDefault(callStatus(), {}).then(function(status) {
			mode = status.mode;
			management = status.management_links || {};
		});
	},

	monitor: monitor,

	speed: function(dev) {
		return speed(dev, new Set());
	},

	pon: function(dev, compact) {
		if (dev.getName() != 'pon0')
			return null;
		if (!dev.getCarrier())
			return _('no link');
		if (mode != 'gpon')
			return _('Connected');
		/* Optical line rates, not Ethernet negotiation or IP throughput. */
		return E('span', {
			'title': _('GPON nominal optical line rate: 2.488 Gbit/s downstream, 1.244 Gbit/s upstream')
		}, compact ? [ '2.48 / 1.24', E('br'), 'Gbit/s' ] : [ '2.48 Gbit/s / 1.24 Gbit/s' ]);
	}
});
