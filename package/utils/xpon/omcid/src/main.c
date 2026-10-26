// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "daemon.h"

static volatile sig_atomic_t stopping, trace_toggle;
static void stop_signal(int signal) { (void)signal; stopping = 1; }
static void trace_signal(int signal) { (void)signal; trace_toggle = 1; }

struct initial_config {
	u8 value[OMCI_CONFIG_MAX + 1][OMCI_MAX_CONFIG_VALUE];
	u8 len[OMCI_CONFIG_MAX + 1], source[OMCI_CONFIG_MAX + 1];
};

static int kernel_status(struct omcid *ctx, struct omcid_message *reply)
{
	struct omcid_request q;

	omcid_request_init(&q, ctx->transport.family, OMCI_CMD_GET);
	omcid_put(&q, OMCI_ATTR_DEV_ID, &ctx->transport.device, sizeof(ctx->transport.device));
	return omcid_rpc(&ctx->transport, &q, true, reply);
}

static int import_status(struct omcid *ctx, const struct nlmsghdr *n, bool initial)
{
	const struct nlattr *tb[OMCI_ATTR_MAX + 1];
	struct omci_device *d = &ctx->engine;
	u32 generation, flags;
	bool gen_changed, channel_changed, new_channel_up;
	int ret = omcid_parse(n, tb, OMCI_ATTR_MAX);

	if (ret || omcid_scalar(tb[OMCI_ATTR_GENERATION], &generation, sizeof(generation)) ||
	    omcid_scalar(tb[OMCI_ATTR_FLAGS], &flags, sizeof(flags)))
		return ret ? ret : -EBADMSG;
	gen_changed = ctx->transport.generation != generation;
	ctx->transport.generation = generation;
#define READ_STATUS(attr, field) do { \
	if (tb[OMCI_ATTR_##attr] && omcid_scalar(tb[OMCI_ATTR_##attr], &d->field, sizeof(d->field))) \
		return -EBADMSG; \
} while (0)
	READ_STATUS(IFINDEX, ifindex);
	READ_STATUS(ONU_ID, onu_id);
	READ_STATUS(GEM_PORT_ID, gem_port_id);
	READ_STATUS(STATE, state);
	READ_STATUS(CAPABILITIES, capabilities);
	READ_STATUS(OWNER_PORTID, owner_portid);
#undef READ_STATUS
	new_channel_up = !!(flags & OMCI_F_CHANNEL_UP);
	channel_changed = d->channel_up != new_channel_up;
	d->channel_up = new_channel_up;
	if (!initial && (gen_changed || channel_changed)) {
		omcid_olt_watch_link(&ctx->olt, d->channel_up, omci_native_now());
		ctx->have_rx_sequence = false;
		omci_agent_channel_changed(d, d->channel_up);
		return omcid_checkpoint_save(ctx);
	}
	return 0;
}

static int fetch_config(struct omcid *ctx, struct initial_config *config)
{
	u16 key;

	memset(config, 0, sizeof(*config));
	for (key = 1; key <= OMCI_CONFIG_MAX; key++) {
		struct omcid_request q;
		struct omcid_message reply;
		const struct nlattr *tb[OMCI_ATTR_MAX + 1];
		int ret;

		if (key == OMCI_CONFIG_AGENT_DYING_GASP)
			continue;
		omcid_request_init(&q, ctx->transport.family, OMCI_CMD_CONFIG_GET);
		omcid_put(&q, OMCI_ATTR_DEV_ID, &ctx->transport.device, sizeof(ctx->transport.device));
		omcid_put(&q, OMCI_ATTR_CONFIG_KEY, &key, sizeof(key));
		ret = omcid_rpc(&ctx->transport, &q, true, &reply);
		if (ret)
			return ret;
		ret = omcid_parse((void *)reply.data, tb, OMCI_ATTR_MAX);
		if (ret || !tb[OMCI_ATTR_CONFIG_VALUE] ||
		    omcid_length(tb[OMCI_ATTR_CONFIG_VALUE]) > OMCI_MAX_CONFIG_VALUE ||
		    omcid_scalar(tb[OMCI_ATTR_CONFIG_SOURCE], &config->source[key], 1))
			return ret ? ret : -EBADMSG;
		config->len[key] = omcid_length(tb[OMCI_ATTR_CONFIG_VALUE]);
		memcpy(config->value[key], omcid_data(tb[OMCI_ATTR_CONFIG_VALUE]), config->len[key]);
	}
	return 0;
}

static int apply_config(struct omcid *ctx, const struct initial_config *config)
{
	u16 key;
	int ret;

	/* Force/profile are applied last, after identity and the UNI model. */
	for (key = 1; key <= OMCI_CONFIG_MAX; key++) {
		if (key == OMCI_CONFIG_AGENT_DYING_GASP || key == OMCI_CONFIG_OLT_PROFILE ||
		    key == OMCI_CONFIG_OLT_PROFILE_FORCE)
			continue;
		ret = omci_agent_config_set_source(&ctx->engine, key, config->value[key],
						  config->len[key], config->source[key]);
		if (ret || ctx->error)
			goto fail;
	}
	for (key = OMCI_CONFIG_OLT_PROFILE; key <= OMCI_CONFIG_OLT_PROFILE_FORCE; key++) {
		ret = omci_agent_config_set_source(&ctx->engine, key, config->value[key],
						  config->len[key], config->source[key]);
		if (ret || ctx->error)
			goto fail;
	}
	return 0;
fail:
	fprintf(stderr, "omcid: kernel config key %u (%u bytes, source %u) rejected\n",
		key, config->len[key], config->source[key]);
	return ret ? ret : ctx->error;
}

static int listener_open(const char *path)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct stat st;
	int fd, ret;

	if (strlen(path) >= sizeof(addr.sun_path))
		return -ENAMETOOLONG;
	strcpy(addr.sun_path, path);
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -errno;
	if (!lstat(path, &st)) {
		int probe;

		if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) {
			ret = -EPERM;
			goto fail;
		}
		probe = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
		if (probe < 0) {
			ret = -errno;
			goto fail;
		}
		ret = connect(probe, (void *)&addr, sizeof(addr));
		ret = ret ? -errno : -EADDRINUSE;
		close(probe);
		if (ret != -ECONNREFUSED)
			goto fail;
		if (unlink(path)) {
			ret = -errno;
			goto fail;
		}
	} else if (errno != ENOENT) {
		ret = -errno;
		goto fail;
	}
	if (bind(fd, (void *)&addr, sizeof(addr))) {
		ret = -errno;
		goto fail;
	}
	if (chmod(path, 0600) || listen(fd, 8)) {
		ret = -errno;
		unlink(path);
		goto fail;
	}
	return fd;
fail:
	close(fd);
	return ret;
}

static void control_client(struct omcid *ctx, int listener)
{
	struct omcid_message request, reply;
	struct ucred peer;
	struct timeval timeout = { .tv_usec = 200000 };
	socklen_t size = sizeof(peer);
	struct nlmsghdr *n = (void *)request.data;
	int fd, ret;
	ssize_t len;

	fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
	if (fd < 0)
		return;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid != 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)))
		goto out;
	len = recv(fd, request.data, sizeof(request.data), MSG_TRUNC);
	if (len < (ssize_t)(NLMSG_HDRLEN + GENL_HDRLEN) || len > (ssize_t)sizeof(request.data) ||
	    n->nlmsg_len != (size_t)len)
		goto out;
	request.len = len;
	ret = omcid_control(ctx, n, &reply);
	if (ret) {
		struct nlmsghdr *error_header = (void *)reply.data;
		struct nlmsgerr *error = (void *)(reply.data + NLMSG_HDRLEN);

		memset(&reply, 0, sizeof(reply));
		reply.len = NLMSG_LENGTH(sizeof(*error));
		error_header->nlmsg_len = reply.len;
		error_header->nlmsg_seq = n->nlmsg_seq;
		error_header->nlmsg_type = NLMSG_ERROR;
		error->error = ret;
		error->msg = *n;
	}
	(void)send(fd, reply.data, reply.len, MSG_NOSIGNAL);
out:
	close(fd);
}

static int handle_record(struct omcid *ctx, const struct nlmsghdr *n)
{
	const struct genlmsghdr *g = NLMSG_DATA(n);
	const struct nlattr *tb[OMCI_ATTR_MAX + 1];
	u32 generation, flags = 0;
	u64 sequence;
	u8 event;
	int ret;

	if (n->nlmsg_type != ctx->transport.family || n->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN ||
	    g->version != OMCI_GENL_VERSION)
		return -EPROTO;
	ret = omcid_parse(n, tb, OMCI_ATTR_MAX);
	if (ret || omcid_scalar(tb[OMCI_ATTR_GENERATION], &generation, sizeof(generation)))
		return ret ? ret : -EBADMSG;
	if (g->cmd == OMCI_CMD_EVENT) {
		if ((s32)(generation - ctx->transport.generation) < 0)
			return 0;
		if (omcid_scalar(tb[OMCI_ATTR_EVENT], &event, sizeof(event)))
			return -EBADMSG;
		ret = import_status(ctx, n, false);
		if (ret)
			return ret;
		if (event == OMCI_EVENT_RESOURCE_CHANGE && ctx->engine.channel_up) {
			ret = omci_device_reconcile_services(&ctx->engine);
			if (!ret && ctx->engine.agent.enabled)
				ret = omci_device_resume_retained(&ctx->engine);
			return ret == -ENODATA ? 0 : ret;
		}
		return 0;
	}
	if (g->cmd != OMCI_CMD_RX)
		return -EPROTO;
	if (generation != ctx->transport.generation) {
		if ((s32)(generation - ctx->transport.generation) > 0 && ctx->transport.fd > 0) {
			struct omcid_message reply;

			ret = kernel_status(ctx, &reply);
			if (ret)
				return ret;
			ret = import_status(ctx, (void *)reply.data, false);
			if (ret)
				return ret;
		}
		if (generation != ctx->transport.generation || !ctx->engine.channel_up)
			return 0;
	}
	if (!ctx->engine.channel_up)
		return 0;
	if (omcid_scalar(tb[OMCI_ATTR_SEQUENCE], &sequence, sizeof(sequence)) ||
	    !tb[OMCI_ATTR_PDU] || omcid_length(tb[OMCI_ATTR_PDU]) > OMCI_MAX_PDU_LEN)
		return -EBADMSG;
	/* Loss means the daemon cannot certify its authoritative transaction state. */
	if (ctx->have_rx_sequence && sequence != ctx->last_rx_sequence + 1)
		return -ENOBUFS;
	ctx->have_rx_sequence = true;
	ctx->last_rx_sequence = sequence;
	if (tb[OMCI_ATTR_FLAGS] && omcid_scalar(tb[OMCI_ATTR_FLAGS], &flags, sizeof(flags)))
		return -EBADMSG;
	omcid_trace(ctx, "rx", sequence, flags, omcid_data(tb[OMCI_ATTR_PDU]),
		    omcid_length(tb[OMCI_ATTR_PDU]));
	omcid_olt_watch_rx(&ctx->olt, omcid_data(tb[OMCI_ATTR_PDU]),
			   omcid_length(tb[OMCI_ATTR_PDU]), omci_native_now());
	ret = omci_native_receive(&ctx->engine, omcid_data(tb[OMCI_ATTR_PDU]),
				 omcid_length(tb[OMCI_ATTR_PDU]));
	if (ret == -EINVAL || ret == -EMSGSIZE || ret == -EPROTO) {
		ctx->engine.rx_dropped++;
		return 0;
	}
	return ret;
}

static int recover_status(struct omcid *ctx, const struct nlmsghdr *n)
{
	u32 previous = ctx->transport.generation;
	int ret;

	/* Never retry an old OLT transaction against a newly ranged session. */
	ctx->error = 0;
	ret = import_status(ctx, n, false);
	if (ret || ctx->error)
		return ret ? ret : ctx->error;
	if (ctx->transport.generation == previous) {
		/* A prior attempt may have imported this generation before replay failed. */
		ctx->have_rx_sequence = false;
		omci_agent_channel_changed(&ctx->engine, ctx->engine.channel_up);
		if (ctx->error)
			return ctx->error;
	}
	ret = omcid_retry_identity(ctx);
	if (ret)
		return ret;
	if (ctx->engine.channel_up && ctx->engine.agent.enabled) {
		ret = omci_device_resume_retained(&ctx->engine);
		if (ret && ret != -ENODATA)
			return ret;
		if (ctx->error)
			return ctx->error;
	}
	return omcid_checkpoint_save(ctx);
}

static int recover_session(struct omcid *ctx)
{
	struct omcid_message reply;
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < 4; attempt++) {
		ret = kernel_status(ctx, &reply);
		if (ret)
			return ret;
		ret = recover_status(ctx, (void *)reply.data);
		if (ret != -ESTALE)
			return ret;
	}
	return -ESTALE;
}

static int loop(struct omcid *ctx, int listener)
{
	while (!stopping) {
		struct pollfd fds[2] = { { .fd = ctx->transport.fd, .events = POLLIN },
			{ .fd = listener, .events = POLLIN } };
		struct delayed_work *timers[] = { &ctx->engine.agent.alarm_work,
			&ctx->engine.agent.pm_work, &ctx->engine.agent.reboot_work };
		int timeout = -1, ret;
		unsigned int i;
		long left;

		if (ctx->error == -ESTALE) {
			ret = recover_session(ctx);
			if (ret)
				return ret;
		}
		if (ctx->error)
			break;
		if (trace_toggle) {
			trace_toggle = 0;
			omcid_trace_toggle(ctx);
		}
		omci_native_timers(&ctx->engine);
		if (ctx->error)
			continue;
		for (i = 0; i < ARRAY_SIZE(timers); i++) {
			if (!timers[i]->pending)
				continue;
			left = (long)(timers[i]->due - jiffies);
			if (left < 0)
				left = 0;
			if (timeout < 0 || left < timeout)
				timeout = left;
		}
		left = omcid_olt_watch_poll(&ctx->olt, omci_native_now(),
					    ctx->engine.agent.mib_sync, stdout);
		if (left >= 0 && (timeout < 0 || left < timeout))
			timeout = left;
		/* Never while a request waits: that is what the deferral is for. */
		left = ctx->transport.count ? -1 :
		       omcid_checkpoint_poll(ctx, omci_native_now());
		if (ctx->error)
			continue;
		if (left >= 0 && (timeout < 0 || left < timeout))
			timeout = left;
		if (ctx->transport.count)
			timeout = 0;
		ret = poll(fds, ARRAY_SIZE(fds), timeout);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ctx->transport.count || (fds[0].revents & POLLIN)) {
			struct omcid_message message;
			struct nlmsghdr *n;
			int left;

			ret = omcid_transport_next(&ctx->transport, &message, 0);
			if (ret)
				return ret;
			left = message.len;
			for (n = (void *)message.data; NLMSG_OK(n, left); n = NLMSG_NEXT(n, left)) {
				ret = handle_record(ctx, n);
				if (ret == -ESTALE || (!ret && ctx->error == -ESTALE)) {
					ctx->error = -ESTALE;
					ret = recover_session(ctx);
				}
				if (ret)
					return ret;
			}
			if (left)
				return -EBADMSG;
		}
		if (!ctx->transport.count && !ctx->error && (fds[1].revents & POLLIN))
			control_client(ctx, listener);
		if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP | POLLNVAL))
			return -EIO;
	}
	return ctx->error;
}

/* A missing attribute (another PON driver) is not an error. */
static void pon_write(const char *attr, const char *value)
{
	char path[96];
	int fd;

	snprintf(path, sizeof(path), "/sys/class/net/pon0/device/%s", attr);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	if (write(fd, value, strlen(value)) < 0)
		fprintf(stderr, "omcid: cannot write %s to %s\n", value, path);
	close(fd);
}

static void reboot_request(struct omci_device *odev)
{
	struct omcid *ctx = odev->priv;

	if (omcid_checkpoint_save(ctx)) {
		ctx->error = -EIO;
		return;
	}
	execl("/sbin/reboot", "reboot", (char *)NULL);
	ctx->error = -errno;
}

static int checkpoint_lock(const char *path)
{
	char lock[256];
	struct stat st;
	int fd;

	if (strncmp(path, "/tmp/", 5) || strstr(path, "/../") ||
	    snprintf(lock, sizeof(lock), "%s.lock", path) >= (int)sizeof(lock))
		return -EINVAL;
	fd = open(lock, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & 0777) != 0600 || flock(fd, LOCK_EX | LOCK_NB)) {
		close(fd);
		return -EBUSY;
	}
	return fd;
}


int main(int argc, char **argv)
{
	struct omcid ctx = { .transport.fd = -1, .restoring = true };
	struct initial_config config;
	struct omcid_message status;
	const struct nlattr *tb[OMCI_ATTR_MAX + 1];
	const struct omci_device_ops bootstrap_ops = { .get_ani_topology = omcid_get_topology };
	const char *path = "/var/run/omcid.sock";
	char state[128];
	u32 device = 0, flags = OMCI_BIND_F_AGENT;
	u8 mode, enabled;
	int opt, ret, listener = -1, lock = -1;
	bool initialized = false, fresh = false, trace = false, release = false;
	const char *stage = "lock";

	while ((opt = getopt(argc, argv, "d:s:p:trh")) != -1) {
		switch (opt) {
		case 'd': {
			char *end;
			unsigned long id;

			errno = 0;
			id = strtoul(optarg, &end, 10);
			if (errno || *end || id > UINT32_MAX)
				return 2;
			device = id;
			break;
		}
		case 's':
			ctx.checkpoint = optarg;
			break;
		case 'p':
			path = optarg;
			break;
		case 't':
			trace = true;
			break;
		case 'r':
			release = true;
			break;
		default:
			fprintf(stderr, "Usage: omcid [-d device] [-s /tmp/state] [-p control-socket] [-t] [-r]\n");
			return opt == 'h' ? 0 : 2;
		}
	}
	if (geteuid() || optind != argc)
		return 2;
	umask(077);
	snprintf(state, sizeof(state), "/tmp/omcid-%u.state", device);
	if (!ctx.checkpoint)
		ctx.checkpoint = state;
	lock = checkpoint_lock(ctx.checkpoint);
	if (lock < 0) {
		ret = lock;
		goto out;
	}
	/* -t: record from the first PDU, so a boot's provisioning is in the trace. */
	if (trace)
		omcid_trace_toggle(&ctx);
	stage = "transport";
	ret = omcid_transport_open(&ctx.transport, device);
	if (ret)
		goto out;
	stage = "status";
	ret = kernel_status(&ctx, &status);
	if (ret)
		goto out;
	ret = omcid_parse((void *)status.data, tb, OMCI_ATTR_MAX);
	if (ret || omcid_scalar(tb[OMCI_ATTR_RESPONDER_MODE], &mode, sizeof(mode)) ||
	    omcid_scalar(tb[OMCI_ATTR_AGENT_ENABLED], &enabled, sizeof(enabled))) {
		ret = ret ? ret : -EPROTO;
		goto out;
	}
	ret = import_status(&ctx, (void *)status.data, true);
	if (ret || !(ctx.engine.capabilities & OMCI_CAP_BACKEND)) {
		ret = ret ? ret : -EOPNOTSUPP;
		goto out;
	}
	if (mode == OMCI_RESPONDER_NONE && !access(ctx.checkpoint, F_OK)) {
		ret = -EEXIST;
		goto out;
	}
	fresh = mode == OMCI_RESPONDER_NONE;
	stage = "bind";
	{
		struct omcid_request q;

		omcid_request_init(&q, ctx.transport.family, OMCI_CMD_BIND);
		omcid_put(&q, OMCI_ATTR_DEV_ID, &device, sizeof(device));
		omcid_put(&q, OMCI_ATTR_BIND_FLAGS, &flags, sizeof(flags));
		omcid_put(&q, OMCI_ATTR_GENERATION, &ctx.transport.generation, sizeof(ctx.transport.generation));
		ret = omcid_rpc(&ctx.transport, &q, false, NULL);
		if (ret)
			goto out;
	}
	stage = "fetch_config";
	ret = fetch_config(&ctx, &config);
	if (ret)
		goto out;
	ret = kernel_status(&ctx, &status);
	if (!ret)
		ret = import_status(&ctx, (void *)status.data, true);
	if (ret)
		goto out;
	/* Handoff disables the old responder, not the requested daemon setting. */
	config.value[OMCI_CONFIG_AGENT_ENABLED][0] = enabled;
	ctx.identity.valid = OMCI_IDENTITY_F_SERIAL_NUMBER | OMCI_IDENTITY_F_VENDOR_ID |
		OMCI_IDENTITY_F_PASSWORD | OMCI_IDENTITY_F_VERSION | OMCI_IDENTITY_F_EQUIPMENT_ID;
	memcpy(ctx.identity.serial_number, config.value[OMCI_CONFIG_SERIAL_NUMBER], 8);
	memcpy(ctx.identity.vendor_id, config.value[OMCI_CONFIG_VENDOR_ID], 4);
	memcpy(ctx.identity.password, config.value[OMCI_CONFIG_PASSWORD], 10);
	memcpy(ctx.identity.version, config.value[OMCI_CONFIG_HARDWARE_VERSION], OMCI_HARDWARE_VERSION_LEN);
	memcpy(ctx.identity.equipment_id, config.value[OMCI_CONFIG_EQUIPMENT_ID], OMCI_OLT_EQUIPMENT_ID_LEN);
	ctx.parent.identity = &ctx.identity;
	ctx.parent.onu_type = config.value[OMCI_CONFIG_ONU_TYPE][0];
	ctx.parent.uni_count = config.value[OMCI_CONFIG_UNI_COUNT][0];
	ctx.parent.reboot = reboot_request;
	ctx.engine.parent = &ctx.parent;
	ctx.engine.priv = &ctx;
	ctx.engine.ops = &bootstrap_ops;
	ctx.engine.started = true;
	stage = "engine_init";
	ret = omci_agent_init(&ctx.engine);
	if (ret)
		goto out;
	initialized = true;
	if (!fresh) {
		stage = "checkpoint_load";
		ret = omcid_checkpoint_load(&ctx);
		/*
		 * No checkpoint at all: the previous owner died before its first
		 * save, so nothing was acknowledged to the OLT. Start a fresh MIB,
		 * as a reboot would; a damaged or mismatched one still fails.
		 */
		if (ret == -ENOENT) {
			fprintf(stderr, "omcid: no checkpoint from the previous owner, starting a fresh MIB\n");
			fresh = true;
			ret = 0;
		}
		if (ret)
			goto out;
	}
	ctx.engine.ops = &omcid_backend_ops;
	stage = "backend_reset";
	ret = omcid_backend(&ctx, OMCI_BACKEND_RESET, NULL, 0, NULL, 0);
	if (ret)
		goto out;
	stage = "set_uni";
	ret = ctx.engine.ops->set_uni(&ctx.engine, ctx.uni_entity,
				      ctx.uni_known ? ctx.uni_enabled : true);
	if (ret)
		goto out;
	stage = "apply_config";
	if (fresh)
		ret = apply_config(&ctx, &config);
	if (ret)
		goto out;
	stage = "resume_retained";
	if (ctx.engine.channel_up) {
		ret = omci_device_resume_retained(&ctx.engine);
		if (ret && ret != -ENODATA && ret != -EOPNOTSUPP)
			goto out;
		mod_delayed_work(NULL, &ctx.engine.agent.alarm_work, HZ);
	}
	ctx.restoring = false;
	stage = "checkpoint_save";
	ret = omcid_checkpoint_save(&ctx);
	if (ret)
		goto out;
	stage = "listener";
	listener = listener_open(path);
	if (listener < 0) {
		ret = listener;
		goto out;
	}
	signal(SIGTERM, stop_signal);
	signal(SIGINT, stop_signal);
	signal(SIGUSR1, trace_signal);
	setvbuf(stdout, NULL, _IOLBF, 0);
	fprintf(stderr, "omcid: exclusive userspace owner ready, device %u generation %u\n",
		device, ctx.transport.generation);
	/* -r: the identity came from the kernel's config, which gpon_provision
	 * filled from /etc/config/omci; range now rather than when rpcd loads. */
	if (release) {
		pon_write("gpon_identity_wait", "0");
		pon_write("allow_tx", "1");
	}
	stage = "loop";
	ret = loop(&ctx, listener);
	if (!ret)
		ret = omcid_checkpoint_save(&ctx);
out:
	if (listener >= 0) {
		close(listener);
		unlink(path);
	}
	if (ctx.trace)
		fclose(ctx.trace);
	/* Closing the owner leaves the kernel's userspace latch set. */
	omcid_transport_close(&ctx.transport);
	if (initialized) {
		ctx.engine.ops = &bootstrap_ops;
		omci_agent_cleanup(&ctx.engine);
	}
	if (lock >= 0)
		close(lock);
	if (ret)
		fprintf(stderr, "omcid: stopped at %s without kernel fallback: %s (%d)\n",
			stage, strerror(-ret), ret);
	return ret ? 1 : 0;
}
