/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OMCI_DAEMON_PROXY_H
#define OMCI_DAEMON_PROXY_H
#include <sys/un.h>

/* ENOENT at connect means the optional daemon is absent. Never retry a write
 * through the kernel after it could have reached the authoritative daemon.
 */
static int omci_daemon_request(const struct nlmsghdr *request, void *buffer,
			       size_t capacity, size_t *length, bool *handled)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX,
		.sun_path = "/var/run/omcid.sock" };
	struct timeval timeout = { .tv_sec = OMCI_NL_TIMEOUT_SEC };
	struct nlmsghdr *reply = buffer;
	struct ucred peer;
	socklen_t peer_len = sizeof(peer);
	ssize_t count;
	int fd, ret;

	*handled = true;
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
	    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
		ret = -errno;
		goto out;
	}
	if (connect(fd, (void *)&address, sizeof(address))) {
		ret = -errno;
		if (errno == ENOENT)
			*handled = false;
		goto out;
	}
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) ||
	    peer_len != sizeof(peer) || peer.uid != 0) {
		ret = -EPERM;
		goto out;
	}
	count = send(fd, request, request->nlmsg_len, MSG_NOSIGNAL);
	if (count < 0 || (size_t)count != request->nlmsg_len) {
		ret = count < 0 ? -errno : -EIO;
		goto out;
	}
	count = recv(fd, buffer, capacity, MSG_TRUNC);
	if (count < 0) {
		ret = -errno;
		goto out;
	}
	if (count < NLMSG_HDRLEN || (size_t)count > capacity ||
	    reply->nlmsg_len != (size_t)count || reply->nlmsg_seq != request->nlmsg_seq) {
		ret = -EBADMSG;
		goto out;
	}
	if (reply->nlmsg_type == NLMSG_ERROR) {
		struct nlmsgerr *error = NLMSG_DATA(reply);

		ret = count == NLMSG_LENGTH(sizeof(*error)) && error->error <= 0 ?
			error->error : -EBADMSG;
	} else {
		struct genlmsghdr *g = NLMSG_DATA(reply);
		const struct genlmsghdr *q = NLMSG_DATA(request);

		ret = (size_t)count >= NLMSG_HDRLEN + GENL_HDRLEN &&
			reply->nlmsg_type == request->nlmsg_type && g->cmd == q->cmd &&
			g->version == OMCI_GENL_VERSION ? 0 : -EPROTO;
	}
	if (!ret)
		*length = count;
out:
	close(fd);
	return ret;
}
#endif
