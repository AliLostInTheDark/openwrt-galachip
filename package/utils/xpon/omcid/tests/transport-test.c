// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "transport.h"

static unsigned int checks, scenario, position, count;
static struct omcid_message packets[70];
static int mock_poll(struct pollfd *fds, nfds_t nfds, int timeout);
static ssize_t mock_sendto(int fd, const void *data, size_t len, int flags,
			   const struct sockaddr *to, socklen_t addrlen);
static ssize_t mock_recvmsg(int fd, struct msghdr *msg, int flags);
static int mock_close(int fd) { return 0; }
#define sendto mock_sendto
#define recvmsg mock_recvmsg
#define poll mock_poll
#define close mock_close
#include "../src/transport.c"
#undef sendto
#undef recvmsg
#undef poll
#undef close
#define CHECK(v) do { assert(v); checks++; } while (0)

static void add_packet(const struct omcid_request *q, bool ack, bool async)
{
	struct omcid_message *m = &packets[count++];
	struct nlmsghdr *n = (void *)m->data;

	memset(m, 0, sizeof(*m));
	n->nlmsg_type = ack ? NLMSG_ERROR : q->header.nlmsg_type;
	n->nlmsg_seq = async ? 0 : q->header.nlmsg_seq;
	m->len = ack ? NLMSG_LENGTH(sizeof(struct nlmsgerr)) : NLMSG_HDRLEN + GENL_HDRLEN;
	n->nlmsg_len = m->len;
	if (ack) {
		struct nlmsgerr *e = NLMSG_DATA(n);

		e->msg = q->header;
	} else {
		struct genlmsghdr *g = NLMSG_DATA(n);

		*g = q->generic;
		if (async)
			g->cmd = OMCI_CMD_EVENT;
	}
}

static ssize_t mock_sendto(int fd, const void *data, size_t len, int flags,
			   const struct sockaddr *to, socklen_t addrlen)
{
	const struct omcid_request *q = data;
	unsigned int i;

	position = count = 0;
	if (scenario == 7)
		return len - 1;
	if (scenario == 8)
		return len;
	if (scenario == 9) {
		for (i = 0; i <= OMCID_BACKLOG_MAX; i++)
			add_packet(q, false, true);
		return len;
	}
	add_packet(q, false, true);
	add_packet(q, scenario != 1, false);
	add_packet(q, scenario == 1, false);
	if (scenario == 2)
		((struct nlmsghdr *)packets[1].data)->nlmsg_seq++;
	if (scenario == 3)
		((struct genlmsghdr *)NLMSG_DATA((struct nlmsghdr *)packets[2].data))->version--;
	if (scenario == 4)
		((struct nlmsgerr *)NLMSG_DATA((struct nlmsghdr *)packets[1].data))->error = -EIO;
	return len;
}

static int mock_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	fds[0].revents = POLLIN;
	return position < count;
}

static ssize_t mock_recvmsg(int fd, struct msghdr *msg, int flags)
{
	struct sockaddr_nl *src = msg->msg_name;
	struct omcid_message *m = &packets[position++];

	*src = (struct sockaddr_nl) { .nl_family = AF_NETLINK };
	if (scenario == 5)
		src->nl_pid = 99;
	if (scenario == 6)
		msg->msg_flags |= MSG_TRUNC;
	memcpy(msg->msg_iov[0].iov_base, m->data, m->len);
	return m->len;
}

int main(void)
{
	const int expected[] = { 0, 0, -EPROTO, -EPROTO, -EIO, -EPROTO,
		-EMSGSIZE, -EIO, -ETIMEDOUT, -ENOBUFS };
	struct omcid_request q;
	const struct nlattr *tb[OMCI_ATTR_MAX + 1];
	for (scenario = 0; scenario < sizeof(expected) / sizeof(*expected); scenario++) {
		struct omcid_transport t = { .fd = 7, .family = 37 };
		struct omcid_message out;

		omcid_request_init(&q, t.family, OMCI_CMD_GET);
		CHECK(omcid_rpc(&t, &q, true, &out) == expected[scenario]);
		if (!expected[scenario]) {
			CHECK(t.count == 1 && !omcid_transport_next(&t, &out, 0));
			CHECK(((struct genlmsghdr *)NLMSG_DATA((struct nlmsghdr *)out.data))->cmd == OMCI_CMD_EVENT);
		}
		omcid_transport_close(&t);
		CHECK(!t.count && t.fd == -1);
	}
	omcid_request_init(&q, 37, OMCI_CMD_GET);
	CHECK(!omcid_parse(&q.header, tb, OMCI_ATTR_MAX));
	CHECK(omcid_put(&q, 1, NULL, 1) == -EINVAL);
	CHECK(omcid_put(&q, 1, "x", OMCID_MESSAGE_MAX) == -EMSGSIZE);
	CHECK(!omcid_put(&q, OMCI_ATTR_DEV_ID, "abcd", 4));
	CHECK(!omcid_parse(&q.header, tb, OMCI_ATTR_MAX));
	CHECK(omcid_scalar(tb[OMCI_ATTR_DEV_ID], &scenario, 1) == -EBADMSG);
	CHECK(!omcid_put(&q, OMCI_ATTR_DEV_ID, "abcd", 4));
	CHECK(omcid_parse(&q.header, tb, OMCI_ATTR_MAX) == -EBADMSG);
	q.header.nlmsg_len = 1;
	CHECK(omcid_parse(&q.header, tb, OMCI_ATTR_MAX) == -EBADMSG);
	printf("%u netlink ordering/sequence/loss/parser checks passed\n", checks);
	return 0;
}
