// SPDX-License-Identifier: GPL-2.0-only
/*
 * The Nokia (Airtel) profile seeds the whole HGU service graph as default
 * MIB objects; once the OMCC is up those seeds alone must yield services,
 * as the OLT only audits the MIB afterwards.
 */
#include "native.h"

static unsigned int checks, services;
static bool realtek_resources, full_uni;
static u16 last_uni;
#define CHECK(v) do { assert(v); checks++; } while (0)

static int tx(struct omci_device *odev, struct sk_buff *skb, u16 gem)
{
	kfree_skb(skb);
	return 0;
}

static int topology(struct omci_device *odev, struct omci_ani_topology *t)
{
	if (realtek_resources) {
		*t = (struct omci_ani_topology) {
			.tcont_base = 0x8000, .scheduler_base = 0x8000,
			.queue_base = 0x8000, .tcont_count = 8, .queues_per_tcont = 1,
			.maximum_queue_size = 3276, .allocated_queue_size = 3276,
			.queue_config_option = 1, .scheduler_policy = 1,
			.gem_port_count = 16, .downstream_queues_per_uni = 1,
		};
		return 0;
	}
	*t = (struct omci_ani_topology) { .tcont_base = 0x8000, .scheduler_base = 0x8000,
		.queue_base = 0x8000, .tcont_count = 15, .queues_per_tcont = 8,
		.maximum_queue_size = 65535, .allocated_queue_size = 4,
		.queue_config_option = 1, .scheduler_policy = 1,
		.gem_port_count = 256, .qos_configuration = BIT(3) | BIT(4),
		.downstream_queues_per_uni = 8,
		.flags = OMCI_TOPOLOGY_F_PROFILE_SERVICES |
		(full_uni ? OMCI_TOPOLOGY_F_FULL_UNI_ENTITY : 0) };
	return 0;
}

static int tcont(struct omci_device *odev, u16 entity, u16 alloc, bool valid) { return 0; }

static int gem(struct omci_device *odev, u16 entity, u16 port, u16 tcont_entity,
	       u8 direction, bool valid, bool encrypted) { return 0; }

static int service(struct omci_device *odev, const struct omci_service_config *s)
{
	services++;
	last_uni = s->uni_entity_id;
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
	CHECK(last_uni == 1);
	CHECK(odev.agent.operational);
	omci_agent_cleanup(&odev);

	/* Hardware port binding must distinguish VEIP 0x601 from PPTP 0x101. */
	full_uni = true;
	services = 0;
	memset(&odev.agent, 0, sizeof(odev.agent));
	odev.channel_up = false;
	CHECK(!omci_agent_init(&odev));
	CHECK(!omci_agent_config_set_source(&odev, OMCI_CONFIG_OLT_PROFILE_FORCE, &profile,
					    1, OMCI_CONFIG_SOURCE_NETLINK));
	odev.channel_up = true;
	omci_agent_channel_changed(&odev, true);
	CHECK(services > 0 && last_uni == 0x601);
	omci_agent_cleanup(&odev);

	/* The same OLT profile must not invent services on another provider. */
	realtek_resources = true;
	services = 0;
	memset(&odev.agent, 0, sizeof(odev.agent));
	odev.channel_up = false;
	CHECK(!omci_agent_init(&odev));
	CHECK(!omci_agent_config_set_source(&odev, OMCI_CONFIG_OLT_PROFILE_FORCE, &profile,
					    1, OMCI_CONFIG_SOURCE_NETLINK));
	odev.channel_up = true;
	omci_agent_channel_changed(&odev, true);
	{
		struct omci_mib_object object;

		CHECK(!services && !odev.agent.services.count);
		CHECK(odev.agent.config.uni_count == 4);
		CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_PPTP_ETHERNET_UNI, 0x0104, &object));
		CHECK(omci_agent_mib_get(&odev, OMCI_CLASS_GEM_PORT_CTP, 256, &object) == -ENOENT);
		CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_TCONT, 0x8000, &object));
		CHECK(get_unaligned_be16(object.data) == 0xffff);
		CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_PRIORITY_QUEUE, 0x8007, &object));
		CHECK(get_unaligned_be16(object.data + 9) == 0x8007);
		CHECK(get_unaligned_be16(object.data + 11) == 0);
		CHECK(omci_agent_mib_get(&odev, OMCI_CLASS_PRIORITY_QUEUE, 0x8008, &object) == -ENOENT);
		CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_ONU2_G, 0, &object));
		CHECK(get_unaligned_be16(object.data + 29) == 16);
		CHECK(!get_unaligned_be16(object.data + 32));
	}
	omci_agent_cleanup(&odev);
	printf("%u seeded-service checks passed\n", checks);
	return 0;
}
