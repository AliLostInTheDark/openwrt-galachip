// SPDX-License-Identifier: GPL-2.0-only
/*
 * A minimal client of the xpon generic netlink family, for the daemons
 * and the ucode module: one request on a fresh socket per call.
 */

#include <errno.h>
#include <net/if.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/xpon.h>

#include "xpon-nl.h"

#define XPON_NL_BUF	2048

struct xpon_nl_msg {
	struct nlmsghdr nlh;
	struct genlmsghdr genl;
	char attrs[512];
};

static void put_attr(struct xpon_nl_msg *m, int type, const void *data, int len)
{
	struct nlattr *a = (struct nlattr *)((char *)m + NLMSG_ALIGN(m->nlh.nlmsg_len));

	a->nla_type = type;
	a->nla_len = NLA_HDRLEN + len;
	memcpy((char *)a + NLA_HDRLEN, data, len);
	m->nlh.nlmsg_len = NLMSG_ALIGN(m->nlh.nlmsg_len) + NLA_ALIGN(a->nla_len);
}

static int attr_len(int type)
{
	switch (type) {
	case XPON_ATTR_IFINDEX:
	case XPON_ATTR_TX_HOLD_SECONDS:
	case XPON_ATTR_EPON_GENERATION:
	case XPON_ATTR_EPON_KEY:
	case XPON_ATTR_EPON_HOLDOVER_MS:
	case XPON_ATTR_EPON_REGISTERED:
		return 4;
	case XPON_ATTR_SFU_UNTAG_VLAN:
		return 2;
	case XPON_ATTR_EPON_MAC:
		return 6;
	default:
		return 1;
	}
}

static int exchange(int fd, struct xpon_nl_msg *m, char *buf)
{
	struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
	ssize_t n;

	/* Only a SET is acked: a GET's reply or error is its only answer. */
	m->nlh.nlmsg_flags = NLM_F_REQUEST |
			     (m->genl.cmd == XPON_CMD_SET && m->nlh.nlmsg_type != GENL_ID_CTRL ?
			      NLM_F_ACK : 0);
	if (send(fd, m, m->nlh.nlmsg_len, 0) < 0)
		return -errno;
	for (;;) {
		n = recv(fd, buf, XPON_NL_BUF, 0);
		if (n < 0)
			return -errno;
		for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, n); nlh = NLMSG_NEXT(nlh, n)) {
			if (nlh->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = NLMSG_DATA(nlh);

				return e->error;
			}
			/* A reply comes before the ack: keep it for the caller. */
			if (nlh->nlmsg_type >= NLMSG_MIN_TYPE) {
				if (nlh != (struct nlmsghdr *)buf)
					memmove(buf, nlh, nlh->nlmsg_len);
				return 1;
			}
		}
	}
}

static const struct nlattr *find_attr(const char *buf, int type)
{
	const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
	const struct nlattr *a = (const struct nlattr *)
		((const char *)NLMSG_DATA(nlh) + GENL_HDRLEN);
	int left = nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);

	for (; left >= NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && a->nla_len <= left;
	     left -= NLA_ALIGN(a->nla_len),
	     a = (const struct nlattr *)((const char *)a + NLA_ALIGN(a->nla_len)))
		if ((a->nla_type & NLA_TYPE_MASK) == type)
			return a;
	return NULL;
}

static const struct nlattr *nla_next(const struct nlattr *a, int *left)
{
	*left -= NLA_ALIGN(a->nla_len);
	return (const struct nlattr *)((const char *)a + NLA_ALIGN(a->nla_len));
}

#define nla_for_each(a, start, len, left) \
	for ((a) = (start), (left) = (len); \
	     (left) >= NLA_HDRLEN && (a)->nla_len >= NLA_HDRLEN && (a)->nla_len <= (left); \
	     (a) = nla_next((a), &(left)))

static const void *nla_payload(const struct nlattr *a)
{
	return (const char *)a + NLA_HDRLEN;
}

/* The family's id and, with @group, its "events" multicast group. */
static int family_id(int fd, char *buf, uint32_t *group)
{
	struct xpon_nl_msg m = {
		.nlh = { .nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN), .nlmsg_type = GENL_ID_CTRL },
		.genl = { .cmd = CTRL_CMD_GETFAMILY, .version = 1 },
	};
	const struct nlattr *a, *g, *f;
	int ret, gl, fl;

	put_attr(&m, CTRL_ATTR_FAMILY_NAME, XPON_GENL_NAME, sizeof(XPON_GENL_NAME));
	ret = exchange(fd, &m, buf);
	if (ret <= 0)
		return ret ? ret : -ENOENT;
	if (group) {
		a = find_attr(buf, CTRL_ATTR_MCAST_GROUPS);
		if (!a)
			return -ENOENT;
		*group = 0;
		nla_for_each(g, (const struct nlattr *)nla_payload(a), a->nla_len - NLA_HDRLEN, gl) {
			uint32_t id = 0;
			int named = 0;

			nla_for_each(f, (const struct nlattr *)nla_payload(g), g->nla_len - NLA_HDRLEN, fl) {
				if (f->nla_type == CTRL_ATTR_MCAST_GRP_ID)
					id = *(const uint32_t *)nla_payload(f);
				else if (f->nla_type == CTRL_ATTR_MCAST_GRP_NAME)
					named = !strcmp(nla_payload(f), "events");
			}
			if (named)
				*group = id;
		}
		if (!*group)
			return -ENOENT;
	}
	a = find_attr(buf, CTRL_ATTR_FAMILY_ID);
	return a ? *(const uint16_t *)nla_payload(a) : -ENOENT;
}

static int open_socket(void)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);

	if (fd < 0)
		return -errno;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int ret = -errno;

		close(fd);
		return ret;
	}
	return fd;
}

static void parse_state(const char *buf, struct xpon_nl_state *st)
{
	const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
	const struct nlattr *a;
	int left;

	memset(st, 0, sizeof(*st));
	nla_for_each(a, (const struct nlattr *)((const char *)NLMSG_DATA(nlh) + GENL_HDRLEN),
		     (int)(nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN)), left) {
		int type = a->nla_type & NLA_TYPE_MASK, len = a->nla_len - NLA_HDRLEN;
		const void *p = nla_payload(a);
		uint8_t u8 = len >= 1 ? *(const uint8_t *)p : 0;
		uint32_t u32 = len >= 4 ? *(const uint32_t *)p : 0;

		if (type >= 64)
			continue;
		switch (type) {
		case XPON_ATTR_MODE: st->mode = u8; break;
		case XPON_ATTR_REGISTRATION: st->registration = u8; break;
		case XPON_ATTR_CARRIER: st->carrier = u8; break;
		case XPON_ATTR_SIGNAL_DETECT: st->signal_detect = u8; break;
		case XPON_ATTR_LOS: st->los = u8; break;
		case XPON_ATTR_TX_ALLOW: st->tx_allow = u8; break;
		case XPON_ATTR_TX_HOLD: st->tx_hold = u8; break;
		case XPON_ATTR_TX_HOLD_SECONDS: st->tx_hold_seconds = u32; break;
		case XPON_ATTR_IDENTITY_WAIT: st->identity_wait = u8; break;
		case XPON_ATTR_EPON_OAM_LINK: st->epon_oam_link = u8; break;
		case XPON_ATTR_EPON_REGISTER: st->epon_register = u8; break;
		case XPON_ATTR_EPON_SYNC: st->epon_sync = u8; break;
		case XPON_ATTR_EPON_NACK_SILENT: st->epon_nack_silent = u8; break;
		case XPON_ATTR_EPON_HOLDOVER_MS: st->epon_holdover_ms = u32; break;
		case XPON_ATTR_EPON_GENERATION: st->epon_generation = u32; break;
		case XPON_ATTR_EPON_REGISTERED: st->epon_registered = u32; break;
		case XPON_ATTR_SFU_OFFLOAD: st->sfu_offload = u8; break;
		case XPON_ATTR_SFU_UNTAG_VLAN:
			st->sfu_untag_vlan = len >= 2 ? *(const uint16_t *)p : 0;
			break;
		case XPON_ATTR_EPON_MAC:
			if (len != sizeof(st->epon_mac))
				continue;
			memcpy(st->epon_mac, p, len);
			break;
		case XPON_ATTR_MODULE_DIAG:
			if (len != sizeof(st->module_diag))
				continue;
			memcpy(st->module_diag, p, len);
			break;
		case XPON_ATTR_MODULE_THRESHOLDS:
			if (len != sizeof(st->module_thresholds))
				continue;
			memcpy(st->module_thresholds, p, len);
			break;
		case XPON_ATTR_SFU_VLAN_MAP:
			if (len % sizeof(struct xpon_vlan_map) ||
			    len > (int)sizeof(st->sfu_vlan_map))
				continue;
			memcpy(st->sfu_vlan_map, p, len);
			st->sfu_vlan_maps = len / sizeof(struct xpon_vlan_map);
			break;
		default:
			continue;
		}
		st->present |= 1ULL << type;
	}
}

/* One request: a GET's reply into @st, or a SET of @attrs. */
static int request(const char *ifname, int cmd, const struct xpon_nl_attr *attrs, int n,
		   struct xpon_nl_state *st)
{
	struct xpon_nl_msg m = {
		.nlh = { .nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN) },
		.genl = { .cmd = cmd, .version = XPON_GENL_VERSION },
	};
	uint32_t ifindex = if_nametoindex(ifname);
	char buf[XPON_NL_BUF];
	int fd, id, ret, i;

	if (!ifindex)
		return -ENODEV;
	fd = open_socket();
	if (fd < 0)
		return fd;
	id = family_id(fd, buf, NULL);
	if (id < 0) {
		ret = id;
		goto out;
	}
	m.nlh.nlmsg_type = id;
	put_attr(&m, XPON_ATTR_IFINDEX, &ifindex, 4);
	for (i = 0; i < n; i++) {
		uint8_t v8 = attrs[i].value;
		uint16_t v16 = attrs[i].value;
		int l = attrs[i].data ? attrs[i].len : attr_len(attrs[i].type);

		put_attr(&m, attrs[i].type,
			 attrs[i].data ? attrs[i].data : l == 1 ? (const void *)&v8 :
			 l == 2 ? (const void *)&v16 : (const void *)&attrs[i].value, l);
	}
	ret = exchange(fd, &m, buf);
	if (ret > 0) {
		if (st)
			parse_state(buf, st);
		ret = 0;
	}
out:
	close(fd);
	return ret;
}

int xpon_nl_query(const char *ifname, struct xpon_nl_state *st)
{
	return request(ifname, XPON_CMD_GET, NULL, 0, st);
}

int xpon_nl_get(const char *ifname, int attr, uint32_t *value)
{
	struct xpon_nl_state st;
	int ret = xpon_nl_query(ifname, &st);

	if (ret)
		return ret;
	if (!XPON_NL_HAS(&st, attr))
		return -EOPNOTSUPP;
	switch (attr) {
	case XPON_ATTR_TX_ALLOW: *value = st.tx_allow; break;
	case XPON_ATTR_TX_HOLD: *value = st.tx_hold; break;
	case XPON_ATTR_TX_HOLD_SECONDS: *value = st.tx_hold_seconds; break;
	case XPON_ATTR_IDENTITY_WAIT: *value = st.identity_wait; break;
	case XPON_ATTR_EPON_OAM_LINK: *value = st.epon_oam_link; break;
	case XPON_ATTR_EPON_REGISTER: *value = st.epon_register; break;
	case XPON_ATTR_EPON_HOLDOVER_MS: *value = st.epon_holdover_ms; break;
	case XPON_ATTR_SFU_OFFLOAD: *value = st.sfu_offload; break;
	case XPON_ATTR_SFU_UNTAG_VLAN: *value = st.sfu_untag_vlan; break;
	default: return -EINVAL;
	}
	return 0;
}

int xpon_nl_get_raw(const char *ifname, int attr, void *data, int len)
{
	struct xpon_nl_state st;
	int ret = xpon_nl_query(ifname, &st);

	if (ret)
		return ret;
	if (attr != XPON_ATTR_EPON_MAC || len != sizeof(st.epon_mac))
		return -EINVAL;
	if (!XPON_NL_HAS(&st, attr))
		return -EOPNOTSUPP;
	memcpy(data, st.epon_mac, len);
	return 0;
}

int xpon_nl_setv(const char *ifname, const struct xpon_nl_attr *attrs, int n)
{
	return request(ifname, XPON_CMD_SET, attrs, n, NULL);
}

int xpon_nl_set(const char *ifname, int attr, uint32_t value)
{
	struct xpon_nl_attr a = { .type = attr, .value = value };

	return xpon_nl_setv(ifname, &a, 1);
}

int xpon_nl_event_open(void)
{
	char buf[XPON_NL_BUF];
	uint32_t group;
	int fd, id;

	fd = open_socket();
	if (fd < 0)
		return fd;
	id = family_id(fd, buf, &group);
	if (id < 0) {
		close(fd);
		return id;
	}
	if (setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &group, sizeof(group)) < 0) {
		id = -errno;
		close(fd);
		return id;
	}
	return fd;
}

void xpon_nl_event_drain(int fd)
{
	char buf[XPON_NL_BUF];

	while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0)
		;
}
