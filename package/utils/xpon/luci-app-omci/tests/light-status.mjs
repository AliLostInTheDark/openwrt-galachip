// SPDX-License-Identifier: GPL-2.0-only
import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const pkg = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const xpon = path.dirname(pkg);
const read = p => fs.readFileSync(p, 'utf8');
const backend = read(path.join(xpon, 'omci-agent/files/usr/share/rpcd/ucode/omci.agent'));

function extract(source, token) {
  const start = source.indexOf(token);
  assert(start >= 0, token);
  let i = source.indexOf('{', start) + 1, depth = 1;
  while (depth) {
    if (source[i] === '{') depth++;
    if (source[i] === '}') depth--;
    i++;
  }
  return source.slice(start, i);
}

if (process.argv.includes('--backend')) {
  process.stdout.write(`
let config_reads = 0, mib_reads = 0, status_reads = 0, errors = 0;
let fail = false, native = false, sysfs_reads = 0;
const DEVICE_DEFAULT = 0;
const onu_type_names = ['other', 'sfu'];
const credentials_names = ['initial'];
function profile_name(v) { return v == 3 ? 'nokia-alcl' : 'generic'; }
function bits_to_names(v, bits) { return []; }
const quirk_bits = [];
function load_config(device) { config_reads++; return {serial: 'TEST00000000'}; }
function readfile(p) {
  sysfs_reads++;
  die('unexpected read: ' + p);
}
const XPON_SYSFS = '/sys/class/xpon/pon0';
const omci = {
  status: function(device, diagnostics) {
    status_reads++;
    return fail ? null : native ? {state: 5, onu_type: 1, responder: 'userspace', loid_status: 3,
      alarms_text: diagnostics ? '263 0000 reported low-rx-power' : null} :
      {state: 5, onu_type: 1, olt_profile_effective: 3, responder: 'userspace',
       alarms_text: '263 0000 reported low-rx-power'};
  },
  mib_get: function(device, c, e) { mib_reads++; return {olt_g: {vendor_id: 'TEST'}}; }
};
function native_error(request, op) { errors++; return {failed: op}; }
${extract(backend, 'function device_arg(')}
${extract(backend, 'function decorate_status(')}
const method = {${extract(backend, 'status_live: {')}}.status_live;
for (let i = 0; i < 8640; i++) {
  let s = method.call({args: {device: 0}}).status;
  if (s.state != 5 || s.olt_profile_effective_name != 'nokia-alcl' ||
      s.omci_alarms[0].alarms[0] != 'low-rx-power' || s.identity != null || s.olt != null)
    die('bad live status');
}
if (config_reads || mib_reads || status_reads != 8640) die('live status queried static objects');
let full = decorate_status(0, {onu_type: 1});
if (config_reads != 1 || mib_reads != 1 || full.identity.serial != 'TEST00000000' || full.olt.vendor_id != 'TEST')
  die('full SSH status changed');
native = true;
let n = method.call({args: {device: 0}}).status;
if (n.loid_status != 3 || n.omci_alarms[0].alarms[0] != 'low-rx-power')
  die('native live status lost omcid diagnostics');
let native_full = decorate_status(0, {onu_type: 1, responder: 'userspace', loid_status: 3,
  alarms_text: '263 0000 reported low-rx-power'});
if (native_full.omci_alarms[0].alarms[0] != 'low-rx-power' || sysfs_reads)
  die('status read kernel sysfs');
fail = true;
if (method.call({args: {}}).failed != 'status' || errors != 1) die('lost status error');
print('8640 lean RPC calls avoided config/MIB reads; full status and errors preserved\\n');
`);
  process.exit(0);
}

const resource = path.join(pkg, 'htdocs/luci-static/resources');
const rpcSource = read(path.join(resource, 'omci/rpc.js'));
const declarations = [];
new Function('baseclass', 'rpc', rpcSource)(
  {extend: x => x}, {declare: spec => {declarations.push(spec); return () => Promise.resolve({});}});
assert(declarations.some(x => x.method === 'status_live'));
assert(!declarations.some(x => x.object === 'omci.optstat'));

function view(file, data, mode = 'gpon') {
  const calls = {}, polls = [], reads = [];
  const nodes = new Map();
  let replacements = 0;
  const count = key => {calls[key] = (calls[key] || 0) + 1;};
  const context = vm.createContext({
    view: {extend: x => x},
    poll: {add: (fn, seconds) => polls.push({fn, seconds})},
    dom: {content: (node, content) => {node.children = content; replacements++;}},
    document: {getElementById: id => nodes.get(id), head: {appendChild: node => node}},
    fs: {
      read: p => {reads.push(p); return Promise.resolve(data[p] ?? null);},
      stat: p => {reads.push(p); return Promise.resolve(null);}
    },
    omci: {
      status: () => {count('full'); return Promise.resolve(data.status || {});},
      liveStatus: () => {count('live'); return Promise.resolve(data.live || {});},
      vlanStatus: () => {count('vlan'); return Promise.resolve(data.vlan || null);},
      mibList: () => {count('mib'); return Promise.resolve(data.mib || []);}
    },
    window: {setTimeout: fn => setImmediate(fn)},
    baseclass: {extend: x => x},
    rpc: {declare: () => () => { count('public'); return Promise.resolve(data.public || {}); }},
    uci: {load: () => Promise.resolve(), get: () => mode},
    ui: {createHandlerFn: () => () => {}},
    _: x => x,
    E: (tag, attrs, children) => {
      if (children === undefined && Array.isArray(attrs)) {children = attrs; attrs = {};}
      const node = {tag, attrs, children};
      if (attrs?.id) nodes.set(attrs.id, node);
      return node;
    },
    L: {resolveDefault: (promise, fallback) => promise.catch(() => fallback), bind: (f, self) => f.bind(self)}
  });
  vm.runInContext(`String.prototype.format = function(...args) {
    let i = 0; return this.replace(/%[0-9.]*[sdxf]/g, () => String(args[i++]));
  };`, context);
  const load = file => vm.runInContext('(function(){' + read(file) + '\n})()', context);
  context.mib = load(path.join(resource, 'omci/mib.js'));
  context.widgets = load(path.join(xpon, 'luci-app-xpon/htdocs/luci-static/resources/xpon/widgets.js'));
  const result = load(file);
  return {result, data, calls, polls, reads, nodes, replaced: () => replacements};
}

// The module's levels, as xpon.public passes bosa_thresholds (a sample module).
const thresholds = {tx_alarm_high_nw: 3981100, tx_alarm_low_nw: 891300, tx_warn_high_nw: 3548100, tx_warn_low_nw: 1000000,
  rx_alarm_high_nw: 251200, rx_alarm_low_nw: 1000, rx_warn_high_nw: 199500, rx_warn_low_nw: 1300};
const main = view(path.join(resource, 'view/omci/status.js'), {
  public: {thresholds},
  mib: [
    {class_id: 262, entity_id: 0x8000, origin_name: 'olt', data: '0400'},
    {class_id: 262, entity_id: 0x8001, origin_name: 'hardware', data: '00ff'},
    {class_id: 268, entity_id: 1, origin_name: 'olt', data: '04d28000'},
    {class_id: 84, entity_id: 1, origin_name: 'olt', vlan_filter: {entries: [{vid: 100, pbit: 0}]}}
  ],
  status: {state: 5, agent_operational: true, bosa_rx_power_nw: 3000, mib_sync: 1, omci_alarms: [],
    identity: {serial: 'TEST00000000', 'vendor-id': 'TEST', 'hardware-version': '1.0',
      'software-version-0': 'image-0', 'software-version-1': 'image-1', 'equipment-id': 'test-onu', 'omcc-version': 160},
    uni_count: 1, traffic_management_option: 0, reboot: 1,
    olt: {vendor_id: 'TEST'}, olt_profile_configured_name: 'auto'},
  live: {state: 4, agent_operational: false, bosa_rx_power_nw: 2000, mib_sync: 1, omci_alarms: []},
  vlan: {mode: 'transparent'}
});
main.result.render(await main.result.load());
await new Promise(r => setImmediate(r));
assert.equal(main.calls.mib, 1);
const flat = node => node == null ? '' : typeof node !== 'object' ? String(node) :
  Array.isArray(node) ? node.map(flat).join(' ') : flat(node.children);
const provisioned = flat(main.nodes.get('omci-provisioned-root'));
assert.match(provisioned, /Alloc-ID 1024 and carries its services on GEM port 1234/);
assert.match(provisioned, /VLAN 100/);
assert.doesNotMatch(provisioned, /Alloc-ID 1024, 255/);
assert.match(flat(main.nodes.get('omci-line-root')), /In range -25\.23 dBm .* -35 -30 -25 -20 -15 -10 -5 .*above the low warning.*Module: warning -28\.86\d* to -7\.00\d* dBm, alarm -30 \/ -5\.99\d* dBm.*Laser off/);
assert.equal(main.polls.length, 1);
assert.equal(main.polls[0].seconds, 10);
const nodeCount = main.nodes.size;
assert.equal(main.nodes.get('omci-status-serial').children, 'TEST00000000');
assert.equal(main.nodes.get('omci-status-uni_count').children, '1');
assert.equal(main.nodes.get('omci-status-traffic_management_option').children, '0');
for (let i = 0; i < 8640; i++) await main.polls[0].fn();
assert.equal(main.calls.mib, 1, 'MIB list re-read without a MIB change');
assert.equal(main.calls.public, 1, 'module thresholds re-read on every poll');
main.data.live.mib_sync = 2;
await main.polls[0].fn();
await main.polls[0].fn();
assert.equal(main.calls.mib, 2, 'MIB list not re-read once after a MIB change');
assert.equal(main.calls.full, 1);
assert.equal(main.calls.live, 8642);
assert.equal(main.calls.vlan, 8643);
assert(main.replaced() >= 8640);
assert.equal(main.nodes.size, nodeCount);
assert.equal(main.nodes.get('omci-status-state').children, 'O4');
assert.match(flat(main.nodes.get('omci-line-root')), /assigned ONU ID .* measuring/);
assert.equal(main.nodes.get('omci-status-serial').children, 'TEST00000000');
assert.equal(main.nodes.get('omci-status-software_version_1').children, 'image-1');
assert.equal(main.nodes.get('omci-status-profile_configured').children, 'auto');
assert.equal(main.nodes.get('omci-status-olt_vendor').children, 'TEST');
assert.equal(main.reads.length, 0);

// Channel loss clears the daemon's alarm cache; an empty cache is not an all-clear.
main.data.live.channel_up = false;
main.data.live.gem_port = 65535;
await main.polls[0].fn();
assert.equal(main.nodes.get('omci-status-gem_port').children, 'Not assigned');
assert.equal(main.nodes.get('omci-status-omci_alarm_0').children, 'Unavailable (channel down)');
assert.match(flat(main.nodes.get('omci-summary-root')), /OMCI alarms Unavailable/);
main.data.live.channel_up = true;
main.data.live.gem_port = 256;
main.data.live.omci_alarms = [{class: 263, entity: '8000', alarms: ['low-rx-power'], reported: true}];
await main.polls[0].fn();
assert.equal(main.nodes.get('omci-status-gem_port').children, '256');
assert.equal(main.nodes.get('omci-status-omci_alarm_0').children, 'Active, reported');
assert.equal(main.nodes.get('omci-status-omci_alarm_1').children, 'Clear');
assert.match(flat(main.nodes.get('omci-summary-root')), /OMCI alarms 1 active/);
main.data.live.omci_alarms = [];
await main.polls[0].fn();
assert.match(flat(main.nodes.get('omci-summary-root')), /OMCI alarms None/);


assert(!fs.existsSync(path.join(resource, 'view/omci/line.js')));

const ep = view(path.join(xpon, 'luci-app-epon/htdocs/luci-static/resources/view/epon/status.js'), {
  '/sys/class/net/pon0/device/epon_state': 'signal=1 registered=0x1 nack_silent=0',
  '/sys/class/net/pon0/device/bosa_ddm': 'rx_nw=20000 tx_nw=2000000',
  '/var/run/epon-oamd.json': '{"oam":"send_any","auth":"ok","olt_mac":"00:13:25:88:00:00"}'
}, 'epon');
const epNode = ep.result.render(await ep.result.load());
await ep.polls[0].fn();
assert.match(flat(epNode), /OAM link up with OLT 00:13:25:88:00:00/);
assert.match(flat(epNode), /-16\.99.*3\.01/);
assert.match(flat(epNode), /In window -16\.99/, 'no module levels: the class window applies');
// A partial or disordered threshold set does not describe the module.
const partial = view(path.join(resource, 'view/omci/status.js'), {
  public: {thresholds: {...thresholds, rx_warn_low_nw: 0}}, status: {state: 5, bosa_rx_power_nw: 3000}});
partial.result.render(await partial.result.load());
assert.match(flat(partial.nodes.get('omci-line-root')), /Near limit -25\.23/);
assert.equal(ep.polls[0].seconds, 10);
assert(!ep.reads.some(p => /counters|tcont_stats|gpon_ber/.test(p)));

// The header indicator: GPON from status_live, EPON from the sysfs and oamd state.
async function indicator(network, files, status) {
  const shown = [], polls = [];
  const context = vm.createContext({
    baseclass: {extend: x => x}, _: x => x,
    rpc: {declare: () => () => Promise.resolve(status)},
    fs: {read: p => Promise.resolve(files[p] ?? null)},
    poll: {add: (fn, s) => polls.push(s)},
    ui: {menu: {load: () => Promise.resolve({children: {admin: {children: {network: {children: network}}}}})},
         showIndicator: (...args) => shown.push(args)},
    L: {resolveDefault: (p, d) => p.catch(() => d), bind: (f, self) => f.bind(self), url: p => '/' + p},
    location: {}
  });
  vm.runInContext(`String.prototype.format = function(...args) {
    let i = 0; return this.replace(/%[0-9.]*[sdxf]/g, m => { const v = args[i++]; return m.endsWith('f') ? Number(v).toFixed(1) : String(v); });
  };`, context);
  context.widgets = vm.runInContext('(function(){' + read(path.join(xpon, 'luci-app-xpon/htdocs/luci-static/resources/xpon/widgets.js')) + '\n})()', context);
  const mod = vm.runInContext('(function(){' + read(path.join(xpon, 'luci-app-xpon/htdocs/luci-static/resources/xpon/indicator.js')) + '\n})()', context);
  await mod.__init__.call(mod);
  return {shown, polls};
}
let ind = await indicator({omci: {}}, {}, {state: 5, agent_operational: true, bosa_rx_power_nw: 15000});
assert.deepEqual(ind.polls, [30]);
assert.equal(ind.shown[0][1], 'GPON O5 · -18.2 dBm');
assert.equal(ind.shown[0][3], 'active');
ind = await indicator({omci: {}}, {}, {state: 3, bosa_rx_power_nw: 0});
assert.deepEqual([ind.shown[0][1], ind.shown[0][3]], ['GPON O3 · no light', 'inactive']);
const D = '/sys/class/net/pon0/device/';
ind = await indicator({epon: {}}, {[D + 'epon_state']: 'signal=1 registered=0x1', [D + 'bosa_ddm']: 'rx_nw=12000',
  '/var/run/epon-oamd.json': '{"oam":"send_any","auth":"ok"}'}, {});
assert.deepEqual([ind.shown[0][1], ind.shown[0][3]], ['EPON · -19.2 dBm', 'active']);
ind = await indicator({epon: {}}, {[D + 'epon_state']: 'signal=0'}, {});
assert.deepEqual([ind.shown[0][1], ind.shown[0][3]], ['EPON no light', 'inactive']);
ind = await indicator({}, {}, {});
assert.equal(ind.shown.length + ind.polls.length, 0);
const xponAcl = JSON.parse(read(path.join(xpon, 'luci-app-xpon/root/usr/share/rpcd/acl.d/luci-app-xpon.json')))['luci-app-xpon'];
assert.deepEqual(Object.keys(xponAcl.write.file), ['/usr/sbin/xpon-report -o -']);

const menu = JSON.parse(read(path.join(pkg, 'root/usr/share/luci/menu.d/luci-app-omci.json')));
// MIB reads and edits are on demand; the page must not poll.
const mibRoot = 'admin/network/omci/mib';
assert.equal(menu[mibRoot].action.type, 'firstchild');
assert.deepEqual(Object.keys(menu).filter(k => k.startsWith(mibRoot + '/')),
  ['inventory', 'gpon', 'bridge', 'multicast', 'other'].map(group => mibRoot + '/' + group));
assert(!/\bpoll\b/.test(read(path.join(resource, 'omci/mib-editor.js'))));
for (const group of ['inventory', 'gpon', 'bridge', 'multicast', 'other']) {
  const route = menu[mibRoot + '/' + group].action.path;
  assert.equal(route, 'omci/mib/' + group);
  assert(read(path.join(resource, 'view', route + '.js')).includes('require omci.mib-editor as mib'));
}
assert(!Object.keys(menu).some(k => k.endsWith('/line')));
const acl = JSON.parse(read(path.join(pkg, 'root/usr/share/rpcd/acl.d/luci-app-omci.json')))['luci-app-omci'];
assert(acl.read.ubus['omci.agent'].includes('status_live'));
assert(acl.read.ubus['omci.agent'].includes('mib_list')); // Explicit VLAN configuration reads.
for (const method of ['class_list', 'class_get', 'mib_list', 'mib_get']) {
  assert(acl.read.ubus['omci.agent'].includes(method));
  assert(declarations.some(d => d.method === method));
}
for (const method of ['mib_set', 'mib_delete', 'mib_reset']) {
  assert(acl.write.ubus['omci.agent'].includes(method));
  assert(declarations.some(d => d.method === method));
}
assert(!acl.write.ubus['omci.agent'].includes('raw_tx'));
assert(!acl.read.file);
assert(!acl.write.ubus.file);
assert(!Object.keys(acl.write.file).some(p => /tx_hold|xpon-rogue/.test(p)));
assert(!fs.existsSync(path.join(pkg, 'root/etc/init.d/omci-optstat')));
assert(!fs.existsSync(path.join(pkg, 'root/usr/sbin/omci-optstat')));
assert(!read(path.join(pkg, 'Makefile')).includes('ucode-mod-math'));
assert(!read(path.join(resource, 'view/omci/status.js')).match(/history|median|optstat|samples/i));
console.log('Lean GPON/EPON polling, one-day refresh, menus, ACLs and collector removal passed');
