// SPDX-License-Identifier: GPL-2.0-only
/*
 * epon-oamd: IEEE 802.3ah OAM and CTC extended OAM for a single-LLID EPON
 * ONU, on top of the gc1601-pon driver's pon0 netdev.
 *
 * The EPON MAC does MPCP in hardware. This daemon answers the OLT's OAM on
 * LLID 0: passive 802.3ah discovery, CTC extended discovery, LOID
 * authentication, churning keys and the CTC attributes an OLT reads or
 * writes while provisioning an SFU. Behaviour follows the stock gcoamd.
 * Unknown attributes are always answered
 * (0x86), never dropped, so OLT provisioning does not stall.
 *
 * Driver interface (pon0's platform device in sysfs): epon_state,
 * epon_mac, epon_oam_link, epon_register, epon_key, sfu_untag_vlan and
 * sfu_vlan_map. Status for LuCI: /var/run/epon-oamd.json.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ETH_P_SLOW		0x8809
#define SLOW_OAM		0x03

#define OAM_INFO		0x00
#define OAM_EVENT		0x01
#define OAM_VAR_REQ		0x02
#define OAM_LOOPBACK		0x04
#define OAM_ORG			0xfe

#define FLAG_LOCAL_EVAL		0x0008
#define FLAG_LOCAL_STABLE	0x0010
#define FLAG_REMOTE_EVAL	0x0020
#define FLAG_REMOTE_STABLE	0x0040

#define TLV_END			0x00
#define TLV_LOCAL		0x01
#define TLV_REMOTE		0x02
#define TLV_ORG			0xfe

#define CTC_OUI			0x111111
#define VSOL_OUI		0x8014a8	/* VSOL OLTs: CTC opcodes plus inter-authentication */
#define CTC_GET_REQ		0x01
#define CTC_GET_RESP		0x02
#define CTC_SET_REQ		0x03
#define CTC_SET_RESP		0x04
#define CTC_AUTH		0x05
#define CTC_SW_DOWNLOAD		0x06
#define CTC_CHURNING		0x09
#define CTC_EVENT		0xff

#define BR_STD_ATTR		0x07
#define BR_STD_ACTION		0x09
#define BR_INST_V20		0x36
#define BR_INST_V21		0x37
#define BR_EXT_ATTR		0xc7
#define BR_EXT_ACTION		0xc9

#define VAR_SET_OK		0x80
#define VAR_BAD		0x86	/* not supported / bad parameters */

#define LOCAL_PDU_SIZE		1536	/* stock gcoamd advertises 0x600 */
#define KEEPALIVE_MS		1000
#define LINK_LOST_MS		5000
#define SILENCE_MS		60000
#define CTC_WAIT_MS		30000	/* stock: SEND_ANY this long without CTC discovery */
#define CTC_SILENCE_MS		5000
#define RESTART_SILENCE_MS	2000	/* off the PON so the OLT renegotiates CTC */
#define DISC_TIMEOUT_MS		30000	/* registered this long without an OAM link */

/* CTC PON interface alarms (object 4), stock order 0x0101..0x010a */
#define ALARM_FIRST		0x0101
#define ALARMS			10

enum disc_state { DISC_WAIT, DISC_SEND_LOCAL_REMOTE, DISC_SEND_ANY };
enum ctc_state { CTC_NONE, CTC_OFFERED, CTC_OK, CTC_FAIL };
enum auth_state { AUTH_NONE, AUTH_SENT, AUTH_OK, AUTH_FAIL };

static const char *const disc_names[] = { "wait", "send_local_remote", "send_any" };
static const char *const ctc_names[] = { "none", "offered", "ok", "failed" };
static const char *const auth_names[] = { "none", "sent", "ok", "failed" };

static const uint8_t slow_dst[ETH_ALEN] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x02 };
static const uint8_t ctc_versions[] = { 0x30, 0x21, 0x20, 0x13, 0x01 };

static struct {
	const char *ifname;
	char sysfs[256];
	int fd, ifindex;
	uint8_t mac[ETH_ALEN];
	int debug;

	/* identity, from UCI via the init script */
	char loid[25], password[13];
	char vendor[5], model[5], hw[9], sw[17], ext_model[17];
	char auth_secret[65];		/* VSOL inter-authentication, empty: off */
	uint32_t peer_oui;		/* OUI the OLT used, answered on */
	int interauth;			/* 0 not asked, 1 OLT key ok, -1 mismatch */

	/* VLAN override: "olt" applies the OLT's Port VLAN */
	char vlan_override[16];
	int vlan_override_vid;

	bool registered;
	bool active;		/* 802.3ah active mode: start discovery ourselves */
	/* Identity of the ONU this one replaces; unset: our own. */
	uint32_t info_oui;	/* Local Information OUI */
	uint8_t info_vendor[4];	/* Local Information vendor specific */
	bool info_vendor_set;
	uint8_t chipset_id[8];	/* CTC leaf 0x0003 */
	bool chipset_id_set;
	enum disc_state disc;
	enum ctc_state ctc;
	uint8_t ctc_version;
	enum auth_state auth;
	int auth_fail_reason;
	uint16_t flags, olt_flags;
	uint16_t revision;
	uint8_t olt_local[16];		/* the OLT's Local Information TLV body */
	bool have_olt_local;
	uint8_t olt_mac[ETH_ALEN];
	uint64_t last_rx_ms, last_tx_ms, silent_until_ms;

	/* CTC Port VLAN as the OLT set it */
	uint8_t vlan_mode;
	uint16_t vlan_default;
	struct { uint16_t old_vid, new_vid; } vlan_xlate[8];
	int vlan_xlates;
	uint16_t vlan_trunk[8];
	int vlan_trunks;
	uint8_t vlan_raw[128];		/* N:1 value as set, answered back on a Get */
	size_t vlan_raw_len;

	/* CTC holdover (0x0008): state 1 off, 2 on */
	uint32_t holdover_state, holdover_ms;

	/* gcoam_perodic_handler */
	uint64_t send_any_ms;		/* when the OAM link came up */
	uint64_t disc_since_ms;		/* registered without an OAM link since */
	bool ctc_checked;		/* the 30 s CTC discovery check ran */
	bool signal;
	uint64_t los_ms;		/* light lost, link still held over */

	/* CTC events: admin status (2 on) and thresholds per PON alarm */
	uint32_t alarm_status[ALARMS];
	uint32_t alarm_raise[ALARMS], alarm_clear[ALARMS];
	uint16_t alarms_up;		/* raised alarms, bit n = ALARM_FIRST + n */
	uint16_t event_seq;

	/* CTC ONU TX power control (0x00A1) */
	uint64_t tx_off_until_ms;
	bool tx_off_permanent;

	uint8_t key_index;
	unsigned long rx_pdus, tx_pdus, gets, sets, unknown_leaves, keys;
	bool status_dirty;
} oam = { .ifname = "pon0", .fd = -1, .vendor = "TPLG", .model = "XZG7",
	  .ext_model = "XZ000-G7", .peer_oui = CTC_OUI,
	  .hw = "V1.0", .sw = "OpenWrt", .vlan_override = "olt", .holdover_state = 1 };

/* A permanent CTC transmitter shutdown survives reboots, as stock's rogue flag. */
#define ROGUE_FILE		"/etc/xpon-rogue"

static volatile sig_atomic_t stop, stop_sig, stop_pid;

/*
 * Link events also go to the kernel log, next to the driver's EPON lines,
 * so a dmesg alone shows why the link went down and whether the daemon
 * itself was restarted.
 */
static int kmsg_fd = -1;

static void kmsg(const char *fmt, va_list ap)
{
	char line[256];
	int n;

	if (kmsg_fd < 0)
		return;
	n = snprintf(line, sizeof(line), "<5>epon-oamd[%d]: ", (int)getpid());
	n += vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	if (n >= (int)sizeof(line))
		n = sizeof(line) - 1;
	write(kmsg_fd, line, n);
}

static void logmsg(int level, const char *fmt, ...)
{
	va_list ap;

	if (level > oam.debug)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	if (!level) {
		va_start(ap, fmt);
		kmsg(fmt, ap);
		va_end(ap);
	}
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- driver ---- */

static int sysfs_write(const char *attr, const char *fmt, ...)
{
	char path[320], val[128];
	va_list ap;
	int fd, ret;

	va_start(ap, fmt);
	vsnprintf(val, sizeof(val), fmt, ap);
	va_end(ap);
	snprintf(path, sizeof(path), "%s/%s", oam.sysfs, attr);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	ret = write(fd, val, strlen(val)) < 0 ? -1 : 0;
	if (ret)
		logmsg(0, "%s: %s", path, strerror(errno));
	close(fd);
	return ret;
}

static int sysfs_read(const char *attr, char *buf, size_t size)
{
	char path[320];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s/%s", oam.sysfs, attr);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return 0;
}

static bool driver_registered(bool *signal)
{
	char buf[1024], *p;

	*signal = false;
	if (sysfs_read("epon_state", buf, sizeof(buf)))
		return false;
	p = strstr(buf, " signal=");
	*signal = p && p[8] == '1';
	p = strstr(buf, " registered=");
	return p && strtoul(p + 12, NULL, 0) & 1;
}

static void set_oam_link(bool up)
{
	sysfs_write("epon_oam_link", "%d", up);
}

static int apply_vlan(void)
{
	char map[256] = "";
	size_t len = 0;
	int i, untag = 0;
	uint8_t mode = oam.vlan_mode;
	uint16_t vid = oam.vlan_default;

	if (!strcmp(oam.vlan_override, "transparent")) {
		mode = 0;
	} else if (!strcmp(oam.vlan_override, "tag")) {
		mode = 1;
		vid = oam.vlan_override_vid;
	}

	switch (mode) {
	case 1:		/* tag: the LAN side is untagged */
		untag = vid;
		break;
	case 4:		/* trunk: untagged on the default VLAN, the listed ones pass tagged */
		untag = vid;
		for (i = 0; i < oam.vlan_trunks; i++)
			len += snprintf(map + len, sizeof(map) - len, "t%u:%u ",
					oam.vlan_trunk[i], oam.vlan_trunk[i]);
		break;
	case 2:		/* translation */
	case 3:		/* N:1: each source VLAN up to its target; down to the first source */
		untag = vid;
		for (i = 0; i < oam.vlan_xlates; i++)
			len += snprintf(map + len, sizeof(map) - len, "t%u:%u ",
					oam.vlan_xlate[i].new_vid, oam.vlan_xlate[i].old_vid);
		break;
	default:	/* transparent */
		break;
	}
	if (sysfs_write("sfu_untag_vlan", "%d", untag) ||
	    sysfs_write("sfu_vlan_map", "%s\n", map)) {
		logmsg(0, "VLAN hardware application failed");
		return -1;
	}
	logmsg(1, "VLAN: mode %u, untagged VLAN %d, map '%s'", mode, untag, map);
	oam.status_dirty = true;
	return 0;
}

/* ---- frames ---- */

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v;
}

static uint16_t get16(const uint8_t *p)
{
	return p[0] << 8 | p[1];
}

static void put24(uint8_t *p, uint32_t v)
{
	p[0] = v >> 16;
	p[1] = v >> 8;
	p[2] = v;
}

static uint32_t get24(const uint8_t *p)
{
	return p[0] << 16 | p[1] << 8 | p[2];
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, v >> 16);
	put16(p + 2, v);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)get16(p) << 16 | get16(p + 2);
}

/* Ethernet + slow protocols + OAM header; returns the data offset. */
static size_t oam_header(uint8_t *f, uint8_t code)
{
	memcpy(f, slow_dst, ETH_ALEN);
	memcpy(f + 6, oam.mac, ETH_ALEN);
	put16(f + 12, ETH_P_SLOW);
	f[14] = SLOW_OAM;
	put16(f + 15, oam.flags);
	f[17] = code;
	return 18;
}

static void oam_send(uint8_t *f, size_t len)
{
	struct sockaddr_ll sll = {
		.sll_family = AF_PACKET,
		.sll_ifindex = oam.ifindex,
		.sll_halen = ETH_ALEN,
	};

	if (len < 60) {
		memset(f + len, 0, 60 - len);
		len = 60;
	}
	memcpy(sll.sll_addr, slow_dst, ETH_ALEN);
	if (sendto(oam.fd, f, len, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
		logmsg(1, "send: %s", strerror(errno));
		return;
	}
	oam.tx_pdus++;
	oam.last_tx_ms = now_ms();
}

/* ---- 802.3ah discovery ---- */

/* Stock adopts a smaller OAMPDU size from the OLT (gcoam_check_info). */
static uint16_t local_pdu_size(void)
{
	uint16_t olt;

	if (!oam.have_olt_local)
		return LOCAL_PDU_SIZE;
	olt = get16(oam.olt_local + 5) & 0x7ff;
	return olt < LOCAL_PDU_SIZE ? olt : LOCAL_PDU_SIZE;
}

static size_t put_local_tlv(uint8_t *p)
{
	uint8_t state = oam.disc == DISC_SEND_ANY ? 0x00 : 0x06;	/* mux + parser discard */

	p[0] = TLV_LOCAL;
	p[1] = 16;
	p[2] = 0x01;			/* OAM version */
	put16(p + 3, oam.revision);
	p[5] = state;
	p[6] = 0x14 | oam.active;	/* variable retrieval, loopback, active mode bit 0 */
	put16(p + 7, local_pdu_size());
	put24(p + 9, oam.info_oui ? oam.info_oui : CTC_OUI);
	memcpy(p + 12, oam.info_vendor_set ? oam.info_vendor : oam.mac + 2, 4);
	return 16;
}

/* The CTC extended discovery TLV: our version list, or the agreed version. */
static size_t put_ctc_tlv(uint8_t *p, uint8_t version, bool list)
{
	size_t len = 7;
	unsigned int i;

	p[0] = TLV_ORG;
	put24(p + 2, CTC_OUI);
	p[5] = 1;			/* extension supported */
	p[6] = version;
	if (list) {
		for (i = 0; i < sizeof(ctc_versions); i++) {
			put24(p + len, CTC_OUI);
			p[len + 3] = ctc_versions[i];
			len += 4;
		}
	}
	p[1] = len;
	return len;
}

static void send_info(int ctc_reply, uint8_t ctc_version)
{
	uint8_t f[128];
	size_t len = oam_header(f, OAM_INFO);

	len += put_local_tlv(f + len);
	if (oam.have_olt_local) {
		f[len] = TLV_REMOTE;
		f[len + 1] = 16;
		memcpy(f + len + 2, oam.olt_local, 14);
		len += 16;
	}
	if (ctc_reply == 1)
		len += put_ctc_tlv(f + len, 0, true);
	else if (ctc_reply == 2)
		len += put_ctc_tlv(f + len, ctc_version, false);
	f[len++] = TLV_END;
	oam_send(f, len);
}

static void link_down(const char *why)
{
	if (oam.disc == DISC_SEND_ANY)
		set_oam_link(false);
	if (oam.disc != DISC_WAIT)
		logmsg(0, "OAM link down: %s", why);
	oam.disc = DISC_WAIT;
	oam.ctc = CTC_NONE;
	oam.auth = AUTH_NONE;
	oam.have_olt_local = false;
	oam.flags = FLAG_LOCAL_EVAL;
	oam.olt_flags = 0;
	/* Stock resets the Local Information per link; it counts up from link-up. */
	oam.revision = 0;
	oam.status_dirty = true;
}

/* Off the discovery window for a while, then registration again. */
static void go_silent(const char *why, uint64_t ms)
{
	link_down(why);
	logmsg(0, "silent for %llu s", (unsigned long long)ms / 1000);
	sysfs_write("epon_register", "0");
	oam.silent_until_ms = now_ms() + ms;
}

static int ctc_version_ok(uint8_t v)
{
	unsigned int i;

	for (i = 0; i < sizeof(ctc_versions); i++)
		if (ctc_versions[i] == v)
			return 1;
	return 0;
}

static void handle_info(const uint8_t *src, const uint8_t *d, size_t len)
{
	int ctc_reply = 0;
	uint8_t ctc_version = 0;
	size_t p = 0;

	while (p + 2 <= len && d[p] != TLV_END) {
		uint8_t type = d[p], tlen = d[p + 1];

		if (tlen < 2 || p + tlen > len)
			break;
		if (type == TLV_LOCAL && tlen == 16) {
			uint16_t size = get16(d + p + 7) & 0x7ff;

			if (size < 64) {
				logmsg(0, "OLT OAMPDU size %u too small", size);
			} else {
				memcpy(oam.olt_local, d + p + 2, 14);
				oam.have_olt_local = true;
			}
		} else if (type == TLV_ORG && tlen >= 7 && get24(d + p + 2) == CTC_OUI) {
			bool list = tlen > 7;
			uint8_t v = d[p + 6];

			if (list) {
				/* the OLT offers its versions; answer with ours */
				oam.ctc = CTC_OFFERED;
				ctc_reply = 1;
			} else if (ctc_version_ok(v)) {
				if (oam.ctc != CTC_OK || oam.ctc_version != v)
					logmsg(0, "CTC extended OAM version %#04x", v);
				oam.ctc = CTC_OK;
				oam.ctc_version = v;
				ctc_reply = 2;
				ctc_version = v;
			} else {
				logmsg(0, "CTC version %#04x not supported", v);
				oam.ctc = CTC_FAIL;
			}
			oam.status_dirty = true;
		}
		p += tlen;
	}
	if (!oam.have_olt_local)
		return;

	memcpy(oam.olt_mac, src, ETH_ALEN);
	/* We are satisfied with any OLT that sent its Local Information. */
	oam.flags = FLAG_LOCAL_STABLE;
	if (oam.olt_flags & FLAG_LOCAL_STABLE)
		oam.flags |= FLAG_REMOTE_STABLE;
	else if (oam.olt_flags & FLAG_LOCAL_EVAL)
		oam.flags |= FLAG_REMOTE_EVAL;

	if (oam.disc == DISC_WAIT) {
		oam.disc = DISC_SEND_LOCAL_REMOTE;
		oam.status_dirty = true;
	}
	if (oam.disc == DISC_SEND_LOCAL_REMOTE && oam.olt_flags & FLAG_LOCAL_STABLE) {
		oam.disc = DISC_SEND_ANY;
		oam.send_any_ms = now_ms();
		oam.ctc_checked = false;
		oam.revision++;
		set_oam_link(true);
		logmsg(0, "OAM link up with OLT %02x:%02x:%02x:%02x:%02x:%02x",
		       src[0], src[1], src[2], src[3], src[4], src[5]);
		oam.status_dirty = true;
	} else if (oam.disc == DISC_SEND_ANY && !(oam.olt_flags & FLAG_LOCAL_STABLE)) {
		oam.disc = DISC_SEND_LOCAL_REMOTE;
		set_oam_link(false);
		logmsg(0, "OAM link: OLT no longer stable");
		oam.status_dirty = true;
	}
	send_info(ctc_reply, ctc_version);
}

/* ---- CTC Get / Set ---- */

struct out {
	uint8_t *p;
	size_t len, max;
};

static void out_bytes(struct out *o, const void *b, size_t n)
{
	if (o->len + n <= o->max) {
		memcpy(o->p + o->len, b, n);
		o->len += n;
	}
}

static void out_var(struct out *o, uint8_t branch, uint16_t leaf, uint8_t width,
		    const void *val, size_t n)
{
	uint8_t h[4] = { branch, leaf >> 8, leaf, width };

	out_bytes(o, h, 4);
	if (val)
		out_bytes(o, val, n);
}

static void put_str(uint8_t *p, const char *s, size_t n)
{
	memset(p, 0, n);
	memcpy(p, s, strnlen(s, n));
}

static int read_carrier(const char *ifname)
{
	char path[64], c = '0';
	int fd;

	snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", ifname);
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		if (read(fd, &c, 1) != 1)
			c = '0';
		close(fd);
	}
	return c == '1';
}

/* The raw A2 0x60-0x69 words, as CTC wants them; nothing while the BOSA is unprogrammed. */
static size_t read_ddm(uint8_t *v)
{
	char buf[256];
	unsigned int b;
	char *p;
	int i;

	if (sysfs_read("bosa_ddm", buf, sizeof(buf)) || !(p = strstr(buf, "raw=")))
		return 0;
	for (p += 4, i = 0; i < 10 && sscanf(p, "%2x", &b) == 1; i++, p += 2)
		v[i] = b;
	return i == 10 ? 10 : 0;
}

static size_t port_vlan_value(uint8_t *v, size_t size)
{
	size_t n = 1;
	int i;

	if (!size)
		return 0;
	if (oam.vlan_mode != 0 && size < 5)
		return 0;
	if (oam.vlan_mode == 2) {
		if (oam.vlan_xlates < 0 || oam.vlan_xlates > 8 ||
		    size < 5 + 8 * (size_t)oam.vlan_xlates)
			return 0;
	} else if (oam.vlan_mode == 4) {
		if (oam.vlan_trunks < 0 || oam.vlan_trunks > 8 ||
		    size < 5 + 4 * (size_t)oam.vlan_trunks)
			return 0;
	}
	if (oam.vlan_mode == 3)
		return oam.vlan_raw_len <= size ? (memcpy(v, oam.vlan_raw, oam.vlan_raw_len),
						  oam.vlan_raw_len) : 0;
	v[0] = oam.vlan_mode;
	if (oam.vlan_mode == 0)
		return n;
	put16(v + n, 0x8100);
	put16(v + n + 2, oam.vlan_default);
	n += 4;
	if (oam.vlan_mode == 2) {
		for (i = 0; i < oam.vlan_xlates; i++) {
			put16(v + n, 0x8100);
			put16(v + n + 2, oam.vlan_xlate[i].old_vid);
			put16(v + n + 4, 0x8100);
			put16(v + n + 6, oam.vlan_xlate[i].new_vid);
			n += 8;
		}
	} else if (oam.vlan_mode == 4) {
		for (i = 0; i < oam.vlan_trunks; i++) {
			put16(v + n, 0x8100);
			put16(v + n + 2, oam.vlan_trunk[i]);
			n += 4;
		}
	}
	return n;
}

/*
 * MD5 (RFC 1321), for the VSOL inter-authentication key only. Both sides
 * derive a fixed key, md5_hex(MAC as 12 uppercase hex || secret || role),
 * role "Olt" for the value the OLT sends and "Onu" for ours; the digest
 * goes on the wire as 32 lowercase hex characters in a 64-byte field.
 * Recovered by the xx530v EPON work from the Genexis RTL9607C eponoamd and
 * checked against a live Excitel/VSOL session. The shared secret is not
 * shipped: set it in /etc/config/epon (interauth_secret) for such OLTs.
 */
#define INTERAUTH_FIELD		64

static void md5_block(uint32_t h[4], const uint8_t *b)
{
	static const uint32_t k[64] = {
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
		0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
		0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
		0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
		0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
	};
	static const uint8_t r[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
	uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], w[16], f, t;
	unsigned int i, g;

	for (i = 0; i < 16; i++)
		w[i] = b[4 * i] | b[4 * i + 1] << 8 | b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
	for (i = 0; i < 64; i++) {
		if (i < 16) {
			f = (bb & c) | (~bb & d);
			g = i;
		} else if (i < 32) {
			f = (d & bb) | (~d & c);
			g = (5 * i + 1) % 16;
		} else if (i < 48) {
			f = bb ^ c ^ d;
			g = (3 * i + 5) % 16;
		} else {
			f = c ^ (bb | ~d);
			g = 7 * i % 16;
		}
		t = d;
		d = c;
		c = bb;
		f += a + k[i] + w[g];
		bb += f << r[i / 16 * 4 + i % 4] | f >> (32 - r[i / 16 * 4 + i % 4]);
		a = t;
	}
	h[0] += a;
	h[1] += bb;
	h[2] += c;
	h[3] += d;
}

static void md5_hex(const char *msg, char out[33])
{
	uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
	size_t len = strlen(msg), i;
	uint8_t blk[64];
	uint64_t bits = (uint64_t)len * 8;

	for (i = 0; i + 64 <= len; i += 64)
		md5_block(h, (const uint8_t *)msg + i);
	memset(blk, 0, sizeof(blk));
	memcpy(blk, msg + i, len - i);
	blk[len - i] = 0x80;
	if (len - i >= 56) {
		md5_block(h, blk);
		memset(blk, 0, sizeof(blk));
	}
	for (i = 0; i < 8; i++)
		blk[56 + i] = bits >> (8 * i);
	md5_block(h, blk);
	for (i = 0; i < 16; i++)
		sprintf(out + 2 * i, "%02x", (h[i / 4] >> (8 * (i % 4))) & 0xff);
}

static void interauth_key(const char *role, char out[33])
{
	char buf[13 + sizeof(oam.auth_secret) + 4];

	snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X%s%s",
		 oam.mac[0], oam.mac[1], oam.mac[2], oam.mac[3], oam.mac[4], oam.mac[5],
		 oam.auth_secret, role);
	md5_hex(buf, out);
}

static void get_attr(struct out *o, uint8_t branch, uint16_t leaf)
{
	uint8_t v[128];
	size_t n = 0;

	memset(v, 0, sizeof(v));
	if (branch == BR_EXT_ATTR) {
		switch (leaf) {
		case 0x0001:		/* ONU SN */
			put_str(v, oam.vendor, 4);
			put_str(v + 4, oam.model, 4);
			memcpy(v + 8, oam.mac, ETH_ALEN);
			put_str(v + 14, oam.hw, 8);
			put_str(v + 22, oam.sw, 16);
			n = 38;
			/* CTC 3.0 adds the extended model, as stock (0x36 bytes from 0x30 up). */
			if (oam.ctc_version >= 0x30) {
				put_str(v + 38, oam.ext_model, 16);
				n = 54;
			}
			break;
		case 0x0002:		/* firmware version, as the stock gcoamd */
			v[0] = 0x10;
			v[1] = 0x02;
			n = 2;
			break;
		case 0x0003:		/* chipset ID: vendor, model 0x1601, revision, date */
			if (oam.chipset_id_set) {
				memcpy(v, oam.chipset_id, 8);
				n = 8;
				break;
			}
			v[0] = 'G';
			v[1] = 'C';
			put16(v + 2, 0x1601);
			v[4] = 0x0f;
			v[5] = 0x14;
			v[6] = 0x06;
			v[7] = 0x14;
			n = 8;
			break;
		case 0x0004:		/* capabilities-1: one GE port, 8 queues each way */
			v[0] = 0x01;	/* services: GE */
			v[1] = 1;	/* GE ports */
			v[9] = 0x01;	/* GE port bitmap (2..9), port 1 */
			/* 10 FE ports, 11..18 FE bitmap, 19 POTS, 20 E1: none */
			v[21] = 8;	/* upstream queues */
			v[22] = 8;	/* upstream queues per port */
			v[23] = 8;	/* downstream queues */
			v[24] = 8;	/* downstream queues per port */
			n = 26;		/* 25 battery backup: none */
			break;
		case 0x0005:		/* optical diagnosis: SFF-8472 temp, Vcc, bias, TX, RX */
			n = read_ddm(v);
			break;
		case 0x0007:		/* capabilities-2: SFU, one PON, one GE port */
			v[4] = 1;	/* multi-LLID: 1 */
			v[6] = 1;	/* PON interfaces */
			v[8] = 1;	/* interface types */
			put16(v + 13, 1);	/* GE ports (type 0) */
			n = 16;
			break;
		case 0x000c:		/* capabilities-3 */
			n = oam.ctc_version >= 0x30 ? 3 : 2;
			break;
		case 0x0011:		/* Ethernet link state */
			v[0] = read_carrier("eth0");
			n = 1;
			break;
		case 0x0021:		/* Port VLAN */
			n = port_vlan_value(v, sizeof(v));
			break;
		case 0x2040:		/* VSOL inter-authentication: our key */
			if (!oam.auth_secret[0])
				break;
			interauth_key("Onu", (char *)v);
			v[32] = 0;
			n = INTERAUTH_FIELD;
			break;
		case 0x0008:		/* holdover: state, time in ms */
			put32(v, oam.holdover_state);
			put32(v + 4, oam.holdover_ms);
			n = 8;
			break;
		}
	} else if (branch == BR_STD_ATTR) {
		switch (leaf) {
		case 0x0025:		/* aPhyAdminState: enabled */
		case 0x004f:		/* aAutoNegAdminState: enabled */
			put16(v + 2, 2);
			n = 4;
			break;
		case 0x0139:		/* aFECAbility: not supported */
		case 0x013a:		/* aFECmode: disabled */
			put16(v + 2, 3);
			n = 4;
			break;
		}
	}

	if (n) {
		out_var(o, branch, leaf, n, v, n);
	} else {
		oam.unknown_leaves++;
		logmsg(1, "get %#04x/%#06x: not supported", branch, leaf);
		out_var(o, branch, leaf, VAR_BAD, NULL, 0);
	}
}

static uint8_t set_port_vlan(const uint8_t *v, size_t n)
{
	typeof(oam) old = oam;
	uint16_t def;
	size_t p;

	/*
	 * gcoam_ctc_org_set_vlan: 0 transparent, 1 tag, 2 translation pairs,
	 * 3 N:1 tables {u16 entries, target tag, entries x source tag},
	 * 4 trunk list. Tags are TPID + TCI; the kernel map holds 8 entries.
	 */
	if (n < 1 || v[0] > 4 || (v[0] == 0 && n != 1) || (v[0] != 0 && n < 5))
		return VAR_BAD;
	def = v[0] ? get16(v + 3) & 0xfff : 0;
	if (v[0] && def == 4095)
		return VAR_BAD;
	if (v[0] && !def)
		def = 1;		/* stock: "change default vlan" */
	oam.vlan_mode = v[0];
	oam.vlan_default = def;
	oam.vlan_xlates = oam.vlan_trunks = 0;
	oam.vlan_raw_len = 0;

	switch (v[0]) {
	case 1:
		if (n != 5)
			goto bad;
		break;
	case 2:
		if ((n - 5) % 8 || (n - 5) / 8 > 8)
			goto bad;
		for (p = 5; p < n; p += 8) {
			uint16_t src = get16(v + p + 2) & 0xfff;
			uint16_t dst = get16(v + p + 6) & 0xfff;

			if (!src || src == 4095 || !dst || dst == 4095)
				goto bad;
			oam.vlan_xlate[oam.vlan_xlates].old_vid = src;
			oam.vlan_xlate[oam.vlan_xlates++].new_vid = dst;
		}
		break;
	case 3: {
		unsigned int tables, t, k;

		if (n < 7 || n > sizeof(oam.vlan_raw))
			goto bad;
		tables = get16(v + 5);
		for (p = 7, t = 0; t < tables; t++) {
			unsigned int entries;
			uint16_t target;

			if (p + 6 > n)
				goto bad;
			entries = get16(v + p);
			target = get16(v + p + 4) & 0xfff;
			p += 6;
			if (!target || target == 4095 || p + 4 * entries > n)
				goto bad;
			for (k = 0; k < entries; k++, p += 4) {
				uint16_t src = get16(v + p + 2) & 0xfff;

				if (!src || src == 4095 || oam.vlan_xlates == 8)
					goto bad;
				oam.vlan_xlate[oam.vlan_xlates].old_vid = src;
				oam.vlan_xlate[oam.vlan_xlates++].new_vid = target;
			}
		}
		if (p != n)
			goto bad;
		memcpy(oam.vlan_raw, v, n);
		oam.vlan_raw_len = n;
		break;
	}
	case 4:
		if ((n - 5) % 4 || (n - 5) / 4 > 8)
			goto bad;
		for (p = 5; p < n; p += 4) {
			uint16_t vid = get16(v + p + 2) & 0xfff;

			if (!vid || vid == 4095)
				goto bad;
			oam.vlan_trunk[oam.vlan_trunks++] = vid;
		}
		break;
	}
	logmsg(0, "OLT Port VLAN: mode %u, default VLAN %u", oam.vlan_mode, oam.vlan_default);
	if (apply_vlan()) {
		oam = old;
		if (apply_vlan()) {
			if (sysfs_write("epon_oam_link", "0"))
				logmsg(0, "VLAN rollback and data gate writes failed");
			else
				logmsg(0, "VLAN rollback failed; OAM data link disabled");
		}
		return VAR_BAD;
	}
	return VAR_SET_OK;
bad:
	oam = old;
	return VAR_BAD;
}

static bool reboot_pending;

/*
 * Pause, policing, loop detection, classification, multicast, alarm admin,
 * holdover, management and MAC aging (EPON-OAM-STOCK.md section 3.3).
 * LLID queue config (0x0051) stays refused, as stock does by default.
 */
static bool set_acked(uint16_t leaf)
{
	switch (leaf) {
	case 0x0009:
	case 0x0012: case 0x0013: case 0x0016: case 0x0017: case 0x0018:
	case 0x0031:
	case 0x0041: case 0x0042: case 0x0043: case 0x0044: case 0x0045:
	case 0x0081: case 0x0082:
	case 0x00a4:
		return true;
	}
	return false;
}

/* gcoam_ctc_org_set_holdover (0x0008): {u32 state 1 off / 2 on, u32 ms}. */
static uint8_t set_holdover(const uint8_t *v, size_t n)
{
	uint32_t state, ms;

	if (n != 8)
		return VAR_BAD;
	state = get32(v);
	ms = get32(v + 4);
	if ((state != 1 && state != 2) || ms > 1000)
		return VAR_BAD;
	if (sysfs_write("epon_holdover", "%u", state == 2 ? ms : 0))
		return VAR_BAD;
	oam.holdover_state = state;
	oam.holdover_ms = ms;
	logmsg(0, "holdover %s, %u ms", state == 2 ? "on" : "off", ms);
	return VAR_SET_OK;
}

static void tx_release(const char *why)
{
	oam.tx_off_until_ms = 0;
	oam.tx_off_permanent = false;
	unlink(ROGUE_FILE);
	sysfs_write("tx_hold", "0");
	logmsg(0, "transmitter back on: %s", why);
	oam.status_dirty = true;
}

/*
 * gcoam_ctc_org_set_onu_tx_pwr_ctl (0x00A1): {u16 action, ONU MAC, u32
 * transmitter ID}. Action 0 turns the laser back on, 0xffff off for good
 * (kept across reboots), anything else off for that many seconds. Our MAC
 * or broadcast; a broadcast cannot undo a permanent shutdown.
 */
static uint8_t set_tx_power(const uint8_t *v, size_t n)
{
	static const uint8_t bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	uint16_t action;
	bool mine;
	int fd;

	if (n != 8 && n != 12)
		return VAR_BAD;
	action = get16(v);
	mine = !memcmp(v + 2, oam.mac, ETH_ALEN);
	if (!mine && memcmp(v + 2, bcast, ETH_ALEN))
		return VAR_SET_OK;

	if (!action) {
		if (oam.tx_off_permanent && !mine)
			logmsg(0, "broadcast re-enable ignored: transmitter off for good");
		else if (oam.tx_off_until_ms || oam.tx_off_permanent)
			tx_release("OLT re-enabled it");
		return VAR_SET_OK;
	}
	if (sysfs_write("tx_hold", "1"))
		return VAR_BAD;
	if (action == 0xffff) {
		oam.tx_off_permanent = true;
		oam.tx_off_until_ms = 0;
		fd = open(ROGUE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0)
			close(fd);
		logmsg(0, "OLT turned the transmitter off for good (%s); rm %s to undo",
		       mine ? "this ONU" : "broadcast", ROGUE_FILE);
	} else if (!oam.tx_off_permanent) {
		oam.tx_off_until_ms = now_ms() + action * 1000ULL;
		logmsg(0, "OLT turned the transmitter off for %u s", action);
	}
	oam.status_dirty = true;
	return VAR_SET_OK;
}

/*
 * The OLT's half of the VSOL inter-authentication. The Genexis ONU reboots
 * on a mismatch; we only log it, since a wrong secret here would otherwise
 * be a boot loop.
 */
static uint8_t set_interauth(const uint8_t *v, size_t n)
{
	char want[33];

	if (!oam.auth_secret[0])
		return VAR_BAD;
	interauth_key("Olt", want);
	oam.interauth = n >= 32 && !memcmp(v, want, 32) ? 1 : -1;
	if (oam.interauth > 0)
		logmsg(0, "VSOL inter-authentication: OLT key accepted");
	else
		logmsg(0, "VSOL inter-authentication: OLT key mismatch (%zu bytes), expected %s",
		       n, want);
	oam.status_dirty = true;
	return VAR_SET_OK;
}

static uint8_t set_attr(uint8_t branch, uint16_t leaf, const uint8_t *v, size_t n)
{
	if (branch == BR_EXT_ATTR && leaf == 0x2040)
		return set_interauth(v, n);
	if (branch == BR_EXT_ATTR && leaf == 0x0021)
		return set_port_vlan(v, n);
	if (branch == BR_EXT_ATTR && leaf == 0x0008)
		return set_holdover(v, n);
	if (branch == BR_EXT_ACTION && leaf == 0x00a1)
		return set_tx_power(v, n);
	if (branch == BR_EXT_ACTION && leaf == 0x0001) {
		logmsg(0, "OLT requested an ONU reset");
		reboot_pending = true;
		return VAR_SET_OK;
	}
	/*
	 * Provisioning sets stock gcoamd acknowledges: CTC OLTs push these to
	 * every ONU and treat a refusal as a failed configuration.
	 */
	if (branch == BR_EXT_ATTR && set_acked(leaf)) {
		logmsg(1, "set %#04x/%#06x (%zu bytes): acknowledged, not applied", branch, leaf, n);
		return VAR_SET_OK;
	}
	oam.unknown_leaves++;
	logmsg(1, "set %#04x/%#06x (%zu bytes): not supported", branch, leaf, n);
	return VAR_BAD;
}

/* Width byte: 0 means 128, 0x80 and above carry no value. */
static size_t var_width(uint8_t w)
{
	return w >= 0x80 ? 0 : w ? w : 128;
}

static void handle_get_set(const uint8_t *d, size_t len, bool set, uint8_t *f, size_t hdr)
{
	struct out o = { f, hdr, 1500 };
	size_t p = 0;

	while (p + 3 <= len) {
		uint8_t branch = d[p];
		uint16_t leaf = get16(d + p + 1);

		if (branch == 0)
			break;
		p += 3;
		if (branch == BR_INST_V20 || branch == BR_INST_V21) {
			size_t w = p < len ? var_width(d[p]) : 0;

			if (p + 1 + w > len)
				break;
			out_bytes(&o, d + p - 3, 4 + w);	/* echo the instance */
			p += 1 + w;
			continue;
		}
		if (!set) {
			get_attr(&o, branch, leaf);
			continue;
		}
		{
			size_t w = p < len ? var_width(d[p]) : 0;

			if (p + 1 + w > len)
				break;
			out_var(&o, branch, leaf, set_attr(branch, leaf, d + p + 1, w), NULL, 0);
			p += 1 + w;
		}
	}
	f[o.len++] = 0;
	oam_send(f, o.len);
}

/* ---- CTC authentication and churning ---- */

static void handle_auth(const uint8_t *d, size_t len, uint8_t *f, size_t hdr)
{
	size_t n = hdr;

	if (len < 1)
		return;
	switch (d[0]) {
	case 0x01:		/* Auth Request */
		f[n++] = 0x02;
		if (len >= 4 && d[3] == 0x01) {
			put16(f + n, 37);
			f[n + 2] = 0x01;
			put_str(f + n + 3, oam.loid, 24);
			put_str(f + n + 27, oam.password, 12);
			n += 39;
			oam.auth = AUTH_SENT;
			logmsg(0, "LOID authentication requested, sent LOID '%s'", oam.loid);
		} else {
			put16(f + n, 2);
			f[n + 2] = 0x00;	/* NAK */
			f[n + 3] = 0x01;	/* desired: LOID + password */
			n += 4;
		}
		oam_send(f, n);
		break;
	case 0x03:		/* Auth Success */
		oam.auth = AUTH_OK;
		oam.auth_fail_reason = 0;
		logmsg(0, "LOID authentication succeeded");
		break;
	case 0x04:		/* Auth Failure: 1 LOID, 2 password, 3 LOID conflict */
		oam.auth = AUTH_FAIL;
		oam.auth_fail_reason = len >= 4 ? d[3] : 0;
		logmsg(0, "LOID authentication failed (reason %d)", oam.auth_fail_reason);
		go_silent("authentication failed", SILENCE_MS);
		oam.auth = AUTH_FAIL;
		break;
	}
	oam.status_dirty = true;
}

static void handle_churning(const uint8_t *d, size_t len, uint8_t *f, size_t hdr)
{
	uint8_t idx;
	uint32_t key;

	if (len < 2 || d[0] != 0x00)	/* New Key Request */
		return;
	idx = (d[1] & 1) ^ 1;
	if (getrandom(&key, sizeof(key), 0) != sizeof(key)) {
		logmsg(0, "churning: random key generation failed");
		return;
	}
	key &= 0xffffff;
	/* Program the unused slot before the OLT can switch to it. */
	if (sysfs_write("epon_key", "0 %u %06x", idx, key)) {
		logmsg(0, "churning: key installation failed");
		return;
	}
	f[hdr] = 0x01;		/* New Key Response */
	f[hdr + 1] = idx;
	put24(f + hdr + 2, key);
	oam_send(f, hdr + 5);
	oam.key_index = idx;
	oam.keys++;
	logmsg(1, "churning: new key in slot %u", idx);
}

/*
 * gcoam_ctc_org_fw_download_handler: {data type, u16 length, u16 TID,
 * u16 opcode, ...}. We take no images: a write request, data or end of
 * download gets the TFTP error stock sends when it cannot update (opcode 5,
 * code 4, message); activate (8) and commit (10) answer 9 and 11 with the
 * failure flag set.
 */
static size_t sw_download_reply(const uint8_t *d, size_t len, uint8_t *p)
{
	static const char msg[] = "software download not supported";
	uint16_t tid, op;
	size_t n;

	if (len < 7)
		return 0;
	tid = get16(d + 3);
	op = get16(d + 5);
	switch (op) {
	case 2: case 3: case 6:
		n = 9 + sizeof(msg);
		p[0] = 0x01;
		put16(p + 1, n);
		put16(p + 3, tid);
		put16(p + 5, 5);		/* error */
		put16(p + 7, 4);		/* illegal operation */
		memcpy(p + 9, msg, sizeof(msg));
		break;
	case 8: case 10:
		n = 8;
		p[0] = op == 8 ? 0x03 : 0x04;
		put16(p + 1, n);
		put16(p + 3, tid);
		put16(p + 5, op + 1);
		p[7] = 1;			/* failed */
		break;
	default:
		return 0;
	}
	logmsg(0, "OLT software download opcode %u: refused", op);
	return n;
}

static int alarm_index(uint16_t id)
{
	return id >= ALARM_FIRST && id < ALARM_FIRST + ALARMS ? id - ALARM_FIRST : -1;
}

/*
 * gcoam_ctc_org_event_handler: sub-type 1 status request and 2 status set
 * answer 3, 4 threshold request and 5 threshold set answer 6. Entries are
 * {u16 object type, u32 instance, u16 alarm ID} then a u32 status or two
 * u32 thresholds; an alarm we do not report reads back 0xffffffff.
 */
static size_t event_reply(const uint8_t *d, size_t len, uint8_t *p)
{
	uint8_t sub = len ? d[0] : 0;
	bool thr = sub == 4 || sub == 5, set = sub == 2 || sub == 5;
	size_t in = 3, out = 3, entry = thr ? 16 : 12, key = 8;
	unsigned int count, i;

	if (len < 3 || !sub || sub == 3 || sub > 5)
		return 0;
	count = get16(d + 1);
	p[0] = thr ? 6 : 3;
	put16(p + 1, count);
	for (i = 0; i < count && out + entry < 1400; i++) {
		size_t need = set ? entry : key;
		int a;

		if (in + need > len)
			break;
		memcpy(p + out, d + in, key);
		a = alarm_index(get16(d + in + 6));
		if (set && a >= 0) {
			if (thr) {
				oam.alarm_raise[a] = get32(d + in + 8);
				oam.alarm_clear[a] = get32(d + in + 12);
			} else {
				oam.alarm_status[a] = get32(d + in + 8);
			}
		}
		if (thr) {
			put32(p + out + 8, a < 0 ? 0xffffffff : oam.alarm_raise[a]);
			put32(p + out + 12, a < 0 ? 0xffffffff : oam.alarm_clear[a]);
		} else {
			put32(p + out + 8, a < 0 ? 0xffffffff : oam.alarm_status[a]);
		}
		in += need;
		out += entry;
	}
	put16(p + 1, i);
	return out;
}

/*
 * The stock statistic task's PON interface alarms, raised and cleared from
 * the BOSA's own SFF-8472 alarm flags (A2 0x70: bit 15 temp high .. bit 6 RX
 * power low) for alarms the OLT turned on. Event Notification as
 * FUN_00025468 builds it: sequence, then {0xfe, length, CTC OUI, object 4,
 * instance 0, alarm ID, timestamp 0, state 0 raise / 1 clear, u32 info}.
 */
static const struct { uint16_t flag; uint8_t ddm; } alarm_src[ALARMS] = {
	{ 1 << 7, 4 }, { 1 << 6, 4 },	/* RX power high, low */
	{ 1 << 9, 3 }, { 1 << 8, 3 },	/* TX power high, low */
	{ 1 << 11, 2 }, { 1 << 10, 2 },	/* bias high, low */
	{ 1 << 13, 1 }, { 1 << 12, 1 },	/* Vcc high, low */
	{ 1 << 15, 0 }, { 1 << 14, 0 },	/* temperature high, low */
};

static void send_alarm(unsigned int a, bool raise, uint32_t info)
{
	uint8_t f[128];
	size_t n = oam_header(f, OAM_EVENT);

	put16(f + n, ++oam.event_seq);
	n += 2;
	f[n] = TLV_ORG;
	f[n + 1] = 20;
	put24(f + n + 2, CTC_OUI);
	put16(f + n + 5, 4);			/* object: PON interface */
	put32(f + n + 7, 0);
	put16(f + n + 11, ALARM_FIRST + a);
	put16(f + n + 13, 0);			/* timestamp */
	f[n + 15] = raise ? 0 : 1;
	put32(f + n + 16, info);
	oam_send(f, n + 20);
	logmsg(0, "%s alarm %#06x", raise ? "raised" : "cleared", ALARM_FIRST + a);
}

static void check_alarms(void)
{
	char buf[256], *p;
	uint16_t flags, now_up = 0;
	unsigned int a;
	uint8_t raw[10];

	if (oam.disc != DISC_SEND_ANY || sysfs_read("bosa_ddm", buf, sizeof(buf)) ||
	    !(p = strstr(buf, "alarms=")) || read_ddm(raw) != 10)
		return;
	flags = strtoul(p + 7, NULL, 0);
	for (a = 0; a < ALARMS; a++) {
		bool on = oam.alarm_status[a] == 2 && flags & alarm_src[a].flag;
		bool was = oam.alarms_up & 1 << a;

		if (on)
			now_up |= 1 << a;
		if (on != was)
			send_alarm(a, on, get16(raw + 2 * alarm_src[a].ddm));
	}
	oam.alarms_up = now_up;
}

static void handle_org(const uint8_t *d, size_t len)
{
	uint8_t f[1600];
	size_t hdr;
	uint8_t op;

	if (len < 4 || (get24(d) != CTC_OUI && get24(d) != VSOL_OUI)) {
		logmsg(1, "organisation specific OAMPDU, OUI %06x: ignored", len >= 3 ? get24(d) : 0);
		return;
	}
	/* Answer under the OUI the request came on. */
	oam.peer_oui = get24(d);
	op = d[3];
	hdr = oam_header(f, OAM_ORG);
	put24(f + hdr, oam.peer_oui);
	d += 4;
	len -= 4;

	switch (op) {
	case CTC_GET_REQ:
		oam.gets++;
		f[hdr + 3] = CTC_GET_RESP;
		handle_get_set(d, len, false, f, hdr + 4);
		break;
	case CTC_SET_REQ:
		oam.sets++;
		f[hdr + 3] = CTC_SET_RESP;
		handle_get_set(d, len, true, f, hdr + 4);
		if (reboot_pending) {
			sync();
			execl("/sbin/reboot", "reboot", NULL);
		}
		break;
	case CTC_AUTH:
		f[hdr + 3] = CTC_AUTH;
		handle_auth(d, len, f, hdr + 4);
		break;
	case CTC_CHURNING:
		f[hdr + 3] = CTC_CHURNING;
		handle_churning(d, len, f, hdr + 4);
		break;
	case CTC_SW_DOWNLOAD:
	case CTC_EVENT: {
		size_t n = op == CTC_EVENT ? event_reply(d, len, f + hdr + 4) :
					     sw_download_reply(d, len, f + hdr + 4);

		f[hdr + 3] = op;
		if (n)
			oam_send(f, hdr + 4 + n);
		else
			logmsg(1, "CTC opcode %#04x: malformed, ignored", op);
		break;
	}
	default:
		logmsg(1, "CTC opcode %#04x: ignored", op);
		break;
	}
}

/* The OAMPDU being handled, for the crash report. */
static uint8_t last_frame[96];
static size_t last_frame_len;

static void handle_frame(const uint8_t *f, size_t len)
{
	uint8_t code;

	if (len < 18 || get16(f + 12) != ETH_P_SLOW || f[14] != SLOW_OAM)
		return;
	last_frame_len = len < sizeof(last_frame) ? len : sizeof(last_frame);
	memcpy(last_frame, f, last_frame_len);
	oam.rx_pdus++;
	oam.last_rx_ms = now_ms();
	oam.olt_flags = get16(f + 15);
	code = f[17];

	switch (code) {
	case OAM_INFO:
		handle_info(f + 6, f + 18, len - 18);
		break;
	case OAM_ORG:
		if (oam.disc == DISC_SEND_ANY)
			handle_org(f + 18, len - 18);
		break;
	case OAM_VAR_REQ:
	case OAM_LOOPBACK:
		logmsg(1, "OAMPDU code %#04x: not supported", code);
		break;
	default:
		logmsg(1, "OAMPDU code %#04x from the OLT: unexpected", code);
		break;
	}
}

/* ---- status ---- */

static void write_status(void)
{
	FILE *fp = fopen("/var/run/epon-oamd.json.tmp", "w");

	if (!fp)
		return;
	fprintf(fp,
		"{\"registered\":%s,\"oam\":\"%s\",\"ctc\":\"%s\",\"ctc_version\":%u,"
		"\"auth\":\"%s\",\"auth_fail_reason\":%d,\"loid\":\"%s\","
		"\"olt_mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"onu_mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
		"\"vlan_mode\":%u,\"vlan_default\":%u,\"vlan_override\":\"%s\",\"silent\":%s,\"tx_off\":\"%s\","
		"\"rx_pdus\":%lu,\"tx_pdus\":%lu,\"gets\":%lu,\"sets\":%lu,\"unknown_leaves\":%lu,\"keys\":%lu,"
		"\"interauth\":\"%s\",\"flags\":\"%#06x\",\"olt_flags\":\"%#06x\"}\n",
		oam.registered ? "true" : "false", disc_names[oam.disc], ctc_names[oam.ctc],
		oam.ctc_version, auth_names[oam.auth], oam.auth_fail_reason, oam.loid,
		oam.olt_mac[0], oam.olt_mac[1], oam.olt_mac[2], oam.olt_mac[3], oam.olt_mac[4],
		oam.olt_mac[5], oam.mac[0], oam.mac[1], oam.mac[2], oam.mac[3], oam.mac[4],
		oam.mac[5], oam.vlan_mode, oam.vlan_default, oam.vlan_override,
		oam.silent_until_ms ? "true" : "false",
		oam.tx_off_permanent ? "permanent" : oam.tx_off_until_ms ? "timed" : "no", oam.rx_pdus, oam.tx_pdus, oam.gets,
		oam.sets, oam.unknown_leaves, oam.keys,
		!oam.auth_secret[0] ? "off" : !oam.interauth ? "not-asked" :
		oam.interauth > 0 ? "ok" : "mismatch", oam.flags, oam.olt_flags);
	fclose(fp);
	rename("/var/run/epon-oamd.json.tmp", "/var/run/epon-oamd.json");
	oam.status_dirty = false;
}

/* ---- main ---- */

/* pon0's platform device carries the epon_* and sfu_* attributes. */
static int find_sysfs(void)
{
	snprintf(oam.sysfs, sizeof(oam.sysfs), "/sys/class/net/%s/device", oam.ifname);
	return access(oam.sysfs, F_OK);
}

static int read_mac(void)
{
	char buf[32];
	unsigned int m[6];
	int i;

	if (sysfs_read("epon_mac", buf, sizeof(buf)) ||
	    sscanf(buf, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
		return -1;
	for (i = 0; i < 6; i++)
		oam.mac[i] = m[i];
	return 0;
}

static int open_socket(void)
{
	struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_SLOW) };

	oam.ifindex = if_nametoindex(oam.ifname);
	if (!oam.ifindex)
		return -1;
	oam.fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_SLOW));
	if (oam.fd < 0)
		return -1;
	sll.sll_ifindex = oam.ifindex;
	return bind(oam.fd, (struct sockaddr *)&sll, sizeof(sll));
}

/* procd respawns a crashed daemon silently: say why, with the PDU it was on. */
static void on_fault(int sig, siginfo_t *si, void *ctx)
{
	char line[400];
	size_t i;
	int n;

	(void)ctx;
	n = snprintf(line, sizeof(line), "epon-oamd: fatal signal %d at %p, last OAMPDU (%zu bytes):",
		     sig, si->si_addr, last_frame_len);
	for (i = 0; i < last_frame_len && n < (int)sizeof(line) - 4; i++)
		n += snprintf(line + n, sizeof(line) - n, " %02x", last_frame[i]);
	line[n++] = '\n';
	write(STDERR_FILENO, line, n);
	if (kmsg_fd >= 0)
		write(kmsg_fd, line, n);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void on_signal(int sig, siginfo_t *si, void *ctx)
{
	(void)ctx;
	stop_sig = sig;
	stop_pid = si->si_pid;
	stop = 1;
}

/* Who stopped us: a stop from outside is otherwise indistinguishable from a crash. */
static void log_stop(void)
{
	char path[32], comm[32] = "?";
	FILE *fp;

	snprintf(path, sizeof(path), "/proc/%d/comm", (int)stop_pid);
	fp = fopen(path, "r");
	if (fp) {
		if (fgets(comm, sizeof(comm), fp))
			comm[strcspn(comm, "\n")] = 0;
		fclose(fp);
	}
	logmsg(0, "stopped by signal %d from pid %d (%s)", (int)stop_sig, (int)stop_pid, comm);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: epon-oamd [-i ifname] [-L loid] [-P password] [-V vendor] [-M model]\n"
		"                 [-H hwver] [-S swver] [-E extmodel] [-A interauth-secret] [-a]\n"
		"                 [-O oui-hex6] [-I vendorinfo-hex8] [-C chipsetid-hex16]\n"
		"                 [-v olt|transparent|tag:VID] [-d level]\n");
	exit(2);
}

static int parse_hex(const char *s, uint8_t *out, size_t n)
{
	size_t i;
	unsigned int b;

	if (strlen(s) != 2 * n)
		return -1;
	for (i = 0; i < n; i++) {
		if (sscanf(s + 2 * i, "%2x", &b) != 1)
			return -1;
		out[i] = b;
	}
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t last_poll = 0, last_status = 0;
	int opt;

	while ((opt = getopt(argc, argv, "i:L:P:V:M:H:S:E:A:aO:I:C:v:d:")) != -1) {
		switch (opt) {
		case 'i': oam.ifname = optarg; break;
		case 'L': snprintf(oam.loid, sizeof(oam.loid), "%s", optarg); break;
		case 'P': snprintf(oam.password, sizeof(oam.password), "%s", optarg); break;
		case 'V': snprintf(oam.vendor, sizeof(oam.vendor), "%s", optarg); break;
		case 'M': snprintf(oam.model, sizeof(oam.model), "%s", optarg); break;
		case 'H': snprintf(oam.hw, sizeof(oam.hw), "%s", optarg); break;
		case 'S': snprintf(oam.sw, sizeof(oam.sw), "%s", optarg); break;
		case 'E': snprintf(oam.ext_model, sizeof(oam.ext_model), "%s", optarg); break;
		case 'a': oam.active = true; break;
		case 'O': {
			uint8_t o[3];

			if (parse_hex(optarg, o, 3))
				usage();
			oam.info_oui = get24(o);
			break;
		}
		case 'I':
			if (parse_hex(optarg, oam.info_vendor, 4))
				usage();
			oam.info_vendor_set = true;
			break;
		case 'C':
			if (parse_hex(optarg, oam.chipset_id, 8))
				usage();
			oam.chipset_id_set = true;
			break;
		case 'A': snprintf(oam.auth_secret, sizeof(oam.auth_secret), "%s", optarg); break;
		case 'v':
			if (!strncmp(optarg, "tag:", 4)) {
				snprintf(oam.vlan_override, sizeof(oam.vlan_override), "tag");
				oam.vlan_override_vid = atoi(optarg + 4) & 0xfff;
			} else {
				snprintf(oam.vlan_override, sizeof(oam.vlan_override), "%s", optarg);
			}
			break;
		case 'd': oam.debug = atoi(optarg); break;
		default: usage();
		}
	}

	kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (find_sysfs() || read_mac()) {
		fprintf(stderr, "epon-oamd: %s has no EPON driver attributes\n", oam.ifname);
		return 1;
	}
	/* pon-mode already held the transmitter off at boot. */
	oam.tx_off_permanent = !access(ROGUE_FILE, F_OK);
	if (open_socket()) {
		fprintf(stderr, "epon-oamd: %s: %s\n", oam.ifname, strerror(errno));
		return 1;
	}
	{
		struct sigaction sa = { .sa_sigaction = on_signal, .sa_flags = SA_SIGINFO };

		sigaction(SIGTERM, &sa, NULL);
		sigaction(SIGINT, &sa, NULL);
	}
	{
		struct sigaction sa = { .sa_sigaction = on_fault, .sa_flags = SA_SIGINFO };

		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS, &sa, NULL);
		sigaction(SIGFPE, &sa, NULL);
		sigaction(SIGILL, &sa, NULL);
		sigaction(SIGABRT, &sa, NULL);
	}

	oam.flags = FLAG_LOCAL_EVAL;
	set_oam_link(false);
	/*
	 * Still registered from a previous instance: the OLT keeps that OAM
	 * session and never repeats CTC discovery, so leave and come back.
	 */
	if (driver_registered(&oam.signal))
		go_silent("registered before start, registering again", RESTART_SILENCE_MS);
	else
		sysfs_write("epon_register", "1");
	apply_vlan();
	logmsg(0, "epon-oamd on %s, ONU MAC %02x:%02x:%02x:%02x:%02x:%02x, LOID '%s'",
	       oam.ifname, oam.mac[0], oam.mac[1], oam.mac[2], oam.mac[3], oam.mac[4],
	       oam.mac[5], oam.loid);
	write_status();

	while (!stop) {
		struct pollfd pfd = { .fd = oam.fd, .events = POLLIN };
		uint8_t buf[2048];
		uint64_t now;

		if (poll(&pfd, 1, 100) > 0 && pfd.revents & POLLIN) {
			ssize_t n = recv(oam.fd, buf, sizeof(buf), 0);

			if (n > 0 && !oam.silent_until_ms)
				handle_frame(buf, n);
		}

		now = now_ms();
		if (now - last_poll >= 1000) {
			bool signal;
			bool reg = driver_registered(&signal);

			last_poll = now;
			/* gcoam_get_holdover_status: light loss inside holdover is ignored. */
			if (!signal && oam.signal) {
				oam.los_ms = now;
				logmsg(0, "optical signal lost");
			}
			oam.signal = signal;
			if (signal) {
				oam.los_ms = 0;
			} else if (oam.los_ms && oam.disc != DISC_WAIT &&
				   now - oam.los_ms >= (oam.holdover_state == 2 ? oam.holdover_ms : 0)) {
				oam.los_ms = 0;
				link_down("loss of signal");
			}
			if (reg != oam.registered) {
				logmsg(0, "MPCP %s", reg ? "registered" : "deregistered");
				oam.registered = reg;
				oam.status_dirty = true;
				if (!reg)
					link_down("MPCP deregistered");
			}
			if (oam.tx_off_until_ms && now >= oam.tx_off_until_ms)
				tx_release("off period over");
			if (oam.silent_until_ms && now >= oam.silent_until_ms) {
				logmsg(0, "silence over, registering again");
				oam.silent_until_ms = 0;
				sysfs_write("epon_register", "1");
				oam.status_dirty = true;
			}
			if (oam.disc != DISC_WAIT && now - oam.last_rx_ms >= LINK_LOST_MS) {
				/* gcoam_keep_alive_handler: lost while the LOID was out is a failure. */
				if (oam.auth == AUTH_SENT)
					go_silent("LOID authentication never answered", SILENCE_MS);
				else
					link_down("no OAMPDU from the OLT for 5 s");
			}
			if (oam.disc == DISC_SEND_ANY && !oam.ctc_checked &&
			    now - oam.send_any_ms >= CTC_WAIT_MS) {
				oam.ctc_checked = true;
				if (oam.auth == AUTH_SENT)
					go_silent("LOID authentication never answered", SILENCE_MS);
				else if (oam.ctc != CTC_OK)
					go_silent("CTC discovery did not finish", CTC_SILENCE_MS);
			}
			/*
			 * An OLT that keeps an old OAM session can leave
			 * discovery stalled for good: register again.
			 */
			if (oam.registered && oam.disc != DISC_SEND_ANY && !oam.silent_until_ms) {
				if (!oam.disc_since_ms) {
					oam.disc_since_ms = now;
				} else if (now - oam.disc_since_ms >= DISC_TIMEOUT_MS) {
					oam.disc_since_ms = 0;
					go_silent("OAM discovery did not finish", RESTART_SILENCE_MS);
				}
			} else {
				oam.disc_since_ms = 0;
			}
			check_alarms();
			/*
			 * Passive (stock, and what 802.3ah expects of an ONU) waits
			 * for the OLT's first Information PDU; active mode opens
			 * discovery itself for OLTs that never do.
			 */
			if ((oam.disc != DISC_WAIT ||
			     (oam.active && oam.registered && !oam.silent_until_ms)) &&
			    now - oam.last_tx_ms >= KEEPALIVE_MS)
				send_info(0, 0);
		}
		if (oam.status_dirty || now - last_status >= 5000) {
			write_status();
			last_status = now;
		}
	}

	set_oam_link(false);
	log_stop();
	return 0;
}
