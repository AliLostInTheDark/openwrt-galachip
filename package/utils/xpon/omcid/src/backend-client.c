// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "daemon.h"

#define OMCI_ACTION_TEST_RESULT 27
/* The PON lock can be held across a whole switch table rewrite: up to 3 s. */
#define OMCID_BUSY_RETRIES 1500
#define OMCID_TRACE_PATH "/tmp/omcid-trace.txt"
#define OMCID_TRACE_MAX (1024 * 1024)

/* SIGUSR1 starts or stops a bounded capture in the omci-monitor -r format. */
void omcid_trace_toggle(struct omcid *ctx)
{
	int fd;

	if (ctx->trace) {
		fclose(ctx->trace);
		ctx->trace = NULL;
		fprintf(stderr, "omcid: OMCI trace stopped\n");
		return;
	}
	unlink(OMCID_TRACE_PATH);
	fd = open(OMCID_TRACE_PATH, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd >= 0) {
		ctx->trace = fdopen(fd, "w");
		if (!ctx->trace)
			close(fd);
	}
	if (!ctx->trace) {
		fprintf(stderr, "omcid: cannot trace to %s\n", OMCID_TRACE_PATH);
		return;
	}
	fprintf(stderr, "omcid: tracing OMCI to %s (raw OMCI, keep private)\n", OMCID_TRACE_PATH);
}

void omcid_trace(struct omcid *ctx, const char *direction, u64 sequence, u32 flags,
		 const void *pdu, size_t len)
{
	const u8 *p = pdu;
	size_t i;

	if (!ctx->trace)
		return;
	fprintf(ctx->trace, "# ms %lu\n%s %u %llu %u ", omci_native_now(), direction,
		ctx->transport.generation, (unsigned long long)sequence, flags);
	for (i = 0; i < len; i++)
		fprintf(ctx->trace, "%02x", p[i]);
	fputc('\n', ctx->trace);
	/* A looping OLT must not fill the 32 MiB board's /tmp. */
	if (fflush(ctx->trace) || ftello(ctx->trace) > OMCID_TRACE_MAX)
		omcid_trace_toggle(ctx);
}

static int transport_result(struct omcid *ctx, int ret)
{
	/* A missing reply can hide a completed write. Do not acknowledge new state. */
	if (ret == -ETIMEDOUT || ret == -ENOBUFS || ret == -EPROTO ||
	    ret == -EBADMSG || ret == -EMSGSIZE || ret == -ECONNRESET || ret == -EPIPE)
		ctx->error = ret;
	return ret;
}

static int backend_call(struct omcid *ctx, u16 op, const void *data, size_t len,
			void *out, size_t out_len)
{
	struct omcid_transport *t = &ctx->transport;
	struct omcid_message reply;
	struct omcid_request request;
	const struct nlattr *tb[OMCI_ATTR_MAX + 1];
	u32 generation;
	u16 returned_op;
	unsigned int retries;
	int ret;

	if (ctx->error)
		return ctx->error;
	for (retries = 0; ; retries++) {
		omcid_request_init(&request, t->family, OMCI_CMD_BACKEND);
		omcid_put(&request, OMCI_ATTR_DEV_ID, &t->device, sizeof(t->device));
		omcid_put(&request, OMCI_ATTR_GENERATION, &t->generation, sizeof(t->generation));
		omcid_put(&request, OMCI_ATTR_BACKEND_OP, &op, sizeof(op));
		if (len && omcid_put(&request, OMCI_ATTR_BACKEND_DATA, data, len))
			return -EMSGSIZE;
		ret = omcid_rpc(t, &request, true, &reply);
		if (ret != -EBUSY || retries == OMCID_BUSY_RETRIES)
			break;
		usleep(2000);
	}
	if (ret)
		return transport_result(ctx, ret);
	ret = omcid_parse((void *)reply.data, tb, OMCI_ATTR_MAX);
	if (ret || omcid_scalar(tb[OMCI_ATTR_GENERATION], &generation, sizeof(generation)) ||
	    omcid_scalar(tb[OMCI_ATTR_BACKEND_OP], &returned_op, sizeof(returned_op)))
		return transport_result(ctx, ret ? ret : -EBADMSG);
	if (generation != t->generation || returned_op != op)
		return transport_result(ctx, -EPROTO);
	if (out_len) {
		if (!tb[OMCI_ATTR_BACKEND_DATA] || omcid_length(tb[OMCI_ATTR_BACKEND_DATA]) != out_len)
			return transport_result(ctx, -EPROTO);
		memcpy(out, omcid_data(tb[OMCI_ATTR_BACKEND_DATA]), out_len);
	} else if (tb[OMCI_ATTR_BACKEND_DATA]) {
		return transport_result(ctx, -EPROTO);
	}
	return 0;
}

/*
 * Hardware writes are rare, so a refused one is always logged with its
 * request; OMCID_DEBUG=1 also logs the ones that succeed. -EAGAIN is a
 * service waiting for its PLOAM Alloc-ID, retried on the resource change;
 * -ESTALE is a write for a session that just ended, replayed in the new one.
 */
int omcid_backend(struct omcid *ctx, u16 op, const void *data, size_t len,
		  void *out, size_t out_len)
{
	static int debug = -1;
	bool read = op == OMCI_BACKEND_TOPOLOGY || op == OMCI_BACKEND_TELEMETRY ||
		    op == OMCI_BACKEND_UNI_LINK;
	int ret = backend_call(ctx, op, data, len, out, out_len);

	if (debug < 0)
		debug = getenv("OMCID_DEBUG") && *getenv("OMCID_DEBUG") == '1';
	if (!read && ((ret && ret != -EAGAIN && ret != -ESTALE) || debug)) {
		const u8 *p = data;
		size_t i;

		fprintf(stderr, "omcid: backend op %u -> %d:", op, ret);
		for (i = 0; i < len && i < 48; i++)
			fprintf(stderr, " %02x", p[i]);
		fputc('\n', stderr);
	}
	return ret;
}

static int topology(struct omci_device *odev, struct omci_ani_topology *t)
{
	struct omci_backend_topology result;
	int ret = omcid_backend(odev->priv, OMCI_BACKEND_TOPOLOGY, NULL, 0, &result, sizeof(result));

	if (ret)
		return ret;
	*t = (struct omci_ani_topology) {
		.tcont_base = result.tcont_base, .scheduler_base = result.scheduler_base,
		.queue_base = result.queue_base, .maximum_queue_size = result.maximum_queue_size,
		.allocated_queue_size = result.allocated_queue_size, .tcont_count = result.tcont_count,
		.queues_per_tcont = result.queues_per_tcont,
		.queue_config_option = result.queue_config_option, .scheduler_policy = result.scheduler_policy,
	};
	return 0;
}

static int tcont(struct omci_device *odev, u16 entity, u16 alloc, bool valid)
{
	struct omci_backend_tcont q = { entity, alloc, valid };

	return omcid_backend(odev->priv, OMCI_BACKEND_TCONT, &q, sizeof(q), NULL, 0);
}

static int gem(struct omci_device *odev, u16 entity, u16 port, u16 tcont,
	       u8 direction, bool valid, bool encrypted)
{
	struct omci_backend_gem q = { entity, port, tcont, direction, valid, encrypted };

	return omcid_backend(odev->priv, OMCI_BACKEND_GEM, &q, sizeof(q), NULL, 0);
}

static int qos(struct omci_device *odev, u16 entity, const struct omci_tcont_qos *value)
{
	struct omci_backend_qos q = { .entity = entity, .enabled = value->enabled,
		.policy = value->policy };

	memcpy(q.weight, value->weight, sizeof(q.weight));
	return omcid_backend(odev->priv, OMCI_BACKEND_TCONT_QOS, &q, sizeof(q), NULL, 0);
}

static int rate(struct omci_device *odev, u16 port, bool upstream, u32 rate, u32 burst)
{
	struct omci_backend_rate q = { port, upstream, rate, burst };

	return omcid_backend(odev->priv, OMCI_BACKEND_GEM_RATE, &q, sizeof(q), NULL, 0);
}

static int uni(struct omci_device *odev, u16 entity, bool enabled)
{
	struct omci_backend_uni q = { entity, enabled };
	struct omcid *ctx = odev->priv;
	int ret = omcid_backend(ctx, OMCI_BACKEND_UNI, &q, sizeof(q), NULL, 0);

	if (!ret) {
		ctx->uni_known = true;
		ctx->uni_enabled = enabled;
		ctx->uni_entity = entity;
	}
	return ret;
}

static int attrs(struct omci_device *odev, u16 entity, const struct omci_uni_attrs *a)
{
	struct omci_backend_uni_attrs q = { entity, a->pause_quanta, a->ageing_s,
		a->learn_limit, a->down_kbps, a->up_kbps, a->auto_detection, a->max_frame_size };

	return omcid_backend(odev->priv, OMCI_BACKEND_UNI_ATTRS, &q, sizeof(q), NULL, 0);
}

static int service(struct omci_device *odev, const struct omci_service_config *s)
{
	struct omci_backend_service q = { .cookie = s->cookie, .uni_entity = s->uni_entity_id,
		.gem_entity = s->gem_ctp_entity_id, .gem_port = s->gem_port_id,
		.tcont_entity = s->tcont_entity_id, .alloc_id = s->alloc_id, .vlan = s->vlan_id,
		.pcp = s->pcp, .queue = s->queue, .direction = s->direction,
		.uni_mode = s->uni_vlan_mode, .uni_vlan = s->uni_vlan_id,
		.multicast_ani = s->multicast_ani_entity_id };

	if (s->vlan_valid) q.flags |= OMCI_BACKEND_SVC_VLAN;
	if (s->pcp_valid) q.flags |= OMCI_BACKEND_SVC_PCP;
	if (s->vlan_treatment_valid) q.flags |= OMCI_BACKEND_SVC_TREATMENT;
	if (s->multicast_ani_valid) q.flags |= OMCI_BACKEND_SVC_MULTICAST_ANI;
	if (s->multicast) q.flags |= OMCI_BACKEND_SVC_MULTICAST;
	if (s->default_service) q.flags |= OMCI_BACKEND_SVC_DEFAULT;
	memcpy(q.treatment, s->vlan_treatment, sizeof(q.treatment));
	return omcid_backend(odev->priv, OMCI_BACKEND_SERVICE, &q, sizeof(q), NULL, 0);
}

static int service_delete(struct omci_device *odev, u32 cookie)
{
	struct omci_backend_cookie q = { cookie };

	return omcid_backend(odev->priv, OMCI_BACKEND_SERVICE_DELETE, &q, sizeof(q), NULL, 0);
}

static int telemetry(struct omci_device *odev, struct omci_telemetry *t)
{
	struct omci_backend_telemetry out;
	int ret = omcid_backend(odev->priv, OMCI_BACKEND_TELEMETRY, NULL, 0, &out, sizeof(out));

	if (ret)
		return ret;
	*t = (struct omci_telemetry) { .valid = out.valid, .bosa_temperature_mc = out.temperature_mc,
		.bosa_voltage_uv = out.voltage_uv, .bosa_bias_ua = out.bias_ua,
		.bosa_tx_power_nw = out.tx_power_nw, .bosa_rx_power_nw = out.rx_power_nw,
		.bosa_alarms = out.alarms, .downstream_fec = out.downstream_fec,
		.upstream_fec = out.upstream_fec, .bip_errors = out.bip_errors,
		.fec_corrected_bytes = out.fec[0], .fec_corrected_codewords = out.fec[1],
		.fec_uncorrectable_codewords = out.fec[2], .fec_total_codewords = out.fec[3],
		.fec_seconds = out.fec[4] };
	return 0;
}

static int uni_link(struct omci_device *odev, u16 entity, bool *up)
{
	struct omci_backend_link q = { .entity = entity }, out;
	int ret = omcid_backend(odev->priv, OMCI_BACKEND_UNI_LINK, &q, sizeof(q), &out, sizeof(out));

	if (!ret)
		*up = out.up;
	return ret;
}

static void operational(struct omci_device *odev, bool operational)
{
	struct omci_backend_operational q = { operational };
	struct omcid *ctx = odev->priv;
	int ret = omcid_backend(ctx, OMCI_BACKEND_OPERATIONAL, &q, sizeof(q), NULL, 0);

	if (ret && (ret != -ESTALE || operational))
		ctx->error = ret;
}

static void config_changed(struct omci_device *odev, u16 key, const struct omci_identity *id)
{
	struct omcid *ctx = odev->priv;
	struct omci_backend_identity q = {};
	int ret;

	if (key == OMCI_CONFIG_SERIAL_NUMBER || key == OMCI_CONFIG_VENDOR_ID) {
		q.valid |= OMCI_BACKEND_ID_SERIAL;
		memcpy(q.serial, id->serial_number, sizeof(q.serial));
	} else if (key == OMCI_CONFIG_PASSWORD) {
		q.valid |= OMCI_BACKEND_ID_PASSWORD;
		memcpy(q.password, id->password, sizeof(q.password));
	} else {
		return;
	}
	ret = omcid_backend(ctx, OMCI_BACKEND_IDENTITY, &q, sizeof(q), NULL, 0);
	if (ret == -ESTALE) {
		ctx->pending_identity.valid |= q.valid;
		if (q.valid & OMCI_BACKEND_ID_SERIAL)
			memcpy(ctx->pending_identity.serial, q.serial, sizeof(q.serial));
		if (q.valid & OMCI_BACKEND_ID_PASSWORD)
			memcpy(ctx->pending_identity.password, q.password, sizeof(q.password));
	}
	if (ret)
		ctx->error = ret;
	else {
		if (q.valid & OMCI_BACKEND_ID_SERIAL)
			memcpy(ctx->identity.serial_number, q.serial, sizeof(q.serial));
		if (q.valid & OMCI_BACKEND_ID_PASSWORD)
			memcpy(ctx->identity.password, q.password, sizeof(q.password));
	}
}

int omcid_retry_identity(struct omcid *ctx)
{
	struct omci_backend_identity *q = &ctx->pending_identity;
	int ret;

	if (!q->valid)
		return 0;
	ret = omcid_backend(ctx, OMCI_BACKEND_IDENTITY, q, sizeof(*q), NULL, 0);
	if (ret)
		return ret;
	if (q->valid & OMCI_BACKEND_ID_SERIAL)
		memcpy(ctx->identity.serial_number, q->serial, sizeof(q->serial));
	if (q->valid & OMCI_BACKEND_ID_PASSWORD)
		memcpy(ctx->identity.password, q->password, sizeof(q->password));
	q->valid = 0;
	return 0;
}

static int tx(struct omci_device *odev, struct sk_buff *skb, u16 gem_port)
{
	struct omcid *ctx = odev->priv;
	struct omcid_transport *t = &ctx->transport;
	struct omcid_request q;
	unsigned int retries;
	int ret;

	if (ctx->error)
		return ctx->error;
	/* Test Result is a second packet, not the Test's cached acknowledgement. */
	if (skb->len >= 3 && (skb->data[2] & 0x20) &&
	    (skb->data[2] & 0x1f) != OMCI_ACTION_TEST_RESULT &&
	    (!odev->agent.last_response || odev->agent.last_response->len != skb->len ||
	     memcmp(odev->agent.last_response->data, skb->data, skb->len))) {
		ctx->error = -ENOMEM;
		return ctx->error;
	}
	ret = omcid_checkpoint_save(ctx);

	if (ret) {
		ctx->error = ret;
		return ret;
	}
	for (retries = 0; ; retries++) {
		omcid_request_init(&q, t->family, OMCI_CMD_TX);
		omcid_put(&q, OMCI_ATTR_DEV_ID, &t->device, sizeof(t->device));
		omcid_put(&q, OMCI_ATTR_GENERATION, &t->generation, sizeof(t->generation));
		ret = omcid_put(&q, OMCI_ATTR_PDU, skb->data, skb->len);
		if (!ret)
			ret = omcid_rpc(t, &q, false, NULL);
		if (ret != -EBUSY || retries == OMCID_BUSY_RETRIES)
			break;
		usleep(2000);
	}
	if (!ret) {
		omcid_trace(ctx, "tx", 0, 0, skb->data, skb->len);
		kfree_skb(skb);
	}
	return transport_result(ctx, ret);
}

const struct omci_device_ops omcid_backend_ops = {
	.xmit = tx, .get_ani_topology = topology, .set_tcont = tcont, .set_gem_port = gem,
	.set_tcont_qos = qos, .set_gem_rate = rate, .set_uni = uni, .set_uni_attrs = attrs,
	.replace_service = service, .delete_service = service_delete, .get_telemetry = telemetry,
	.get_uni_link = uni_link, .set_operational = operational,
	.config_changed = config_changed,
};
