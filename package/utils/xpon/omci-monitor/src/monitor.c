// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "netlink.h"
#include "wire.h"

static volatile sig_atomic_t stopped;

static void stop_monitor(int sig)
{
	(void)sig;
	stopped = 1;
}

static uint64_t monotonic_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int capture_flush(FILE *fp)
{
	off_t pos;

	if (fflush(fp))
		return -EIO;
	pos = ftello(fp);
	if (pos < 0)
		return -EIO;
	/* A looping OLT must not exhaust the 32 MiB ONU's temporary filesystem. */
	return pos > 1024 * 1024 ? -EFBIG : 0;
}

static int decode_hex(const char *s, unsigned char *out, size_t capacity)
{
	size_t n = 0;
	unsigned int byte;
	int used;

	while (*s) {
		if (*s == '\n' || *s == '\r') {
			s++;
			continue;
		}
		if (n == capacity || !s[1] ||
		    sscanf(s, "%2x%n", &byte, &used) != 1 || used != 2 ||
		    strspn(s, "0123456789abcdefABCDEF") < 2)
			return -EINVAL;
		out[n++] = byte;
		s += 2;
	}
	return n;
}

static int report_pdu(const char *direction, uint32_t generation, uint64_t sequence,
		      uint32_t flags, const void *pdu, size_t len, FILE *capture)
{
	struct omci_wire_request r;
	const unsigned char *p = pdu;
	uint64_t when = monotonic_ms();
	int ret = omci_wire_decode(pdu, len, &r);
	size_t i;

	if (capture) {
		int error;

		fprintf(capture, "# ms %" PRIu64 "\n", when);
		fprintf(capture, "%s %u %" PRIu64 " %u ", direction, generation, sequence, flags);
		for (i = 0; i < len; i++)
			fprintf(capture, "%02x", p[i]);
		fputc('\n', capture);
		error = capture_flush(capture);
		if (error)
			return error;
	}
	if (ret) {
		printf("{\"ms\":%" PRIu64 ",\"direction\":\"%s\",\"generation\":%u,"
		       "\"length\":%zu,\"decode_error\":%d}\n", when, direction, generation, len, ret);
		return ret;
	}
	printf("{\"ms\":%" PRIu64 ",\"direction\":\"%s\",\"generation\":%u,"
	       "\"sequence\":%" PRIu64 ",\"flags\":%u,\"length\":%zu,"
	       "\"tci\":%u,\"action\":%u,\"device_id\":%u,\"class\":%u,\"entity\":%u",
	       when, direction, generation, sequence, flags, len, r.transaction_id,
	       r.message_type & 31, r.device_id, r.class_id, r.entity_id);
	if (!strcmp(direction, "tx") && r.payload_len &&
	    ((r.message_type & 31) == 4 || (r.message_type & 31) == 6 ||
	     (r.message_type & 31) == 8 || (r.message_type & 31) == 9))
		printf(",\"result\":%u", r.payload[0]);
	puts("}");
	return 0;
}

static int replay(const char *path)
{
	char line[2 * OMCI_MAX_PDU_LEN + 128];
	unsigned char pdu[OMCI_MAX_PDU_LEN];
	FILE *fp = fopen(path, "r");
	unsigned int errors = 0, lines = 0;

	if (!fp)
		return -errno;
	while (fgets(line, sizeof(line), fp)) {
		char direction[3];
		uint32_t generation, flags;
		uint64_t sequence;
		int offset = 0, n;

		if (line[0] == '#')
			continue;
		lines++;
		if (!strchr(line, '\n') && !feof(fp)) {
			errors++;
			break;
		}
		if (sscanf(line, "%2s %" SCNu32 " %" SCNu64 " %" SCNu32 " %n",
			   direction, &generation, &sequence, &flags, &offset) != 4 ||
		    (strcmp(direction, "rx") && strcmp(direction, "tx"))) {
			errors++;
			continue;
		}
		n = decode_hex(line + offset, pdu, sizeof(pdu));
		if (n < 0 || report_pdu(direction, generation, sequence, flags, pdu, n, NULL))
			errors++;
	}
	if (ferror(fp))
		errors++;
	fclose(fp);
	fprintf(stderr, "replayed=%u errors=%u (framing only, no provisioning)\n", lines, errors);
	return errors ? -EBADMSG : 0;
}

static int live(uint32_t device, FILE *capture)
{
	unsigned char buf[16384] __attribute__((aligned(8)));
	struct monitor_nl ctx;
	uint64_t last_sequence = 0;
	int ret = monitor_open(&ctx, device);

	if (ret)
		return ret;
	fprintf(stderr, "Read-only observer of the kernel OMCI responder.\n");
	while (!stopped) {
		struct nlmsghdr *n;
		int remaining = monitor_receive(&ctx, buf, sizeof(buf));

		if (remaining == -EINTR || remaining == -EAGAIN || remaining == -EWOULDBLOCK)
			continue;
		if (remaining == -ENOBUFS || remaining == -EMSGSIZE) {
			puts("{\"transport_loss\":true}");
			if (capture) {
				fprintf(capture, "# loss ms %" PRIu64 "\n", monotonic_ms());
				ret = capture_flush(capture);
				if (ret)
					break;
			}
			ret = monitor_status(&ctx);
			if (ret)
				break;
			continue;
		}
		if (remaining <= 0) {
			ret = remaining ? remaining : -EIO;
			break;
		}
		for (n = (void *)buf; NLMSG_OK(n, remaining); n = NLMSG_NEXT(n, remaining)) {
			struct monitor_record r;

			if (n->nlmsg_type == NLMSG_ERROR) {
				const struct nlmsgerr *e = NLMSG_DATA(n);

				ret = n->nlmsg_len < NLMSG_LENGTH(sizeof(*e)) ? -EBADMSG : e->error;
				if (ret)
					goto out;
				continue;
			}
			if (n->nlmsg_type != ctx.family)
				continue;
			ret = monitor_parse(n, &r);
			if (ret)
				goto out;
			if (r.command == OMCI_CMD_RX || r.command == OMCI_CMD_TRACE_TX) {
				if (r.command == OMCI_CMD_RX) {
					if (last_sequence && r.sequence > last_sequence + 1)
						printf("{\"rx_gap\":%" PRIu64 "}\n", r.sequence - last_sequence - 1);
					last_sequence = r.sequence;
				}
				ret = report_pdu(r.command == OMCI_CMD_RX ? "rx" : "tx",
						 r.generation, r.sequence, r.flags, r.pdu, r.pdu_len, capture);
				if (ret == -EIO || ret == -EFBIG)
					goto out;
				ret = 0;
			} else {
				if (capture) {
					fprintf(capture, "# status generation %u event %u trace_dropped %" PRIu64 "\n",
						 r.generation, r.event, r.trace_dropped);
					ret = capture_flush(capture);
					if (ret)
						goto out;
				}
				printf("{\"generation\":%u,\"command\":%u,\"event\":%u,"
				       "\"trace_dropped\":%" PRIu64 "}\n",
				       r.generation, r.command, r.event, r.trace_dropped);
				if (r.command == OMCI_CMD_GET && !(r.capabilities & OMCI_CAP_TRACE)) {
					ret = -EOPNOTSUPP;
					goto out;
				}
			}
		}
		if (remaining) {
			ret = -EBADMSG;
			break;
		}
	}
out:
	close(ctx.fd);	/* Socket release removes the observer binding. */
	return ret;
}

int main(int argc, char **argv)
{
	const char *capture_path = NULL, *replay_path = NULL;
	uint32_t device = 0;
	FILE *capture = NULL;
	int opt, ret;

	while ((opt = getopt(argc, argv, "d:c:r:h")) != -1) {
		switch (opt) {
		case 'd': {
			char *end;
			unsigned long value;

			errno = 0;
			value = strtoul(optarg, &end, 10);
			if (errno || !*optarg || *end || value > UINT32_MAX)
				return 2;
			device = value;
			break;
		}
		case 'c': capture_path = optarg; break;
		case 'r': replay_path = optarg; break;
		default:
			fprintf(stderr, "Usage: %s [-d device] [-c NEW-private-capture] | -r capture\n", argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}
	if (optind != argc || (capture_path && replay_path))
		return 2;
	if (capture_path) {
		int fd = open(capture_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);

		if (fd < 0) {
			perror(capture_path);
			return 1;
		}
		capture = fdopen(fd, "w");
		if (!capture) {
			close(fd);
			return 1;
		}
		fputs("Capture includes raw OMCI: credentials and encryption material may be present. Keep private.\n", stderr);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, stop_monitor);
	signal(SIGTERM, stop_monitor);
	ret = replay_path ? replay(replay_path) : live(device, capture);
	if (capture)
		fclose(capture);
	if (ret)
		fprintf(stderr, "omci-monitor: %s\n", strerror(-ret));
	if (ret == -EBUSY && !replay_path)
		fputs("omcid owns the OMCI channel: kill -USR1 $(pidof omcid) starts and stops\n"
		      "/tmp/omcid-trace.txt; read it with omci-monitor -r.\n", stderr);
	return ret ? 1 : 0;
}
