// SPDX-License-Identifier: GPL-2.0-only
/*
 * Replay of a recorded OLT session: every OLT message of a boot, as omcid's
 * trace holds it, goes through the engine and the answer to each Create, Set,
 * Get and Test must be the result code the ONU gave on the board. Run as
 * replay-test SESSION PROFILE; a session file has one baseline PDU in hex per
 * line followed by that result code, or "-" where the answer is data.
 */
#include "native.h"
#include <stdio.h>

static unsigned int checks, replayed, mismatches, creates;

/*
 * Instances the board has from its own configuration and hardware (software
 * images, the OLT-G, the identity MEs): the bare engine of this test does not,
 * so what the OLT asks of them is not compared.
 */
static bool board_state(u16 class_id)
{
	return class_id == 7 || class_id == 131 || class_id == 349 || class_id == 65530;
}
static u8 response[OMCI_MAX_PDU_LEN];
static size_t response_len;
#define CHECK(v) do { assert(v); checks++; } while (0)

static int tx(struct omci_device *odev, struct sk_buff *skb, u16 gem)
{
	CHECK(skb->len <= sizeof(response));
	memcpy(response, skb->data, skb->len);
	response_len = skb->len;
	kfree_skb(skb);
	return 0;
}

static int topology(struct omci_device *odev, struct omci_ani_topology *t)
{
	*t = (struct omci_ani_topology) { .tcont_base = 0x8000, .scheduler_base = 0x8000,
		.queue_base = 0x8000, .tcont_count = 8, .queues_per_tcont = 8,
		.maximum_queue_size = 3276, .allocated_queue_size = 3276,
		.queue_config_option = 1, .scheduler_policy = 1,
		.gem_port_count = 64, .downstream_queues_per_uni = 8,
		.flags = OMCI_TOPOLOGY_F_PROFILE_SERVICES };
	return 0;
}

static int tcont(struct omci_device *odev, u16 entity, u16 alloc, bool valid) { return 0; }

static int gem(struct omci_device *odev, u16 entity, u16 port, u16 tcont_entity,
	       u8 direction, bool valid, bool encrypted) { return 0; }

static int service(struct omci_device *odev, const struct omci_service_config *s) { return 0; }
static int service_delete(struct omci_device *odev, u32 cookie) { return 0; }
static void notify(struct omci_device *odev, u8 event) { }
static void reboot_request(struct omci_device *odev) { }

int main(int argc, char **argv)
{
	struct omci_identity identity = { .valid = OMCI_IDENTITY_F_SERIAL_NUMBER |
		OMCI_IDENTITY_F_VENDOR_ID, .serial_number = "TEST0001", .vendor_id = "TEST" };
	struct device parent = { .identity = &identity, .onu_type = OMCI_ONU_TYPE_HGU,
		.uni_count = 4, .notify = notify, .reboot = reboot_request };
	struct omci_device_ops ops = { .xmit = tx, .get_ani_topology = topology,
		.set_tcont = tcont, .set_gem_port = gem, .replace_service = service,
		.delete_service = service_delete };
	struct omci_device odev = { .parent = &parent, .ops = &ops, .started = true };
	char line[256];
	FILE *fp;
	u8 profile;

	CHECK(argc == 3);
	for (profile = OMCI_OLT_PROFILE_GENERIC; profile <= OMCI_OLT_PROFILE_GENEXIS; profile++)
		if (!strcmp(omci_olt_profile_name(profile), argv[2]))
			break;
	CHECK(profile <= OMCI_OLT_PROFILE_GENEXIS);
	fp = fopen(argv[1], "r");
	CHECK(fp);

	CHECK(!omci_agent_init(&odev));
	CHECK(!omci_agent_config_set_source(&odev, OMCI_CONFIG_OLT_PROFILE_FORCE, &profile,
					    1, OMCI_CONFIG_SOURCE_NETLINK));
	odev.channel_up = true;
	omci_agent_channel_changed(&odev, true);

	while (fgets(line, sizeof(line), fp)) {
		u8 pdu[48];
		unsigned int i, rc = 0;
		bool has_rc;
		char *space;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		space = strchr(line, ' ');
		CHECK(space && space - line == 2 * sizeof(pdu));
		for (i = 0; i < sizeof(pdu); i++) {
			unsigned int v;

			CHECK(sscanf(line + 2 * i, "%2x", &v) == 1);
			pdu[i] = v;
		}
		has_rc = space[1] != '-';
		if (has_rc)
			rc = strtoul(space + 1, NULL, 10);

		response_len = 0;
		CHECK(!omci_native_receive(&odev, pdu, sizeof(pdu)));
		replayed++;
		if (has_rc && (pdu[2] & 0x1f) == 4)
			creates++;
		if (has_rc && !board_state(get_unaligned_be16(pdu + 4)) &&
		    (!response_len || response[8] != rc)) {
			fprintf(stderr, "message %u: class %u action %u gave %u, board %u\n",
				replayed, get_unaligned_be16(pdu + 4), pdu[2] & 0x1f,
				response_len ? response[8] : 0xff, rc);
			mismatches++;
		}
	}
	fclose(fp);
	CHECK(replayed > 100);
	CHECK(creates > 50 && !mismatches);
	omci_agent_cleanup(&odev);
	printf("%u OLT messages replayed, %u checks passed\n", replayed, checks);
	return 0;
}
