// SPDX-License-Identifier: GPL-2.0-only
/* Read-only transport; intentionally has no TX or provisioning entry point. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "netlink.h"

static int attributes(const struct nlmsghdr *nlh, const struct nlattr **tb,
		      unsigned int max)
{
	size_t left = nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
	const unsigned char *p = (const void *)NLMSG_DATA(nlh) + GENL_HDRLEN;

	memset(tb, 0, (max + 1) * sizeof(*tb));
	while (left) {
		const struct nlattr *a = (const void *)p;
		size_t step;
		unsigned int type;

		if (left < NLA_HDRLEN || a->nla_len < NLA_HDRLEN || a->nla_len > left)
			return -EBADMSG;
		step = NLA_ALIGN(a->nla_len);
		if (step > left)
			return -EBADMSG;
		type = a->nla_type & NLA_TYPE_MASK;
		/* Multiple alignment pads are normal in status replies with u64s. */
		if (type <= max && !(max == OMCI_ATTR_MAX && type == OMCI_ATTR_PAD)) {
			if (tb[type])
				return -EBADMSG;
			tb[type] = a;
		}
		left -= step;
		p += step;
	}
	return 0;
}

static int scalar(const struct nlattr *a, void *out, size_t size)
{
	if (!a)
		return 0;
	if (a->nla_len != NLA_HDRLEN + size)
		return -EBADMSG;
	memcpy(out, (const char *)a + NLA_HDRLEN, size);
	return 0;
}

int monitor_parse(const struct nlmsghdr *nlh, struct monitor_record *r)
{
	const struct nlattr *a[OMCI_ATTR_MAX + 1];
	const struct genlmsghdr *g;
	int ret;

	if (nlh->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN)
		return -EBADMSG;
	g = NLMSG_DATA(nlh);
	memset(r, 0, sizeof(*r));
	r->command = g->cmd;
	ret = attributes(nlh, a, OMCI_ATTR_MAX);
	if (ret)
		return ret;
	if (!a[OMCI_ATTR_GENERATION])
		return -EPROTO;
#define READ_ATTR(name, field) \
	if (scalar(a[OMCI_ATTR_##name], &r->field, sizeof(r->field))) \
		return -EBADMSG
	READ_ATTR(GENERATION, generation);
	READ_ATTR(FLAGS, flags);
	READ_ATTR(CAPABILITIES, capabilities);
	READ_ATTR(SEQUENCE, sequence);
	READ_ATTR(EVENT, event);
	READ_ATTR(TRACE_DROPPED, trace_dropped);
#undef READ_ATTR
	if (a[OMCI_ATTR_PDU]) {
		r->pdu = (const char *)a[OMCI_ATTR_PDU] + NLA_HDRLEN;
		r->pdu_len = a[OMCI_ATTR_PDU]->nla_len - NLA_HDRLEN;
		if (!r->pdu_len || r->pdu_len > OMCI_MAX_PDU_LEN)
			return -EMSGSIZE;
	}
	if ((r->command == OMCI_CMD_RX || r->command == OMCI_CMD_TRACE_TX) && !r->pdu)
		return -EBADMSG;
	if (r->command == OMCI_CMD_RX && !a[OMCI_ATTR_SEQUENCE])
		return -EBADMSG;
	return 0;
}

struct request {
	struct nlmsghdr n;
	struct genlmsghdr g;
	unsigned char attrs[128];
};

static void put_attr(struct request *q, uint16_t type, const void *data, size_t len)
{
	struct nlattr *a = (void *)q + q->n.nlmsg_len;

	a->nla_type = type;
	a->nla_len = NLA_HDRLEN + len;
	memcpy((char *)a + NLA_HDRLEN, data, len);
	q->n.nlmsg_len += NLA_ALIGN(a->nla_len);
}

static int send_request(struct monitor_nl *ctx, uint16_t family, uint8_t cmd,
			bool bind)
{
	struct sockaddr_nl dst = { .nl_family = AF_NETLINK };
	struct request q = { .n = { .nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN,
		.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK, .nlmsg_type = family },
		.g = { .cmd = cmd, .version = OMCI_GENL_VERSION } };
	uint32_t flags = OMCI_BIND_F_READ_ONLY;

	q.n.nlmsg_seq = ++ctx->sequence;
	if (family == GENL_ID_CTRL) {
		q.g.version = 1;
		put_attr(&q, CTRL_ATTR_FAMILY_NAME, OMCI_GENL_NAME, sizeof(OMCI_GENL_NAME));
	} else {
		put_attr(&q, OMCI_ATTR_DEV_ID, &ctx->device, sizeof(ctx->device));
		if (bind)
			put_attr(&q, OMCI_ATTR_BIND_FLAGS, &flags, sizeof(flags));
	}
	if (sendto(ctx->fd, &q, q.n.nlmsg_len, 0, (void *)&dst, sizeof(dst)) < 0)
		return -errno;
	return 0;
}

int monitor_receive(struct monitor_nl *ctx, void *buf, size_t size)
{
	struct sockaddr_nl src = {};
	struct iovec iov = { .iov_base = buf, .iov_len = size };
	struct msghdr msg = { .msg_name = &src, .msg_namelen = sizeof(src),
		.msg_iov = &iov, .msg_iovlen = 1 };
	ssize_t n = recvmsg(ctx->fd, &msg, 0);

	if (n < 0)
		return -errno;
	if (msg.msg_flags & MSG_TRUNC)
		return -EMSGSIZE;
	if (msg.msg_namelen != sizeof(src) || src.nl_family != AF_NETLINK || src.nl_pid)
		return -EPROTO;
	return n;
}

int monitor_status(struct monitor_nl *ctx)
{
	return send_request(ctx, ctx->family, OMCI_CMD_GET, false);
}

int monitor_open(struct monitor_nl *ctx, uint32_t device)
{
	struct sockaddr_nl local = { .nl_family = AF_NETLINK };
	struct timeval timeout = { .tv_sec = 2 };
	unsigned char buf[16384] __attribute__((aligned(8)));
	bool found = false;
	int ret, remaining;

	memset(ctx, 0, sizeof(*ctx));
	ctx->device = device;
	ctx->fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (ctx->fd < 0)
		return -errno;
	if (bind(ctx->fd, (void *)&local, sizeof(local)) ||
	    setsockopt(ctx->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) {
		ret = -errno;
		goto fail;
	}
	ret = send_request(ctx, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, false);
	if (ret)
		goto fail;
	/* Consume both response and ACK before subscribing to asynchronous records. */
	while (!found || !ctx->family) {
		struct nlmsghdr *n;

		remaining = monitor_receive(ctx, buf, sizeof(buf));
		if (remaining <= 0) {
			ret = remaining ? remaining : -EIO;
			goto fail;
		}
		for (n = (void *)buf; NLMSG_OK(n, remaining); n = NLMSG_NEXT(n, remaining)) {
			if (n->nlmsg_seq != ctx->sequence)
				continue;
			if (n->nlmsg_type == NLMSG_ERROR) {
				const struct nlmsgerr *e = NLMSG_DATA(n);

				if (n->nlmsg_len < NLMSG_LENGTH(sizeof(*e))) {
					ret = -EBADMSG;
					goto fail;
				}
				if (e->error) {
					ret = e->error;
					goto fail;
				}
				found = true;
			} else if (n->nlmsg_type == GENL_ID_CTRL) {
				const struct nlattr *a[CTRL_ATTR_MAX + 1];
				uint32_t version = 0;

				if (n->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN ||
				    attributes(n, a, CTRL_ATTR_MAX) ||
				    !a[CTRL_ATTR_FAMILY_ID] || !a[CTRL_ATTR_VERSION] ||
				    scalar(a[CTRL_ATTR_FAMILY_ID], &ctx->family, sizeof(ctx->family)) ||
				    scalar(a[CTRL_ATTR_VERSION], &version, sizeof(version)) ||
				    version != OMCI_GENL_VERSION) {
					ret = -EPROTONOSUPPORT;
					goto fail;
				}
			}
		}
		if (remaining) {
			ret = -EBADMSG;
			goto fail;
		}
	}
	if (!ctx->family) {
		ret = -EPROTO;
		goto fail;
	}
	ret = send_request(ctx, ctx->family, OMCI_CMD_BIND, true);
	if (!ret)
		ret = monitor_status(ctx);
	if (!ret)
		return ret;
fail:
	close(ctx->fd);
	ctx->fd = -1;
	return ret;
}
