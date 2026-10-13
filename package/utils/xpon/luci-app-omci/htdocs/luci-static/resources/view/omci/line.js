'use strict';
'require view';
'require poll';
'require dom';
'require fs';
'require ui';

var DEV = '/sys/class/net/pon0/device/';
var ROGUE = '/etc/xpon-rogue';

function read(path) {
	return L.resolveDefault(fs.read(path), null);
}

/* "key=value" words, over one or several lines. */
function parse(raw) {
	var out = {};

	String(raw || '').split(/\s+/).forEach(function(word) {
		var i = word.indexOf('=');
		if (i > 0)
			out[word.substring(0, i)] = word.substring(i + 1);
	});
	return out;
}

function text(v) {
	return v == null || v === '' ? '-' : String(v).trim();
}

function row(label, value) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td left', 'width': '33%' }, label),
		E('td', { 'class': 'td left' }, value)
	]);
}

function table(title, rows) {
	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, title),
		E('table', { 'class': 'table' }, rows)
	]);
}

return view.extend({
	load: function() {
		return Promise.all([
			read(DEV + 'allow_tx'),
			read(DEV + 'tx_hold'),
			L.resolveDefault(fs.stat(ROGUE), null),
			read(DEV + 'sfu_offload'),
			read(DEV + 'gpon_ber'),
			read(DEV + 'pon_counters'),	/* 5 */
			read(DEV + 'tcont_stats')
		]);
	},

	handleRelease: function(ev) {
		return L.resolveDefault(fs.remove(ROGUE), null).then(function() {
			return fs.write(DEV + 'tx_hold', '0');
		}).then(function() {
			ui.addNotification(null, E('p', _('Transmitter hold released.')), 'info');
		}).catch(function(e) {
			ui.addNotification(null, E('p', _('Could not release the hold: %s').format(e.message)), 'error');
		});
	},

	renderAll: function(d) {
		var ber = parse(d[4]), c = parse(d[5]), crows = [], tx;

		if (text(d[1]) == '1')
			tx = E('span', {}, [
				d[2] ? _('Held off for good by the OLT (%s)').format(ROGUE) : _('Held off by the OLT'),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-negative',
					'click': ui.createHandlerFn(this, 'handleRelease')
				}, _('Release'))
			]);
		else
			tx = text(d[0]) == '1' ? _('Allowed') : _('Off');

		Object.keys(c).forEach(function(k) {
			crows.push(row(k, c[k]));
		});

		return E([], [
			table(_('Line'), [
				row(_('Transmitter'), tx),
				row(_('Hardware offload'), d[3] == null ? '-' :
					(text(d[3]) == 'Y' || text(d[3]) == '1' ? _('On') : _('Off')))
			]),
			table(_('Bit errors'), [
				row(_('BER interval'), ber.interval_frames > 0 ?
					_('%d frames').format(ber.interval_frames) : _('Not set by the OLT')),
				row(_('REI sent / failed'), '%s / %s'.format(text(ber.sent), text(ber.failed))),
				row(_('BIP errors / blocks'), '%s / %s'.format(text(ber.bip_errors), text(ber.bip_blocks)))
			]),
			crows.length ? table(_('Counters'), crows) : E([]),
			d[6] ? table(_('T-CONT counters'), [ E('tr', { 'class': 'tr' },
				E('td', { 'class': 'td left' }, E('pre', {}, text(d[6])))) ]) : E([])
		]);
	},

	render: function(d) {
		var node = E('div', {}, this.renderAll(d));

		poll.add(L.bind(function() {
			return this.load().then(L.bind(function(fresh) {
				dom.content(node, this.renderAll(fresh));
			}, this));
		}, this), 5);

		return E([], [
			E('h2', {}, _('GPON line')),
			E('div', { 'class': 'cbi-map-descr' },
				_('Transmitter, hardware offload and line counters of the PON port. Counters are totals since boot.')),
			node
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
