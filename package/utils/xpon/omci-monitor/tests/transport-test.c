// SPDX-License-Identifier: GPL-2.0-only
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>

static int fake_socket(int domain, int type, int protocol);
static int fake_bind(int fd, const struct sockaddr *addr, socklen_t size);
static int fake_setsockopt(int fd, int level, int name, const void *data, socklen_t size);
static ssize_t fake_sendto(int fd, const void *data, size_t size, int flags,
			   const struct sockaddr *dst, socklen_t dst_size);
static ssize_t fake_recvmsg(int fd, struct msghdr *msg, int flags);
static int fake_close(int fd);

#define socket fake_socket
#define bind fake_bind
#define setsockopt fake_setsockopt
#define sendto fake_sendto
#define recvmsg fake_recvmsg
#define close fake_close
#include "../src/netlink.c"
#undef socket
#undef bind
#undef setsockopt
#undef sendto
#undef recvmsg
#undef close

static unsigned int version, sends, closed, checks;
static bool truncated, foreign, empty, bad_tail;
#define CHECK(x) do { assert(x); checks++; } while (0)

static int fake_socket(int domain, int type, int protocol)
{
	CHECK(domain == AF_NETLINK && protocol == NETLINK_GENERIC);
	CHECK(type & SOCK_CLOEXEC);
	return 42;
}

static int fake_bind(int fd, const struct sockaddr *addr, socklen_t size)
{
	CHECK(fd == 42 && addr->sa_family == AF_NETLINK && size == sizeof(struct sockaddr_nl));
	return 0;
}

static int fake_setsockopt(int fd, int level, int name, const void *data, socklen_t size)
{
	const struct timeval *tv = data;

	CHECK(fd == 42 && level == SOL_SOCKET && name == SO_RCVTIMEO);
	CHECK(size == sizeof(*tv) && tv->tv_sec == 2);
	return 0;
}

static ssize_t fake_sendto(int fd, const void *data, size_t size, int flags,
			   const struct sockaddr *dst, socklen_t dst_size)
{
	const struct nlmsghdr *n = data;
	const struct genlmsghdr *g = NLMSG_DATA(n);
	const struct nlattr *a[OMCI_ATTR_MAX + 1];

	CHECK(fd == 42 && !flags && dst->sa_family == AF_NETLINK && dst_size == sizeof(struct sockaddr_nl));
	sends++;
	CHECK(g->cmd != OMCI_CMD_TX || n->nlmsg_type == GENL_ID_CTRL);
	if (n->nlmsg_type != GENL_ID_CTRL) {
		CHECK(g->version == OMCI_GENL_VERSION);
		CHECK(!attributes(n, a, OMCI_ATTR_MAX));
		if (g->cmd == OMCI_CMD_BIND) {
			uint32_t value = 0;

			CHECK(!scalar(a[OMCI_ATTR_BIND_FLAGS], &value, sizeof(value)));
			CHECK(value == OMCI_BIND_F_READ_ONLY);
		}
	}
	return size;
}

static ssize_t fake_recvmsg(int fd, struct msghdr *msg, int flags)
{
	struct sockaddr_nl *src = msg->msg_name;
	struct request q = { .n = { .nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN,
		.nlmsg_type = GENL_ID_CTRL, .nlmsg_seq = 1 }, .g.cmd = CTRL_CMD_NEWFAMILY };
	struct {
		struct nlmsghdr n;
		struct nlmsgerr error;
	} ack = { .n = { .nlmsg_len = NLMSG_LENGTH(sizeof(struct nlmsgerr)),
		.nlmsg_seq = 1, .nlmsg_type = NLMSG_ERROR } };
	uint16_t family = 33;
	size_t len;

	CHECK(fd == 42 && !flags);
	src->nl_family = AF_NETLINK;
	src->nl_pid = foreign ? 123 : 0;
	msg->msg_namelen = sizeof(*src);
	msg->msg_flags = truncated ? MSG_TRUNC : 0;
	if (empty)
		return 0;
	put_attr(&q, CTRL_ATTR_FAMILY_ID, &family, sizeof(family));
	put_attr(&q, CTRL_ATTR_VERSION, &version, sizeof(version));
	len = q.n.nlmsg_len + ack.n.nlmsg_len;
	assert(msg->msg_iov[0].iov_len > len);
	memcpy(msg->msg_iov[0].iov_base, &q, q.n.nlmsg_len);
	memcpy((char *)msg->msg_iov[0].iov_base + q.n.nlmsg_len, &ack, ack.n.nlmsg_len);
	if (bad_tail)
		((char *)msg->msg_iov[0].iov_base)[len++] = 0;
	return len;
}

static int fake_close(int fd)
{
	CHECK(fd == 42);
	closed++;
	return 0;
}

int main(void)
{
	struct monitor_nl ctx;
	unsigned char buf[4096];

	version = OMCI_GENL_VERSION;
	CHECK(!monitor_open(&ctx, 0) && ctx.family == 33 && sends == 3 && !closed);
	fake_close(ctx.fd);
	version = OMCI_GENL_VERSION - 1;
	sends = 0;
	CHECK(monitor_open(&ctx, 0) == -EPROTONOSUPPORT && sends == 1 && closed == 2);
	version = OMCI_GENL_VERSION;
	bad_tail = true;
	CHECK(monitor_open(&ctx, 0) == -EBADMSG);
	bad_tail = false;
	empty = true;
	CHECK(monitor_open(&ctx, 0) == -EIO);
	empty = false;
	ctx.fd = 42;
	foreign = true;
	CHECK(monitor_receive(&ctx, buf, sizeof(buf)) == -EPROTO);
	foreign = false;
	truncated = true;
	CHECK(monitor_receive(&ctx, buf, sizeof(buf)) == -EMSGSIZE);
	printf("%u native netlink lifecycle checks passed\n", checks);
	return 0;
}
