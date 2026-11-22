// SPDX-License-Identifier: GPL-2.0-only
// The G7's public status object, the router's collector and its LuCI view,
// for a GPON and an EPON ONU.
import fs from 'node:fs';
import os from 'node:os';
import vm from 'node:vm';
import path from 'node:path';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const pkg = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const xpon = path.dirname(pkg);
const repo = path.resolve(xpon, '../../..');
const read = p => fs.readFileSync(p, 'utf8');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'xpon-stats-'));

/* --- xpon summary on the ONU, against fixture sysfs files --- */
const ucode = process.env.UCODE || path.join(repo, 'staging_dir/hostpkg/bin/ucode');
// This G7's BOSA: RX warning -28.9/-7.0 dBm, alarm -30.0/-6.0 dBm; TX 0.0/5.5 and -0.5/6.0.
const thresholds = {tx_alarm_high_nw: 3981100, tx_alarm_low_nw: 891300, tx_warn_high_nw: 3548100, tx_warn_low_nw: 1000000,
  rx_alarm_high_nw: 251200, rx_alarm_low_nw: 1000, rx_warn_high_nw: 199500, rx_warn_low_nw: 1300};
const eponDdm = {temp_mc: 45000, vcc_uv: 3300000, bias_ua: 12000, tx_nw: 2000000, rx_nw: 12000, alarms: 16};
const oamd = {generation: 3, registered: true, oam: 'send_any', ctc: 'ok', ctc_version: 48, auth: 'ok',
  auth_fail_reason: 0, loid: 'SECRET-LOID', olt_mac: '00:13:25:88:00:00', onu_mac: 'a8:42:a1:85:11:47',
  vlan_mode: 0, vlan_default: 0, vlan_override: 'tag:3766', rx_pdus: 10, tx_pdus: 11, keys: 2, interauth: 'off'};
let eponPublic = null;
if (fs.existsSync(ucode)) {
  const root = path.join(tmp, 'root');
  for (const [p, body] of Object.entries({
    'sys/class/xpon/pon0/mode_current': 'epon\n',
    'var/run/epon-oamd.json': JSON.stringify(oamd)
  })) {
    fs.mkdirSync(path.dirname(path.join(root, p)), {recursive: true});
    fs.writeFileSync(path.join(root, p), body);
  }
  const module = read(path.join(xpon, 'xpon-agent/files/usr/share/ucode/xpon/summary.uc'))
    .replace(/(['`])\/(sys|var)\//g, `$1${root}/$2/`);
  fs.mkdirSync(path.join(tmp, 'xpon'), {recursive: true});
  fs.writeFileSync(path.join(tmp, 'xpon/summary.uc'), module);
  // The driver state comes over netlink through omci.so; a stand-in for the host.
  fs.writeFileSync(path.join(tmp, 'omci.uc'), `export function xpon_get(ifname) { return ${JSON.stringify({
    tx_allow: true, tx_hold: false, signal_detect: true, epon_sync: true, epon_registered: 1,
    epon_holdover_ms: 200, epon_mac: 'a8:42:a1:85:11:47', ddm: eponDdm, thresholds})}; };\n`);
  const lib = path.join(repo, 'staging_dir/hostpkg/lib');
  eponPublic = JSON.parse(execFileSync(ucode, ['-L', path.join(lib, 'ucode'), '-L', `${tmp}/*.uc`, '-e',
    `import { summary } from 'xpon.summary'; print(summary())`], {env: {...process.env, LD_LIBRARY_PATH: lib}}));
  assert.equal(eponPublic.mode, 'epon');
  assert.equal(eponPublic.ddm.rx_nw, 12000);
  assert.equal(eponPublic.thresholds.rx_warn_low_nw, 1300);
  assert.equal(eponPublic.epon.mac.registered, 1);
  assert.equal(eponPublic.epon.mac.signal, 1);
  assert.equal(eponPublic.epon.oam.olt_mac, '00:13:25:88:00:00');
  assert.equal(eponPublic.epon.allow_tx, true);
  assert(!JSON.stringify(eponPublic).includes('SECRET'), 'xpon summary must not expose the LOID');
  assert.equal(eponPublic.epon.oam.keys, undefined);
  const acl = JSON.parse(read(path.join(xpon, 'xpon-agent/files/usr/share/rpcd/acl.d/xpon-public.json')));
  assert.deepEqual(acl.unauthenticated.read.ubus, {xpon: ['summary', 'status', 'status_live', 'vlan_status']});
} else {
  console.log('xpon summary: skipped, no host ucode');
}
eponPublic ??= {mode: 'epon', ddm: eponDdm, thresholds,
  epon: {mac: {signal: 1, sync: 1, registered: 1}, oam: {...oamd, loid: undefined}, allow_tx: true, tx_hold: false}};

/* --- xpon-stats on the router, against a fake ONU --- */
const bin = path.join(tmp, 'bin');
fs.mkdirSync(bin);
const shim = (name, body) => fs.writeFileSync(path.join(bin, name), body, {mode: 0o755});
// jsonfilter -s JSON -e '@.a.b[1]['x-y'][*]': objects as JSON, scalars raw, nothing found fails.
shim('jsonfilter', `#!/usr/bin/env node
const a = process.argv.slice(2), src = a[a.indexOf('-s') + 1], expr = a[a.indexOf('-e') + 1];
let nodes; try { nodes = [JSON.parse(src)]; } catch { process.exit(1); }
for (const t of expr.slice(1).match(/\\.[\\w-]+|\\[\\*\\]|\\[\\d+\\]|\\['[^']+'\\]/g) || []) {
  nodes = nodes.flatMap(n => n == null || typeof n != 'object' ? [] : t == '[*]' ? Object.values(n) :
    [n[t[0] == '.' ? t.slice(1) : t.startsWith("['") ? t.slice(2, -2) : +t.slice(1, -1)]]).filter(n => n !== undefined);
}
if (!nodes.length) process.exit(1);
console.log(nodes.map(n => typeof n == 'object' ? JSON.stringify(n) : String(n)).join('\\n'));
`);
shim('uci', `#!/bin/sh\n[ "$3" = xpon-stats.main.host ] && echo "$STATS_HOST" || exit 1\n`);
// The ONU: answers each ubus call from $ONU/<object>.<method>.json, or fails.
shim('uclient-fetch', `#!/bin/sh
while [ $# -gt 1 ]; do [ "$1" = --post-data ] && req=$2; shift; done
call=$(echo "$req" | sed 's/.*"0\\{32\\}","\\([^"]*\\)","\\([^"]*\\)".*/\\1.\\2/')
[ -f "$ONU/$call.json" ] || exit 1
printf '{"jsonrpc":"2.0","id":1,"result":[0,%s]}' "$(cat "$ONU/$call.json")"
`);
const script = path.join(pkg, 'root/usr/bin/xpon-stats');
function stats(onu, ...args) {
  const dir = fs.mkdtempSync(path.join(tmp, 'onu-'));
  for (const [call, body] of Object.entries(onu))
    fs.writeFileSync(path.join(dir, call + '.json'), JSON.stringify(body));
  try {
    return execFileSync('sh', [script, ...args], {env: {...process.env, PATH: bin + ':' + process.env.PATH,
      ONU: dir, STATS_HOST: '10.0.0.2'}, encoding: 'utf8'});
  } catch (e) {
    return e.stdout;
  }
}
const gponStatus = {state: 5, onu_id: 3, gem_port: 0, agent_operational: 1, fec_downstream: 2, bosa_rx_power_nw: 15000,
  bosa_tx_power_nw: 1800000, mib_objects: 120, olt_profile_effective_name: 'nokia-alcl',
  identity: {serial: 'TPLG12345678', 'vendor-id': 'TPLG'}, olt: {vendor_id: 'ALCL'},
  omci_alarms: [{class: 11, entity: '0101', reported: true, alarms: ['lan-los']}]};
const gponOnu = {'xpon.status': {status: gponStatus}, 'xpon.status_live': {status: {state: 4}},
  'xpon.vlan_status': {mode: 'transparent', port: 'pon0'}};

let json = JSON.parse(stats(gponOnu, '--json'));
assert.deepEqual([json.host, json.mode, json.public, json.status.state], ['10.0.0.2', 'gpon', null, 5]);
assert.equal(json.vlan.mode, 'transparent');
json = JSON.parse(stats(gponOnu, '--json', '--live'));
assert.equal(json.status.state, 4);
json = JSON.parse(stats({'xpon.summary': {mode: 'gpon', ddm: {}}, ...gponOnu}, '--json'));
assert.deepEqual([json.mode, json.public.mode, json.status.onu_id], ['gpon', 'gpon', 3]);
json = JSON.parse(stats({'xpon.summary': eponPublic, ...gponOnu}, '--json'));
assert.deepEqual([json.mode, json.status, json.public.epon.oam.oam], ['epon', null, 'send_any']);
assert.deepEqual(JSON.parse(stats({}, '--json')), {host: '10.0.0.2', error: 'unreachable'});

let out = stats(gponOnu);
assert.match(out, /^GPON ONU 10\.0\.0\.2/);
assert.match(out, /O5 operation, ONU-ID 3, OMCC GEM 0, FEC down\/up on\/set by OLT/);
assert.match(out, /operational, OLT profile nokia-alcl/);
assert.match(out, /RX -18\.24 dBm\s+TX 2\.55 dBm/);
assert.match(out, /OLT alarms UNI 1: lan-los/);
out = stats({'xpon.summary': eponPublic});
assert.match(out, /^EPON ONU/);
assert.match(out, /signal present, sync yes, MPCP registered/);
assert.match(out, /send_any, CTC ok 48, LOID authentication ok, OLT 00:13:25:88:00:00/);
assert.match(out, /RX -19\.21 dBm\s+TX 3\.01 dBm/);
assert.match(out, /Alarms\s+RX low power \(LOS\)/);
assert.match(stats({}), /not reachable/);

/* --- the LuCI view --- */
function view(data, live) {
  const polls = [], nodes = new Map(), execs = [];
  const context = vm.createContext({
    view: {extend: x => x}, poll: {add: (fn, s) => polls.push({fn, s})},
    dom: {content: (node, c) => { node.children = c; }},
    fs: {exec_direct: (cmd, args) => { execs.push(args.join(' ')); return Promise.resolve(args.includes('--live') ? live : data); }},
    form: {Map: function() { return {section: () => ({option: () => ({})}), render: () => Promise.resolve({tag: 'map'})}; },
      NamedSection: {}, Value: {}},
    document: {getElementById: id => nodes.get(id), head: {appendChild: n => n}},
    _: x => x,
    E: (tag, attrs, children) => {
      if (children === undefined && (Array.isArray(attrs) || typeof attrs != 'object')) { children = attrs; attrs = {}; }
      const node = {tag, attrs, children};
      if (attrs?.id) nodes.set(attrs.id, node);
      return node;
    },
    L: {resolveDefault: (p, d) => p.catch(() => d)}
  });
  vm.runInContext(`String.prototype.format = function(...args) {
    let i = 0; return this.replace(/%%|%[0-9.]*[sdxXf]|%02X|%02x/g, m => m == '%%' ? '%' :
      m.endsWith('f') ? Number(args[i++]).toFixed(+(m.match(/\\.(\\d)/)?.[1] ?? 6)) :
      /x/i.test(m) ? Number(args[i++]).toString(16) : String(args[i++]));
  };`, context);
  const page = vm.runInContext('(function(){' + read(path.join(pkg, 'htdocs/luci-static/resources/view/xpon-stats/status.js')) + '\n})()', context);
  return {page, polls, nodes, execs};
}
const flat = n => n == null ? '' : typeof n != 'object' ? String(n) : Array.isArray(n) ? n.map(flat).join(' ') : flat(n.children);

const gponData = {host: '10.0.0.2', mode: 'gpon', public: null, status: gponStatus, vlan: {mode: 'transparent', port: 'pon0'}};
let lv = view({...gponData, public: {mode: 'gpon', thresholds}}, null);
await lv.page.render(await lv.page.load());
assert.match(flat(lv.nodes.get('xpon-stats-live')), /Receive In range -18\.24 dBm .*above the low warning/);
assert.match(flat(lv.nodes.get('xpon-stats-live')), /Transmit In range 2\.55/);
lv = view({...gponData, status: {...gponStatus, bosa_rx_power_nw: 1200}, public: {mode: 'gpon', thresholds}}, null);
await lv.page.render(await lv.page.load());
assert.match(flat(lv.nodes.get('xpon-stats-live')), /Receive Warning -29\.21/);
lv = view({...gponData, status: {...gponStatus, bosa_rx_power_nw: 900}, public: {mode: 'gpon', thresholds}}, null);
await lv.page.render(await lv.page.load());
assert.match(flat(lv.nodes.get('xpon-stats-live')), /Receive Alarm -30\.46/);
let v = view(gponData, {host: '10.0.0.2', mode: 'gpon', status: {state: 4, bosa_rx_power_nw: 0}});
let page = await v.page.render(await v.page.load());
let shown = flat(v.nodes.get('xpon-stats-live'));
assert.match(shown, /Registered as ONU 3 and provisioned/);
assert.match(shown, /In window -18\.24/);
assert.match(shown, /Near limit 2\.55/);
assert.match(shown, /TPLG12345678/);
assert.match(shown, /UNI 1: LAN LOS/);
assert.equal(v.polls[0].s, 10);
await v.polls[0].fn();
shown = flat(v.nodes.get('xpon-stats-live'));
assert.match(shown, /measuring the fibre distance/);
assert.match(shown, /No light/);
assert.match(shown, /TPLG12345678/, 'live polls keep the identity from the full read');
assert.deepEqual(v.execs, ['--json', '--json --live']);

v = view({host: '10.0.0.2', mode: 'epon', public: eponPublic, status: null}, null);
await v.page.render(await v.page.load());
shown = flat(v.nodes.get('xpon-stats-live'));
assert.match(shown, /OAM link up with OLT 00:13:25:88:00:00; the service is up/);
assert.match(shown, /In range -19\.21 dBm .*Module: warning -28\.9 to -7\.0 dBm/);
assert.match(shown, /RX low power \(LOS\)/);
assert.match(shown, /Tag, set locally/);
assert.doesNotMatch(shown, /SECRET/);

v = view({host: '10.0.0.2', error: 'unreachable'}, null);
await v.page.render(await v.page.load());
assert.match(flat(v.nodes.get('xpon-stats-live')), /not reachable/);

const acl = JSON.parse(read(path.join(pkg, 'root/usr/share/rpcd/acl.d/luci-app-xpon-stats.json')))['luci-app-xpon-stats'];
assert.deepEqual(Object.keys(acl.read.file).sort(), ['/usr/bin/xpon-stats --json', '/usr/bin/xpon-stats --json --live']);
fs.rmSync(tmp, {recursive: true, force: true});
console.log('xpon summary, xpon-stats and the GPON/EPON stats view passed');
