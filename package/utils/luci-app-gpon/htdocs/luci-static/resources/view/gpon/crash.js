'use strict';
'require view';
'require rpc';
'require ui';

var callCrash = rpc.declare({ object: 'gpon', method: 'crash', expect: { '': {} } });

var CRASH_KINDS = [ 'panic', 'pretimeout', 'unclean' ];

function stamp() {
	return new Date().toISOString().replace(/[:.]/g, '-');
}

return view.extend({
	load: function() {
		return L.resolveDefault(callCrash(), {});
	},

	records: function(text) {
		var out = [], cur = null;
		(text || '').split('\n').forEach(function(line) {
			var m = line.match(/^--- seq=(\d+) kind=(\S+) boot=(\d+) uptime=(\d+)s$/);
			if (m) {
				cur = { seq: +m[1], kind: m[2], boot: +m[3], uptime: +m[4], log: '' };
				out.push(cur);
			} else if (cur && line) {
				cur.log += line + '\n';
			}
		});
		return out;
	},

	report: function(r) {
		return [
			'ONU crash report',
			'model:    ' + (r.model || 'unknown'),
			'firmware: ' + (r.release || 'unknown'),
			'kernel:   ' + (r.kernel || 'unknown'),
			'saved:    ' + new Date().toISOString(),
			'',
			r.text || ''
		].join('\n');
	},

	save: function(r) {
		var name = 'crash-report-' + (r.model || 'onu').replace(/[^A-Za-z0-9]+/g, '_') + '-' + stamp() + '.txt';
		var a = E('a', { 'href': URL.createObjectURL(new Blob([ this.report(r) ], { 'type': 'text/plain' })), 'download': name });
		document.body.appendChild(a);
		a.click();
		document.body.removeChild(a);
	},

	render: function(r) {
		var intro = E('p', {}, _('What this unit kept in its own flash about its last resets: a kernel panic with its log, or an UNCLEAN reset (the watchdog or a power cut). If it crashed, download the report and send it to contact@herman-brule.com or attach it to an issue at https://github.com/alphaonex86/openwrt/issues.'));

		if (!r.available)
			return E('div', { 'class': 'cbi-map' }, [
				E('h2', {}, _('Crash')), intro,
				E('p', { 'class': 'alert-message' }, _('This unit keeps no crash records yet: its flash has no crashlog partition.'))
			]);

		var recs = this.records(r.text);
		var crashes = recs.filter(function(x) { return CRASH_KINDS.indexOf(x.kind) >= 0; });
		var rows = recs.map(function(x) {
			var bad = CRASH_KINDS.indexOf(x.kind) >= 0;
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, String(x.seq)),
				E('td', { 'class': 'td' }, bad ? E('strong', {}, x.kind.toUpperCase()) : x.kind),
				E('td', { 'class': 'td' }, String(x.boot)),
				E('td', { 'class': 'td' }, '%ds'.format(x.uptime)),
				E('td', { 'class': 'td' }, x.log ? E('details', {}, [ E('summary', {}, x.log.split('\n')[0]), E('pre', {}, x.log) ]) : '')
			]);
		});

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', {}, _('Crash')), intro,
			E('p', {}, crashes.length ? E('strong', {}, _('%d crash(es) recorded.').format(crashes.length)) : _('No crash recorded.')),
			E('button', { 'class': 'cbi-button cbi-button-action', 'click': ui.createHandlerFn(this, 'save', r) }, [ _('Download the crash report') ]),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, _('Record')), E('th', { 'class': 'th' }, _('Kind')),
					E('th', { 'class': 'th' }, _('Boot')), E('th', { 'class': 'th' }, _('Uptime')),
					E('th', { 'class': 'th' }, _('Kernel log'))
				])
			].concat(rows))
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
