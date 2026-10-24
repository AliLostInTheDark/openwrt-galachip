'use strict';
'require view';
'require dom';
'require ui';
'require fs';
'require uci';
'require form';
'require rpc';
'require tools.password as pwtool';
'require onusys.model as model';

/*
 * Router password, the classic OpenWrt way.
 *
 * Based on luci-mod-system's system/password.js (Apache-2.0). That page offers
 * every /etc/passwd entry (daemon, network, ubus, ...) as a username. Here only
 * accounts that can log in are offered, and with a single one (root) no
 * username field is shown at all. The web-interface (rpcd) user option only
 * appears when such users exist.
 */

const formData = { data: { rpcd: null, rpc_user: null, user: null, oldpw: null, pw1: null, pw2: null } };

const callSetPassword = rpc.declare({
	object: 'luci',
	method: 'setPassword',
	params: [ 'username', 'password', 'oldpassword', 'rpcd' ],
	expect: { result: false }
});

return view.extend({
	checkPassword(section_id, value) {
		const strength = document.querySelector('.onusys-strength');
		const policy = (k) => uci.get('rpcd', 'policy', k);

		if (strength && value?.length) {
			const score = pwtool.score ? pwtool.score(value) : null;
			const level = score != null ? score
				: /^(?=.{8,})(?=.*[A-Z])(?=.*[a-z])(?=.*[0-9])(?=.*\W).*$/.test(value) ? 3
				: /^(?=.{7,})(((?=.*[A-Z])(?=.*[a-z]))|((?=.*[A-Z])(?=.*[0-9]))|((?=.*[a-z])(?=.*[0-9]))).*$/.test(value) ? 2
				: value.length >= 6 ? 1 : 0;
			const [ text, color ] = [ [ _('More Characters'), 'red' ], [ _('Weak'), 'red' ], [ _('Medium'), 'orange' ], [ _('Strong'), 'green' ] ][Math.min(level, 3)];

			strength.innerHTML = '%s: <span style="color:%s">%s</span>'.format(_('Password strength'), color, text);
		}

		if (!policy('enabled'))
			return true;
		if (policy('pw_length') && !pwtool.checkLength(value, policy('pw_length')))
			return _('Policy: min. length of %s characters').format(policy('pw_length'));
		if (policy('digits') && !pwtool.checkDigits(value))
			return _('Policy: contain digits');
		if (policy('uc_lc') && !pwtool.checkUpperLower(value))
			return _('Policy: contain uppercase/lowercase');
		if (policy('special_characters') && !pwtool.checkSpecialChars(value))
			return _('Policy: contain special characters');

		return true;
	},

	load() {
		return Promise.all([
			L.resolveDefault(fs.stat('/usr/sbin/uhttpd'), null),
			L.resolveDefault(fs.lines('/etc/passwd'), []),
			uci.load('rpcd')
		]);
	},

	render([ has_uhttpd, passwd ]) {
		const users = model.loginUsers(passwd);
		const rpcdUsers = has_uhttpd ? model.rpcdUsers(uci.sections('rpcd', 'login').map((s) => s.username)) : [];
		let o;

		formData.data.user = users[0] ?? 'root';

		const m = new form.JSONMap(formData, _('Router Password'),
			users.length > 1 ? _('Changes the password of an account on the device.')
			                 : _('Changes the administrator (%s) password used for the web interface and SSH.').format(formData.data.user));
		m.readonly = !L.hasViewPermission();

		const s = m.section(form.NamedSection, 'data', 'data');

		if (rpcdUsers.length) {
			o = s.option(form.Flag, 'rpcd', _('Web interface user'), _('Change the password of a web-interface (rpcd) user instead.'));
			o.default = o.disabled;

			o = s.option(form.ListValue, 'rpc_user', _('Web interface user'));
			rpcdUsers.forEach((u) => o.value(u));
			o.depends('rpcd', '1');

			o = s.option(form.Value, 'oldpw', _('Current password'));
			o.password = true;
			o.depends('rpcd', '1');
		}

		if (users.length > 1) {
			o = s.option(form.ListValue, 'user', _('Username'));
			users.forEach((u) => o.value(u));
			if (rpcdUsers.length)
				o.depends('rpcd', '0');
		}

		o = s.option(form.Value, 'pw1', _('New password'));
		o.password = true;
		o.validate = L.bind(this.checkPassword, this);
		o.description = E('span', { 'class': 'onusys-strength' });

		o = s.option(form.Value, 'pw2', _('Confirmation'));
		o.password = true;
		o.validate = (section_id, value) => (!value || value == formData.data.pw1 ||
			value == document.querySelector('[data-name="pw1"] input')?.value) ? true : _('The passwords do not match');
		o.renderWidget = function() {
			const node = form.Value.prototype.renderWidget.apply(this, arguments);

			node.querySelector('input').addEventListener('keydown', (ev) => {
				if (ev.keyCode == 13 && !ev.currentTarget.classList.contains('cbi-input-invalid'))
					document.querySelector('.cbi-button-save')?.click();
			});

			return node;
		};

		return m.render();
	},

	handleSave() {
		const map = document.querySelector('.cbi-map');

		return dom.callClassMethod(map, 'save').then(() => {
			const d = formData.data;
			const rpcd = d.rpcd == '1';
			const user = rpcd ? d.rpc_user : (d.user || 'root');

			if (!d.pw1)
				return;
			if (rpcd && !d.oldpw)
				return ui.addNotification(null, E('p', _('Enter the current password of the web interface user.')), 'danger');
			if (d.pw1 != d.pw2)
				return ui.addNotification(null, E('p', _('Given password confirmation did not match, password not changed!')), 'danger');

			return callSetPassword(user, d.pw1, rpcd ? d.oldpw : '', rpcd).then((ok) => {
				ui.addNotification(null, E('p', ok ? _('The password of %s has been changed.').format(user)
				                                   : _('Failed to change the system password.')), ok ? 'info' : 'danger');
				Object.assign(d, { rpcd: null, rpc_user: null, oldpw: null, pw1: null, pw2: null });
				return dom.callClassMethod(map, 'render');
			});
		});
	},

	handleSaveApply: null,
	handleReset: null
});
