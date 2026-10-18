// SPDX-License-Identifier: GPL-2.0-only
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

static ssize_t gate_test_write(int fd, const void *buf, size_t count);
static ssize_t drain_test_recv(int fd, void *buf, size_t len, int flags);
static char fake_rogue_file[320];
#define write gate_test_write
#define recv drain_test_recv
#define ROGUE_FILE fake_rogue_file
#define main epon_daemon_main
#include "../src/epon-oamd.c"
#undef main
#undef write
#undef recv

#include <assert.h>

static int gate_write_error;
static char last_gate_enable[32];
static bool drain_mock;
static int drain_remaining;
static unsigned int drain_calls;

static ssize_t drain_test_recv(int fd, void *buf, size_t len, int flags)
{
	if (!drain_mock)
		return recv(fd, buf, len, flags);
	assert(flags == MSG_DONTWAIT);
	drain_calls++;
	if (!drain_remaining) {
		errno = EAGAIN;
		return -1;
	}
	if (drain_remaining > 0)
		drain_remaining--;
	return 1;
}

static ssize_t gate_test_write(int fd, const void *buf, size_t count)
{
	const char *value = buf;

	if (count > 1 && value[0] == '1' && value[1] == ' ') {
		assert(count < sizeof(last_gate_enable));
		memcpy(last_gate_enable, value, count);
		last_gate_enable[count] = 0;
		if (gate_write_error) {
			errno = gate_write_error;
			return -1;
		}
	}
	return write(fd, buf, count);
}

static void test_vlan_reply(void)
{
	uint8_t frame[1600], small[64];
	struct out response = { frame, 0, sizeof(frame) };
	size_t i;

	oam.vlan_mode = 2;
	oam.vlan_default = 100;
	oam.vlan_xlates = 8;
	for (i = 0; i < 8; i++) {
		oam.vlan_xlate[i].old_vid = i + 10;
		oam.vlan_xlate[i].new_vid = i + 100;
	}
	memset(small, 0xa5, sizeof(small));
	assert(port_vlan_value(small, sizeof(small)) == 0);
	for (i = 0; i < sizeof(small); i++)
		assert(small[i] == 0xa5);
	get_attr(&response, BR_EXT_ATTR, 0x0021);
	assert(response.len == 73);
	assert(frame[3] == 69);
	assert(get16(frame + 71) == 107);
	oam.vlan_xlates = 7;
	assert(port_vlan_value(small, sizeof(small)) == 61);
	oam.vlan_mode = 4;
	oam.vlan_trunks = 8;
	assert(port_vlan_value(small, sizeof(small)) == 37);
	oam.vlan_mode = 0;
	assert(port_vlan_value(small, 1) == 1);
	assert(port_vlan_value(small, 0) == 0);
}

static void test_information_size(void)
{
	uint8_t tlv[16], src[ETH_ALEN] = { 0 };

	put_local_tlv(tlv);
	/* State/configuration must not be mistaken for the PDU size. */
	tlv[5] = 0;
	tlv[6] = 1;
	oam.have_olt_local = false;
	put16(tlv + 7, 1518);
	handle_info(src, tlv, sizeof(tlv));
	assert(oam.have_olt_local);
	oam.have_olt_local = false;
	put16(tlv + 7, 63);
	handle_info(src, tlv, sizeof(tlv));
	assert(!oam.have_olt_local);
	put16(tlv + 7, 0);
	handle_info(src, tlv, sizeof(tlv));
	assert(!oam.have_olt_local);
	put16(tlv + 7, 64);
	handle_info(src, tlv, sizeof(tlv) - 1);
	assert(!oam.have_olt_local);
	handle_info(src, tlv, sizeof(tlv));
	assert(oam.have_olt_local);

	/* A smaller OLT size is adopted, a larger one is not (stock). */
	oam.have_olt_local = false;
	put16(tlv + 7, 1518);
	handle_info(src, tlv, sizeof(tlv));
	put_local_tlv(tlv);
	assert(get16(tlv + 7) == 1518);
	oam.have_olt_local = false;
	tlv[0] = TLV_LOCAL;
	put16(tlv + 7, 2000);
	handle_info(src, tlv, sizeof(tlv));
	put_local_tlv(tlv);
	assert(get16(tlv + 7) == LOCAL_PDU_SIZE);
}

static void test_unsupported_sets(void)
{
	uint8_t value[16] = { 0 };
	const uint16_t refused[] = { 0x0051, 0x00a1, 0x0099 };
	const uint16_t acked[] = { 0x0031, 0x0041, 0x0081, 0x00a4 };
	size_t i;

	for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
		assert(set_attr(BR_EXT_ATTR, refused[i], value, sizeof(value)) == VAR_BAD);
	for (i = 0; i < sizeof(acked) / sizeof(acked[0]); i++)
		assert(set_attr(BR_EXT_ATTR, acked[i], value, sizeof(value)) == VAR_SET_OK);
	assert(set_attr(BR_STD_ATTR, 0x0025, value, sizeof(value)) == VAR_BAD);
	assert(set_attr(BR_EXT_ACTION, 0xffff, value, sizeof(value)) == VAR_BAD);
	assert(!reboot_pending);
	assert(set_attr(BR_EXT_ACTION, 0x0001, NULL, 0) == VAR_SET_OK);
	assert(reboot_pending);
	reboot_pending = false;
}

static void test_interop_diagnostics(void)
{
	uint8_t org[] = { 0x12, 0x34, 0x56, 0x80 };
	uint8_t value = 0;
	unsigned long ignored = oam.acked_unapplied;
	unsigned long ouis = oam.unknown_ouis, opcodes = oam.unknown_opcodes;
	char *json = NULL;
	size_t size;
	FILE *fp;

	assert(set_attr(BR_EXT_ATTR, 0x0041, &value, 1) == VAR_SET_OK);
	assert(oam.acked_unapplied == ignored + 1);
	assert(set_attr(BR_EXT_ATTR, 0xeeee, &value, 1) == VAR_BAD);
	assert(oam.last_unknown_branch == BR_EXT_ATTR && oam.last_unknown_leaf == 0xeeee);
	handle_org(org, 3);
	assert(oam.unknown_ouis == ouis);
	handle_org(org, sizeof(org));
	assert(oam.unknown_ouis == ouis + 1 && oam.last_unknown_oui == 0x123456);
	put24(org, CTC_OUI);
	handle_org(org, sizeof(org));
	assert(oam.unknown_opcodes == opcodes + 1 && oam.last_unknown_opcode == 0x80);
	fp = open_memstream(&json, &size);
	assert(fp);
	write_status_json(fp);
	assert(!fclose(fp));
	assert(strstr(json, "\"last_unknown_oui\":1193046"));
	assert(strstr(json, "\"last_unknown_opcode\":128"));
	assert(strstr(json, "\"last_unknown_leaf\":61166"));
	assert(strstr(json, "\"acked_unapplied\":"));
	free(json);
}

static void test_key_install_failure(void)
{
	uint8_t request[] = { 0, 0 }, frame[16];
	size_t i;

	memset(frame, 0xa5, sizeof(frame));
	oam.keys = 0;
	oam.key_index = 0;
	handle_churning(request, sizeof(request), frame, 0);
	assert(oam.keys == 0);
	assert(oam.key_index == 0);
	for (i = 0; i < sizeof(frame); i++)
		assert(frame[i] == 0xa5);
}

static void test_vlan_set_validation(void)
{
	uint8_t mode[] = { 3 }, tag[] = { 1, 0x81, 0, 0, 100 };
	uint8_t translation[] = { 2, 0x81, 0, 0, 100,
		0x81, 0, 0, 10, 0x81, 0, 0, 20 };

	oam.vlan_mode = 0;
	oam.vlan_default = 0;
	assert(set_port_vlan(mode, sizeof(mode)) == VAR_BAD);
	mode[0] = 4;
	assert(set_port_vlan(mode, sizeof(mode)) == VAR_BAD);
	assert(set_port_vlan(tag, 4) == VAR_BAD);
	tag[4] = 0;
	assert(set_port_vlan(tag, sizeof(tag)) == VAR_BAD);
	assert(set_port_vlan(translation, sizeof(translation) - 1) == VAR_BAD);
	translation[8] = 0;
	assert(set_port_vlan(translation, sizeof(translation)) == VAR_BAD);
	assert(oam.vlan_mode == 0 && oam.vlan_default == 0);
	tag[4] = 100;
	assert(set_port_vlan(tag, sizeof(tag)) == VAR_BAD);
	assert(oam.vlan_mode == 0 && oam.vlan_default == 0);
}

/* A writable stand-in for the driver's sysfs directory. */
static char fake_sysfs[] = "/tmp/epon-oamd-sysfs-XXXXXX";

static void fake_write(const char *attr, const char *value)
{
	char path[320];
	int fd;

	snprintf(path, sizeof(path), "%s/%s", fake_sysfs, attr);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, value, strlen(value)) == (ssize_t)strlen(value));
	close(fd);
}

static void fake_reset(const char *attr)
{
	fake_write(attr, "");
}

static void fake_remove(const char *attr)
{
	char path[320];

	snprintf(path, sizeof(path), "%s/%s", fake_sysfs, attr);
	assert(!unlink(path) || errno == ENOENT);
}

static void fake_expect(const char *attr, const char *want)
{
	char buf[256] = "";

	assert(!sysfs_read(attr, buf, sizeof(buf)));
	assert(!strcmp(buf, want));
}

static void test_vlan_modes(void)
{
	/* N:1: default 100, one table: target 200 <- 10, 11 */
	uint8_t agg[] = { 3, 0x81, 0, 0, 100, 0, 1,
		0, 2, 0x81, 0, 0, 200, 0x81, 0, 0, 10, 0x81, 0, 0, 11 };
	uint8_t trunk[] = { 4, 0x81, 0, 0, 100, 0x81, 0, 0, 10, 0x81, 0, 0, 20 };
	uint8_t vid0[] = { 1, 0x81, 0, 0, 0 };
	uint8_t frame[1600];
	struct out response = { frame, 0, sizeof(frame) };

	fake_reset("sfu_untag_vlan");
	fake_reset("sfu_vlan_map");
	assert(set_port_vlan(agg, sizeof(agg)) == VAR_SET_OK);
	fake_expect("sfu_vlan_map", "t200:10 t200:11 \n");
	get_attr(&response, BR_EXT_ATTR, 0x0021);
	assert(frame[3] == sizeof(agg) && !memcmp(frame + 4, agg, sizeof(agg)));
	agg[sizeof(agg) - 1] = 0;
	assert(set_port_vlan(agg, sizeof(agg)) == VAR_BAD);
	assert(set_port_vlan(agg, sizeof(agg) - 4) == VAR_BAD);

	fake_reset("sfu_vlan_map");
	assert(set_port_vlan(trunk, sizeof(trunk)) == VAR_SET_OK);
	fake_expect("sfu_vlan_map", "t10:10 t20:20 \n");

	fake_reset("sfu_untag_vlan");
	assert(set_port_vlan(vid0, sizeof(vid0)) == VAR_SET_OK);
	assert(oam.vlan_default == 1);
	fake_expect("sfu_untag_vlan", "1");
}

static void test_holdover(void)
{
	uint8_t on[] = { 0, 0, 0, 2, 0, 0, 0x01, 0xf4 }, bad[] = { 0, 0, 0, 3, 0, 0, 0, 0 };

	fake_reset("epon_holdover");
	assert(set_attr(BR_EXT_ATTR, 0x0008, on, sizeof(on)) == VAR_SET_OK);
	fake_expect("epon_holdover", "500");
	assert(set_attr(BR_EXT_ATTR, 0x0008, bad, sizeof(bad)) == VAR_BAD);
	assert(oam.holdover_state == 2 && oam.holdover_ms == 500);
}

static void test_tx_power(void)
{
	uint8_t v[12] = { 0, 5 };

	memcpy(oam.mac, "\x02\x00\x00\x00\x00\x01", ETH_ALEN);
	memcpy(v + 2, oam.mac, ETH_ALEN);
	/* An older driver must not ACK a timer it cannot own. */
	fake_write("tx_hold", "1");
	assert(set_attr(BR_EXT_ACTION, 0x00a1, v, sizeof(v)) == VAR_BAD);
	fake_expect("tx_hold", "1");
	fake_reset("tx_hold_seconds");
	assert(set_attr(BR_EXT_ACTION, 0x00a1, v, sizeof(v)) == VAR_SET_OK);
	fake_expect("tx_hold_seconds", "5");
	fake_expect("tx_hold", "1");
	assert(oam.tx_hold_seconds == 5 && !oam.tx_off_permanent);

	/* A new daemon observes the kernel's timer, without owning its expiry. */
	oam.tx_hold_seconds = 0;
	update_timed_hold();
	assert(oam.tx_hold_seconds == 5);
	fake_write("tx_hold_seconds", "0");
	update_timed_hold();
	assert(!oam.tx_hold_seconds);
	fake_expect("tx_hold", "1");

	/* Release after a restart still uses the timed interface, never tx_hold. */
	v[1] = 0;
	fake_reset("tx_hold_seconds");
	assert(set_attr(BR_EXT_ACTION, 0x00a1, v, sizeof(v)) == VAR_SET_OK);
	fake_expect("tx_hold_seconds", "0");
	fake_expect("tx_hold", "1");
	fake_remove("tx_hold_seconds");
	assert(set_attr(BR_EXT_ACTION, 0x00a1, v, sizeof(v)) == VAR_BAD);
	fake_expect("tx_hold", "1");

	put16(v, 65534);
	fake_reset("tx_hold_seconds");
	assert(set_attr(BR_EXT_ACTION, 0x00a1, v, sizeof(v)) == VAR_SET_OK);
	fake_expect("tx_hold_seconds", "65534");
	assert(oam.tx_hold_seconds == 65534);

	/* someone else's MAC: no effect */
	put16(v, 5);
	v[7] ^= 1;
	fake_reset("tx_hold_seconds");
	assert(set_attr(BR_EXT_ACTION, 0x00a1, v, sizeof(v)) == VAR_SET_OK);
	fake_expect("tx_hold_seconds", "");
	assert(set_attr(BR_EXT_ACTION, 0x00a1, v, 5) == VAR_BAD);
	v[7] ^= 1;

	/* Permanent holds keep the rogue marker and refuse temporary overrides. */
	put16(v, 0xffff);
	fake_reset("tx_hold");
	assert(set_attr(BR_EXT_ACTION, 0x00a1, v, sizeof(v)) == VAR_SET_OK);
	fake_expect("tx_hold", "1");
	assert(oam.tx_off_permanent && !access(ROGUE_FILE, F_OK));
	put16(v, 5);
	assert(set_attr(BR_EXT_ACTION, 0x00a1, v, sizeof(v)) == VAR_BAD);
	fake_expect("tx_hold_seconds", "");
	put16(v, 0);
	memset(v + 2, 0xff, ETH_ALEN);
	assert(set_attr(BR_EXT_ACTION, 0x00a1, v, sizeof(v)) == VAR_SET_OK);
	fake_expect("tx_hold", "1");
	assert(oam.tx_off_permanent && !access(ROGUE_FILE, F_OK));
	memcpy(v + 2, oam.mac, ETH_ALEN);
	/* A failed permanent release cannot remove the safety marker. */
	fake_remove("tx_hold");
	assert(set_attr(BR_EXT_ACTION, 0x00a1, v, sizeof(v)) == VAR_BAD);
	assert(oam.tx_off_permanent && !access(ROGUE_FILE, F_OK));
	fake_reset("tx_hold");
	assert(set_attr(BR_EXT_ACTION, 0x00a1, v, sizeof(v)) == VAR_SET_OK);
	fake_expect("tx_hold", "0");
	assert(!oam.tx_off_permanent && access(ROGUE_FILE, F_OK));
}

static void test_driver_state(void)
{
	const char *old = "mode=epon oam_link=1 signal=1 sync=1 registered=0x1\n";
	const char *current =
		"mode=epon oam_link=1 signal=1 sync=1 registered=0x1 generation=4294967295\n";
	struct driver_state state;
	char path[320];
	int fd;

	assert(!parse_driver_state(old, &state));
	assert(state.registered && state.signal && state.oam_link && !state.have_generation);
	assert(!parse_driver_state(current, &state));
	assert(state.have_generation && state.generation == UINT32_MAX);
	assert(parse_driver_state("signal=1", &state));
	assert(parse_driver_state(" signal=1 registered=1 generation=-1", &state));
	assert(parse_driver_state(" signal=1 registered=1 generation=4294967296", &state));
	assert(parse_driver_state(" signal=1 registered=1 generation=bad", &state));
	assert(parse_driver_state(" signal=1 registered=1 generation=1bad", &state));

	fake_write("epon_state", current);
	snprintf(path, sizeof(path), "%s/epon_state", fake_sysfs);
	fd = open(path, O_RDONLY);
	assert(fd >= 0);
	assert(!read_driver_state(fd, &state) && state.generation == UINT32_MAX);
	/* A second read must seek back and rearm, rather than see EOF. */
	assert(!read_driver_state(fd, &state) && state.generation == UINT32_MAX);
	fake_write("epon_state", old);
	assert(!read_driver_state(fd, &state) && !state.have_generation);
	close(fd);
	assert(!read_driver_state(-1, &state) && state.registered);
	fake_remove("epon_state");
	assert(read_driver_state(-1, &state));
}

static void test_session_recovery(void)
{
	struct driver_state state = {
		.registered = true, .signal = true,
		.have_generation = true, .generation = UINT32_MAX,
		.have_oam_link = true, .oam_link = true,
	};
	uint8_t tlv[16], src[ETH_ALEN] = { 2, 0, 0, 0, 0, 2 };

	fake_reset("epon_oam_link");
	oam.registered = true;
	oam.signal = true;
	oam.have_generation = false;
	oam.disc = DISC_SEND_ANY;
	assert(!update_driver_state(&state, 1000));
	fake_expect("epon_oam_link", "");
	assert(!update_driver_state(&state, 2000));
	assert(oam.disc == DISC_SEND_ANY);

	oam.ctc = CTC_OK;
	oam.ctc_version = 0x30;
	oam.auth = AUTH_OK;
	oam.auth_fail_reason = 7;
	oam.interauth = 1;
	oam.have_olt_local = true;
	oam.flags = FLAG_LOCAL_STABLE | FLAG_REMOTE_STABLE;
	oam.olt_flags = FLAG_LOCAL_STABLE;
	oam.ctc_checked = true;
	oam.disc_since_ms = 1000;
	oam.send_any_ms = 1000;
	oam.los_ms = 1000;
	oam.silent_until_ms = 60000;
	memcpy(oam.olt_mac, src, sizeof(src));
	/* Coalesced loss/recovery, including u32 wrap: levels are unchanged. */
	state.generation = 0;
	assert(update_driver_state(&state, 3000));
	assert(oam.registered && oam.signal && oam.generation == 0);
	assert(oam.disc == DISC_WAIT && oam.ctc == CTC_NONE && oam.auth == AUTH_NONE);
	assert(!oam.ctc_version && !oam.auth_fail_reason && !oam.interauth);
	assert(!oam.have_olt_local && !oam.olt_mac[0] && !oam.olt_flags);
	assert(oam.flags == FLAG_LOCAL_EVAL && !oam.ctc_checked);
	assert(!oam.disc_since_ms && !oam.send_any_ms && !oam.los_ms);
	assert(oam.silent_until_ms == 60000);
	fake_expect("epon_oam_link", "0");
	assert(!update_driver_state(&state, 4000));
	assert(oam.disc == DISC_WAIT);

	/* The gate only reopens through a fresh OAM discovery exchange. */
	oam.silent_until_ms = 0;
	put_local_tlv(tlv);
	oam.olt_flags = FLAG_LOCAL_EVAL;
	handle_info(src, tlv, sizeof(tlv));
	assert(oam.disc == DISC_SEND_LOCAL_REMOTE);
	fake_expect("epon_oam_link", "0");
	oam.olt_flags = FLAG_LOCAL_STABLE;
	/* The driver can change epoch between our read and the enable write. */
	gate_write_error = ESTALE;
	handle_info(src, tlv, sizeof(tlv));
	assert(!strcmp(last_gate_enable, "1 0"));
	assert(oam.disc == DISC_WAIT && !oam.have_olt_local);
	assert(oam.ctc == CTC_NONE && oam.auth == AUTH_NONE && !oam.send_any_ms);
	fake_expect("epon_oam_link", "0");
	gate_write_error = 0;
	state.generation = 1;
	assert(update_driver_state(&state, 4500));
	oam.olt_flags = FLAG_LOCAL_STABLE;
	handle_info(src, tlv, sizeof(tlv));
	assert(oam.disc == DISC_SEND_ANY);
	fake_expect("epon_oam_link", "1 1");

	/* With no generation support, a cleared gate also forces discovery. */
	state.have_generation = false;
	state.oam_link = false;
	fake_reset("epon_oam_link");
	assert(update_driver_state(&state, 5000));
	assert(oam.disc == DISC_WAIT && !oam.have_generation);
	fake_expect("epon_oam_link", "0");
	assert(!update_driver_state(&state, 6000));
	oam.olt_flags = FLAG_LOCAL_STABLE;
	handle_info(src, tlv, sizeof(tlv));
	assert(oam.disc == DISC_SEND_ANY);
	fake_expect("epon_oam_link", "1");

	/* Legacy level-only drivers still reset on MPCP deregistration. */
	state.have_oam_link = false;
	state.registered = false;
	oam.disc = DISC_SEND_ANY;
	assert(update_driver_state(&state, 7000));
	assert(!oam.registered && oam.disc == DISC_WAIT);
}

static void test_discard_oam(void)
{
	drain_mock = true;
	drain_remaining = 0;
	drain_calls = 0;
	discard_oam();
	assert(drain_calls == 1);
	drain_remaining = 3;
	drain_calls = 0;
	discard_oam();
	assert(drain_calls == 4 && !drain_remaining);
	/* Simulate an endless arrival stream, rather than a finite backlog. */
	drain_remaining = -1;
	drain_calls = 0;
	discard_oam();
	assert(drain_calls == 64);
	drain_mock = false;
}

static void test_status_generation(void)
{
	const struct {
		bool have_generation;
		uint32_t generation;
		const char *json;
	} cases[] = {
		{ true, UINT32_MAX, "\"generation\":4294967295," },
		{ true, 0, "\"generation\":0," },
		{ false, 123, "\"generation\":null," },
	};
	char buf[2048];
	size_t i, n;
	FILE *fp;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		oam.have_generation = cases[i].have_generation;
		oam.generation = cases[i].generation;
		fp = tmpfile();
		assert(fp);
		write_status_json(fp);
		rewind(fp);
		n = fread(buf, 1, sizeof(buf) - 1, fp);
		buf[n] = 0;
		assert(strstr(buf, cases[i].json));
		fclose(fp);
	}
}

static void test_sw_download(void)
{
	/* type 1, length, TID 0x1234, opcode 2 (write request) */
	uint8_t wrq[] = { 1, 0, 20, 0x12, 0x34, 0, 2, 'f', 'w', 0 };
	uint8_t act[] = { 3, 0, 8, 0x12, 0x35, 0, 8, 0 };
	uint8_t out[128];
	size_t n;

	n = sw_download_reply(wrq, sizeof(wrq), out);
	assert(n == 9 + sizeof("software download not supported"));
	assert(out[0] == 1 && get16(out + 1) == n && get16(out + 3) == 0x1234);
	assert(get16(out + 5) == 5 && get16(out + 7) == 4 && out[n - 1] == 0);
	n = sw_download_reply(act, sizeof(act), out);
	assert(n == 8 && out[0] == 3 && get16(out + 3) == 0x1235);
	assert(get16(out + 5) == 9 && out[7] == 1);
	act[6] = 10;
	n = sw_download_reply(act, sizeof(act), out);
	assert(n == 8 && out[0] == 4 && get16(out + 5) == 11 && out[7] == 1);
	assert(sw_download_reply(act, 3, out) == 0);
}

static void test_events(void)
{
	/* status set: RX power high (0x0101) on, unknown 0x0999 */
	uint8_t set[] = { 2, 0, 2,
		0, 4, 0, 0, 0, 0, 0x01, 0x01, 0, 0, 0, 2,
		0, 4, 0, 0, 0, 0, 0x09, 0x99, 0, 0, 0, 2 };
	uint8_t get[] = { 1, 0, 1, 0, 4, 0, 0, 0, 0, 0x01, 0x01 };
	uint8_t thr[] = { 5, 0, 1, 0, 4, 0, 0, 0, 0, 0x01, 0x02,
		0, 0, 0, 7, 0, 0, 0, 9 };
	uint8_t out[256];
	size_t n;

	n = event_reply(set, sizeof(set), out);
	assert(n == 3 + 2 * 12 && out[0] == 3 && get16(out + 1) == 2);
	assert(get32(out + 3 + 8) == 2 && get32(out + 15 + 8) == 0xffffffff);
	assert(oam.alarm_status[0] == 2);
	n = event_reply(get, sizeof(get), out);
	assert(n == 15 && get32(out + 11) == 2);
	n = event_reply(thr, sizeof(thr), out);
	assert(n == 19 && out[0] == 6 && get32(out + 11) == 7 && get32(out + 15) == 9);
	assert(oam.alarm_raise[1] == 7 && oam.alarm_clear[1] == 9);
	assert(event_reply(get, 2, out) == 0);
}

static void test_vsol(void)
{
	static const uint8_t mac[ETH_ALEN] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0xaa };
	char long_msg[131], key[33];
	uint8_t frame[256];
	struct out o = { frame, 0, sizeof(frame) };

	md5_hex("abc", key);
	assert(!strcmp(key, "900150983cd24fb0d6963f7d28e17f72"));
	memset(long_msg, 'x', 130);
	long_msg[130] = 0;
	md5_hex(long_msg, key);
	assert(!strcmp(key, "cfc92f41305ca00c9f9584bf67ae1b54"));

	memcpy(oam.mac, mac, ETH_ALEN);
	oam.auth_secret[0] = 0;
	get_attr(&o, BR_EXT_ATTR, 0x2040);
	assert(frame[3] == 0x86);		/* off without a secret: not supported */
	o.len = 0;
	snprintf(oam.auth_secret, sizeof(oam.auth_secret), "test-secret");
	get_attr(&o, BR_EXT_ATTR, 0x2040);
	assert(frame[3] == 64 && !memcmp(frame + 4, "4b7bbed0724fb2a03ccb36af9b34a5e4", 32));
	assert(frame[4 + 32] == 0 && frame[4 + 63] == 0);
	assert(set_attr(BR_EXT_ATTR, 0x2040, (const uint8_t *)"e40b0cc7dd151e106e422c39bb5c7a8e", 32) ==
	       VAR_SET_OK && oam.interauth == 1);
	assert(set_attr(BR_EXT_ATTR, 0x2040, (const uint8_t *)"00000000000000000000000000000000", 32) ==
	       VAR_SET_OK && oam.interauth == -1);
	oam.auth_secret[0] = 0;

	/* ONU SN: 38 bytes before CTC 3.0, 54 with the extended model after. */
	o.len = 0;
	oam.ctc_version = 0x21;
	get_attr(&o, BR_EXT_ATTR, 0x0001);
	assert(frame[3] == 38);
	o.len = 0;
	oam.ctc_version = 0x30;
	get_attr(&o, BR_EXT_ATTR, 0x0001);
	assert(frame[3] == 54 && !memcmp(frame + 4 + 38, "XZ000-G7", 8));
}

int main(void)
{
	strcpy(oam.sysfs, "/dev/null/epon-oamd-test");
	test_vlan_reply();
	test_information_size();
	test_unsupported_sets();
	test_interop_diagnostics();
	test_key_install_failure();
	test_vlan_set_validation();
	test_sw_download();
	test_vsol();
	test_events();
	assert(mkdtemp(fake_sysfs));
	strcpy(oam.sysfs, fake_sysfs);
	snprintf(fake_rogue_file, sizeof(fake_rogue_file), "%s/rogue", fake_sysfs);
	test_vlan_modes();
	test_holdover();
	test_tx_power();
	test_driver_state();
	test_session_recovery();
	test_discard_oam();
	test_status_generation();
	fake_remove("sfu_untag_vlan");
	fake_remove("sfu_vlan_map");
	fake_remove("epon_holdover");
	fake_remove("tx_hold");
	fake_remove("tx_hold_seconds");
	fake_remove("epon_oam_link");
	assert(!rmdir(fake_sysfs));
	puts("OAM tests passed");
	return 0;
}
