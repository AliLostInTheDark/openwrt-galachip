// SPDX-License-Identifier: GPL-2.0-only
/*
 * OMCI voice provisioning (G.988 9.9) for the uvoice stack.
 *
 * provisioning() turns the SIP managed entities the OLT created into one
 * settings table per POTS port, following the pointers from the VoIP voice
 * CTP: SIP user data, SIP agent config data, authentication security method,
 * network address, large string, TCP/UDP config data, VoIP media profile,
 * voice service profile and RTP profile data.
 *
 * reports() turns uvoice's status into the read-only attributes the ONU
 * keeps: the POTS UNI hook and operational state, the VoIP line status, the
 * SIP agent status and the VoIP configuration state.
 */

export const CLASS = {
	pots_uni: 53, voice_service_profile: 58, tcp_udp: 136, network_address: 137,
	voip_config: 138, voice_ctp: 139, line_status: 141, media_profile: 142,
	rtp_profile: 143, auth: 148, sip_agent: 150, sip_user: 153, large_string: 157,
};

const POTS_SLOT = 0x02;
const NULL_POINTER = 0xffff;

/* G.988 9.9.5 codec selection: RTP payload types, as uvoice names them */
const CODECS = { '0': 'pcmu', '8': 'pcma', '18': 'g729', '9': 'g722' };

function bytes(data) {
	let out = [];

	for (let i = 0; i + 1 < length(data ?? ''); i += 2)
		push(out, hex(substr(data, i, 2)));
	return out;
}

function u8(b, at) { return b[at] ?? 0; }
function u16(b, at) { return (u8(b, at) << 8) | u8(b, at + 1); }
function u32(b, at) { return (u16(b, at) << 16) | u16(b, at + 2); }

/* A fixed-length string attribute: up to the first NUL, printable only */
function text(b, at, len) {
	let s = '';

	for (let i = at; i < at + len; i++) {
		let c = u8(b, i);

		if (c == 0)
			break;
		if (c >= 0x20 && c < 0x7f)
			s += chr(c);
	}
	return s;
}

function hexbyte(v) { return sprintf('%02x', v & 0xff); }

function index(objects) {
	let mib = {};

	for (let o in objects) {
		mib[o.class_id] ??= {};
		mib[o.class_id][o.entity_id] = { origin: o.origin, data: bytes(o.data) };
	}
	return mib;
}

function lookup(mib, class_id, entity_id) {
	return entity_id == NULL_POINTER ? null : mib[class_id]?.[entity_id];
}

/* G.988 9.12.5: the parts of a large string, joined */
function large_string(mib, id) {
	let o = lookup(mib, CLASS.large_string, id);

	if (!o)
		return null;
	let s = '';
	for (let i = 0; i < min(u8(o.data, 0), 15); i++)
		s += text(o.data, 1 + i * 25, 25);
	return length(s) ? s : null;
}

/* G.988 9.12.3: a network address names a large string (and its credentials) */
function network_address(mib, id) {
	let o = lookup(mib, CLASS.network_address, id);

	return o ? large_string(mib, u16(o.data, 2)) : null;
}

/* sip:user@host;params -> { user, host } */
function split_uri(uri) {
	let m = match(uri ?? '', /^(sips?:)?([^@;]*)@?([^;>]*)/);

	if (!m)
		return {};
	return length(m[3]) ? { user: m[2], host: m[3] } : { user: m[2] };
}

function credentials(mib, id) {
	let o = lookup(mib, CLASS.auth, id);

	if (!o)
		return {};
	return {
		username: text(o.data, 1, 25) + text(o.data, 76, 14),
		password: text(o.data, 26, 25),
		realm: text(o.data, 51, 25),
	};
}

function codecs(o) {
	let order = [];

	for (let i = 0; i < 4; i++) {
		let name = CODECS[`${u8(o.data, 3 + i * 3)}`];

		if (name && !(name in order))
			push(order, name);
	}
	return {
		codecs: order,
		silence_suppression: u8(o.data, 5) ? 1 : 0,
		packet_period: u8(o.data, 4),
	};
}

/*
 * Settings per POTS port number (1-based), only for ports the OLT provisioned
 * with a VoIP voice CTP and SIP user data. Each value is the uvoice option it
 * sets; a missing value leaves the local setting alone.
 */
export function provisioning(objects) {
	let mib = index(objects), ports = {};

	for (let ctp_id, ctp in mib[CLASS.voice_ctp] ?? {}) {
		let pptp = u16(ctp.data, 2);
		let user = lookup(mib, CLASS.sip_user, u16(ctp.data, 0));

		if (pptp >> 8 != POTS_SLOT || !user)
			continue;

		let agent = lookup(mib, CLASS.sip_agent, u16(user.data, 0));
		let aor = split_uri(large_string(mib, u16(user.data, 2)));
		let auth = credentials(mib, u16(user.data, 29));
		let p = {
			enabled: 1,
			sip_username: aor.user,
			sip_callerid_name: text(user.data, 4, 25) || null,
			sip_auth_username: auth.username || null,
			sip_secret: auth.password || null,
			sip_realm: auth.realm || null,
		};

		if (agent) {
			let expiry = u32(agent.data, 14);
			let transport = lookup(mib, CLASS.tcp_udp, u16(agent.data, 12));

			p.sip_proxy = large_string(mib, u16(agent.data, 0));
			p.sip_outbound_proxy = large_string(mib, u16(agent.data, 2));
			p.sip_domain = large_string(mib, u16(agent.data, 22)) ?? aor.host;
			p.sip_registrar = network_address(mib, u16(agent.data, 25));
			p.sip_registration_expiry = expiry ? expiry : null;
			if (transport) {
				p.sip_local_port = u16(transport.data, 0) || null;
				p.transport = { '6': 'tcp', '17': 'udp' }[`${u8(transport.data, 2)}`];
			}
			p.sip_agent = u16(user.data, 0);
		} else {
			p.sip_domain = aor.host;
		}

		let media = lookup(mib, CLASS.media_profile, u16(ctp.data, 4));
		if (media) {
			let c = codecs(media);
			let vsp = lookup(mib, CLASS.voice_service_profile, u16(media.data, 1));

			p.codecs = c.codecs;
			p.silence_suppression = c.silence_suppression;
			if (vsp) {
				p.jitter_target = u16(vsp.data, 1) || null;
				p.lec_enable = u8(vsp.data, 5) ? 1 : 0;
			}
		}

		ports[pptp & 0xff] = p;
	}
	return ports;
}

/* G.988 9.9.11 VoIP line state from the uvoice line machine */
const LINE_STATE = {
	IDLE: 0, OFFHOOK_DIAL: 1, COLLECTING: 2, SPRE_COLLECT: 2, XFER_COLLECT: 2,
	RINGING: 3, CW_OFFERED: 3, CALLING: 5, PROCEEDING: 4, CONNECTED: 6,
	TWO_CALLS: 6, CONF: 6, HOLDING: 6, HOLD_DIAL: 1, PENDING: 5, CLEARING: 7,
	ROH: 9, SILENT: 8, OOS: 10,
};

/* G.988 9.9.11 VoIP voice server status, and 9.9.3 SIP status */
function server_status(account) {
	switch (account?.state) {
	case 'registered':
	case 'refreshing':
		return { line: 1, sip: 1 };
	case 'backoff':
		if (account.last_code == 401 || account.last_code == 403 || account.last_code == 407)
			return { line: 5, sip: 4 };
		if (account.last_code >= 500)
			return { line: 7, sip: 4 };
		return { line: 6, sip: 5 };
	}
	return { line: 0, sip: 0 };
}

function report(class_id, entity_id, fields) {
	let mask = 0, data = [];

	for (let f in fields) {
		mask |= 1 << (16 - f.attr);
		for (let i = 0; i < f.len; i++)
			data[f.at + i] = (f.value >> (8 * (f.len - 1 - i))) & 0xff;
	}
	for (let i = 0; i < length(data); i++)
		data[i] ??= 0;
	return { class_id, entity_id, attr_mask: mask, data: join('', map(data, hexbyte)) };
}

/*
 * Reports for every provisioned port from `voice status`: line N of uvoice is
 * POTS port N + 1.
 */
export function reports(status, ports, applied) {
	let out = [], agents = {};

	for (let n, p in ports) {
		let id = (POTS_SLOT << 8) | +n;
		let call = filter(status?.call ?? [], c => c.line == +n - 1)[0];
		let account = filter(status?.lines ?? [], l => l.line == +n - 1)[0];
		let state = call?.state ?? 'OOS';
		let server = server_status(account);
		let offhook = !(state in ['IDLE', 'RINGING', 'OOS']);
		let leg = call?.calls?.[0];

		push(out, report(CLASS.pots_uni, id, [
			{ attr: 9, at: 9, len: 1, value: state == 'OOS' ? 1 : 0 },
			{ attr: 10, at: 10, len: 1, value: offhook ? 1 : 0 },
		]));
		push(out, report(CLASS.line_status, id, [
			{ attr: 1, at: 0, len: 2, value: leg?.codec == 'pcma' ? 8 : leg?.codec == 'g729' ? 18 : 0 },
			{ attr: 2, at: 2, len: 1, value: server.line },
			{ attr: 3, at: 3, len: 1, value: leg ? 1 : 0 },
			{ attr: 4, at: 4, len: 2, value: leg?.ptime ?? 0 },
			{ attr: 8, at: 58, len: 1, value: LINE_STATE[state] ?? 10 },
		]));
		if (p.sip_agent != null && !(p.sip_agent in agents)) {
			agents[p.sip_agent] = true;
			push(out, report(CLASS.sip_agent, p.sip_agent, [
				{ attr: 9, at: 24, len: 1, value: server.sip },
			]));
		}
	}
	push(out, report(CLASS.voip_config, 0, [
		{ attr: 6, at: 9, len: 1, value: length(ports) ? (applied ? 1 : 2) : 0 },
	]));
	return out;
}
