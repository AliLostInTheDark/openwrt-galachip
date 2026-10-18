// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define main omcid_program_main
#include "../src/main.c"
#undef main

static unsigned int checks, writes, sent;
static bool stale_write;
#define CHECK(v) do { assert(v); checks++; } while (0)
static int tx(struct omci_device *d, struct sk_buff *skb, u16 port)
{
	sent++;
	kfree_skb(skb);
	return 0;
}
static int tcont(struct omci_device *d, u16 entity, u16 alloc, bool valid)
{
	writes++;
	return stale_write ? -ESTALE : 0;
}
static int topology(struct omci_device *d, struct omci_ani_topology *t)
{
	*t = (struct omci_ani_topology) { .tcont_base = 0x8000, .scheduler_base = 0x8000,
		.queue_base = 0x8000, .tcont_count = 15, .queues_per_tcont = 8,
		.maximum_queue_size = 65535, .allocated_queue_size = 4,
		.queue_config_option = 1, .scheduler_policy = 1 };
	return 0;
}
static void event(struct omcid_request *q, u32 generation, u8 event, bool up)
{
	u32 flags = up ? OMCI_F_CHANNEL_UP : 0;

	omcid_request_init(q, 37, OMCI_CMD_EVENT);
	CHECK(!omcid_put(q, OMCI_ATTR_GENERATION, &generation, sizeof(generation)));
	CHECK(!omcid_put(q, OMCI_ATTR_FLAGS, &flags, sizeof(flags)));
	CHECK(!omcid_put(q, OMCI_ATTR_EVENT, &event, sizeof(event)));
}
static void rx(struct omcid_request *q, u32 generation, u64 sequence, bool malformed)
{
	u8 pdu[44] = { 0, 1, 0x48, 0x0a, 0x01, 0x06, 0x80, 0, 0x80, 0, 1, 0 };

	omcid_request_init(q, 37, OMCI_CMD_RX);
	CHECK(!omcid_put(q, OMCI_ATTR_GENERATION, &generation, sizeof(generation)));
	CHECK(!omcid_put(q, OMCI_ATTR_SEQUENCE, &sequence, sizeof(sequence)));
	CHECK(!omcid_put(q, OMCI_ATTR_PDU, pdu, malformed ? 3 : sizeof(pdu)));
}
static void rx_custom(struct omcid_request *q, u32 generation, u64 sequence,
		      u16 tci, u8 action, u16 class_id, u16 entity_id,
		      const void *data, size_t len)
{
	u8 pdu[44] = { 0 };

	put_unaligned_be16(tci, pdu);
	pdu[2] = action | 0x40;
	pdu[3] = 0x0a;
	put_unaligned_be16(class_id, pdu + 4);
	put_unaligned_be16(entity_id, pdu + 6);
	if (len && data)
		memcpy(pdu + 8, data, len > 32 ? 32 : len);
	omcid_request_init(q, 37, OMCI_CMD_RX);
	CHECK(!omcid_put(q, OMCI_ATTR_GENERATION, &generation, sizeof(generation)));
	CHECK(!omcid_put(q, OMCI_ATTR_SEQUENCE, &sequence, sizeof(sequence)));
	CHECK(!omcid_put(q, OMCI_ATTR_PDU, pdu, sizeof(pdu)));
}
int main(void)
{
	struct omcid ctx = { .restoring = true };
	struct omci_identity id = { .valid = OMCI_IDENTITY_F_SERIAL_NUMBER,
		.serial_number = "TEST0001" };
	struct device parent = { .identity = &id, .onu_type = OMCI_ONU_TYPE_SFU, .uni_count = 1 };
	const struct omci_device_ops ops = { .xmit = tx, .set_tcont = tcont, .get_ani_topology = topology };
	struct omcid_request q;
	struct omci_mib_object object;
	unsigned int before;

	ctx.transport.family = 37;
	ctx.transport.generation = 1;
	ctx.engine = (struct omci_device) { .parent = &parent, .ops = &ops,
		.started = true, .channel_up = true };
	CHECK(!omci_agent_init(&ctx.engine));
	rx(&q, 1, 10, false);
	CHECK(!handle_record(&ctx, &q.header) && sent == 1);
	CHECK(!omci_agent_mib_get(&ctx.engine, OMCI_CLASS_TCONT, 0x8000, &object));
	CHECK(get_unaligned_be16(object.data) == 256);
	before = writes;
	rx(&q, 1, 11, false);
	CHECK(!handle_record(&ctx, &q.header) && writes == before && ctx.engine.agent.duplicates == 1);
	rx(&q, 1, 13, false);
	CHECK(handle_record(&ctx, &q.header) == -ENOBUFS && writes == before);
	event(&q, 2, OMCI_EVENT_CHANNEL_DOWN, false);
	CHECK(!handle_record(&ctx, &q.header));
	CHECK(!ctx.engine.channel_up && !ctx.engine.agent.last_request);
	CHECK(!omci_agent_mib_get(&ctx.engine, OMCI_CLASS_TCONT, 0x8000, &object));
	event(&q, 1, OMCI_EVENT_CHANNEL_UP, true);
	CHECK(!handle_record(&ctx, &q.header) && ctx.transport.generation == 2 && !ctx.engine.channel_up);
	event(&q, 3, OMCI_EVENT_CHANNEL_UP, true);
	CHECK(!handle_record(&ctx, &q.header) && ctx.engine.channel_up && writes > before);
	before = writes;
	event(&q, 3, OMCI_EVENT_RESOURCE_CHANGE, true);
	CHECK(!handle_record(&ctx, &q.header) && writes > before);
	rx(&q, 2, 14, false);
	CHECK(!handle_record(&ctx, &q.header) && !ctx.have_rx_sequence);
	rx(&q, 4, 14, false);
	CHECK(!handle_record(&ctx, &q.header));
	rx(&q, 3, 14, true);
	CHECK(!handle_record(&ctx, &q.header) && ctx.engine.rx_dropped == 1);
	rx(&q, 3, 15, false);
	CHECK(!handle_record(&ctx, &q.header));
	CHECK(ctx.engine.agent.last_response && ctx.engine.agent.duplicates == 1);
	ctx.error = -ESTALE;
	event(&q, 4, OMCI_EVENT_CHANNEL_DOWN, false);
	CHECK(!recover_status(&ctx, &q.header));
	CHECK(!ctx.error && !ctx.engine.channel_up && !ctx.have_rx_sequence);
	CHECK(!ctx.engine.agent.last_request && !ctx.engine.agent.last_response);
	ctx.error = -ESTALE;
	CHECK(!recover_status(&ctx, &q.header) && !ctx.error);
	before = writes;
	rx(&q, 4, 20, false);
	CHECK(!handle_record(&ctx, &q.header) && writes == before);
	event(&q, 5, OMCI_EVENT_CHANNEL_UP, true);
	CHECK(!handle_record(&ctx, &q.header) && ctx.engine.channel_up);
	stale_write = true;
	ctx.error = -ESTALE;
	CHECK(recover_status(&ctx, &q.header) == -ESTALE);
	stale_write = false;
	CHECK(!recover_status(&ctx, &q.header) && !ctx.error);

	/* Active MIB table read & MIB upload interrupted by optical drop (Nokia ISAM 7360 scenario) */
	u8 me_table_attr[2] = { 0x80, 0 };
	rx_custom(&q, 5, 21, 1, 9, OMCI_CLASS_OMCI, 0, me_table_attr, sizeof(me_table_attr));
	CHECK(!handle_record(&ctx, &q.header));
	CHECK(ctx.engine.agent.table_snapshot != NULL);
	rx_custom(&q, 5, 22, 2, 13, OMCI_CLASS_ONU_DATA, 0, NULL, 0);
	CHECK(!handle_record(&ctx, &q.header));
	u8 seq0[2] = { 0, 0 };
	rx_custom(&q, 5, 23, 3, 14, OMCI_CLASS_ONU_DATA, 0, seq0, sizeof(seq0));
	CHECK(!handle_record(&ctx, &q.header));
	CHECK(ctx.engine.agent.upload_index > 0);

	/* Fiber disconnect in the middle of active provisioning */
	event(&q, 5, OMCI_EVENT_CHANNEL_DOWN, false);
	CHECK(!handle_record(&ctx, &q.header));
	CHECK(!ctx.engine.channel_up && !ctx.engine.agent.table_snapshot);
	CHECK(ctx.engine.agent.upload_index == 0);
	CHECK(!ctx.have_rx_sequence);

	/* Fiber returns with new generation 6 (re-ranged) */
	event(&q, 6, OMCI_EVENT_CHANNEL_UP, true);
	CHECK(!handle_record(&ctx, &q.header) && ctx.engine.channel_up);
	CHECK(ctx.transport.generation == 6);

	/* Rapid flapping: Down (6) -> Up (7) -> Down (7) -> Up (8) */
	event(&q, 6, OMCI_EVENT_CHANNEL_DOWN, false);
	CHECK(!handle_record(&ctx, &q.header) && !ctx.engine.channel_up);
	event(&q, 7, OMCI_EVENT_CHANNEL_UP, true);
	CHECK(!handle_record(&ctx, &q.header) && ctx.engine.channel_up);
	event(&q, 7, OMCI_EVENT_CHANNEL_DOWN, false);
	CHECK(!handle_record(&ctx, &q.header) && !ctx.engine.channel_up);
	event(&q, 8, OMCI_EVENT_CHANNEL_UP, true);
	CHECK(!handle_record(&ctx, &q.header) && ctx.engine.channel_up);
	CHECK(ctx.transport.generation == 8 && !ctx.have_rx_sequence);

	/* Nokia restarts fresh MIB Reset and Upload on new generation 8 */
	rx_custom(&q, 8, 1, 10, 15, OMCI_CLASS_ONU_DATA, 0, NULL, 0);
	CHECK(!handle_record(&ctx, &q.header));
	rx_custom(&q, 8, 2, 11, 13, OMCI_CLASS_ONU_DATA, 0, NULL, 0);
	CHECK(!handle_record(&ctx, &q.header));

	omci_agent_cleanup(&ctx.engine);
	printf("%u daemon event/duplicate/outage/grant/sequence checks passed\n", checks);
	return 0;
}
