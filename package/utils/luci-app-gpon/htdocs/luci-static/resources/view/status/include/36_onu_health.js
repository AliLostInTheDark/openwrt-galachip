'use strict';
'require baseclass';
'require rpc';
'require tools.onu-health as health';

var callHealth = rpc.declare({ object: 'onu-health', method: 'status' });

return baseclass.extend({
	title: _('ONU diagnostics'),

	load: function() {
		return callHealth().catch(function(error) {
			return { rpc_error: String(error) };
		});
	},

	render: function(data) {
		data = data || {};
		var rows = health.describe(data), clients = [], previous = this.previousClients || {};
		var current = {}, uptime = Number(data.uptime);
		((data.wifi || {}).interfaces || []).forEach(function(iface) {
			if (iface.stations_rc !== 0)
				return;
			health.parseStations(iface.stations).forEach(function(station) {
				station.interface = iface.name;
				clients.push(station);
				current[iface.name + '/' + station.mac] = true;
			});
		});
		var complete = ((data.wifi || {}).interfaces || []).every(function(iface) {
			return iface.info_rc === 0 && iface.stations_rc === 0;
		});
		if (!data.rpc_error && data.wifi && data.wifi.inventory_rc === 0 && complete) {
			if (isFinite(uptime) && uptime >= this.previousUptime) {
				Object.keys(previous).forEach(function(peer) {
					if (!current[peer])
						rows.push({ component: peer, level: 'warning', message: _('Client no longer associated since the previous update. A disconnect alone does not identify a hardware fault.') });
				});
			}
			this.previousClients = current;
			this.previousUptime = uptime;
		}
		var problems = rows.filter(function(row) { return row.level === 'warning' || row.level === 'danger'; });
		if (!problems.length) {
			/* nothing to report: the status page keeps a section it once showed, so hide it again */
			var none = E('div', {});
			if (typeof window !== 'undefined')
				window.setTimeout(function() {
					var section = none.parentNode && none.parentNode.parentNode;
					if (section)
						section.style.display = 'none';
				}, 0);
			return none;
		}
		var table = E('table', { 'class': 'table' });
		problems.forEach(function(row) {
			table.appendChild(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td', 'width': '25%' }, [ row.component ]),
				E('td', { 'class': 'td' }, [ E('span', { 'class': 'label ' + row.level }, [ _(row.message) ]) ])
			]));
		});
		var detail = E('table', { 'class': 'table' }, [ E('tr', { 'class': 'tr table-titles' },
			[ _('Interface / client'), _('TX packets'), _('Retries'), _('Failed TX'), _('Received signal') ].map(function(title) {
				return E('th', { 'class': 'th' }, [ title ]);
			})) ]);
		clients.forEach(function(station) {
			var signal = station.signal_avg == null ? station.signal : station.signal_avg;
			var values = [ station.interface + ' / ' + station.mac, station.tx_packets,
				station.tx_retries, station.tx_failed, signal == null ? null : signal + ' dBm' ];
			detail.appendChild(E('tr', { 'class': 'tr' }, values.map(function(value) {
				return E('td', { 'class': 'td' }, [ value == null ? _('Unavailable') : String(value) ]);
			})));
		});
		var download = E('button', { 'class': 'btn cbi-button', 'click': function() {
			var contents = { captured_at: new Date().toISOString(), status: rows, snapshot: data };
			var url = window.URL.createObjectURL(new Blob([ JSON.stringify(contents, null, 2) + '\n' ], { type: 'application/json' }));
			var link = E('a', { href: url, download: 'onu-diagnostics.json' });
			document.body.appendChild(link);
			link.click();
			link.remove();
			window.setTimeout(function() { window.URL.revokeObjectURL(url); }, 1000);
		} }, [ _('Download diagnostics') ]);
		return E('div', {}, [ table, clients.length ? detail : '', download ]);
	}
});
