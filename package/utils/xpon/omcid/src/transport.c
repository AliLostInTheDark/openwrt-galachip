// SPDX-License-Identifier: GPL-2.0-only
/* One owner socket; RPC replies and asynchronous OMCI records are demultiplexed. */
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "transport.h"

static uint64_t monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void omcid_request_init(struct omcid_request *q, uint16_t family, uint8_t command)
{
	memset(q, 0, sizeof(*q));
	q->header.nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN;
	q->header.nlmsg_type = family;
	q->header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	q->generic.cmd = command;
	q->generic.version = family == GENL_ID_CTRL ? 1 : OMCI_GENL_VERSION;
}

int omcid_put(struct omcid_request *q, uint16_t type, const void *data, size_t len)
{
	struct nlattr *a;
	size_t step;

	if (len > UINT16_MAX - NLA_HDRLEN || (!data && len))
		return -EINVAL;
	step = NLA_ALIGN(NLA_HDRLEN + len);
	if (step > sizeof(*q) - q->header.nlmsg_len)
		return -EMSGSIZE;
	a = (void *)((char *)q + q->header.nlmsg_len);
	a->nla_type = type;
	a->nla_len = NLA_HDRLEN + len;
	if (len)
		memcpy((char *)a + NLA_HDRLEN, data, len);
	q->header.nlmsg_len += step;
	return 0;
}

int omcid_parse(const struct nlmsghdr *n, const struct nlattr **tb, unsigned int max)
{
	size_t left;
	const char *p;

	if (n->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN || n->nlmsg_len > OMCID_MESSAGE_MAX)
		return -EBADMSG;
	left = n->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
	p = (const char *)NLMSG_DATA(n) + GENL_HDRLEN;
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
		if (type <= max && !(max == OMCI_ATTR_MAX && type == OMCI_ATTR_PAD)) {
			if (tb[type])
				return -EBADMSG;
			tb[type] = a;
		}
		p += step;
		left -= step;
	}
	return 0;
}

int omcid_scalar(const struct nlattr *a, void *value, size_t size)
{
	if (!a || omcid_length(a) != size)
		return -EBADMSG;
	memcpy(value, omcid_data(a), size);
	return 0;
}

static int receive(struct omcid_transport *t, struct omcid_message *out, int timeout)
{
	struct pollfd pfd = { .fd = t->fd, .events = POLLIN };
	struct sockaddr_nl src = {};
	struct iovec iov = { .iov_base = out->data, .iov_len = sizeof(out->data) };
	struct msghdr msg = { .msg_name = &src, .msg_namelen = sizeof(src),
		.msg_iov = &iov, .msg_iovlen = 1 };
	ssize_t n;
	int ret = poll(&pfd, 1, timeout);

	if (ret <= 0)
		return ret ? -errno : -ETIMEDOUT;
	n = recvmsg(t->fd, &msg, 0);
	if (n < 0)
		return -errno;
	if ((msg.msg_flags & MSG_TRUNC) || n > (ssize_t)sizeof(out->data))
		return -EMSGSIZE;
	if (src.nl_pid || src.nl_family != AF_NETLINK || msg.msg_namelen != sizeof(src))
		return -EPROTO;
	out->len = n;
	return 0;
}

static int enqueue(struct omcid_transport *t, const struct nlmsghdr *n)
{
	struct omcid_message *message;
	unsigned int index;

	if (t->count == OMCID_BACKLOG_MAX)
		return -ENOBUFS;
	message = malloc(sizeof(*message));
	if (!message)
		return -ENOMEM;
	message->len = n->nlmsg_len;
	memcpy(message->data, n, message->len);
	index = (t->head + t->count) % OMCID_BACKLOG_MAX;
	t->pending[index] = message;
	t->count++;
	return 0;
}

int omcid_rpc(struct omcid_transport *t, struct omcid_request *q, bool reply,
	      struct omcid_message *out)
{
	struct sockaddr_nl dst = { .nl_family = AF_NETLINK };
	uint64_t deadline = monotonic_ms() + 2000;
	bool got_reply = !reply, ack = false;
	uint8_t expected = q->header.nlmsg_type == GENL_ID_CTRL ? CTRL_CMD_NEWFAMILY : q->generic.cmd;
	ssize_t sent;

	q->header.nlmsg_seq = ++t->sequence;
	sent = sendto(t->fd, q, q->header.nlmsg_len, 0, (void *)&dst, sizeof(dst));
	if (sent < 0)
		return -errno;
	if (sent != q->header.nlmsg_len)
		return -EIO;
	while (!ack || !got_reply) {
		struct omcid_message message;
		struct nlmsghdr *n;
		int left, ret;
		uint64_t now = monotonic_ms();

		if (now >= deadline)
			return -ETIMEDOUT;
		ret = receive(t, &message, deadline - now);
		if (ret)
			return ret;
		left = message.len;
		for (n = (void *)message.data; NLMSG_OK(n, left); n = NLMSG_NEXT(n, left)) {
			if (n->nlmsg_len > OMCID_MESSAGE_MAX)
				return -EMSGSIZE;
			if (!n->nlmsg_seq && n->nlmsg_type == t->family) {
				ret = enqueue(t, n);
				if (ret)
					return ret;
				continue;
			}
			if (n->nlmsg_seq != q->header.nlmsg_seq)
				return -EPROTO;
			if (n->nlmsg_type == NLMSG_ERROR) {
				const struct nlmsgerr *error = NLMSG_DATA(n);

				if (n->nlmsg_len < NLMSG_LENGTH(sizeof(*error)) || error->error > 0)
					return -EBADMSG;
				if (error->error)
					return error->error;
				ack = true;
			} else {
				const struct genlmsghdr *g = NLMSG_DATA(n);

				if (!reply || got_reply || n->nlmsg_type != q->header.nlmsg_type ||
				    n->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN || g->cmd != expected ||
				    (t->family && g->version != OMCI_GENL_VERSION))
					return -EPROTO;
				out->len = n->nlmsg_len;
				memcpy(out->data, n, out->len);
				got_reply = true;
			}
		}
		if (left)
			return -EBADMSG;
	}
	return 0;
}

int omcid_transport_next(struct omcid_transport *t, struct omcid_message *message, int timeout)
{
	if (t->count) {
		struct omcid_message *pending = t->pending[t->head];

		*message = *pending;
		free(pending);
		t->head = (t->head + 1) % OMCID_BACKLOG_MAX;
		t->count--;
		return 0;
	}
	return receive(t, message, timeout);
}

void omcid_transport_close(struct omcid_transport *t)
{
	if (t->fd >= 0)
		close(t->fd);
	while (t->count) {
		free(t->pending[t->head]);
		t->head = (t->head + 1) % OMCID_BACKLOG_MAX;
		t->count--;
	}
	t->fd = -1;
}

int omcid_transport_open(struct omcid_transport *t, uint32_t device)
{
	struct sockaddr_nl local = { .nl_family = AF_NETLINK };
	struct omcid_request q;
	struct omcid_message reply;
	const struct nlattr *tb[CTRL_ATTR_MAX + 1];
	uint32_t version;
	int ret;

	memset(t, 0, sizeof(*t));
	t->device = device;
	t->fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (t->fd < 0)
		return -errno;
	if (bind(t->fd, (void *)&local, sizeof(local))) {
		ret = -errno;
		goto fail;
	}
	omcid_request_init(&q, GENL_ID_CTRL, CTRL_CMD_GETFAMILY);
	omcid_put(&q, CTRL_ATTR_FAMILY_NAME, OMCI_GENL_NAME, sizeof(OMCI_GENL_NAME));
	ret = omcid_rpc(t, &q, true, &reply);
	if (ret)
		goto fail;
	ret = omcid_parse((void *)reply.data, tb, CTRL_ATTR_MAX);
	if (ret || omcid_scalar(tb[CTRL_ATTR_FAMILY_ID], &t->family, sizeof(t->family)) ||
	    omcid_scalar(tb[CTRL_ATTR_VERSION], &version, sizeof(version)) || version != OMCI_GENL_VERSION) {
		ret = ret ? ret : -EPROTONOSUPPORT;
		goto fail;
	}
	return 0;
fail:
	omcid_transport_close(t);
	return ret;
}
