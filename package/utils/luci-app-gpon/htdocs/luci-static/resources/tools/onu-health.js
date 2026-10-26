'use strict';
'require baseclass';

function integer(value) {
	return /^-?\d+$/.test(String(value)) ? Number(value) : null;
}

function parseGpon(text) {
	var result = {};
	String(text || '').split('\n').forEach(function(line) {
		var m = line.match(/^([a-z_]+):\s*(\S+)\s*$/);
		if (m)
			result[m[1]] = m[1] === 'optics_backend' ? m[2] : integer(m[2]);
	});
	return result;
}

function parseStations(text) {
	var stations = [], station;
	String(text || '').split('\n').forEach(function(line) {
		var m = line.match(/^Station ([0-9a-f:]{17})\b/i);
		if (m) {
			station = { mac: m[1] };
			stations.push(station);
			return;
		}
		if (!station)
			return;
		m = line.match(/^\s*(tx packets|rx packets|tx retries|tx failed|inactive time|connected time):\s*(\d+)/);
		if (m)
			station[m[1].replace(/ /g, '_')] = Number(m[2]);
		m = line.match(/^\s*(signal|signal avg):\s*(-?\d+)(?:\s+\[([^\]]+)\])?/);
		if (m) {
			station[m[1].replace(/ /g, '_')] = Number(m[2]);
			if (m[3])
				station[m[1].replace(/ /g, '_') + '_chains'] = m[3];
		}
	});
	return stations;
}

function describe(data) {
	data = data || {};
	var rows = [], gpon = parseGpon(data.gpon && data.gpon.snapshot), wifi = data.wifi || {};
	function add(component, level, message) {
		rows.push({ component: component, level: level, message: message });
	}
	if (data.rpc_error)
		add('Diagnostics', 'warning', 'Diagnostics unavailable: ' + data.rpc_error);
	if (!data.gpon || !data.gpon.available || gpon.driver_ready == null || gpon.init_error == null)
		add('GPON hardware/driver', 'warning', 'Device diagnostics unavailable; hardware health is unknown.');
	else if (gpon.init_error != null && gpon.init_error !== 0)
		add('GPON hardware/driver', 'danger', 'Optical initialization failed (error ' + gpon.init_error + '). Check hardware, calibration and driver diagnostics.');
	else if (gpon.driver_ready !== 1 || gpon.activation_ready !== 1)
		add('GPON hardware/driver', 'warning', 'Device is not activated. Configuration or initialization needs checking; this does not establish physical damage.');
	else
		add('GPON hardware/driver', 'success', 'No initialization error reported. This is not a hardware self-test.');
	if (gpon.los === 1)
		add('Fiber link', 'warning', 'LOS: no optical signal. A fiber or upstream link problem is not a chip failure.');
	else if (gpon.onu_state === 5)
		add('Fiber link', 'success', 'O5: registered with the OLT. Internet access is a separate check.');
	else if (gpon.los === 0)
		add('Fiber link', 'warning', 'Optical signal present; ONU is not in O5. Check registration and provisioning.');
	else
		add('Fiber link', 'warning', 'Optical signal status is not available.');

	if (wifi.configuration_rc !== 0 || wifi.configured_radios == null)
		add('WiFi hardware/driver', 'warning', 'Radio configuration could not be read.');
	else if (!wifi.configured_radios)
		add('WiFi', 'notice', 'No radio configured.');
	else if (!wifi.enabled_radios)
		add('WiFi', 'notice', 'Radios are disabled in configuration.');
	else if (!(wifi.radios || []).length)
		add('WiFi hardware/driver', 'danger', 'An enabled radio has no registered device. Check driver initialization, firmware and hardware.');
	else {
		add('WiFi hardware/driver', 'notice', 'Radio detected. This does not prove that frames reach a client.');
		if (wifi.inventory_rc !== 0)
			add('WiFi diagnostics', 'warning', 'Wireless interface inventory failed.');
		var clients = 0;
		(wifi.interfaces || []).forEach(function(iface) {
			if (iface.info_rc !== 0 || iface.stations_rc !== 0) {
				add(iface.name, 'warning', 'Wireless counters could not be read.');
				return;
			}
			parseStations(iface.stations).forEach(function(station) {
				clients++;
				if (station.tx_failed > 0)
					add('WiFi client ' + station.mac, 'warning', station.tx_failed + ' failed transmissions recorded during this association. Retries: ' + (station.tx_retries == null ? 'unavailable' : station.tx_retries) + '. This is a link symptom, not proof of a damaged chip.');
				else if (station.tx_failed == null)
					add('WiFi client ' + station.mac, 'warning', 'Transmission failure counter is unavailable.');
			});
		});
		if (!clients)
			add('WiFi link', 'notice', 'No associated client observed; client connectivity has not been tested.');
	}
	(wifi.driver_counters || []).forEach(function(item) {
		if (item.rc !== 0) {
			add('WiFi diagnostics', 'warning', 'Driver counters could not be read.');
			return;
		}
		var match = String(item.snapshot).match(/\btxdma_err[=:\s]+(\d+)/);
		var hang = String(item.snapshot).match(/\bhang_resets[=:\s]+(\d+)/);
		if ((match && Number(match[1])) || (hang && Number(hang[1])))
			add('WiFi hardware/driver', 'danger', 'The driver recorded DMA errors or transmit hangs. Inspect the saved counters and kernel log; this does not alone identify a physical fault.');
	});
	return rows;
}

return baseclass.extend({ parseGpon: parseGpon, parseStations: parseStations, describe: describe });
