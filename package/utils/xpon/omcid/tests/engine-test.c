// SPDX-License-Identifier: GPL-2.0-only
#include "native.h"

static unsigned int checks, transmissions, writes, notifications, reboots;
static u8 response[OMCI_MAX_PDU_LEN];
static size_t response_len;
static int hardware_error;
#define CHECK(v) do { assert(v); checks++; } while (0)

static int tx(struct omci_device *odev, struct sk_buff *skb, u16 gem)
{
	CHECK(skb->len <= sizeof(response));
	memcpy(response, skb->data, skb->len);
	response_len = skb->len;
	transmissions++;
	kfree_skb(skb);
	return 0;
}

static int tcont(struct omci_device *odev, u16 entity, u16 alloc, bool valid)
{
	writes++;
	return hardware_error;
}

static int topology(struct omci_device *odev, struct omci_ani_topology *t)
{
	*t = (struct omci_ani_topology) { .tcont_base = 0x8000, .scheduler_base = 0x8000,
		.queue_base = 0x8000, .tcont_count = 15, .queues_per_tcont = 8,
		.maximum_queue_size = 65535, .allocated_queue_size = 4,
		.queue_config_option = 1, .scheduler_policy = 1 };
	return 0;
}

static void notify(struct omci_device *odev, u8 event) { notifications++; }
static void reboot_request(struct omci_device *odev) { reboots++; }

static void storage_tests(void)
{
	struct xarray a;
	unsigned long key;
	void *value;
	unsigned int i, seen = 0;
	int item;
	char text[PAGE_SIZE];

	CHECK(omci_native_emit(text, PAGE_SIZE, "ignored") == 0);
	CHECK(omci_native_emit(text, -1, "ignored") == 0);
	CHECK(omci_native_emit(text, PAGE_SIZE - 2, "long string") == 1);
	CHECK(!text[PAGE_SIZE - 1]);
	xa_init(&a);
	CHECK(!xa_reserve(&a, 1, 0) && xa_empty(&a));
	CHECK(!xa_store(&a, ULONG_MAX, &item, 0));
	CHECK(!xa_store(&a, 0, &item, 0));
	xa_for_each(&a, key, value) {
		CHECK(value == &item);
		seen++;
	}
	CHECK(seen == 2 && !xa_find_after(&a, &key, ULONG_MAX, 0));
	xa_release(&a, 1);
	CHECK(a.count == 2);
	CHECK(xa_erase(&a, ULONG_MAX) == &item);
	CHECK(xa_erase(&a, 0) == &item && xa_empty(&a));
	for (i = 0; i < 1024; i++)
		CHECK(!xa_store(&a, i, &item, 0));
	CHECK(xa_err(xa_store(&a, 1024, &item, 0)) == -ENOSPC);
	CHECK(xa_store(&a, 511, &item, 0) == &item);
	xa_destroy(&a);
}

static void request(struct omci_device *odev, u16 tci, u8 action, u16 class_id,
		    u16 entity, const void *data, size_t len)
{
	u8 pdu[44] = { 0 };

	CHECK(len <= 32);
	put_unaligned_be16(tci, pdu);
	pdu[2] = action | 0x40;
	pdu[3] = 0x0a;
	put_unaligned_be16(class_id, pdu + 4);
	put_unaligned_be16(entity, pdu + 6);
	if (len)
		memcpy(pdu + 8, data, len);
	CHECK(!omci_native_receive(odev, pdu, sizeof(pdu)));
	CHECK(response_len == sizeof(pdu) && response[2] == (action | 0x20));
}

int main(void)
{
	struct omci_identity identity = { .valid = OMCI_IDENTITY_F_SERIAL_NUMBER |
		OMCI_IDENTITY_F_VENDOR_ID, .serial_number = "TEST0001", .vendor_id = "TEST" };
	struct device parent = { .identity = &identity, .onu_type = OMCI_ONU_TYPE_SFU,
		.uni_count = 1, .notify = notify, .reboot = reboot_request };
	struct omci_device_ops ops = { .xmit = tx, .set_tcont = tcont,
		.get_ani_topology = topology };
	struct omci_device odev = { .parent = &parent, .ops = &ops,
		.started = true, .channel_up = true, .gem_port_id = 0 };
	struct omci_mib_object object;
	u8 data[32] = { 0 }, first[44];
	unsigned int before;

	storage_tests();
	CHECK(!omci_agent_init(&odev));
	CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_ONU_G, 0, &object));
	CHECK(!memcmp(object.data, "TEST", 4));
	CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_PRIORITY_QUEUE, 0x8077, &object));
	CHECK(omci_agent_mib_get(&odev, OMCI_CLASS_PPTP_ETHERNET_UNI, 0x0102, &object) == -ENOENT);
	{
		/* ANI-G SR indication: on by default, follows the config key. */
		u8 sr = 0;

		CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_ANI_G, 0x8001, &object) && object.data[0] == 1);
		CHECK(!omci_agent_config_set(&odev, OMCI_CONFIG_DBA_STATUS_REPORTING, &sr, 1));
		CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_ANI_G, 0x8001, &object) && !object.data[0]);
		sr = 2;
		CHECK(omci_agent_config_set(&odev, OMCI_CONFIG_DBA_STATUS_REPORTING, &sr, 1) == -EINVAL);
		sr = 1;
		CHECK(!omci_agent_config_set(&odev, OMCI_CONFIG_DBA_STATUS_REPORTING, &sr, 1));
		CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_ANI_G, 0x8001, &object) && object.data[0] == 1);
	}
	/* G.988 9.2.10: 8 downstream queues for the UNI, then 8 for the VEIP. */
	CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_PRIORITY_QUEUE, 0x0000, &object));
	CHECK(get_unaligned_be32(object.data + 9) == 0x01010000);
	CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_PRIORITY_QUEUE, 0x0007, &object));
	CHECK(get_unaligned_be32(object.data + 9) == 0x01010007);
	CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_PRIORITY_QUEUE, 0x0008, &object));
	CHECK(get_unaligned_be16(object.data + 9) == 0x0a01 && get_unaligned_be16(object.data + 11) == 0);
	CHECK(omci_agent_mib_get(&odev, OMCI_CLASS_PRIORITY_QUEUE, 0x0010, &object) == -ENOENT);
	put_unaligned_be16(0x8000, data);
	put_unaligned_be16(256, data + 2);
	request(&odev, 1, 8, OMCI_CLASS_TCONT, 0x8000, data, 4);
	CHECK(response[8] == 0);
	CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_TCONT, 0x8000, &object));
	CHECK(get_unaligned_be16(object.data) == 256);
	before = writes;
	memcpy(first, response, sizeof(first));
	request(&odev, 1, 8, OMCI_CLASS_TCONT, 0x8000, data, 4);
	CHECK(writes == before && odev.agent.duplicates == 1 && !memcmp(first, response, 44));
	put_unaligned_be16(257, data + 2);
	hardware_error = -EIO;
	request(&odev, 2, 8, OMCI_CLASS_TCONT, 0x8000, data, 4);
	CHECK(response[8] != 0);
	CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_TCONT, 0x8000, &object));
	CHECK(get_unaligned_be16(object.data) == 256);
	hardware_error = 0;
	memset(data, 0, sizeof(data));
	data[0] = 1;
	request(&odev, 3, 4, OMCI_CLASS_GAL_ETHERNET_PROFILE, 0x1234, data, 1);
	CHECK(response[8] == 0);
	request(&odev, 4, 4, OMCI_CLASS_GAL_ETHERNET_PROFILE, 0x1234, data, 1);
	CHECK(response[8] == 7);
	request(&odev, 5, 6, OMCI_CLASS_GAL_ETHERNET_PROFILE, 0x1234, data, 0);
	CHECK(response[8] == 0);
	CHECK(omci_agent_mib_get(&odev, OMCI_CLASS_GAL_ETHERNET_PROFILE, 0x1234, &object) == -ENOENT);
	request(&odev, 6, 13, OMCI_CLASS_ONU_DATA, 0, data, 0);
	CHECK(get_unaligned_be16(response + 8) > 100);
	memset(data, 0, sizeof(data));
	request(&odev, 7, 14, OMCI_CLASS_ONU_DATA, 0, data, 2);
	CHECK(get_unaligned_be16(response + 8) != 0);
	{
		/*
		 * G.988 A.3.16: walk the whole upload. ONU-G and ONU2-G do not fit
		 * one message, so their attributes continue, whole, in the next;
		 * the ME 171 rule table is never uploaded.
		 */
		u16 total, seq, onu_g_mask = 0, onu2_g_mask = 0;
		unsigned int onu2_g_parts = 0;
		u8 next[2];

		request(&odev, 0x1000, 13, OMCI_CLASS_ONU_DATA, 0, data, 0);
		total = get_unaligned_be16(response + 8);
		for (seq = 0; seq < total; seq++) {
			u16 class_id, mask;

			put_unaligned_be16(seq, next);
			request(&odev, 0x1001 + seq, 14, OMCI_CLASS_ONU_DATA, 0, next, 2);
			class_id = get_unaligned_be16(response + 8);
			mask = get_unaligned_be16(response + 12);
			CHECK(class_id);
			if (class_id == OMCI_CLASS_ONU_G) {
				CHECK(!(onu_g_mask & mask));
				onu_g_mask |= mask;
			} else if (class_id == OMCI_CLASS_ONU2_G) {
				CHECK(!(onu2_g_mask & mask));
				onu2_g_mask |= mask;
				onu2_g_parts++;
			} else if (class_id == OMCI_CLASS_EXTENDED_VLAN) {
				CHECK(!(mask & BIT(10)));
			}
		}
		CHECK(onu_g_mask == GENMASK(15, 3));
		/* ONU2-G reports the connectivity capability and queue scale it stores. */
		put_unaligned_be16(BIT(5) | BIT(2), next);
		request(&odev, 0x2000, 9, OMCI_CLASS_ONU2_G, 0, next, 2);
		CHECK(response[8] == 0 && get_unaligned_be16(response + 9) == (BIT(5) | BIT(2)));
		CHECK(get_unaligned_be16(response + 11) == OMCI_ONU2G_CONNECTIVITY_CAPABILITY);
		CHECK(get_unaligned_be16(response + 13) == 1);
		put_unaligned_be16(BIT(7), next);
		request(&odev, 0x2001, 9, OMCI_CLASS_ONU2_G, 0, next, 2);
		CHECK(response[8] == 0 && get_unaligned_be16(response + 11) == 256);
		CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_TCONT, 0x8005, &object));
		CHECK(get_unaligned_be16(object.data) == 0xffff && object.data[2] == 1);
		/* G.988 9.5.1: sensed type 47, then an AVC when locking disables it. */
		CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_PPTP_ETHERNET_UNI, 0x101, &object));
		CHECK(object.data[1] == 47 && object.data[5] == 0 && object.data[12] == 2);
		u8 lock[3] = { 0x08, 0x00, 1 };
		request(&odev, 0x2002, 8, OMCI_CLASS_PPTP_ETHERNET_UNI, 0x101, lock, 3);
		CHECK(response[8] == 0);
		odev.agent.alarm_work.work.fn(&odev.agent.alarm_work.work);
		CHECK((response[2] & 0x1f) == 17 && get_unaligned_be16(response + 4) ==
		      OMCI_CLASS_PPTP_ETHERNET_UNI && get_unaligned_be16(response + 6) == 0x101);
		CHECK(get_unaligned_be16(response + 8) == BIT(10) && response[10] == 1);
		lock[2] = 0;
		request(&odev, 0x2003, 8, OMCI_CLASS_PPTP_ETHERNET_UNI, 0x101, lock, 3);
		odev.agent.alarm_work.work.fn(&odev.agent.alarm_work.work);
		CHECK(get_unaligned_be16(response + 8) == BIT(10) && response[10] == 0);
		CHECK(onu2_g_mask == GENMASK(15, 2) && onu2_g_parts == 2);
		put_unaligned_be16(total, next);
		request(&odev, 0x1001 + total, 14, OMCI_CLASS_ONU_DATA, 0, next, 2);
		CHECK(!get_unaligned_be16(response + 8) && !get_unaligned_be16(response + 12));
	}
	put_unaligned_be16(0x8000, data);
	request(&odev, 8, 9, OMCI_CLASS_OMCI, 0, data, 2);
	CHECK(response[8] == 0 && odev.agent.table_snapshot_len > 0);
	memset(data + 2, 0, 2);
	request(&odev, 9, 26, OMCI_CLASS_OMCI, 0, data, 4);
	CHECK(response[8] == 0);
	{
		/* G.988 A.2.33: at 10:07:30 the interval has 450 s to the quarter-hour. */
		const u8 now[7] = { 0x07, 0xea, 10, 17, 10, 7, 30 };
		unsigned long before = jiffies;

		request(&odev, 11, 24, OMCI_CLASS_ONU_G, 0, now, sizeof(now));
		CHECK(response[8] == 0 && odev.agent.pm_work.pending);
		CHECK(odev.agent.pm_work.due - before >= 450 * HZ &&
		      odev.agent.pm_work.due - jiffies <= 450 * HZ);
		/* All-zero content is midnight: a full interval. */
		request(&odev, 12, 24, OMCI_CLASS_ONU_G, 0, NULL, 0);
		CHECK(odev.agent.pm_work.due - jiffies > 899 * HZ);
	}
	omci_agent_channel_changed(&odev, false);
	CHECK(!odev.agent.last_request && !odev.agent.table_snapshot);
	CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_TCONT, 0x8000, &object));
	request(&odev, 10, 15, OMCI_CLASS_ONU_DATA, 0, data, 0);
	CHECK(response[8] == 0 && !odev.agent.mib_sync);
	CHECK(!omci_agent_mib_get(&odev, OMCI_CLASS_ONU_G, 0, &object));
	CHECK(omci_native_receive(&odev, data, 3) == -EINVAL);
	CHECK(transmissions >= 10);
	omci_agent_cleanup(&odev);
	CHECK(!odev.agent.mib.count && !odev.agent.services.count);
	printf("%u native MIB/transaction/rollback/table tests passed\n", checks);
	return 0;
}
