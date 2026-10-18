// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/omci.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#define OMCI_NL_TIMEOUT_SEC 2
static unsigned int scenario, checks;
static struct nlmsghdr saved;
static int mock_socket(int a, int b, int c) { return 7; }
static int mock_close(int fd) { return 0; }
static int mock_setsockopt(int fd, int level, int name, const void *data, socklen_t len) { return 0; }
static int mock_getsockopt(int fd, int level, int name, void *data, socklen_t *len)
{
	struct ucred *peer = data;

	peer->uid = scenario == 3 ? 1000 : 0;
	return 0;
}
static int mock_connect(int fd, const struct sockaddr *address, socklen_t len)
{
	if (scenario == 1 || scenario == 2) {
		errno = scenario == 1 ? ENOENT : ECONNREFUSED;
		return -1;
	}
	return 0;
}
static ssize_t mock_send(int fd, const void *data, size_t len, int flags)
{
	saved = *(const struct nlmsghdr *)data;
	return len;
}
static ssize_t mock_recv(int fd, void *data, size_t capacity, int flags)
{
	struct nlmsghdr *n = data;
	struct genlmsghdr *g = NLMSG_DATA(n);

	if (scenario == 4) {
		errno = EAGAIN;
		return -1;
	}
	memset(data, 0, capacity);
	*n = saved;
	n->nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN;
	g->cmd = OMCI_CMD_GET;
	g->version = OMCI_GENL_VERSION;
	if (scenario == 5)
		n->nlmsg_seq++;
	if (scenario == 6) {
		struct nlmsgerr *error = NLMSG_DATA(n);

		n->nlmsg_type = NLMSG_ERROR;
		n->nlmsg_len = NLMSG_LENGTH(sizeof(*error));
		error->error = -ENOENT;
	}
	return scenario == 7 ? capacity + 1 : n->nlmsg_len;
}
#define socket mock_socket
#define connect mock_connect
#define close mock_close
#define setsockopt mock_setsockopt
#define getsockopt mock_getsockopt
#define send mock_send
#define recv mock_recv
#include "../../omci-agent/src/daemon-proxy.h"
#define CHECK(v) do { assert(v); checks++; } while (0)
int main(void)
{
	const int errors[] = { 0, -ENOENT, -ECONNREFUSED, -EPERM, -EAGAIN,
		-EBADMSG, -ENOENT, -EBADMSG };
	struct { struct nlmsghdr n; struct genlmsghdr g; } request = {
		.n = { .nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN, .nlmsg_type = 37, .nlmsg_seq = 42 },
		.g = { .cmd = OMCI_CMD_GET, .version = OMCI_GENL_VERSION } };
	unsigned char buffer[256] __attribute__((aligned(8)));

	for (scenario = 0; scenario < sizeof(errors) / sizeof(*errors); scenario++) {
		size_t len = 0;
		bool handled;

		CHECK(omci_daemon_request(&request.n, buffer, sizeof(buffer), &len, &handled) == errors[scenario]);
		CHECK(handled == (scenario != 1));
	}
	printf("%u private proxy/peer/error/no-write-fallback checks passed\n", checks);
	return 0;
}
