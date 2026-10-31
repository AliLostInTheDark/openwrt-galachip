// SPDX-License-Identifier: GPL-2.0-only
/*
 * QoS the hardware cannot do must not fail the OLT's write: the backend gets
 * what it can apply, and the service stays up.
 */
#include "native.h"

static unsigned int checks, qos_calls;
static u32 up_rate, up_burst;
static struct omci_tcont_qos last_qos;
#define CHECK(v) do { assert(v); checks++; } while (0)

static int tx(struct omci_device *odev, struct sk_buff *skb, u16 gem)
{
	kfree_skb(skb);
	return 0;
}

static int topology(struct omci_device *odev, struct omci_ani_topology *t)
{
	*t = (struct omci_ani_topology) { .tcont_base = 0x8000, .scheduler_base = 0x8000,
		.queue_base = 0x8000, .tcont_count = 8, .queues_per_tcont = 8,
		.maximum_queue_size = 3276, .allocated_queue_size = 3276,
		.queue_config_option = 1, .scheduler_policy = 2,
		.gem_port_count = 256, .qos_configuration = BIT(3) | BIT(4),
		.downstream_queues_per_uni = 8 };
	return 0;
}

static int tcont(struct omci_device *odev, u16 entity, u16 alloc, bool valid) { return 0; }

static int gem(struct omci_device *odev, u16 entity, u16 port, u16 tcont_entity,
	       u8 direction, bool valid, bool encrypted) { return 0; }

static int rate(struct omci_device *odev, u16 port, bool upstream, u32 value, u32 burst)
{
	if (upstream) {
		up_rate = value;
		up_burst = burst;
	}
	return 0;
}

static int qos(struct omci_device *odev, u16 entity, const struct omci_tcont_qos *q)
{
	if (entity == 0x8000) {
		qos_calls++;
		last_qos = *q;
	}
	return 0;
}

static void notify(struct omci_device *odev, u8 event) { }
static void reboot_request(struct omci_device *odev) { }

static int set(struct omci_device *odev, u16 class_id, u16 entity, const u8 *data, size_t len)
{
	struct omci_mib_object object = { .class_id = class_id, .entity_id = entity,
		.attr_mask = 0xffff, .data_len = len };

	memcpy(object.data, data, len);
	return omci_agent_mib_set(odev, &object);
}

int main(void)
{
	struct omci_identity identity = { .valid = OMCI_IDENTITY_F_SERIAL_NUMBER |
		OMCI_IDENTITY_F_VENDOR_ID, .serial_number = "TEST0001", .vendor_id = "TEST" };
	struct device parent = { .identity = &identity, .onu_type = OMCI_ONU_TYPE_SFU,
		.uni_count = 1, .notify = notify, .reboot = reboot_request };
	struct omci_device_ops ops = { .xmit = tx, .get_ani_topology = topology,
		.set_tcont = tcont, .set_gem_port = gem, .set_gem_rate = rate,
		.set_tcont_qos = qos };
	struct omci_device odev = { .parent = &parent, .ops = &ops, .started = true,
		.channel_up = true };
	u8 ctp[15] = { 0x01, 0x00, 0x80, 0x00, 3 }, td[20] = { 0 }, ts[6] = { 0x80, 0x00 };

	CHECK(!omci_agent_init(&odev));

	/* A GEM may name its descriptor before the OLT creates it. */
	put_unaligned_be16(5, ctp + 7);
	CHECK(!set(&odev, OMCI_CLASS_GEM_PORT_CTP, 256, ctp, sizeof(ctp)));
	CHECK(!up_rate);

	/* A plain peak limiter is applied, with its burst. */
	put_unaligned_be32(125000, td + 4);
	put_unaligned_be32(4000, td + 12);
	CHECK(!set(&odev, OMCI_CLASS_TRAFFIC_DESCRIPTOR, 5, td, sizeof(td)));
	CHECK(up_rate == 125000 && up_burst == 4000);

	/* CIR and colour cannot be: the write stands and the peak still applies. */
	put_unaligned_be32(1000, td);
	put_unaligned_be32(250000, td + 4);
	td[16] = 1;
	CHECK(!set(&odev, OMCI_CLASS_TRAFFIC_DESCRIPTOR, 5, td, sizeof(td)));
	CHECK(up_rate == 250000);

	/* A supported scheduler reaches the backend. */
	ts[4] = 2;
	CHECK(!set(&odev, OMCI_CLASS_TRAFFIC_SCHEDULER, 0x8000, ts, sizeof(ts)));
	CHECK(qos_calls && last_qos.enabled && last_qos.policy == 2);

	/* One the hardware lacks falls back to no scheduling, without an error. */
	put_unaligned_be16(0x8001, ts + 2);
	CHECK(!set(&odev, OMCI_CLASS_TRAFFIC_SCHEDULER, 0x8000, ts, sizeof(ts)));
	CHECK(!last_qos.enabled);

	printf("%u QoS compatibility checks passed\n", checks);
	return 0;
}
