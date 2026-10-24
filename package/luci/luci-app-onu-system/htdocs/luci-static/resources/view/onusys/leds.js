'use strict';
'require view';
'require rpc';
'require uci';
'require poll';
'require onusys.model as model';

/*
 * LED control: one row per LED in /sys/class/leds with its live state and a
 * mode. Saved as "config led" sections in /etc/config/system (applied by
 * /etc/init.d/led); PON and LOS fixed states are kept by led-hold.
 */

const callLEDs = rpc.declare({ object: 'luci', method: 'getLEDs', expect: { '': {} } });

function modeLabel(name, mode, s) {
	switch (mode) {
	case 'auto':      return _('Automatic (fibre status)');
	case 'default':   return _('Firmware default');
	case 'on':        return _('Always on');
	case 'off':       return _('Off');
	case 'netdev':    return _('Network activity (%s)').format(s?.dev || 'eth0');
	case 'heartbeat': return _('Heartbeat');
	case 'timer':     return _('Blink');
	default:          return _('Custom (%s)').format(s?.trigger ?? '?');
	}
}

function stateLabel(state) {
	return { on: _('on'), off: _('off'), blinking: _('blinking'), activity: _('network activity') }[state] ?? state;
}

const COLORS = { green: '#16a34a', red: '#dc2626', blue: '#2563eb', amber: '#d97706', orange: '#ea580c', yellow: '#ca8a04', white: '#cbd5e1' };

return view.extend({
	load() {
		return Promise.all([ callLEDs(), uci.load('system') ]);
	},

	sectionFor(name) {
		return uci.sections('system', 'led').find((s) => s.sysfs == name) ?? null;
	},

	describe(name) {
		const fn = model.parseName(name).fn;

		if (!model.isDriverLed(name))
			return '';
		if (fn == 'los')
			return _('Set by the PON driver: blinks while there is no light on the fibre.');

		return _('Set by the PON driver: on when registered with the OLT, blinking while connecting.');
	},

	renderRow(name, led) {
		const s = this.sectionFor(name);
		const mode = model.modeOf(name, s);
		const modes = model.modesFor(name, led.triggers);
		const color = model.parseName(name).color;
		const id = 'led-' + name.replace(/[^a-zA-Z0-9]+/g, '-');

		if (!modes.includes(mode))
			modes.push(mode);

		return E('div', { 'class': 'cbi-value onusys-led', 'data-led': name }, [
			E('label', { 'class': 'cbi-value-title', 'for': id }, [
				E('span', { 'class': 'onusys-dot', 'style': 'background:' + (COLORS[color] ?? '#94a3b8') }),
				model.label(name)
			]),
			E('div', { 'class': 'cbi-value-field' }, [
				E('select', { 'class': 'cbi-input-select', 'id': id, 'data-led': name, 'disabled': this.readonly ? '' : null },
					modes.map((m) => E('option', { 'value': m, 'selected': m == mode ? '' : null }, [ modeLabel(name, m, s) ]))),
				E('div', { 'class': 'cbi-value-description' }, [
					E('span', { 'class': 'onusys-state' }, [ _('Now: %s').format(stateLabel(model.stateOf(led))) ]),
					' · ', E('code', {}, [ name ]),
					this.describe(name) ? E('div', {}, [ this.describe(name) ]) : ''
				])
			])
		]);
	},

	setAll(pick) {
		document.querySelectorAll('select[data-led]').forEach((sel) => {
			const name = sel.dataset.led;
			const want = pick(name, this.leds[name]?.triggers);

			if ([ ...sel.options ].some((o) => o.value == want))
				sel.value = want;
		});
	},

	updateStates(leds) {
		for (const name in leds) {
			const el = document.querySelector(`.onusys-led[data-led="${CSS.escape(name)}"] .onusys-state`);

			if (el)
				el.textContent = _('Now: %s').format(stateLabel(model.stateOf(leds[name])));
		}
	},

	render([ leds ]) {
		this.leds = leds ?? {};
		this.readonly = !L.hasViewPermission();

		const names = model.sortNames(Object.keys(this.leds));

		poll.add(() => callLEDs().then((l) => this.updateStates(l)), 5);

		return E([], [
			E('style', {}, [ '.onusys-dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:8px;vertical-align:middle;box-shadow:0 0 0 2px rgba(0,0,0,.06)}' +
				'.onusys-actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:16px}' ]),
			E('h2', {}, [ _('LEDs') ]),
			E('div', { 'class': 'cbi-map-descr' }, [ _('Choose what each light on the device does. Changes take effect with Save & Apply.') ]),
			E('div', { 'class': 'cbi-section' }, names.length ? [
				E('div', { 'class': 'cbi-section-node' }, names.map((n) => this.renderRow(n, this.leds[n]))),
				E('div', { 'class': 'onusys-actions' }, [
					E('button', { 'class': 'btn cbi-button', 'type': 'button', 'disabled': this.readonly ? '' : null,
						'click': () => this.setAll(() => 'off') }, [ _('All LEDs off') ]),
					E('button', { 'class': 'btn cbi-button', 'type': 'button', 'disabled': this.readonly ? '' : null,
						'click': () => this.setAll((n, t) => model.defaultMode(n, t)) }, [ _('Restore defaults') ])
				])
			] : [ E('em', {}, [ _('This device has no controllable LEDs.') ]) ])
		]);
	},

	handleSave() {
		const existing = () => uci.sections('system', 'led').map((s) => s['.name']);

		document.querySelectorAll('select[data-led]').forEach((sel) => {
			const name = sel.dataset.led, mode = sel.value;
			const s = this.sectionFor(name);

			if (mode == 'custom' || model.modeOf(name, s) == mode)
				return;

			const values = model.sectionValues(name, mode, { dev: s?.dev });

			if (!values) {
				if (s)
					uci.remove('system', s['.name']);
				return;
			}

			const sid = s ? s['.name'] : uci.add('system', 'led', model.sectionName(name, existing()));

			for (const k of model.MANAGED)
				if (!(k in values))
					uci.unset('system', sid, k);

			for (const k in values)
				uci.set('system', sid, k, values[k]);
		});

		return uci.save();
	},

	handleReset: null
});
