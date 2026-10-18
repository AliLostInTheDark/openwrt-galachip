// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { after, test } from 'node:test';
import vm from 'node:vm';

const theme = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repo = resolve(theme, '../../..');
const temporary = mkdtempSync(join(tmpdir(), 'slate-noncss-'));
after(() => rmSync(temporary, { recursive: true, force: true }));

class Element {
	constructor(tag, attrs = {}, children = []) {
		this.tagName = tag;
		this.attrs = attrs;
		this.children = children;
		this.style = { display: 'none' };
		this.events = {};
		this.classList = {
			add: name => { this.attrs.class = `${this.attrs.class || ''} ${name}`.trim(); },
			remove: name => { this.attrs.class = (this.attrs.class || '').split(' ').filter(c => c && c !== name).join(' '); },
			contains: name => (this.attrs.class || '').split(' ').includes(name),
			toggle: (name, force) => {
				const on = force ?? !this.classList.contains(name);
				on ? this.classList.add(name) : this.classList.remove(name);
				return on;
			}
		};
	}
	getAttribute(name) { return this.attrs[name] ?? null; }
	setAttribute(name, value) { this.attrs[name] = String(value); }
	appendChild(child) { this.children.push(child); return child; }
	addEventListener(type, handler) { (this.events[type] ??= []).push(handler); }
	matches() { return this.open ?? false; }
	getBoundingClientRect() { return this.rect; }
}

function menuHarness(path = ['admin', 'network', 'wireless'], requestpath = path) {
	const elements = Object.fromEntries(['topmenu', 'tabmenu', 'modemenu', 'crumbs'].map(id => [id, new Element('ul')]));
	const callbacks = [];
	const events = {};
	const document = {
		querySelector: selector => elements[selector.slice(1)],
		documentElement: { clientWidth: 800 }
	};
	const required = [];
	const L = {
		env: { dispatchpath: path, requestpath },
		require: name => { required.push(name); return Promise.resolve({}); },
		url: (...parts) => '/cgi-bin/luci/' + parts.join('/'),
		isObject: value => value !== null && typeof value === 'object',
		naturalCompare: (a, b) => a.localeCompare(b, 'en', { numeric: true })
	};
	// Exercise the checkout's actual ACL filtering, sorting and alias resolver.
	const core = readFileSync(join(repo, 'feeds/luci/modules/luci-base/htdocs/luci-static/resources/ui.js'), 'utf8');
	const helper = core.slice(core.indexOf('\n\tgetChildren(node) {'), core.indexOf('\n});', core.indexOf('\n\tgetChildren(node) {')));
	const ui = { menu: vm.runInNewContext('({' + helper + '})', { L }) };
	const source = readFileSync(join(theme, 'htdocs/luci-static/resources/menu-slate.js'), 'utf8');
	const renderer = vm.runInNewContext('(function() {' + source + '\n})()', {
		baseclass: { extend: value => value }, ui, L, document,
		E: (tag, attrs, children) => new Element(tag, attrs, children),
		_: text => text,
		window: {
			requestAnimationFrame: callback => callbacks.push(callback),
			addEventListener: (type, handler) => (events[type] ??= []).push(handler)
		}
	});
	return {
		...elements, renderer, document, events, required,
		render(tree) { ui.menu.menu = tree; renderer.render(tree); },
		flush() { callbacks.splice(0).forEach(callback => callback()); }
	};
}

const item = (title, children, extra = {}) => ({ title, satisfied: true, ...(children ? { children } : {}), ...extra });
const tree = () => ({ children: {
	admin: item('Administration', {
		network: item('Network', {
			wireless: item('Wireless', {
				advanced: item('Advanced', { radio: item('Radio') }),
				general: item('General', null, { order: 1 }),
				hidden: item('Hidden', null, { satisfied: false })
			}),
			interfaces: item('Interfaces', null, { order: 1 })
		}),
		logout: item('Logout'),
		restricted: item('Restricted', null, { satisfied: false })
	})
} });

test('navigation uses LuCI filtering and sorting, and renders nested active tabs', () => {
	const h = menuHarness(['admin', 'network', 'wireless', 'advanced', 'radio']);
	h.render(tree());
	assert.equal(h.topmenu.children.length, 2);
	const network = h.topmenu.children.find(node => node.attrs.class.includes('active'));
	assert.ok(network.classList.contains('nav-group') && network.classList.contains('open'));
	const sub = network.children[1];
	assert.equal(sub.attrs.class, 'nav-sub');
	assert.equal(sub.children[0].children[0].attrs.href, '/cgi-bin/luci/admin/network/interfaces');
	const current = sub.children.find(node => node.attrs.class === 'active');
	assert.equal(current.children[0].attrs['aria-current'], 'page');
	assert.deepEqual(h.crumbs.children.map(node => node.children[0]), ['Network', 'Wireless']);
	assert.equal(h.tabmenu.children.length, 2);
	assert.equal(h.tabmenu.children[0].children.length, 2);
	assert.equal(h.tabmenu.children[0].children[1].attrs.class, 'tabmenu-item-advanced active');
	assert.equal(h.tabmenu.children[1].children[0].children[0].attrs.href, '/cgi-bin/luci/admin/network/wireless/advanced/radio');
});

test('missing, leaf and short menu paths do not throw', () => {
	for (const path of [['admin'], ['admin', 'logout', 'missing'], ['admin', 'missing', 'page']]) {
		const h = menuHarness(path);
		assert.doesNotThrow(() => h.render(tree()));
		assert.equal(h.tabmenu.children.length, 0);
	}
});

test('alias menu nodes expose their target tabs', () => {
	const data = tree();
	data.children.admin.children.wifi = item('WiFi', null, { action: { type: 'alias', path: 'admin/network' } });
	const h = menuHarness(['admin', 'wifi', 'wireless', 'advanced']);
	h.render(data);
	assert.equal(h.tabmenu.children.length, 2);
	assert.equal(h.tabmenu.children[0].children[1].children[0].attrs.href, '/cgi-bin/luci/admin/wifi/wireless/advanced');
});

test('mode menu selects the requested mode and defaults to the first mode', () => {
	const data = tree();
	data.children.status = item('Public status', { overview: item('Overview') });
	const selected = menuHarness(['status', 'overview'], ['status']);
	selected.render(data);
	assert.equal(selected.modemenu.style.display, '');
	assert.equal(selected.topmenu.children[0].children[0].attrs.href, '/cgi-bin/luci/status/overview');
	const fallback = menuHarness([], []);
	fallback.render(tree());
	assert.equal(fallback.topmenu.children.length, 2);
	assert.equal(fallback.modemenu.style.display, 'none');
});

test('the PON line indicator loads only where the xPON pages are installed', () => {
	const plain = menuHarness(['admin', 'network', 'wireless']);
	plain.render(tree());
	assert.deepEqual(plain.required, []);
	const data = tree();
	data.children.admin.children.network.children.xpon = item('PON Mode');
	const pon = menuHarness(['admin', 'network', 'wireless']);
	pon.render(data);
	assert.deepEqual(pon.required, ['xpon.indicator']);
});

test('sidebar groups toggle, and leaf entries link directly', () => {
	const h = menuHarness(['admin', 'logout']);
	h.render(tree());
	const network = h.topmenu.children.find(node => !node.classList.contains('nav-leaf'));
	const logout = h.topmenu.children.find(node => node.classList.contains('nav-leaf'));
	const head = network.children[0];
	assert.ok(!network.classList.contains('open'));
	assert.equal(head.attrs['aria-expanded'], 'false');
	head.attrs.click();
	assert.ok(network.classList.contains('open'));
	assert.equal(head.attrs['aria-expanded'], 'true');
	head.attrs.click();
	assert.ok(!network.classList.contains('open'));
	assert.ok(logout.classList.contains('nav-leaf') && logout.classList.contains('active'));
	assert.equal(logout.children[0].attrs.href, '/cgi-bin/luci/admin/logout');
	assert.deepEqual(h.crumbs.children.map(node => node.children[0]), ['Logout']);
});

const defaults = readFileSync(join(theme, 'root/etc/uci-defaults/30_luci-theme-slate'), 'utf8');
const makefile = readFileSync(join(theme, 'Makefile'), 'utf8');
const postrm = makefile.match(/define Package\/luci-theme-slate\/postrm\n([\s\S]*?)\nendef/)[1].replaceAll('$$', '$');
const uciPath = join(temporary, 'uci');
writeFileSync(uciPath, `#!${process.execPath}
const fs = require('node:fs');
const file = process.env.UCI_STATE;
const state = JSON.parse(fs.readFileSync(file));
const args = process.argv.slice(2).filter(arg => arg !== '-q');
const [command, argument] = args;
state.calls.push(args);
let status = 0;
if (process.env.UCI_FAIL === command) status = 1;
else if (command === 'get') {
  if (Object.hasOwn(state.values, argument)) process.stdout.write(state.values[argument]);
  else status = 1;
}
else if (command === 'set') {
  const split = argument.indexOf('=');
  state.values[argument.slice(0, split)] = argument.slice(split + 1);
}
else if (command === 'delete') {
  if (Object.hasOwn(state.values, argument)) delete state.values[argument];
  else status = 1;
}
else if (command === 'show') {
  for (const [key, value] of Object.entries(state.values))
    if (key.startsWith(argument + '.')) process.stdout.write(key + "='" + value + "'\\n");
}
else if (command !== 'commit') throw new Error('Unexpected UCI command: ' + command);
fs.writeFileSync(file, JSON.stringify(state));
process.exit(status);
`, { mode: 0o755 });

/* Themes whose templates the defaults script finds installed. */
const themesDir = join(temporary, 'themes');
for (const name of ['slate', 'slate-dark', 'slate-light', 'custom', 'custom-slate', 'other'])
	mkdirSync(join(themesDir, name), { recursive: true }), writeFileSync(join(themesDir, name, 'header.ut'), '');

function runUci(script, values, environment = {}, args = []) {
	const statePath = join(temporary, 'uci-state.json');
	writeFileSync(statePath, JSON.stringify({ values, calls: [] }));
	const result = spawnSync('sh', ['-c', script, 'slate-test', ...args], {
		encoding: 'utf8',
		stdio: ['ignore', 'pipe', 'pipe'],
		env: { ...process.env, PATH: temporary + ':' + process.env.PATH, UCI_STATE: statePath,
			PKG_UPGRADE: '', IPKG_INSTROOT: '', UCI_FAIL: '', THEMES_DIR: themesDir, ...environment }
	});
	assert.ifError(result.error);
	assert.equal(result.stderr, '');
	return { ...JSON.parse(readFileSync(statePath)), status: result.status };
}

test('install registers variants once, preserves upgrades and reports UCI failures', () => {
	const initial = runUci(defaults, { 'luci.main.mediaurlbase': '/luci-static/bootstrap' });
	assert.equal(initial.status, 0);
	assert.equal(initial.values['luci.main.mediaurlbase'], '/luci-static/slate');
	assert.equal(initial.values['luci.themes.SlateDark'], '/luci-static/slate-dark');
	assert.equal(initial.values['luci.themes.SlateLight'], '/luci-static/slate-light');
	const repeat = runUci(defaults, initial.values);
	assert.ok(repeat.calls.every(([command]) => command === 'get' || command === 'show'));
	const upgrade = runUci(defaults, { 'luci.main.mediaurlbase': '/luci-static/custom' }, { PKG_UPGRADE: '1' });
	assert.equal(upgrade.values['luci.main.mediaurlbase'], '/luci-static/custom');
	const repaired = runUci(defaults, { 'luci.themes.Slate': '/luci-static/custom-slate', 'luci.main.mediaurlbase': '/luci-static/custom' });
	assert.equal(repaired.values['luci.themes.Slate'], '/luci-static/custom-slate');
	assert.equal(repaired.values['luci.main.mediaurlbase'], '/luci-static/custom');
	for (const command of ['set', 'commit'])
		assert.equal(runUci(defaults, {}, { UCI_FAIL: command }).status, 1);
});

test('a kept configuration naming missing themes is repaired', () => {
	const kept = {
		'luci.main.mediaurlbase': '/luci-static/bootstrap-dark',
		'luci.themes.Bootstrap': '/luci-static/bootstrap', 'luci.themes.BootstrapDark': '/luci-static/bootstrap-dark',
		'luci.themes.Slate': '/luci-static/slate', 'luci.themes.SlateDark': '/luci-static/slate-dark',
		'luci.themes.SlateLight': '/luci-static/slate-light', 'luci.themes.Other': '/luci-static/other'
	};
	for (const environment of [{}, { PKG_UPGRADE: '1' }]) {
		const result = runUci(defaults, kept, environment);
		assert.equal(result.status, 0);
		assert.equal(result.values['luci.main.mediaurlbase'], '/luci-static/slate');
		assert.equal(result.values['luci.themes.Bootstrap'], undefined);
		assert.equal(result.values['luci.themes.BootstrapDark'], undefined);
		assert.equal(result.values['luci.themes.Other'], '/luci-static/other');
		assert.equal(result.values['luci.themes.SlateDark'], '/luci-static/slate-dark');
	}
	const custom = runUci(defaults, { ...kept, 'luci.main.mediaurlbase': '/luci-static/slate-dark' });
	assert.equal(custom.values['luci.main.mediaurlbase'], '/luci-static/slate-dark');
	assert.equal(runUci(defaults, kept, { UCI_FAIL: 'delete' }).status, 1);
});

test('removal preserves upgrades and offline roots, and clears only an active Slate selection', () => {
	const base = { 'luci.themes.Slate': '/luci-static/slate', 'luci.themes.SlateDark': '/luci-static/slate-dark',
		'luci.themes.SlateLight': '/luci-static/slate-light', 'luci.themes.Other': '/luci-static/other' };
	for (const [environment, args] of [[{}, ['upgrade']], [{ PKG_UPGRADE: '1' }, []], [{ IPKG_INSTROOT: '/offline' }, []]]) {
		const result = runUci(postrm, base, environment, args);
		assert.equal(result.status, 0);
		assert.equal(result.calls.length, 0);
	}
	for (const variant of ['slate', 'slate-dark', 'slate-light', 'slate-custom', 'other']) {
		const selected = '/luci-static/' + variant;
		const result = runUci(postrm, { ...base, 'luci.main.mediaurlbase': selected }, {}, ['remove']);
		assert.equal(result.status, 0);
		assert.equal(result.values['luci.themes.Slate'], undefined);
		assert.equal(result.values['luci.themes.Other'], '/luci-static/other');
		assert.equal(result.values['luci.main.mediaurlbase'], ['slate', 'slate-dark', 'slate-light'].includes(variant) ? undefined : selected);
	}
});

const ucode = process.env.UCODE || join(repo, 'staging_dir/hostpkg/bin/ucode');
const hostpkg = resolve(dirname(ucode), '..');
const haveUcode = existsSync(ucode);
let htmlReady = false;

function renderTemplate(name, fixture = {}) {
	if (!htmlReady) {
		const compiled = spawnSync(process.env.CC || 'cc', ['-shared', '-fPIC', '-I' + join(hostpkg, 'include'),
			'-I' + join(repo, 'staging_dir/host/include'),
			'-o', join(temporary, 'html.so'), join(repo, 'feeds/luci/contrib/package/ucode-mod-html/src/html.c')],
			{ encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
		assert.equal(compiled.status, 0, compiled.stderr);
		mkdirSync(join(temporary, 'luci'));
		writeFileSync(join(temporary, 'luci/core.uc'), 'export function getuid() { return fixture.uid ?? 1000; }\nexport function getspnam() { return { pwdp: fixture.password ?? "set" }; }\n');
		htmlReady = true;
	}
	const script = `
import { entityencode, striptags } from 'html';
const root = ${JSON.stringify(join(theme, 'ucode/template'))};
const scope = proto({
  entityencode, striptags, _: text => text,
  ubus: { call: () => fixture.board ?? { hostname: 'Router & lab', model: 'Test & board' } },
  dispatcher: { lang: 'en', build_url: (...parts) => '/router/cgi-bin/luci' + (length(parts) ? '/' + join('/', parts) : ''), lookup: () => true },
  http: { prepare_content: () => null },
  ctx: { request_path: ['admin', 'status', 'overview'], path: ['admin', 'status', 'overview'] },
  media: '/luci-static/' + (fixture.theme ?? 'slate'), resource: '/luci-static/resources',
  theme: fixture.theme ?? 'slate', dispatched: {}, duser: 'root',
  version: { luciname: 'LuCI', luciversion: '1', distname: 'OpenWrt & test', distversion: '1', distrevision: 'r1' },
  ...fixture
}, global);
scope.include = (name, extra) => {
  if (name == 'header' || name == 'footer') name = 'themes/' + scope.theme + '/' + name;
  call(loadfile(root + '/' + name + '.ut', { raw_mode: false }), null, proto(extra ?? {}, scope));
};
call(loadfile(root + '/themes/' + scope.theme + '/${name}.ut', { raw_mode: false }), null, scope);
`;
	const result = spawnSync(ucode, ['-L', join(temporary, '*.so'), '-L', join(temporary, '*.uc'),
		'-D', 'fixture=' + JSON.stringify(fixture), '-e', script], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
	assert.equal(result.status, 0, result.stderr);
	assert.equal(result.stderr, '');
	return result.stdout;
}

test('ucode templates compile with package flags and render all theme variants', { skip: !haveUcode }, () => {
	for (const variant of ['slate', 'slate-dark', 'slate-light']) {
		for (const name of ['header', 'footer', 'logo', 'sysauth']) {
			const result = spawnSync(ucode, ['-T,', '-s', '-cno-interp,dynlink=luci.core', '-o', join(temporary, variant + '-' + name + '.uc'),
				join(theme, 'ucode/template/themes', variant, name + '.ut')], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
			assert.equal(result.status, 0, result.stderr);
		}
		const login = renderTemplate('sysauth', { theme: variant });
		assert.ok(login.includes('class="login-card"'));
		assert.ok(login.includes('Router &#38; lab'));
		assert.ok(!login.includes('id="topmenu"'));
		assert.ok(!login.includes("L.require('menu-slate')"));
		if (variant !== 'slate')
			assert.ok(login.includes('data-darkmode="' + (variant === 'slate-dark') + '"'));
		else
			assert.ok(login.includes('prefers-color-scheme'));
	}
	const header = renderTemplate('header', { uid: 0, password: '', board: { hostname: 'Router', rootfs_type: 'initramfs' } });
	assert.ok(header.includes('No password set!'));
	assert.ok(header.includes('recovery (initramfs) mode'));
	assert.ok(header.includes('href="/router/cgi-bin/luci"'));
	assert.ok(header.includes('id="tabmenu"'));
	assert.ok(renderTemplate('footer').includes("L.require('menu-slate')"));
});

test('auth plugin attributes and text are escaped while trusted plugin HTML is preserved', { skip: !haveUcode }, () => {
	const value = 'OTP "code" & <value>';
	const html = renderTemplate('sysauth', {
		auth_message: '<img src=x onerror=alert(1)>',
		auth_fields: [{ name: value, label: value, placeholder: value, inputmode: value, pattern: '["&<>]+',
			maxlength: 6, autocomplete: value, required: true }],
		auth_html: '<p id="trusted-plugin">Challenge</p>',
		auth_assets: [{ type: 'script', src: '/plugin.js?name="otp"&test=1' }]
	});
	assert.ok(html.includes('name="OTP &#34;code&#34; &#38; &#60;value&#62;"'));
	assert.ok(html.includes('pattern="[&#34;&#38;&#60;&#62;]+"'));
	assert.ok(html.includes('maxlength="6"'));
	assert.ok(html.includes('&#60;img src=x onerror=alert(1)&#62;'));
	assert.ok(!html.includes('<img src=x'));
	assert.ok(html.includes('<p id="trusted-plugin">Challenge</p>'));
	assert.ok(html.includes('src="/plugin.js?name=&#34;otp&#34;&#38;test=1"'));
});

test('login stays usable after a cancelled submission and after history restoration', async () => {
	const source = readFileSync(join(theme, 'ucode/template/themes/slate/sysauth.ut'), 'utf8').match(/<script>\s*([\s\S]*?)<\/script>/)[1];
	const formEvents = {}, windowEvents = {}, classes = new Set();
	const btn = { disabled: false, classList: { add: name => classes.add(name), remove: name => classes.delete(name) } };
	const form = { querySelector: () => btn, addEventListener: (type, handler) => { formEvents[type] = handler; } };
	vm.runInNewContext(source, { document: { querySelector: () => form }, setTimeout,
		window: { addEventListener: (type, handler) => { windowEvents[type] = handler; } } });
	const cancelled = { defaultPrevented: false };
	formEvents.submit(cancelled);
	cancelled.defaultPrevented = true;
	await new Promise(resolve => setTimeout(resolve, 0));
	assert.equal(btn.disabled, false);
	formEvents.submit({ defaultPrevented: false });
	await new Promise(resolve => setTimeout(resolve, 0));
	assert.equal(btn.disabled, true);
	assert.ok(classes.has('spinning'));
	windowEvents.pageshow();
	assert.equal(btn.disabled, false);
	assert.ok(!classes.has('spinning'));
});
