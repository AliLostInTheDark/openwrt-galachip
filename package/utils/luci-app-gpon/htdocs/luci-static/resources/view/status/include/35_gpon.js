'use strict';
'require baseclass';
'require rpc';

var callGpon = rpc.declare({ object: 'gpon', method: 'status' });

return baseclass.extend({
	title: _('Fiber (GPON)'),

	load: function() {
		return L.resolveDefault(callGpon(), {});
	},

	render: function(d) {
		d = d || {};

		var state = (d.onu_state_name && d.onu_state_name != 'unknown')
			? ('O' + (d.onu_state || '?') + ' – ' + d.onu_state_name)
			: ('O' + (d.onu_state || '?'));

		var rows = [
			[ _('ONU state'), E('span', { 'class': d.online ? 'label success' : 'label warning' }, [ state ]) ],
			[ _('RX power'),  ((d.optic_rx_dbm || 'n/a')) + ' dBm' ],
			[ _('TX power'),  ((d.optic_tx_dbm || 'n/a')) + ' dBm' ],
			[ _('Signal'),    d.los === true ? _('LOS (no fiber light)') : d.los === false ? _('OK') : _('unknown') ]
		];

		var t = E('table', { 'class': 'table' });
		rows.forEach(function(r) {
			t.appendChild(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'width': '33%' }, [ r[0] ]),
				E('td', { 'class': 'td left' }, [ r[1] ])
			]));
		});

		return t;
	}
});
