// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <unistd.h>
#include "daemon.h"
#define omcid_rpc unused_netlink_rpc
#include "../src/transport.c"
#undef omcid_rpc

static unsigned int checks, writes, transmissions;
static u8 last_tx_action;
static int injected_error;
static struct omci_device hardware;
static struct omci_service_config last_service;
#define CHECK(v) do { assert(v); checks++; } while (0)

int omcid_rpc(struct omcid_transport *t, struct omcid_request *q, bool reply,
	      struct omcid_message *out)
{
	const struct nlattr *tb[OMCI_ATTR_MAX + 1];
	struct omcid_request response;
	u8 data[OMCI_BACKEND_MAX_DATA] __attribute__((aligned(8)));
	u32 generation;
	u16 op;
	size_t len;
	int ret;

	if (injected_error)
		return injected_error;
	CHECK(!omcid_parse(&q->header, tb, OMCI_ATTR_MAX));
	CHECK(!omcid_scalar(tb[OMCI_ATTR_GENERATION], &generation, sizeof(generation)));
	CHECK(generation == t->generation);
	if (q->generic.cmd == OMCI_CMD_TX) {
		CHECK(!reply && tb[OMCI_ATTR_PDU]);
		last_tx_action = ((const u8 *)omcid_data(tb[OMCI_ATTR_PDU]))[2] & 0x1f;
		transmissions++;
		return 0;
	}
	CHECK(q->generic.cmd == OMCI_CMD_BACKEND && reply);
	CHECK(!omcid_scalar(tb[OMCI_ATTR_BACKEND_OP], &op, sizeof(op)));
	ret = omci_backend_execute(&hardware, op,
		tb[OMCI_ATTR_BACKEND_DATA] ? omcid_data(tb[OMCI_ATTR_BACKEND_DATA]) : NULL,
		tb[OMCI_ATTR_BACKEND_DATA] ? omcid_length(tb[OMCI_ATTR_BACKEND_DATA]) : 0, data, &len);
	if (ret)
		return ret;
	omcid_request_init(&response, t->family, OMCI_CMD_BACKEND);
	CHECK(!omcid_put(&response, OMCI_ATTR_GENERATION, &generation, sizeof(generation)));
	CHECK(!omcid_put(&response, OMCI_ATTR_BACKEND_OP, &op, sizeof(op)));
	if (len)
		CHECK(!omcid_put(&response, OMCI_ATTR_BACKEND_DATA, data, len));
	out->len = response.header.nlmsg_len;
	memcpy(out->data, &response, out->len);
	return 0;
}

static int topology(struct omci_device *d, struct omci_ani_topology *t)
{
	*t = (struct omci_ani_topology) { .tcont_base = 0x8000, .scheduler_base = 0x8000,
		.queue_base = 0x8000, .tcont_count = 15, .queues_per_tcont = 8,
		.maximum_queue_size = 65535, .allocated_queue_size = 4,
		.queue_config_option = 1, .scheduler_policy = 1 };
	return 0;
}
static int tcont(struct omci_device *d, u16 entity, u16 alloc, bool valid) { writes++; return 0; }
static int gem(struct omci_device *d, u16 entity, u16 port, u16 tcont, u8 dir, bool valid, bool aes) { writes++; return 0; }
static int qos(struct omci_device *d, u16 entity, const struct omci_tcont_qos *q) { writes++; return 0; }
static int rate(struct omci_device *d, u16 port, bool up, u32 rate, u32 burst) { writes++; return 0; }
static int uni(struct omci_device *d, u16 entity, bool enabled) { writes++; return 0; }
static int attrs(struct omci_device *d, u16 entity, const struct omci_uni_attrs *a) { writes++; return 0; }
static int service(struct omci_device *d, const struct omci_service_config *s) { writes++; last_service = *s; return 0; }
static int remove_service(struct omci_device *d, u32 cookie) { writes++; return 0; }
static int telemetry(struct omci_device *d, struct omci_telemetry *t)
{
	*t = (struct omci_telemetry) { .bip_errors = 0x100000002ULL };
	return 0;
}
static int link_state(struct omci_device *d, u16 entity, bool *up) { *up = true; return 0; }
static int profile(struct omci_device *d, const struct omci_olt_profile_state *s) { return 0; }
static void operational(struct omci_device *d, bool up) {}
static int reset(struct omci_device *d) { writes++; return 0; }
static int identity(struct omci_device *d, const struct omci_identity *id) { writes++; return 0; }

int main(void)
{
	const struct omci_device_ops ops = { .get_ani_topology = topology,
		.set_tcont = tcont, .set_gem_port = gem, .set_tcont_qos = qos,
		.set_gem_rate = rate, .set_uni = uni, .set_uni_attrs = attrs,
		.replace_service = service, .delete_service = remove_service,
		.get_telemetry = telemetry, .get_uni_link = link_state, .set_olt_profile = profile,
		.set_operational = operational, .backend_reset = reset, .backend_identity = identity };
	const struct omci_device_ops offline = {};
	struct omcid ctx = { .transport = { .fd = -1, .family = 37, .generation = 9 } };
	char directory[] = "/tmp/omcid-backend-XXXXXX", path[128];
	struct omci_telemetry telem;
	struct omci_tcont_qos queue = { .enabled = true, .policy = 2, .weight = { 1, 2 } };
	struct omci_uni_attrs a = { .max_frame_size = 1522 };
	struct omci_service_config s = { .cookie = 19, .uni_entity_id = 0x101,
		.gem_ctp_entity_id = 256, .gem_port_id = 256, .tcont_entity_id = 0x8000,
		.alloc_id = 256, .vlan_id = 100, .vlan_valid = true, .pcp_valid = true,
		.pcp = 3, .queue = 0, .direction = OMCI_GEM_PORT_DIRECTION_BIDIRECTIONAL };
	u8 pdu[44] = { 0, 1, 0x48, 0x0a, 0x01, 0x06, 0x80, 0, 0x80, 0, 1, 0 };
	struct omci_mib_object object;
	unsigned int before;
	bool up;

	CHECK(mkdtemp(directory));
	snprintf(path, sizeof(path), "%s/state", directory);
	ctx.checkpoint = path;
	ctx.identity = (struct omci_identity) { .valid = OMCI_IDENTITY_F_SERIAL_NUMBER,
		.serial_number = "TEST0001" };
	ctx.parent = (struct device) { .identity = &ctx.identity,
		.onu_type = OMCI_ONU_TYPE_SFU, .uni_count = 1 };
	ctx.engine = (struct omci_device) { .parent = &ctx.parent, .ops = &omcid_backend_ops,
		.priv = &ctx, .started = true, .channel_up = true };
	hardware.ops = &ops;
	CHECK(!omci_agent_init(&ctx.engine));
	CHECK(!ctx.engine.ops->set_olt_profile);
	{
		u8 profile_id = OMCI_OLT_PROFILE_NOKIA_ALCL;

		CHECK(!omci_agent_config_set(&ctx.engine, OMCI_CONFIG_OLT_PROFILE_FORCE,
					     &profile_id, sizeof(profile_id)));
		CHECK(ctx.engine.agent.profile_effective == profile_id && !ctx.error);
		profile_id = OMCI_OLT_PROFILE_UNSPEC;
		CHECK(!omci_agent_config_set(&ctx.engine, OMCI_CONFIG_OLT_PROFILE_FORCE,
					     &profile_id, sizeof(profile_id)));
	}
	CHECK(!ctx.engine.ops->set_gem_port(&ctx.engine, 256, 256, 0x8000, 3, true, true));
	CHECK(!ctx.engine.ops->set_tcont_qos(&ctx.engine, 0x8000, &queue));
	CHECK(!ctx.engine.ops->set_gem_rate(&ctx.engine, 256, true, 1000, 100));
	CHECK(!ctx.engine.ops->set_uni(&ctx.engine, 0x101, true));
	CHECK(ctx.uni_known && ctx.uni_enabled && ctx.uni_entity == 0x101);
	CHECK(!ctx.engine.ops->set_uni_attrs(&ctx.engine, 0x101, &a));
	CHECK(!ctx.engine.ops->replace_service(&ctx.engine, &s));
	CHECK(last_service.vlan_valid && last_service.vlan_id == 100 && last_service.pcp == 3);
	CHECK(!ctx.engine.ops->delete_service(&ctx.engine, s.cookie));
	CHECK(!ctx.engine.ops->get_telemetry(&ctx.engine, &telem) && telem.bip_errors == 0x100000002ULL);
	CHECK(!ctx.engine.ops->get_uni_link(&ctx.engine, 0x101, &up) && up);
	ctx.trace = tmpfile();
	CHECK(ctx.trace);
	CHECK(!omci_native_receive(&ctx.engine, pdu, sizeof(pdu)) && transmissions == 1);
	{
		char line[256];

		/* One omci-monitor -r record: a time comment, then direction/generation/hex. */
		rewind(ctx.trace);
		CHECK(fgets(line, sizeof(line), ctx.trace) && !strncmp(line, "# ms ", 5));
		CHECK(fgets(line, sizeof(line), ctx.trace) && !strncmp(line, "tx 9 0 0 0001", 13));
		fclose(ctx.trace);
		ctx.trace = NULL;
	}
	CHECK(!omci_agent_mib_get(&ctx.engine, OMCI_CLASS_TCONT, 0x8000, &object));
	CHECK(get_unaligned_be16(object.data) == 256 && !ctx.error);
	CHECK(!omcid_checkpoint_load(&ctx));
	CHECK(ctx.engine.agent.last_response && !ctx.engine.agent.services.count);
	before = writes;
	CHECK(!omci_native_receive(&ctx.engine, pdu, sizeof(pdu)) && transmissions == 2 && writes == before);
	pdu[1] = 3;
	pdu[2] = 0x52;
	put_unaligned_be16(OMCI_CLASS_ANI_G, pdu + 4);
	put_unaligned_be16(0x8001, pdu + 6);
	CHECK(!omci_native_receive(&ctx.engine, pdu, sizeof(pdu)));
	CHECK(!ctx.error && transmissions == 4 && last_tx_action == 27);
	injected_error = -ESTALE;
	ctx.engine.ops->set_operational(&ctx.engine, false);
	CHECK(!ctx.error);
	ctx.engine.ops->set_operational(&ctx.engine, true);
	CHECK(ctx.error == -ESTALE);
	ctx.error = 0;
	{
		struct omci_identity changed = ctx.identity;

		memcpy(changed.serial_number, "FAIL0002", sizeof(changed.serial_number));
		ctx.engine.ops->config_changed(&ctx.engine, OMCI_CONFIG_SERIAL_NUMBER, &changed);
		CHECK(ctx.error == -ESTALE);
		CHECK(!memcmp(ctx.identity.serial_number, "TEST0001", sizeof(changed.serial_number)));
	}
	ctx.error = 0;
	CHECK(!omcid_checkpoint_save(&ctx));
	CHECK(!omcid_checkpoint_load(&ctx));
	CHECK(!memcmp(ctx.identity.serial_number, "TEST0001", sizeof(ctx.identity.serial_number)));
	CHECK(omcid_retry_identity(&ctx) == -ESTALE && ctx.pending_identity.valid);
	CHECK(!memcmp(ctx.identity.serial_number, "TEST0001", sizeof(ctx.identity.serial_number)));
	injected_error = 0;
	CHECK(!omcid_retry_identity(&ctx) && !ctx.pending_identity.valid);
	CHECK(!memcmp(ctx.identity.serial_number, "FAIL0002", sizeof(ctx.identity.serial_number)));
	before = writes;
	injected_error = -ETIMEDOUT;
	CHECK(ctx.engine.ops->set_tcont(&ctx.engine, 0x8000, 256, true) == -ETIMEDOUT);
	CHECK(ctx.error == -ETIMEDOUT && writes == before);
	pdu[1] = 2;
	CHECK(!omci_native_receive(&ctx.engine, pdu, sizeof(pdu)) && transmissions == 4);
	CHECK(omcid_checkpoint_save(&ctx) == -ETIMEDOUT);
	ctx.engine.ops = &offline;
	omci_agent_cleanup(&ctx.engine);
	CHECK(!unlink(path) && !rmdir(directory));
	printf("%u native engine/client/typed backend roundtrip checks passed\n", checks);
	return 0;
}
