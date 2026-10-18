/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OMCI_MONITOR_NETLINK_H
#define OMCI_MONITOR_NETLINK_H

#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "include/linux/omci.h"

struct monitor_nl {
	int fd;
	uint16_t family;
	uint32_t sequence;
	uint32_t device;
};

struct monitor_record {
	uint8_t command;
	uint32_t generation;
	uint32_t flags;
	uint32_t capabilities;
	uint8_t event;
	uint64_t sequence;
	uint64_t trace_dropped;
	const void *pdu;
	size_t pdu_len;
};

int monitor_parse(const struct nlmsghdr *nlh, struct monitor_record *record);
int monitor_open(struct monitor_nl *ctx, uint32_t device);
int monitor_receive(struct monitor_nl *ctx, void *buf, size_t size);
int monitor_status(struct monitor_nl *ctx);

#endif
