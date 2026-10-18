// SPDX-License-Identifier: GPL-2.0-only
/*
 * Find the VLANs that carry the internet service behind a GPON ONU, as the
 * vendor vlan_search does: send a PPPoE PADI and a DHCP DISCOVER on the PON
 * interface tagged with every candidate VLAN, and report the VLANs that get a
 * PADO or a DHCP OFFER back. VID 0 stands for untagged.
 *
 *   omci-vlanscan [-i IFACE] [-w SECONDS] [-p PRIO] [-j] VID[:PRIO]...
 */

#include <arpa/inet.h>
#include <errno.h>
#include <glob.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_PROBES	8192
#define MAX_RESULTS	256
#define PROBE_MAGIC	0x56530000u	/* high 16 bits of Host-Uniq and xid */

#define ETH_P_PPPOE_DISC	0x8863
#define PPPOE_CODE_PADI		0x09
#define PPPOE_CODE_PADO		0x07
#define PPPOE_TAG_END		0x0000
#define PPPOE_TAG_SERVICE_NAME	0x0101
#define PPPOE_TAG_AC_NAME	0x0102
#define PPPOE_TAG_HOST_UNIQ	0x0103

#define DHCP_MAGIC_COOKIE	0x63825363
#define DHCP_DISCOVER		1
#define DHCP_OFFER		2

struct result {
	int vid;
	int prio;
	int reply_prio;
	bool dhcp;
	char detail[96];
};

struct probe {
	int vid;
	int prio;
};

static struct result results[MAX_RESULTS];
static int nresults;
static uint8_t mac[ETH_ALEN];
static int default_prio;

static uint32_t probe_cookie(int vid, int prio)
{
	return htonl(PROBE_MAGIC | (prio << 12) | vid);
}

static size_t put_eth(uint8_t *p, int vid, int prio, uint16_t proto)
{
	size_t n = 0;

	memset(p, 0xff, ETH_ALEN);
	memcpy(p + ETH_ALEN, mac, ETH_ALEN);
	n = 2 * ETH_ALEN;
	if (vid) {
		uint16_t tci = htons((prio << 13) | vid);
		uint16_t tpid = htons(ETH_P_8021Q);

		memcpy(p + n, &tpid, 2);
		memcpy(p + n + 2, &tci, 2);
		n += 4;
	}
	proto = htons(proto);
	memcpy(p + n, &proto, 2);
	return n + 2;
}

static size_t build_padi(uint8_t *p, int vid, int prio)
{
	size_t n = put_eth(p, vid, prio, ETH_P_PPPOE_DISC);
	uint32_t uniq = probe_cookie(vid, prio);
	uint8_t *tags;

	p[n] = 0x11;			/* version 1, type 1 */
	p[n + 1] = PPPOE_CODE_PADI;
	p[n + 2] = p[n + 3] = 0;	/* session 0 */
	p[n + 4] = 0;
	p[n + 5] = 12;			/* payload: two tags */
	tags = p + n + 6;
	tags[0] = PPPOE_TAG_SERVICE_NAME >> 8;
	tags[1] = PPPOE_TAG_SERVICE_NAME & 0xff;
	tags[2] = tags[3] = 0;
	tags[4] = PPPOE_TAG_HOST_UNIQ >> 8;
	tags[5] = PPPOE_TAG_HOST_UNIQ & 0xff;
	tags[6] = 0;
	tags[7] = 4;
	memcpy(tags + 8, &uniq, 4);
	return n + 6 + 12;
}

static uint16_t ip_checksum(const uint8_t *p, size_t len)
{
	uint32_t sum = 0;

	for (; len > 1; p += 2, len -= 2)
		sum += (p[0] << 8) | p[1];
	if (len)
		sum += p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return htons(~sum);
}

static size_t build_discover(uint8_t *p, int vid, int prio)
{
	size_t n = put_eth(p, vid, prio, ETH_P_IP);
	uint8_t *ip = p + n, *udp = ip + 20, *b = udp + 8;
	uint32_t xid = probe_cookie(vid, prio), cookie = htonl(DHCP_MAGIC_COOKIE);
	size_t blen = 240 + 9, len;

	memset(ip, 0, 20 + 8 + blen);
	b[0] = 1;			/* BOOTREQUEST */
	b[1] = 1;			/* Ethernet */
	b[2] = ETH_ALEN;
	memcpy(b + 4, &xid, 4);
	b[10] = 0x80;			/* broadcast reply */
	memcpy(b + 28, mac, ETH_ALEN);
	memcpy(b + 236, &cookie, 4);
	b[240] = 53;			/* message type */
	b[241] = 1;
	b[242] = DHCP_DISCOVER;
	b[243] = 55;			/* parameter list: mask, router, DNS */
	b[244] = 3;
	b[245] = 1;
	b[246] = 3;
	b[247] = 6;
	b[248] = 255;

	len = 8 + blen;
	udp[0] = 0; udp[1] = 68;
	udp[2] = 0; udp[3] = 67;
	udp[4] = len >> 8; udp[5] = len & 0xff;

	len += 20;
	ip[0] = 0x45;
	ip[2] = len >> 8; ip[3] = len & 0xff;
	ip[8] = 64;
	ip[9] = 17;			/* UDP */
	memset(ip + 16, 0xff, 4);	/* 0.0.0.0 -> 255.255.255.255 */
	*(uint16_t *)(ip + 10) = ip_checksum(ip, 20);

	return n + len;
}

static void add_result(int vid, int vprio, int reply_prio, bool dhcp,
		       const char *detail)
{
	int i;

	for (i = 0; i < nresults; i++)
		if (results[i].vid == vid && results[i].prio == vprio &&
		    results[i].dhcp == dhcp)
			return;
	if (nresults == MAX_RESULTS)
		return;
	results[nresults].vid = vid;
	results[nresults].prio = vprio;
	results[nresults].reply_prio = reply_prio;
	results[nresults].dhcp = dhcp;
	snprintf(results[nresults].detail, sizeof(results[0].detail), "%s",
		 detail);
	nresults++;
}

/* Keep VID and transmitted priority in the echoed Host-Uniq or xid. */
static bool probe_identity(uint32_t value, int *vid, int *prio)
{
	value = ntohl(value);
	if ((value & 0xffff0000u) != PROBE_MAGIC || (value & 0x8000))
		return false;
	*vid = value & 0xfff;
	*prio = (value >> 12) & 7;
	return *vid <= 4094;
}

static void parse_pado(const uint8_t *p, size_t len, int reply_prio)
{
	char ac[64] = "";
	int vid = -1, prio = 0;
	size_t off = 6, plen;

	if (len < 6 || p[1] != PPPOE_CODE_PADO)
		return;
	plen = (p[4] << 8) | p[5];
	if (plen + 6 < len)
		len = plen + 6;

	while (off + 4 <= len) {
		uint16_t type = (p[off] << 8) | p[off + 1];
		uint16_t tlen = (p[off + 2] << 8) | p[off + 3];
		const uint8_t *v = p + off + 4;

		if (off + 4 + tlen > len || type == PPPOE_TAG_END)
			break;
		if (type == PPPOE_TAG_HOST_UNIQ && tlen == 4) {
			uint32_t uniq;

			memcpy(&uniq, v, 4);
			if (!probe_identity(uniq, &vid, &prio))
				vid = -1;
		} else if (type == PPPOE_TAG_AC_NAME) {
			snprintf(ac, sizeof(ac), "%.*s",
				 tlen < sizeof(ac) ? tlen : (int)sizeof(ac) - 1,
				 (const char *)v);
		}
		off += 4 + tlen;
	}

	if (vid >= 0)
		add_result(vid, prio, reply_prio, false, ac[0] ? ac : "PADO");
}

static void parse_offer(const uint8_t *ip, size_t len, int reply_prio)
{
	char detail[96], yi[16], sid[16] = "";
	const uint8_t *b;
	size_t ihl, blen, i;
	bool offer = false;
	uint32_t xid;
	int vid, prio;

	if (len < 20 || ip[9] != 17)
		return;
	ihl = (ip[0] & 0x0f) * 4;
	if (len < ihl + 8 + 240 || ip[ihl + 1] != 67 || ip[ihl + 3] != 68)
		return;
	b = ip + ihl + 8;
	blen = len - ihl - 8;
	if (b[0] != 2 || memcmp(b + 236, "\x63\x82\x53\x63", 4))
		return;
	memcpy(&xid, b + 4, 4);
	if (!probe_identity(xid, &vid, &prio))
		return;

	for (i = 240; i < blen && b[i] != 255;) {
		if (b[i] == 0) {
			i++;
			continue;
		}
		if (i + 2 > blen || i + 2 + b[i + 1] > blen)
			break;
		if (b[i] == 53 && b[i + 1] == 1)
			offer = b[i + 2] == DHCP_OFFER;
		else if (b[i] == 54 && b[i + 1] == 4)
			inet_ntop(AF_INET, b + i + 2, sid, sizeof(sid));
		i += 2 + b[i + 1];
	}
	if (!offer)
		return;

	inet_ntop(AF_INET, b + 16, yi, sizeof(yi));
	snprintf(detail, sizeof(detail), "offer %s%s%s", yi,
		 sid[0] ? " from " : "", sid);
	add_result(vid, prio, reply_prio, true, detail);
}

static void receive(int fd, int wait_ms)
{
	struct timespec start, now;
	uint8_t buf[2048];
	char cbuf[CMSG_SPACE(sizeof(struct tpacket_auxdata))];

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
		struct sockaddr_ll from;
		struct msghdr msg = {
			.msg_name = &from, .msg_namelen = sizeof(from),
			.msg_iov = &iov, .msg_iovlen = 1,
			.msg_control = cbuf, .msg_controllen = sizeof(cbuf),
		};
		struct cmsghdr *cmsg;
		int left, vprio = 0;
		uint16_t proto;
		size_t off = 2 * ETH_ALEN;
		ssize_t len;

		clock_gettime(CLOCK_MONOTONIC, &now);
		left = wait_ms - (int)((now.tv_sec - start.tv_sec) * 1000 +
				       (now.tv_nsec - start.tv_nsec) / 1000000);
		if (left <= 0 || poll(&pfd, 1, left) <= 0)
			break;

		len = recvmsg(fd, &msg, MSG_DONTWAIT);
		if (len < ETH_HLEN || from.sll_pkttype == PACKET_OUTGOING)
			continue;

		/* The kernel moves a received VLAN tag out of the frame */
		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			struct tpacket_auxdata *aux;

			if (cmsg->cmsg_level != SOL_PACKET ||
			    cmsg->cmsg_type != PACKET_AUXDATA)
				continue;
			aux = (struct tpacket_auxdata *)CMSG_DATA(cmsg);
			if (aux->tp_status & TP_STATUS_VLAN_VALID)
				vprio = aux->tp_vlan_tci >> 13;
		}

		proto = (buf[off] << 8) | buf[off + 1];
		if (proto == ETH_P_8021Q && (size_t)len >= off + 6) {
			vprio = buf[off + 2] >> 5;
			off += 4;
			proto = (buf[off] << 8) | buf[off + 1];
		}
		off += 2;

		if (proto == ETH_P_PPPOE_DISC)
			parse_pado(buf + off, len - off, vprio);
		else if (proto == ETH_P_IP)
			parse_offer(buf + off, len - off, vprio);
	}
}

static void usage(void)
{
	fprintf(stderr,
		"usage: omci-vlanscan [-i IFACE] [-w SECONDS] [-p PRIO] [-j] VID[:PRIO]...\n"
		"  VID 0 is untagged; -p sets the default probe priority\n");
	exit(2);
}

/*
 * A driver that forwards services in its switch (sfu_offload) sends their
 * replies to the LAN, not to pon0, and its LAN VLAN rewrite (sfu_untag_vlan,
 * sfu_vlan_map) also applies to pon0: an untagged probe would leave tagged.
 * Clear all three while scanning; PAUSE_PID tells omci-agent to wait.
 */
#define PAUSE_PID "/var/run/omci-vlanscan.pid"

static const char *const pause_attrs[] = {
	"sfu_offload", "sfu_untag_vlan", "sfu_vlan_map",
};
static const char *const pause_vals[] = { "0", "0", "\n" };

static struct {
	char path[256];
	char val[128];
} paused[3];

static void attr_write(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");

	if (f) {
		fputs(val, f);
		fclose(f);
	}
}

static void offload_restore(void)
{
	unsigned int i;

	for (i = 0; i < 3; i++) {
		if (paused[i].path[0])
			attr_write(paused[i].path, paused[i].val);
		paused[i].path[0] = 0;
	}
	unlink(PAUSE_PID);
}

static void offload_signal(int sig)
{
	offload_restore();
	_exit(128 + sig);
}

static void offload_pause(void)
{
	char pattern[64], pid[16];
	unsigned int i;
	glob_t g;
	FILE *f;

	snprintf(pid, sizeof(pid), "%d\n", getpid());
	attr_write(PAUSE_PID, pid);
	atexit(offload_restore);
	signal(SIGINT, offload_signal);
	signal(SIGTERM, offload_signal);

	for (i = 0; i < 3; i++) {
		snprintf(pattern, sizeof(pattern), "/sys/class/xpon/*/device/%s",
			 pause_attrs[i]);
		if (glob(pattern, 0, NULL, &g))
			continue;
		f = fopen(g.gl_pathv[0], "r");
		if (f) {
			/* sfu_vlan_map: a newline clears the map, keep it */
			if (!fgets(paused[i].val, sizeof(paused[i].val), f))
				snprintf(paused[i].val, sizeof(paused[i].val), "\n");
			fclose(f);
			snprintf(paused[i].path, sizeof(paused[i].path), "%s",
				 g.gl_pathv[0]);
			attr_write(paused[i].path, pause_vals[i]);
		}
		globfree(&g);
	}
}

int main(int argc, char **argv)
{
	const char *ifname = "pon0";
	static struct probe probes[MAX_PROBES];
	int nprobes = 0, wait_s = 4, one = 1, fd, opt, i;
	struct sockaddr_ll sll = { .sll_family = AF_PACKET };
	struct ifreq ifr = { 0 };
	bool json = false;
	uint8_t frame[600];

	while ((opt = getopt(argc, argv, "i:w:p:j")) != -1) {
		switch (opt) {
		case 'i':
			ifname = optarg;
			break;
		case 'w':
			wait_s = atoi(optarg);
			break;
		case 'p':
			default_prio = atoi(optarg);
			if (default_prio < 0 || default_prio > 7)
				usage();
			break;
		case 'j':
			json = true;
			break;
		default:
			usage();
		}
	}
	for (i = optind; i < argc; i++) {
		char *end;
		long vid, vprio = default_prio;

		if (nprobes == MAX_PROBES)
			usage();
		vid = strtol(argv[i], &end, 10);
		if (end == argv[i])
			usage();
		if (*end == ':') {
			char *start = end + 1;

			vprio = strtol(start, &end, 10);
			if (end == start)
				usage();
		}
		if (*end || vid < 0 || vid > 4094 || vprio < 0 || vprio > 7)
			usage();
		probes[nprobes++] = (struct probe) { .vid = vid, .prio = vprio };
	}
	if (!nprobes || wait_s < 1)
		usage();

	fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		fprintf(stderr, "%s: %s\n", ifname, strerror(errno));
		return 1;
	}
	sll.sll_ifindex = ifr.ifr_ifindex;
	sll.sll_protocol = htons(ETH_P_ALL);
	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0 ||
	    bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0 ||
	    setsockopt(fd, SOL_PACKET, PACKET_AUXDATA, &one, sizeof(one)) < 0) {
		fprintf(stderr, "%s: %s\n", ifname, strerror(errno));
		return 1;
	}
	memcpy(mac, ifr.ifr_hwaddr.sa_data, ETH_ALEN);

	offload_pause();
	for (i = 0; i < nprobes; i++) {
		size_t len;

		len = build_padi(frame, probes[i].vid, probes[i].prio);
		if (send(fd, frame, len, 0) < 0)
			fprintf(stderr, "VID %d PCP %d: %s\n", probes[i].vid,
				probes[i].prio, strerror(errno));
		len = build_discover(frame, probes[i].vid, probes[i].prio);
		if (send(fd, frame, len, 0) < 0)
			fprintf(stderr, "VID %d PCP %d: %s\n", probes[i].vid,
				probes[i].prio, strerror(errno));
		/* Pace a long list, and collect replies while sending it */
		if (nprobes > 16)
			receive(fd, 2);
	}
	receive(fd, wait_s * 1000);
	close(fd);
	offload_restore();

	if (json) {
		printf("[");
		for (i = 0; i < nresults; i++) {
			const char *c;

			printf("%s{\"vid\":%d,\"prio\":%d,\"reply_prio\":%d,\"type\":\"%s\",\"detail\":\"",
			       i ? "," : "", results[i].vid, results[i].prio,
			       results[i].reply_prio,
			       results[i].dhcp ? "dhcp" : "pppoe");
			for (c = results[i].detail; *c; c++)
				if (*c >= 0x20 && *c != '"' && *c != '\\')
					putchar(*c);
			printf("\"}");
		}
		printf("]\n");
		return 0;
	}

	if (!nresults) {
		printf("No PPPoE or DHCP server answered on %d probe%s.\n", nprobes,
		       nprobes == 1 ? "" : "s");
		return 1;
	}
	printf("%-10s %-5s %-5s %-6s %s\n", "VLAN", "sent", "reply", "type", "detail");
	for (i = 0; i < nresults; i++) {
		char vid[12];

		if (results[i].vid)
			snprintf(vid, sizeof(vid), "%d", results[i].vid);
		else
			snprintf(vid, sizeof(vid), "untagged");
		printf("%-10s %-5d %-5d %-6s %s\n", vid, results[i].prio,
		       results[i].reply_prio,
		       results[i].dhcp ? "DHCP" : "PPPoE", results[i].detail);
	}
	return 0;
}
