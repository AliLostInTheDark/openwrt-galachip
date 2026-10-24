'use strict';
'require baseclass';

/*
 * Pure helpers for the LED and password pages: no I/O, unit tested in
 * tests/test_model.py.
 *
 * LEDs are configured the OpenWrt way, as "config led" sections in
 * /etc/config/system that /etc/init.d/led applies. The xPON driver drives
 * the PON, LOS and fibre LEDs itself on every line-state change; a fixed
 * state for those gets "option hold '1'", and the led-hold service
 * re-applies it after the driver's changes.
 */

const DRIVER_LEDS = [ 'pon', 'los', 'fiber' ];
const NAMES = { power: 'Power', lan: 'LAN', wan: 'WAN', pon: 'PON', los: 'LOS', fiber: 'Fibre', internet: 'Internet', wlan: 'Wi-Fi', usb: 'USB' };
const ORDER = [ 'power', 'pon', 'los', 'fiber', 'lan', 'wan', 'internet', 'wlan', 'usb' ];
const MANAGED = [ 'trigger', 'default', 'hold', 'dev', 'mode', 'delayon', 'delayoff', 'interval', 'port' ];
const NO_LOGIN = /(^|\/)(false|nologin)$/;

return baseclass.extend({
	DRIVER_LEDS,
	MANAGED,

	/* "green:pon" / "tplink:green:pon" -> { color, fn } */
	parseName(name) {
		const parts = String(name ?? '').split(':');

		if (parts.length >= 3)
			return { color: parts[1] || null, fn: parts.slice(2).join(':') };
		if (parts.length == 2)
			return { color: parts[0] || null, fn: parts[1] };

		return { color: null, fn: parts[0] };
	},

	label(name) {
		const fn = this.parseName(name).fn;

		return NAMES[fn] ?? (fn.charAt(0).toUpperCase() + fn.slice(1)).replace(/[-_]+/g, ' ');
	},

	isDriverLed(name) {
		return DRIVER_LEDS.includes(this.parseName(name).fn);
	},

	/* LED names in display order: known functions first, then by name. */
	sortNames(names) {
		const rank = (n) => { const i = ORDER.indexOf(this.parseName(n).fn); return i < 0 ? ORDER.length : i; };

		return [ ...names ].sort((a, b) => rank(a) - rank(b) || (a < b ? -1 : a > b ? 1 : 0));
	},

	/* Modes offered for an LED, given the kernel triggers it supports. */
	modesFor(name, triggers) {
		if (this.isDriverLed(name))
			return [ 'auto', 'on', 'off' ];

		const has = (t) => (triggers ?? []).includes(t);
		const modes = [ 'default', 'on', 'off' ];

		if (has('netdev')) modes.push('netdev');
		if (has('heartbeat')) modes.push('heartbeat');
		if (has('timer')) modes.push('timer');

		return modes;
	},

	/* The mode a "config led" section (or none) stands for. */
	modeOf(name, s) {
		if (!s)
			return this.isDriverLed(name) ? 'auto' : 'default';

		const trigger = s.trigger ?? 'none';

		if (trigger == 'none')
			return (s['default'] == '1') ? 'on' : 'off';
		if (this.isDriverLed(name))
			return 'custom';
		if (trigger == 'default-on')
			return 'on';
		if ([ 'netdev', 'heartbeat', 'timer' ].includes(trigger))
			return trigger;

		return 'custom';
	},

	/* Options to write for a mode, or null to remove the section. */
	sectionValues(name, mode, opts) {
		const base = { name: this.label(name), sysfs: name };

		if (mode == 'auto' || mode == 'default')
			return null;

		if (this.isDriverLed(name)) {
			if (mode != 'on' && mode != 'off')
				throw new Error('unsupported mode ' + mode);

			return { ...base, trigger: 'none', 'default': mode == 'on' ? '1' : '0', hold: '1' };
		}

		switch (mode) {
		case 'on':        return { ...base, trigger: 'default-on', 'default': '1' };
		case 'off':       return { ...base, trigger: 'none', 'default': '0' };
		case 'netdev':    return { ...base, trigger: 'netdev', dev: opts?.dev || 'eth0', mode: 'link tx rx' };
		case 'heartbeat': return { ...base, trigger: 'heartbeat' };
		case 'timer':     return { ...base, trigger: 'timer', delayon: '500', delayoff: '500' };
		}

		throw new Error('unsupported mode ' + mode);
	},

	/* The firmware's out-of-the-box behaviour, for "Restore defaults". */
	defaultMode(name, triggers) {
		const fn = this.parseName(name).fn;

		if (this.isDriverLed(name))
			return 'auto';
		if ([ 'lan', 'wan', 'internet' ].includes(fn) && (triggers ?? []).includes('netdev'))
			return 'netdev';
		if (fn == 'power')
			return 'on';

		return 'default';
	},

	/* Name for a new section: "green:pon" -> "led_pon" (unique among existing ones). */
	sectionName(name, existing) {
		const base = 'led_' + this.parseName(name).fn.toLowerCase().replace(/[^a-z0-9_]+/g, '_');
		let n = base, i = 2;

		while ((existing ?? []).includes(n))
			n = base + '_' + i++;

		return n;
	},

	/* What the LED is doing now, from luci.getLEDs data. */
	stateOf(led) {
		const t = led?.active_trigger;

		if (t == 'timer' || t == 'heartbeat')
			return 'blinking';
		if (t == 'netdev')
			return 'activity';

		return (+led?.brightness > 0) ? 'on' : 'off';
	},

	/* Accounts that can log in: a real shell, not /bin/false or nologin. root first. */
	loginUsers(passwdLines) {
		const users = [];

		for (const line of passwdLines ?? []) {
			const p = String(line).split(':');

			if (p.length < 7 || !p[0] || NO_LOGIN.test(p[6].trim()) || users.includes(p[0]))
				continue;

			users.push(p[0]);
		}

		return users.sort((a, b) => (a == 'root' ? -1 : b == 'root' ? 1 : 0));
	},

	/* rpcd (web interface) logins other than root, without duplicates. */
	rpcdUsers(usernames) {
		return [ ...new Set((usernames ?? []).filter((u) => u && u != 'root')) ];
	}
});
