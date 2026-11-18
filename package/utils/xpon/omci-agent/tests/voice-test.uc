// Run: ucode -L "$PWD/files/usr/share/ucode/*.uc" tests/voice-test.uc
import { provisioning, reports, CLASS } from 'omci.voice';

let fails = 0;

function check(what, got, want) {
	if (sprintf('%J', got) !== sprintf('%J', want)) {
		warn(`${what}: got ${sprintf('%J', got)}, want ${sprintf('%J', want)}\n`);
		fails++;
	}
}

function hex(list) {
	return join('', map(list, b => sprintf('%02x', b)));
}

function str(s, len) {
	let out = [];

	for (let i = 0; i < len; i++)
		push(out, i < length(s) ? ord(s, i) : 0);
	return out;
}

function be16(v) { return [ (v >> 8) & 0xff, v & 0xff ]; }
function be32(v) { return [ ...be16(v >> 16), ...be16(v) ]; }

function large(s) {
	let parts = [ 1, ...str(s, 25) ];

	for (let i = 1; i < 15; i++)
		parts = [ ...parts, ...str('', 25) ];
	return parts;
}

function object(class_id, entity_id, data) {
	return { class_id, entity_id, origin: 2, data: hex(data) };
}

/* One provisioned port: CTP -> SIP user -> agent, auth, strings, media */
let mib = [
	object(CLASS.voice_ctp, 1, [ ...be16(0x10), ...be16(0x0201), ...be16(0x20), 0 ]),
	object(CLASS.sip_user, 0x10, [ ...be16(0x30), ...be16(0x40), ...str('Home', 25),
		...be16(0x50), ...be16(0xffff), ...be32(0), ...be16(0xffff), ...be16(0xffff),
		...be16(0xffff), ...be16(0x0201), 0, 0 ]),
	object(CLASS.sip_agent, 0x30, [ ...be16(0x41), ...be16(0xffff), ...be32(0), ...be32(0),
		...be16(0x60), ...be32(3600), ...be32(0), ...be16(0x42), 0, ...be16(0x70),
		...be32(0), 0, 0, 0, 0, 0, 0, 0, ...be16(0xffff) ]),
	object(CLASS.large_string, 0x40, large('sip:+911234567890@ims.example.in')),
	object(CLASS.large_string, 0x41, large('proxy.example.in:5060')),
	object(CLASS.large_string, 0x42, large('ims.example.in')),
	object(CLASS.large_string, 0x43, large('registrar.example.in')),
	object(CLASS.network_address, 0x70, [ ...be16(0xffff), ...be16(0x43) ]),
	object(CLASS.auth, 0x50, [ 1, ...str('+911234567890@ims', 25), ...str('s3cret', 25),
		...str('ims.example.in', 25), ...str('.example.in', 14) ]),
	object(CLASS.tcp_udp, 0x60, [ ...be16(5060), 17, 0, ...be16(0) ]),
	object(CLASS.media_profile, 0x20, [ 0, ...be16(0x21), 8, 20, 0, 0, 20, 0, 18, 20, 0,
		99, 20, 0, 1, ...be16(0xffff) ]),
	object(CLASS.voice_service_profile, 0x21, [ 0, ...be16(40), ...be16(200), 1 ]),
	/* a CTP on an Ethernet slot is not a phone port */
	object(CLASS.voice_ctp, 2, [ ...be16(0x10), ...be16(0x0101), ...be16(0x20), 0 ]),
];

let ports = provisioning(mib);
let p = ports['1'];

check('ports', keys(ports), [ '1' ]);
check('user', p.sip_username, '+911234567890');
check('display name', p.sip_callerid_name, 'Home');
check('auth user', p.sip_auth_username, '+911234567890@ims.example.in');
check('secret', p.sip_secret, 's3cret');
check('realm', p.sip_realm, 'ims.example.in');
check('proxy', p.sip_proxy, 'proxy.example.in:5060');
check('domain', p.sip_domain, 'ims.example.in');
check('registrar', p.sip_registrar, 'registrar.example.in');
check('expiry', p.sip_registration_expiry, 3600);
check('local port', p.sip_local_port, 5060);
check('transport', p.transport, 'udp');
check('codecs', p.codecs, [ 'pcma', 'pcmu', 'g729' ]);
check('jitter', p.jitter_target, 40);
check('echo cancel', p.lec_enable, 1);
check('nothing provisioned', provisioning([]), {});

/* A registered line, off hook in a call */
let status = {
	call: [ { line: 0, state: 'CONNECTED', calls: [ { codec: 'pcma', ptime: 20 } ] } ],
	lines: [ { line: 0, state: 'registered' } ],
};
let r = reports(status, ports, true);
let by = {};
for (let x in r)
	by[`${x.class_id}/${x.entity_id}`] = x;

check('pots mask', by['53/513'].attr_mask, (1 << 7) | (1 << 6));
check('pots hook', substr(by['53/513'].data, 18, 4), '0001');
check('line status', substr(by['141/513'].data, 4, 2), '01');
check('line state', substr(by['141/513'].data, 116, 2), '06');
check('sip status', substr(by['150/48'].data, 48, 2), '01');
check('config state', substr(by['138/0'].data, 18, 2), '01');

let r2 = reports({ lines: [ { line: 0, state: 'backoff', last_code: 403 } ] }, ports, true);
check('auth failure', substr(filter(r2, x => x.class_id == 141)[0].data, 4, 2), '05');

if (fails)
	exit(1);
print('OMCI voice provisioning checks passed\n');
