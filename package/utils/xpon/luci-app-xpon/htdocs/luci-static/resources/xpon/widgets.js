'use strict';
'require baseclass';
'require fs';
'require rpc';
'require ui';

/*
 * Shared GPON/EPON status widgets: optical power against the module's own
 * warning and alarm levels (the class window when it has none),
 * the activation stages, and what saving a setting costs. Colours come from
 * the Slate tokens, with LuCI's generic variables as the fallback.
 */

var CSS = [
	'.xpon-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,300px),1fr));gap:16px;margin:0 0 20px}',
	'.xpon-panel{min-width:0;padding:16px 18px;border:1px solid var(--border,var(--border-color-medium,#d9dde3));border-radius:var(--radius,12px);background:var(--surface,var(--background-color-high,#fff));box-shadow:var(--shadow,none)}',
	'.xpon-panel>h4{margin:0 0 10px;padding:0;border:0;font-size:13px;font-weight:600;color:var(--text-2,var(--text-color-medium,#555))}',
	'.xpon-pill{display:inline-flex;align-items:center;gap:6px;padding:2px 9px;border-radius:999px;font-size:12px;font-weight:600;white-space:nowrap;background:var(--surface-2,var(--background-color-medium,#eef0f3));color:var(--text-2,var(--text-color-medium,#555))}',
	'.xpon-pill::before{content:"";width:6px;height:6px;border-radius:50%;background:currentColor}',
	'.xpon-ok{background:var(--success-soft,rgba(16,185,129,.12));color:var(--success,#047857)}',
	'.xpon-warn{background:var(--warn-soft,rgba(245,158,11,.14));color:var(--warn,#b45309)}',
	'.xpon-bad{background:var(--danger-soft,rgba(220,38,38,.1));color:var(--danger,#dc2626)}',
	'.xpon-gauge-head{display:flex;align-items:center;justify-content:space-between;gap:8px}',
	'.xpon-gauge-title{font-weight:600}',
	'.xpon-gauge-value{margin:6px 0 12px;font-size:34px;font-weight:650;line-height:1.1;letter-spacing:-.02em;font-variant-numeric:tabular-nums}',
	'.xpon-gauge-value small{margin-left:6px;font-size:14px;font-weight:500;letter-spacing:0;color:var(--text-3,var(--text-color-low,#888))}',
	'.xpon-gauge-bar{position:relative;height:10px;border-radius:999px;background:var(--surface-3,var(--background-color-low,#e5e7eb))}',
	'.xpon-gauge-window{position:absolute;top:0;bottom:0;border-radius:999px;background:var(--accent-soft,rgba(16,185,129,.18));box-shadow:inset 0 0 0 1px var(--accent-ring,rgba(16,185,129,.4))}',
	'.xpon-gauge-alarm{position:absolute;top:-2px;bottom:-2px;width:2px;margin-left:-1px;background:var(--danger,#dc2626)}',
	'.xpon-gauge-mark{position:absolute;top:-4px;width:4px;height:18px;margin-left:-2px;border-radius:2px;background:var(--text,var(--text-color-highest,#111));box-shadow:0 0 0 2px var(--surface,var(--background-color-high,#fff))}',
	'.xpon-gauge-scale{display:flex;justify-content:space-between;margin-top:6px;font-size:11px;font-variant-numeric:tabular-nums;color:var(--text-3,var(--text-color-low,#888))}',
	'.xpon-gauge-note{margin-top:8px;font-size:12px;color:var(--text-2,var(--text-color-medium,#555))}',
	'.xpon-stages{display:flex;gap:4px;margin:2px 0 12px;container-type:inline-size}',
	'.xpon-stage{flex:1 1 0;min-width:0;padding:7px 4px;border-radius:var(--radius-sm,8px);text-align:center;font-size:12px;font-weight:600;line-height:1.25;background:var(--surface-2,var(--background-color-medium,#eef0f3));color:var(--text-3,var(--text-color-low,#888))}',
	'.xpon-stage span{display:block;overflow:hidden;font-size:11px;font-weight:500;white-space:nowrap;text-overflow:ellipsis}',
	'.xpon-stage.done{background:var(--accent-soft,rgba(16,185,129,.14));color:var(--accent,#047857)}',
	'.xpon-stage.current{background:var(--accent,#047857);color:var(--on-accent,#fff)}',
	'.xpon-stage.fail{background:var(--danger-soft,rgba(220,38,38,.1));color:var(--danger,#dc2626)}',
	'.xpon-stage-text{margin:0;color:var(--text-2,var(--text-color-medium,#555))}',
	'.xpon-cost{display:inline-flex;align-items:center;gap:5px;margin-right:6px;padding:1px 8px;border-radius:999px;font-size:11px;font-weight:600;line-height:18px;white-space:nowrap;vertical-align:1px}',
	'.xpon-cost::before{content:"";width:6px;height:6px;border-radius:50%;background:currentColor}',
	'.xpon-cost-live{background:var(--success-soft,rgba(16,185,129,.12));color:var(--success,#047857)}',
	'.xpon-cost-next{background:var(--info-soft,rgba(59,130,246,.1));color:var(--info,#1d4ed8)}',
	'.xpon-cost-drop{background:var(--warn-soft,rgba(245,158,11,.14));color:var(--warn,#b45309)}',
	'.xpon-legend{display:flex;flex-wrap:wrap;gap:6px 16px;margin:0 0 16px;font-size:12px;color:var(--text-2,var(--text-color-medium,#555))}',
	'.xpon-legend>div{display:flex;align-items:center;flex-wrap:wrap}',
	'.xpon-cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,260px),1fr));gap:12px}',
	'.xpon-card{min-width:0;padding:12px 14px;border:1px solid var(--border,var(--border-color-medium,#d9dde3));border-radius:var(--radius-md,10px);background:var(--surface-2,var(--background-color-medium,#f6f7f9))}',
	'.xpon-card>strong{display:block;margin-bottom:4px;font-size:15px}',
	'.xpon-card>p{margin:2px 0;font-size:13px;color:var(--text-2,var(--text-color-medium,#555));overflow-wrap:anywhere}',
	'.xpon-lead{margin:0 0 12px;overflow-wrap:anywhere}',
	/* The PON pages are read-outs: an arrow, not the text I-beam, except where one types. */
	'body:is([data-page^="admin-network-omci"],[data-page^="admin-network-xpon"],[data-page^="admin-network-epon"]) #maincontent{cursor:default}',
	'body:is([data-page^="admin-network-omci"],[data-page^="admin-network-xpon"],[data-page^="admin-network-epon"]) #maincontent :is(input:not([type=checkbox],[type=radio],[type=button],[type=submit],[type=reset]),textarea){cursor:text}',
	'body:is([data-page^="admin-network-omci"],[data-page^="admin-network-xpon"],[data-page^="admin-network-epon"]) #maincontent :is(a[href],button,select,summary,label[for],.cbi-button,.btn){cursor:pointer}',
	/* Stage names need about 80px each; below that the sentence says where it stands. */
	'@container (max-width:420px){.xpon-stage span{display:none}.xpon-stage{padding:8px 2px}}',
	'@media (max-width:600px){.xpon-panel{padding:14px}.xpon-gauge-value{font-size:28px}}'
].join('');

var costs = {
	live: [ _('Applies at once'), _('takes effect when saved') ],
	next: [ _('At next registration'), _('saved now; the OLT reads it when the ONU registers again') ],
	drop: [ _('Interrupts service'), _('the ONU registers or discovers OAM again; service drops for a few seconds') ]
};

var callPublic = rpc.declare({
	object: 'xpon.public',
	method: 'status',
	expect: { '': {} }
});

var limitsCache;

/* bosa_thresholds in nW, as xpon.public passes them, to dBm levels. */
function levels(t, dir) {
	var dbm = function(key) {
		var n = Number(t['%s_%s_nw'.format(dir, key)]);
		return Number.isFinite(n) && n > 0 ? 10 * Math.log10(n / 1e6) : null;
	};
	var l = { alarmLo: dbm('alarm_low'), warnLo: dbm('warn_low'), warnHi: dbm('warn_high'), alarmHi: dbm('alarm_high') };

	/* Only a complete, ordered set describes the module. */
	return l.alarmLo != null && l.alarmLo <= l.warnLo && l.warnLo < l.warnHi && l.warnHi <= l.alarmHi ? l : null;
}

function clamp(v, lo, hi) {
	return Math.min(hi, Math.max(lo, v));
}

return baseclass.extend({
	style: function() {
		if (!document.getElementById('xpon-widgets-style'))
			document.head.appendChild(E('style', { 'id': 'xpon-widgets-style' }, CSS));
	},

	/* SFF-8472 power in nW to dBm; null without light. */
	dbm: function(nw) {
		var n = Number(nw);
		return Number.isFinite(n) && n > 0 ? 10 * Math.log10(n / 1e6) : null;
	},

	/* The module's RX and TX levels, read once per page; null fields without them. */
	limits: function() {
		if (!limitsCache)
			limitsCache = L.resolveDefault(callPublic(), {}).then(function(p) {
				var t = (p && p.thresholds) || {};
				return { rx: levels(t, 'rx'), tx: levels(t, 'tx') };
			});
		return limitsCache;
	},

	panel: function(title, children) {
		return E('div', { 'class': 'xpon-panel' }, [ title ? E('h4', {}, title) : '' ].concat(children));
	},

	/*
	 * @g: title, value (dBm or null), scale min/max, the class window lo/hi
	 * with the names of its edges, note, the text for no reading, and the
	 * module's levels (limits), which replace the class window when known.
	 */
	gauge: function(g) {
		var span = g.max - g.min, pct = function(v) { return clamp((v - g.min) / span * 100, 0, 100); };
		var v = g.value, m = g.limits, pill, cls, margin = '', ticks = [], note = g.note, i;
		var lo = m ? m.warnLo : g.lo, hi = m ? m.warnHi : g.hi;
		var loName = m ? _('the low warning') : g.loName, hiName = m ? _('the high warning') : g.hiName;

		if (v == null) {
			pill = g.empty;
			cls = g.emptyBad ? 'xpon-bad' : '';
		} else if (m && (v < m.alarmLo || v > m.alarmHi)) {
			pill = _('Alarm');
			cls = 'xpon-bad';
		} else if (v < lo || v > hi) {
			pill = m ? _('Warning') : v < lo ? _('Below window') : _('Above window');
			cls = m ? 'xpon-warn' : 'xpon-bad';
		} else if (!m && Math.min(v - lo, hi - v) < 3) {
			pill = _('Near limit');
			cls = 'xpon-warn';
		} else {
			pill = m ? _('In range') : _('In window');
			cls = 'xpon-ok';
		}

		if (v != null)
			margin = (v >= lo ? _('%.1f dB above %s').format(v - lo, loName) : _('%.1f dB below %s').format(lo - v, loName)) +
				' · ' +
				(v <= hi ? _('%.1f dB below %s').format(hi - v, hiName) : _('%.1f dB above %s').format(v - hi, hiName));
		if (m)
			note = [ _('Module: warning %.1f to %.1f dBm, alarm %.1f / %.1f dBm').format(m.warnLo, m.warnHi, m.alarmLo, m.alarmHi),
				 E('br'), g.note ];

		/* Round steps; scales are chosen so that the step divides them. */
		var step = [ 1, 2, 5, 10 ].filter(function(s) { return span / s <= 6; })[0] || 10;
		for (i = g.min; i <= g.max; i += step)
			ticks.push(E('span', {}, String(i)));

		return E('div', { 'class': 'xpon-gauge' }, [
			E('div', { 'class': 'xpon-gauge-head' }, [
				E('span', { 'class': 'xpon-gauge-title' }, g.title),
				E('span', { 'class': 'xpon-pill ' + cls }, pill)
			]),
			E('div', { 'class': 'xpon-gauge-value' }, v == null ? '–' : [ v.toFixed(2), E('small', {}, 'dBm') ]),
			E('div', { 'class': 'xpon-gauge-bar', 'role': 'img',
				   'aria-label': '%s: %s'.format(g.title, v == null ? pill : v.toFixed(2) + ' dBm') }, [
				E('div', { 'class': 'xpon-gauge-window', 'style': 'left:%.2f%%;width:%.2f%%'.format(pct(lo), pct(hi) - pct(lo)) }),
				m ? E('div', { 'class': 'xpon-gauge-alarm', 'style': 'left:%.2f%%'.format(pct(m.alarmLo)) }) : '',
				m ? E('div', { 'class': 'xpon-gauge-alarm', 'style': 'left:%.2f%%'.format(pct(m.alarmHi)) }) : '',
				v == null ? '' : E('div', { 'class': 'xpon-gauge-mark', 'style': 'left:%.2f%%'.format(pct(v)) })
			]),
			E('div', { 'class': 'xpon-gauge-scale' }, ticks),
			E('div', { 'class': 'xpon-gauge-note' }, margin ? [ margin, E('br') ].concat(note) : note)
		]);
	},

	/* @items: [ label, title ] pairs; stages before @current are done. */
	stages: function(items, current, failed, text) {
		return E([], [
			E('div', { 'class': 'xpon-stages' }, items.map(function(item, i) {
				var cls = i < current ? ' done' : i == current ? (failed ? ' fail' : ' current') : '';

				return E('div', { 'class': 'xpon-stage' + cls, 'title': item[1],
						  'aria-current': i == current ? 'step' : null }, [ item[0], E('span', {}, item[1]) ]);
			})),
			E('p', { 'class': 'xpon-stage-text' }, text)
		]);
	},

	/* Badge markup for a form option description. */
	cost: function(kind) {
		return '<span class="xpon-cost xpon-cost-%s">%s</span>'.format(kind, costs[kind][0]);
	},

	annotate: function(option, kind) {
		option.description = this.cost(kind) + (option.description || '');
	},

	legend: function(kinds) {
		return E('div', { 'class': 'xpon-legend' }, kinds.map(function(kind) {
			return E('div', {}, [ E('span', { 'class': 'xpon-cost xpon-cost-' + kind }, costs[kind][0]), costs[kind][1] ]);
		}));
	},

	/* xpon-report masks serial numbers, passwords and LOIDs by default. */
	report: function() {
		var save = function(text) {
			var url = URL.createObjectURL(new Blob([ text ], { type: 'text/plain' }));
			var link = E('a', { 'href': url, 'download': 'xpon-report-%s.txt'.format(new Date().toISOString().replace(/[:.]/g, '-')) });

			document.body.appendChild(link);
			link.click();
			link.remove();
			window.setTimeout(function() { URL.revokeObjectURL(url); }, 1000);
		};

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Diagnostics')),
			E('p', {}, _('Collects the PON state, settings and the kernel log into one text file to attach to a bug report. Serial numbers, passwords and LOIDs are masked.')),
			E('button', {
				'class': 'cbi-button cbi-button-action',
				'click': ui.createHandlerFn(this, function() {
					return fs.exec_direct('/usr/sbin/xpon-report', [ '-o', '-' ]).then(save).catch(function(e) {
						ui.addNotification(null, E('p', _('Could not collect the report: %s').format(e.message || e)), 'error');
					});
				})
			}, _('Download diagnostic report'))
		]);
	},

	/* @lines: strings; the first line is the title. */
	card: function(lines) {
		return E('div', { 'class': 'xpon-card' }, [ E('strong', {}, lines[0]) ].concat(lines.slice(1).map(function(l) {
			return E('p', {}, l);
		})));
	}
});
