// SPDX-License-Identifier: GPL-2.0-only
/* RAM-only, mode-0600 checkpoints; never write provisioning state to NOR. */
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "daemon.h"

#define CHECKPOINT_MAGIC 0x4f4d4331
#define CHECKPOINT_VERSION 4
#define CHECKPOINT_MAX (2U * 1024 * 1024)
#define CHECKPOINT_MAX_TABLE (256U * 1024)

struct checkpoint_header {
	u32 magic, version, size, object_size, count, generation, checksum;
	struct omci_agent_config config;
	struct omci_ani_topology topology;
	u32 upload_index, profile_quirks, flags;
	u16 mib_sync;
	u16 uni_entity;
	u8 profile_effective, loid_status, voice_calls, pm_interval, alarm_sequence, veip_learned;
	u8 alarm_snapshot_count;
	struct omci_alarm_entry alarm_snapshot[OMCI_ALARM_SNAPSHOT_LEN];
	u64 pm_base[5], ber_base;
	unsigned long ber_since, pm_due, table_jiffies, reboot_due;
	u32 request_len, response_len, table_len;
	u16 table_class, table_entity, table_mask;
	u8 serial[8], password[10];
};

enum {
	CP_ENABLED = 1, CP_PERMISSIVE = 2, CP_FAKE = 4, CP_REBOOT = 8,
	CP_SW_DOWNLOAD = 16, CP_PM_RUNNING = 32, CP_BER_VALID = 64, CP_RESPONSE_FAKE = 128,
	CP_UNI_KNOWN = 256, CP_UNI_ENABLED = 512,
	CP_REBOOT_PENDING = 1024,
};

static u32 checksum(const void *data, size_t len)
{
	const u8 *p = data;
	u32 hash = 2166136261U;

	while (len--)
		hash = (hash ^ *p++) * 16777619U;
	return hash;
}

static int full_io(int fd, void *data, size_t len, bool writing)
{
	u8 *p = data;

	while (len) {
		ssize_t n = writing ? write(fd, p, len) : read(fd, p, len);

		if (n < 0)
			return -errno;
		if (!n)
			return -EIO;
		p += n;
		len -= n;
	}
	return 0;
}

int omcid_checkpoint_save(struct omcid *ctx)
{
	struct omci_agent *a = &ctx->engine.agent;
	struct checkpoint_header *h;
	struct omci_mib_object *object;
	unsigned long index;
	u8 *blob, *p;
	size_t size, count = 0;
	char temporary[256];
	int fd, ret;

	if (ctx->error)
		return ctx->error;
	if (ctx->restoring)
		return 0;
	ctx->save_due = 0;
	/* Keep the last accepted identity checkpoint until its retry succeeds. */
	if (ctx->pending_identity.valid)
		return 0;
	if (!ctx->checkpoint)
		return -EINVAL;
	xa_for_each(&a->mib, index, object)
		count++;
	if (count > 1024 || a->table_snapshot_len > CHECKPOINT_MAX_TABLE)
		return -E2BIG;
	size = sizeof(*h) + count * sizeof(*object) + a->table_snapshot_len;
	if (a->last_request)
		size += a->last_request->len;
	if (a->last_response)
		size += a->last_response->len;
	if (size > CHECKPOINT_MAX)
		return -E2BIG;
	blob = calloc(1, size);
	if (!blob)
		return -ENOMEM;
	h = (void *)blob;
	h->magic = CHECKPOINT_MAGIC;
	h->version = CHECKPOINT_VERSION;
	h->size = size;
	h->object_size = sizeof(*object);
	h->count = count;
	h->generation = ctx->transport.generation;
	h->config = a->config;
	h->topology = a->topology;
	h->mib_sync = a->mib_sync;
	h->uni_entity = ctx->uni_entity;
	h->upload_index = a->upload_index;
	h->profile_effective = a->profile_effective;
	h->profile_quirks = a->profile_quirks;
	h->veip_learned = a->veip_learned;
	h->loid_status = a->loid_status;
	h->voice_calls = a->voice_calls;
	h->pm_interval = a->pm_interval;
	h->alarm_sequence = a->alarm_sequence;
	h->alarm_snapshot_count = a->alarm_snapshot_count;
	memcpy(h->alarm_snapshot, a->alarm_snapshot, sizeof(h->alarm_snapshot));
	memcpy(h->pm_base, a->pm_base, sizeof(h->pm_base));
	h->ber_base = a->ber_base;
	h->ber_since = a->ber_since;
	h->pm_due = a->pm_work.due;
	h->reboot_due = a->reboot_work.due;
	h->table_jiffies = a->table_snapshot_jiffies;
	h->table_class = a->table_snapshot_class_id;
	h->table_entity = a->table_snapshot_entity_id;
	h->table_mask = a->table_snapshot_mask;
	h->request_len = a->last_request ? a->last_request->len : 0;
	h->response_len = a->last_response ? a->last_response->len : 0;
	h->table_len = a->table_snapshot_len;
	h->flags = (a->enabled ? CP_ENABLED : 0) | (a->permissive ? CP_PERMISSIVE : 0) |
		(a->fake_omci ? CP_FAKE : 0) | (a->reboot ? CP_REBOOT : 0) |
		(a->sw_download_seen ? CP_SW_DOWNLOAD : 0) | (a->pm_running ? CP_PM_RUNNING : 0) |
		(a->ber_valid ? CP_BER_VALID : 0) | (a->last_response_fake ? CP_RESPONSE_FAKE : 0);
	h->flags |= (ctx->uni_known ? CP_UNI_KNOWN : 0) | (ctx->uni_enabled ? CP_UNI_ENABLED : 0);
	if (a->reboot_work.pending)
		h->flags |= CP_REBOOT_PENDING;
	memcpy(h->serial, a->config.serial_number, sizeof(h->serial));
	memcpy(h->password, a->config.password, sizeof(h->password));
	p = blob + sizeof(*h);
	xa_for_each(&a->mib, index, object) {
		memcpy(p, object, sizeof(*object));
		p += sizeof(*object);
	}
	if (h->request_len) {
		memcpy(p, a->last_request->data, h->request_len);
		p += h->request_len;
	}
	if (h->response_len) {
		memcpy(p, a->last_response->data, h->response_len);
		p += h->response_len;
	}
	if (h->table_len)
		memcpy(p, a->table_snapshot, h->table_len);
	h->checksum = checksum(blob, size);
	if (snprintf(temporary, sizeof(temporary), "%s.new.%ld", ctx->checkpoint,
		     (long)getpid()) >= (int)sizeof(temporary)) {
		free(blob);
		return -ENAMETOOLONG;
	}
	fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		free(blob);
		return -errno;
	}
	ret = full_io(fd, blob, size, true);
	if (!ret && fsync(fd))
		ret = -errno;
	if (close(fd) && !ret)
		ret = -errno;
	if (!ret && rename(temporary, ctx->checkpoint))
		ret = -errno;
	if (ret)
		unlink(temporary);
	free(blob);
	return ret;
}

/*
 * Saving takes ~200 ms on this CPU, so an answer must not wait for it: save
 * once the OLT has been quiet for a second, and at least every 5 s while it
 * is not. A crash loses at most that much; the OLT's MIB data sync audit
 * then reprovisions.
 */
#define CHECKPOINT_IDLE_MS 1000
#define CHECKPOINT_MAX_DELAY_MS 5000

void omcid_checkpoint_defer(struct omcid *ctx)
{
	unsigned long now = omci_native_now();

	if (!ctx->save_due)
		ctx->save_first = now;
	ctx->save_due = min(now + CHECKPOINT_IDLE_MS,
			    ctx->save_first + CHECKPOINT_MAX_DELAY_MS);
}

/* Saves a due checkpoint; returns ms until the next one, or -1 for none. */
long omcid_checkpoint_poll(struct omcid *ctx, unsigned long now)
{
	int ret;

	if (!ctx->save_due)
		return -1;
	if ((long)(ctx->save_due - now) > 0)
		return ctx->save_due - now;
	ctx->save_due = 0;
	ret = omcid_checkpoint_save(ctx);
	if (ret)
		ctx->error = ret;
	return -1;
}

static bool object_valid(const u8 *raw)
{
	const struct omci_mib_object *o = (const void *)raw;
	static const size_t bool_offsets[] = {
		offsetof(struct omci_mib_object, pending_delete),
		offsetof(struct omci_mib_object, qos_configured),
		offsetof(struct omci_mib_object, arc_clean),
		offsetof(struct omci_mib_object, olt_g.vendor_id_valid),
		offsetof(struct omci_mib_object, olt_g.equipment_id_valid),
		offsetof(struct omci_mib_object, olt_g.version_valid),
		offsetof(struct omci_mib_object, olt_g.valid),
		offsetof(struct omci_mib_object, vlan_filter.valid),
		offsetof(struct omci_mib_object, extended_vlan.valid),
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(bool_offsets); i++)
		if (raw[bool_offsets[i]] > 1)
			return false;
	for (i = 0; i < OMCI_EXT_VLAN_MAX_RULES; i++)
		if (raw[offsetof(struct omci_mib_object, extended_vlan.rules) +
			i * sizeof(struct omci_extended_vlan_rule) +
			offsetof(struct omci_extended_vlan_rule, delete)] > 1)
			return false;
	return o->data_len <= OMCI_MAX_ATTR_DATA && o->origin <= OMCI_MIB_ORIGIN_HARDWARE &&
		o->vlan_filter.num_entries <= OMCI_VLAN_FILTER_MAX_ENTRIES &&
		o->extended_vlan.rule_count <= OMCI_EXT_VLAN_MAX_RULES &&
		!o->olt_g.vendor_id[OMCI_OLT_VENDOR_ID_LEN] &&
		!o->olt_g.equipment_id[OMCI_OLT_EQUIPMENT_ID_LEN] &&
		!o->olt_g.version[OMCI_OLT_VERSION_LEN];
}

int omcid_checkpoint_load(struct omcid *ctx)
{
	struct checkpoint_header *h;
	struct omci_agent *a = &ctx->engine.agent;
	struct xarray restored;
	struct sk_buff *request = NULL, *response = NULL;
	void *table = NULL;
	struct stat st;
	u8 *blob = NULL, *p;
	u32 digest;
	unsigned int i;
	int fd, ret;

	xa_init(&restored);
	fd = open(ctx->checkpoint, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & 0777) != 0600 || st.st_size < (off_t)sizeof(*h) ||
	    st.st_size > CHECKPOINT_MAX) {
		ret = -EPERM;
		goto out;
	}
	blob = malloc(st.st_size);
	if (!blob) {
		ret = -ENOMEM;
		goto out;
	}
	ret = full_io(fd, blob, st.st_size, false);
	if (ret)
		goto out;
	h = (void *)blob;
	digest = h->checksum;
	h->checksum = 0;
	if (h->magic != CHECKPOINT_MAGIC || h->version != CHECKPOINT_VERSION ||
	    h->size != st.st_size || h->object_size != sizeof(struct omci_mib_object) ||
	    h->count > 1024 || h->table_len > CHECKPOINT_MAX_TABLE ||
	    h->request_len > OMCI_MAX_PDU_LEN || h->response_len > OMCI_MAX_PDU_LEN ||
	    h->size != sizeof(*h) + h->count * sizeof(struct omci_mib_object) +
		 h->request_len + h->response_len + h->table_len ||
	    checksum(blob, st.st_size) != digest || (h->flags & ~2047) ||
	    h->alarm_snapshot_count > OMCI_ALARM_SNAPSHOT_LEN ||
	    !omci_profile_forceable(h->profile_effective) || !h->profile_effective ||
	    !omci_profile_valid(h->config.olt_profile) ||
	    !omci_profile_forceable(h->config.olt_profile_force) ||
	    h->profile_quirks != omci_profile_quirks(h->profile_effective) ||
	    h->config.traffic_mgmt_option > 2 ||
	    !h->config.uni_count || h->config.uni_count > 16 || h->config.onu_type > OMCI_ONU_TYPE_CBU ||
	    (!!h->request_len != !!h->response_len)) {
		ret = -EBADMSG;
		goto out;
	}
	if (memcmp(&h->topology, &a->topology, sizeof(h->topology)) ||
	    memcmp(h->serial, ctx->identity.serial_number, sizeof(h->serial)) ||
	    memcmp(h->password, ctx->identity.password, sizeof(h->password))) {
		ret = -EXDEV;
		goto out;
	}
	p = blob + sizeof(*h);
	for (i = 0; i < h->count; i++) {
		struct omci_mib_object *object;
		unsigned long key;

		if (!object_valid(p)) {
			ret = -EBADMSG;
			goto out;
		}
		object = kmemdup(p, sizeof(*object), 0);
		if (!object) {
			ret = -ENOMEM;
			goto out;
		}
		key = ((unsigned long)object->class_id << 16) | object->entity_id;
		if (xa_load(&restored, key)) {
			free(object);
			ret = -EBADMSG;
			goto out;
		}
		ret = xa_err(xa_store(&restored, key, object, 0));
		if (ret) {
			free(object);
			goto out;
		}
		p += sizeof(*object);
	}
	if (h->request_len) {
		struct omci_wire_request q, r;

		if (omci_wire_decode(p, h->request_len, &q) ||
		    omci_wire_decode(p + h->request_len, h->response_len, &r) ||
		    !(q.message_type & 0x40) || !(r.message_type & 0x20) ||
		    (q.message_type & 0x1f) != (r.message_type & 0x1f) ||
		    q.transaction_id != r.transaction_id || q.class_id != r.class_id ||
		    q.entity_id != r.entity_id || q.device_id != r.device_id) {
			ret = -EBADMSG;
			goto out;
		}
		request = alloc_skb(h->request_len, 0);
		response = alloc_skb(h->response_len, 0);
		if (!request || !response) {
			ret = -ENOMEM;
			goto out;
		}
		skb_put_data(request, p, h->request_len);
		p += h->request_len;
		skb_put_data(response, p, h->response_len);
		p += h->response_len;
	}
	if (h->table_len) {
		table = kmemdup(p, h->table_len, 0);
		if (!table) {
			ret = -ENOMEM;
			goto out;
		}
	}
	/* Validation/allocation completed before replacing the authoritative state. */
	while (a->mib.count)
		free(xa_erase(&a->mib, a->mib.entries[0].key));
	while (a->services.count)
		free(xa_erase(&a->services, a->services.entries[0].key));
	xa_destroy(&a->mib);
	a->mib = restored;
	xa_init(&restored);
	a->config = h->config;
	ctx->uni_known = !!(h->flags & CP_UNI_KNOWN);
	ctx->uni_enabled = !!(h->flags & CP_UNI_ENABLED);
	ctx->uni_entity = h->uni_entity;
	a->mib_sync = h->mib_sync;
	a->upload_index = h->upload_index;
	a->profile_effective = h->profile_effective;
	a->profile_quirks = h->profile_quirks;
	a->veip_learned = h->veip_learned;
	a->loid_status = h->loid_status;
	a->voice_calls = h->voice_calls;
	a->alarm_sequence = h->alarm_sequence;
	a->alarm_snapshot_count = h->alarm_snapshot_count;
	memcpy(a->alarm_snapshot, h->alarm_snapshot, sizeof(a->alarm_snapshot));
	a->pm_interval = h->pm_interval;
	memcpy(a->pm_base, h->pm_base, sizeof(a->pm_base));
	a->ber_base = h->ber_base;
	a->ber_since = h->ber_since;
	a->enabled = !!(h->flags & CP_ENABLED);
	a->permissive = !!(h->flags & CP_PERMISSIVE);
	a->fake_omci = !!(h->flags & CP_FAKE);
	a->reboot = !!(h->flags & CP_REBOOT);
	a->sw_download_seen = !!(h->flags & CP_SW_DOWNLOAD);
	a->pm_running = !!(h->flags & CP_PM_RUNNING);
	a->ber_valid = !!(h->flags & CP_BER_VALID);
	a->last_response_fake = !!(h->flags & CP_RESPONSE_FAKE);
	a->operational = false;
	{
		struct omci_uni_attrs *attrs;
		unsigned long index;

		xa_for_each(&a->uni_attrs, index, attrs) {
			xa_erase(&a->uni_attrs, index);
			kfree(attrs);
		}
	}
	kfree_skb(a->last_request);
	kfree_skb(a->last_response);
	a->last_request = request;
	a->last_response = response;
	a->last_request_hash = request ? jhash(request->data, request->len, 0) : 0;
	request = response = NULL;
	free(a->table_snapshot);
	a->table_snapshot = table;
	table = NULL;
	a->table_snapshot_len = h->table_len;
	a->table_snapshot_class_id = h->table_class;
	a->table_snapshot_entity_id = h->table_entity;
	a->table_snapshot_mask = h->table_mask;
	a->table_snapshot_jiffies = h->table_jiffies;
	if (a->pm_running) {
		a->pm_work.pending = true;
		a->pm_work.due = h->pm_due;
	}
	a->reboot_work.pending = !!(h->flags & CP_REBOOT_PENDING);
	a->reboot_work.due = h->reboot_due;
	if (h->generation != ctx->transport.generation) {
		kfree_skb(a->last_request);
		kfree_skb(a->last_response);
		a->last_request = a->last_response = NULL;
		free(a->table_snapshot);
		a->table_snapshot = NULL;
		a->table_snapshot_len = 0;
	}
	ret = 0;
out:
	close(fd);
	while (restored.count)
		free(xa_erase(&restored, restored.entries[0].key));
	xa_destroy(&restored);
	kfree_skb(request);
	kfree_skb(response);
	free(table);
	free(blob);
	return ret;
}
