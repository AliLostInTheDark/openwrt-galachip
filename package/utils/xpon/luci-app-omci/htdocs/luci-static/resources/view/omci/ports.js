'use strict';
'require view';
'require form';
'require uci';
'require omci.rpc as omci';

function portSpec(ports, name) {
	return (ports || []).find(function(p) { return p.split(':')[0] == name; });
}

return view.extend({
	load: function() {
		return Promise.all([ uci.load('network'), omci.vlanStatus() ]);
	},

	render: function(data) {
		var status = data[1], ports = status.port_tagging_ports || [];
		var bridge = uci.sections('network', 'device').find(function(s) { return s.name == 'br-lan'; });
		var reserved = status.reserved_vids || [];
		var o;

		if (!bridge || !status.port_vlan || !ports.length)
			return E('p', {}, _('Per-port bridge VLAN control is unavailable on this device. Use the global LAN VLAN mode in General settings.'));

		var m = new form.Map('network', _('LAN port VLANs'),
			_('Configure VLAN membership for each LAN port. Native assigns untagged incoming frames to that VLAN; tagged or untagged controls outgoing frames. The OLT must provision a service before that VLAN can reach the PON. VLANs without a PON service remain local.'));
		var s = m.section(form.NamedSection, bridge['.name'], 'device', _('Bridge VLAN policy'));
		s.addremove = false;

		o = s.option(form.Flag, 'vlan_filtering', _('Enable per-port VLAN control'));
		o.default = '0';
		o.rmempty = false;
		o.description = _('When disabled, the global LAN VLAN mode applies. When enabled, this table controls the bridge. Include your current LAN port in the management VLAN to retain access. Save & Apply uses LuCI’s normal connectivity rollback.');

		ports.forEach(function(port) {
			o = s.option(form.ListValue, 'omci_pcp_' + port, _('%s upstream priority').format(port.toUpperCase()));
			o.value('-1', _('Preserve packet priority'));
			for (var p = 0; p < 8; p++)
				o.value(String(p), String(p));
			o.default = '-1';
			o.rmempty = false;
			o.depends('vlan_filtering', '1');
			o.description = _('Optionally replace the 802.1p priority sent towards the OLT. Downstream priority is preserved.');
		});

		if (uci.get('network', 'lan') == 'interface') {
			s = m.section(form.NamedSection, 'lan', 'interface', _('ONU management'));
			o = s.option(form.Value, 'device', _('Management device'));
			o.rmempty = false;
			o.placeholder = 'br-lan.1';
			o.description = _('Use br-lan.VID for a VLAN with ONU (CPU) membership, for example br-lan.1. Your management IP address is preserved.');
			o.validate = function(id, value) {
				var match = /^br-lan(?:\.([1-9][0-9]{0,3}))?$/.exec(value);
				return match && (!match[1] || (Number(match[1]) <= 4094 &&
					reserved.indexOf(Number(match[1])) < 0)) ? true : _('Use br-lan or br-lan.VID with an available VLAN ID (1–4094).');
			};
		}

		s = m.section(form.GridSection, 'bridge-vlan', _('VLAN membership'));
		s.anonymous = true;
		s.addremove = true;
		s.sortable = true;
		s.filter = function(id) { return uci.get('network', id, 'device') == 'br-lan'; };
		s.add = function() {
			var id = uci.add('network', 'bridge-vlan');
			uci.set('network', id, 'device', 'br-lan');
			uci.set('network', id, 'ports', [ 'pon0:t' ]);
			return id;
		};

		o = s.option(form.Value, 'vlan', _('VLAN ID'));
		o.datatype = 'range(1,4094)';
		o.editable = true;
		o.rmempty = false;
		o.validate = function(id, value) {
			var vid = Number(value);
			if (reserved.indexOf(vid) >= 0)
				return _('VLAN %d is reserved by this device.').format(vid);
			var duplicate = uci.sections('network', 'bridge-vlan').some(function(row) {
				return row['.name'] != id && row.device == 'br-lan' &&
					Number(this.formvalue(row['.name'])) == vid;
			}, this);
			return duplicate ? _('Each VLAN may appear only once.') : true;
		};

		o = s.option(form.Flag, 'local', _('ONU (CPU)'));
		o.editable = true;
		o.default = '1';
		o.rmempty = false;
		o.description = _('Allow the ONU itself to use this VLAN. Set the LAN interface device under Network → Interfaces to br-lan.VID for management on that VLAN.');

		ports.concat([ 'pon0' ]).forEach(function(port) {
			var option = s.option(form.ListValue, '_port_' + port,
				port == 'pon0' ? _('PON') : port.toUpperCase());
			option.value('-', _('Not a member'));
			option.value('t', _('Tagged'));
			if (port != 'pon0') {
				option.value('u', _('Untagged'));
				option.value('t*', _('Tagged, native'));
				option.value('u*', _('Untagged, native'));
			}
			option.rmempty = false;
			option.editable = true;
			option.cfgvalue = function(id) {
				var spec = portSpec(uci.get('network', id, 'ports'), port);
				return spec ? (spec.split(':')[1] || 'u*') : '-';
			};
			option.write = function(id, value) {
				var list = (uci.get('network', id, 'ports') || []).filter(function(p) {
					return p.split(':')[0] != port;
				});
				if (value != '-')
					list.push(port + ':' + value);
				uci.set('network', id, 'ports', list);
			};
			option.validate = function(id, value) {
				if (value.indexOf('*') < 0)
					return true;
				var duplicate = uci.sections('network', 'bridge-vlan').some(function(row) {
					return row['.name'] != id && row.device == 'br-lan' &&
						(this.formvalue(row['.name']) || '').indexOf('*') >= 0;
				}, this);
				return duplicate ? _('Each LAN port may have only one native VLAN.') : true;
			};
		});

		return m.render();
	}
});
