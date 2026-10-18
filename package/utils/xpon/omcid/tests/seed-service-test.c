// SPDX-License-Identifier: GPL-2.0-only
/*
 * The Nokia (Airtel) profile seeds the whole HGU service graph as default
 * MIB objects; once the OMCC is up those seeds alone must yield services,
 * as the OLT only audits the MIB afterwards.
 */
#include "native.h"

static unsigned int checks, services;
#define CHECK(v) do { assert(v); checks++; } while (0)

static int tx(struct omci_device *odev, struct sk_buff *skb, u16 gem)
{
	kfree_skb(skb);
	return 0;
}

static int topology(struct omci_device *odev, struct omci_ani_topology *t)
{
	*t = (struct omci_ani_topology) { .tcont_base = 0x8000, .scheduler_base = 0x8000,
		.queue_base = 0x8000, .tcont_count = 15, .queues_per_tcont = 8,
		.maximum_queue_size = 65535, .allocated_queue_size = 4,
		.queue_config_option = 1, .scheduler_policy = 1 };
	return 0;
}

static int tcont(struct omci_device *odev, u16 entity, u16 alloc, bool valid) { return 0; }

static int gem(struct omci_device *odev, u16 entity, u16 port, u16 tcont_entity,
	       u8 direction, bool valid, bool encrypted) { return 0; }

static int service(struct omci_device *odev, const struct omci_service_config *s)
{
	services++;
	return 0;
}

static int service_delete(struct omci_device *odev, u32 cookie) { return 0; }
static void notify(struct omci_device *odev, u8 event) { }
static void reboot_request(struct omci_device *odev) { }

int main(void)
{
	struct omci_identity identity = { .valid = OMCI_IDENTITY_F_SERIAL_NUMBER |
		OMCI_IDENTITY_F_VENDOR_ID, .serial_number = "TEST0001", .vendor_id = "TEST" };
	struct device parent = { .identity = &identity, .onu_type = OMCI_ONU_TYPE_HGU,
		.uni_count = 4, .notify = notify, .reboot = reboot_request };
	struct omci_device_ops ops = { .xmit = tx, .get_ani_topology = topology,
		.set_tcont = tcont, .set_gem_port = gem, .replace_service = service,
		.delete_service = service_delete };
	struct omci_device odev = { .parent = &parent, .ops = &ops, .started = true };
	u8 profile = OMCI_OLT_PROFILE_NOKIA_ALCL;

	CHECK(!omci_agent_init(&odev));
	CHECK(!omci_agent_config_set_source(&odev, OMCI_CONFIG_OLT_PROFILE_FORCE, &profile,
					    1, OMCI_CONFIG_SOURCE_NETLINK));
	odev.channel_up = true;
	omci_agent_channel_changed(&odev, true);
	{
		struct omci_mib_object object;

		/* Four UNIs take queues 0..31; the VEIP's eight moved with it to 0x601. */
		CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_PRIORITY_QUEUE, 0x20, &object));
		CHECK(get_unaligned_be16(object.data + 9) == 0x0601);
	}
	CHECK(services > 0);
	CHECK(odev.agent.operational);
	omci_agent_cleanup(&odev);
	printf("%u seeded-service checks passed\n", checks);
	return 0;
}
