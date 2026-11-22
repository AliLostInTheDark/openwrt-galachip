/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OMCID_DAEMON_H
#define OMCID_DAEMON_H
#include "native.h"
#include "transport.h"
#include <linux/omci-backend.h>

#define OMCID_OLT_ISSUE_MAX	32

struct omcid_olt_issue {
	u16 class_id;
	u16 inst;
	u8 msg_type;
	u8 result;
	u16 count;
};

struct omcid_olt_watch {
	unsigned long down_ms, up_ms, last_ms, outage_ms;
	unsigned int messages;
	u16 count[32];
	struct omcid_olt_issue issues[OMCID_OLT_ISSUE_MAX];
	u8 issue_count;
	u16 issue_overflow;
	bool active, down, had_outage;
};

struct omcid {
	struct omcid_transport transport;
	struct omci_device engine;
	struct device parent;
	struct omci_identity identity;
	struct omci_backend_identity pending_identity;
	uint64_t last_rx_sequence;
	bool have_rx_sequence, restoring;
	bool uni_known, uni_enabled;
	u16 uni_entity;
	int error;
	const char *checkpoint;
	/* A deferred checkpoint: when it is due, and when it first was. */
	unsigned long save_due, save_first;
	FILE *trace;
	struct omcid_olt_watch olt;
};
extern const struct omci_device_ops omcid_backend_ops;
int omcid_get_topology(struct omci_device *odev, struct omci_ani_topology *t);
int omcid_backend_put(struct omcid_request *q, const struct omci_backend_fields *fields,
		      const void *data);
int omcid_backend_get(const struct nlattr *nest, const struct omci_backend_fields *fields,
		      void *data);
int omcid_backend(struct omcid *ctx, u16 op, const void *data, size_t len,
		  void *out, size_t out_len);
int omcid_checkpoint_save(struct omcid *ctx);
void omcid_checkpoint_defer(struct omcid *ctx);
long omcid_checkpoint_poll(struct omcid *ctx, unsigned long now);
int omcid_checkpoint_load(struct omcid *ctx);
int omcid_retry_identity(struct omcid *ctx);
void omcid_trace_toggle(struct omcid *ctx);
void omcid_olt_watch_link(struct omcid_olt_watch *w, bool up, unsigned long now);
void omcid_olt_watch_rx(struct omcid_olt_watch *w, const u8 *pdu, size_t len,
			unsigned long now);
void omcid_olt_watch_tx(struct omcid_olt_watch *w, const u8 *pdu, size_t len);
long omcid_olt_watch_poll(struct omcid_olt_watch *w, unsigned long now,
			  u16 mib_sync, FILE *out, bool kernel_log, bool force);
void omcid_trace(struct omcid *ctx, const char *direction, u64 sequence, u32 flags,
		 const void *pdu, size_t len);
int omcid_control(struct omcid *ctx, const struct nlmsghdr *request,
		  struct omcid_message *reply);
#endif
