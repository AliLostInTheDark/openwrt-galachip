// The ACS the OLT configured (ME 340 -> 137 -> 157 and 148), shown masked or in clear,
// and the JSON snapshot in both forms. Run: node tests/acs-view.mjs
import fs from 'node:fs';
const R = new URL('../htdocs/luci-static/resources/', import.meta.url).pathname;
String.prototype.format = function(...a) { let i = 0; return this.replace(/%(0?)(\d*)([sdx])/g, (m, z, w, t) => { let v = a[i++]; v = t === 'x' ? Number(v).toString(16) : String(v); return w ? v.padStart(+w, z ? '0' : ' ') : v; }); };
const _ = (s) => s;
// minimal fake DOM: E() builds {tag, attrs, kids}; text() flattens
const E = (tag, attrs, kids) => {
  if (Array.isArray(attrs) || typeof attrs === 'string' || (attrs && attrs.tag)) { kids = attrs; attrs = {}; }
  const k = kids == null ? [] : Array.isArray(kids) ? kids : [kids];
  return { tag, attrs: attrs || {}, kids: k.flat(Infinity) };
};
const flat = (n) => n == null ? '' : typeof n === 'object' ? [n.kids.map(flat).join(' ')].join('') : String(n);
const baseclass = { extend: (o) => o };
const view = { extend: (o) => o };
const dom = { content: () => {} };
const load = (file, injected, tail) => {
  let src = fs.readFileSync(R + file, 'utf8').replace(/^'use strict';/m, '').replace(/^'require[^\n]*\n/gm, '');
  src = src.replace(/\nreturn (baseclass|view)\.extend\(/, '\nvar __exp = $1.extend(') + '\n' + tail;
  return new Function('E', '_', 'baseclass', 'view', 'dom', 'L', 'poll', 'layout', 'widgets', 'omci', 'mib', 'document', 'window', ...Object.keys(injected), src)(E, _, baseclass, view, dom, {}, {}, {}, {}, {}, injected.mib || {}, { getElementById: () => null }, {}, ...Object.values(injected));
};
const mib = load('omci/mib.js', {}, 'return __exp;');
const status = load('view/omci/status.js', { mib }, 'return { renderOltDetails, snapshot, setCache: (o) => { cachedObjects = o; }, setShow: (v) => { showSecrets = v; } };');

const hex = (b) => b.map((x) => x.toString(16).padStart(2, '0')).join('');
const pad = (s, n) => { const a = [...Buffer.from(s)]; while (a.length < n) a.push(0); return a; };
const url = 'http://acs.isp.example:7547/cwmp';
const objs = [
  { class_id: 340, entity_id: 1, data: hex([1, 0, 7, 0xff, 0xff]), fields: { admin_state: 'unlocked', acs_address: '0x0007' }, olt_written: true },
  { class_id: 137, entity_id: 7, data: hex([0, 9, 0, 5]), fields: {} },
  { class_id: 157, entity_id: 5, data: hex([2, ...pad(url.slice(0, 25), 25), ...pad(url.slice(25), 25)]), fields: {} },
  { class_id: 148, entity_id: 9, data: hex([1, ...pad('subscriber42', 25), ...pad('hunter2pw', 25), ...pad('isp', 25), ...pad('', 14)]), fields: {} },
  { class_id: 134, entity_id: 3, data: '', fields: { ip: '10.20.30.40', gateway: '10.20.30.1', primary_dns: '8.8.8.8' }, olt_written: true },
];
status.setCache(objs);
let bad = 0;
const check = (what, ok) => { if (!ok) { console.log('FAIL', what); bad++; } };

status.setShow(false);
const masked = flat(status.renderOltDetails(objs));
check('masked view shows the ACS URL', masked.includes(url));
check('masked view shows the username', masked.includes('subscriber42'));
check('masked view hides the password', !masked.includes('hunter2pw') && masked.includes('•••'));
check('masked view shows the IP host', masked.includes('10.20.30.40'));

status.setShow(true);
const clear = flat(status.renderOltDetails(objs));
check('unmasked view shows the password', clear.includes('hunter2pw'));

const snapM = JSON.stringify(status.snapshot({ serial: 'ZYOT12345678', identity: { loid: 'LOID1' } }, null, {}, false));
const snapC = JSON.stringify(status.snapshot({ serial: 'ZYOT12345678', identity: { loid: 'LOID1' } }, null, {}, true));
check('masked snapshot hides the password, user, serial and LOID', !/hunter2pw|subscriber42|ZYOT12345678|LOID1/.test(snapM) && snapM.includes('"masked":true'));
check('masked snapshot keeps the ACS URL', snapM.includes(url));
check('unmasked snapshot keeps them', /hunter2pw/.test(snapC) && /subscriber42/.test(snapC) && /ZYOT12345678/.test(snapC) && snapC.includes('"masked":false'));
console.log(bad ? 'FAILED' : 'render checks passed (masked and unmasked views, both snapshots)');
process.exit(bad ? 1 : 0);
