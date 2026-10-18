// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "daemon.h"
/* Inspect private checkpoint fields to test checksummed-but-invalid input. */
#include "../src/checkpoint.c"

static unsigned int checks, writes, sent, resets;
static int hardware_error;
static u8 response[OMCI_MAX_PDU_LEN];
#define CHECK(v) do { assert(v); checks++; } while (0)

static int tx(struct omci_device *d, struct sk_buff *skb, u16 port)
{
	memcpy(response, skb->data, skb->len);
	sent++;
	kfree_skb(skb);
	return 0;
}

static int topology(struct omci_device *d, struct omci_ani_topology *t)
{
	*t = (struct omci_ani_topology) { .tcont_base = 0x8000, .scheduler_base = 0x8000,
		.queue_base = 0x8000, .tcont_count = 15, .queues_per_tcont = 8,
		.maximum_queue_size = 65535, .allocated_queue_size = 4,
		.queue_config_option = 1, .scheduler_policy = 1 };
	return hardware_error;
}

static int tcont(struct omci_device *d, u16 entity, u16 alloc, bool valid)
{
	writes++;
	return hardware_error;
}

static int telemetry(struct omci_device *d, struct omci_telemetry *t)
{
	*t = (struct omci_telemetry) { .valid = OMCI_TELEMETRY_F_BIP_ERRORS,
		.bip_errors = 0x100000002ULL };
	return hardware_error;
}

static int reset(struct omci_device *d) { resets++; return hardware_error; }
static int identity(struct omci_device *d, const struct omci_identity *id)
{
	CHECK(id->valid & OMCI_IDENTITY_F_SERIAL_NUMBER);
	return hardware_error;
}

static struct omci_device_ops ops = { .xmit = tx, .get_ani_topology = topology,
	.set_tcont = tcont, .get_telemetry = telemetry, .backend_reset = reset,
	.backend_identity = identity };

static void init(struct omcid *ctx, const char *path)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->identity = (struct omci_identity) { .valid = OMCI_IDENTITY_F_SERIAL_NUMBER |
		OMCI_IDENTITY_F_VENDOR_ID | OMCI_IDENTITY_F_PASSWORD,
		.serial_number = "TEST0001", .vendor_id = "TEST" };
	ctx->parent = (struct device) { .identity = &ctx->identity,
		.onu_type = OMCI_ONU_TYPE_SFU, .uni_count = 1 };
	ctx->engine.parent = &ctx->parent;
	ctx->engine.ops = &ops;
	ctx->engine.started = ctx->engine.channel_up = true;
	ctx->transport.generation = 7;
	ctx->checkpoint = path;
	CHECK(!omci_agent_init(&ctx->engine));
}

static void request(struct omcid *ctx, u16 tci, u8 action, u16 class, u16 entity,
		    const void *data, size_t len)
{
	u8 pdu[44] = {};

	put_unaligned_be16(tci, pdu);
	pdu[2] = action | 0x40;
	pdu[3] = 0x0a;
	put_unaligned_be16(class, pdu + 4);
	put_unaligned_be16(entity, pdu + 6);
	if (len)
		memcpy(pdu + 8, data, len);
	CHECK(!omci_native_receive(&ctx->engine, pdu, sizeof(pdu)));
}

static void corrupt(const char *path, unsigned int kind)
{
	struct checkpoint_header *h;
	struct omci_mib_object *object;
	struct stat st;
	u8 *data;
	int fd = open(path, O_RDWR);

	CHECK(fd >= 0 && !fstat(fd, &st));
	data = malloc(st.st_size);
	CHECK(data && read(fd, data, st.st_size) == st.st_size);
	h = (void *)data;
	object = (void *)(data + sizeof(*h));
	if (kind == 0)
		data[st.st_size - 1] ^= 1;
	else {
		if (kind == 1)
			data[sizeof(*h) + offsetof(struct omci_mib_object, pending_delete)] = 2;
		if (kind == 2)
			memcpy(object + 1, object, sizeof(*object));
		if (kind == 3)
			h->profile_quirks ^= 1;
		if (kind == 4)
			h->count = UINT32_MAX;
		if (kind == 5)
			h->size--;
		if (kind == 6 && h->response_len)
			data[sizeof(*h) + h->count * sizeof(*object) + h->request_len] ^= 1;
		h->checksum = 0;
		h->checksum = checksum(data, st.st_size);
	}
	CHECK(lseek(fd, 0, SEEK_SET) == 0 && write(fd, data, st.st_size) == st.st_size);
	CHECK(!close(fd));
	free(data);
}

static void backend_tests(struct omcid *ctx)
{
	u8 output[OMCI_BACKEND_MAX_DATA] __attribute__((aligned(8)));
	struct omci_backend_tcont t = { 0x8000, 256, 1 };
	struct omci_backend_identity id = { .valid = OMCI_BACKEND_ID_SERIAL,
		.serial = "TEST0001" };
	struct omci_device_ops missing = {};
	size_t len;
	unsigned int op, i, before = writes;
	const size_t sizes[] = { 0, 0, sizeof(t), sizeof(struct omci_backend_gem),
		sizeof(struct omci_backend_qos), sizeof(struct omci_backend_rate),
		sizeof(struct omci_backend_uni), sizeof(struct omci_backend_uni_attrs),
		sizeof(struct omci_backend_service), sizeof(struct omci_backend_cookie),
		0, sizeof(struct omci_backend_link), sizeof(struct omci_backend_profile),
		sizeof(struct omci_backend_operational), sizeof(id), 0 };
	u8 input[OMCI_BACKEND_MAX_DATA] __attribute__((aligned(8))) = {};

	for (op = 1; op <= OMCI_BACKEND_RESET; op++)
		for (i = 0; i < sizeof(input); i++)
			if (i != sizes[op])
				CHECK(omci_backend_execute(&ctx->engine, op, input, i, output, &len) == -EINVAL);
	CHECK(writes == before);
	CHECK(!omci_backend_execute(&ctx->engine, OMCI_BACKEND_TCONT, &t, sizeof(t), output, &len));
	CHECK(!len && writes == before + 1);
	t.alloc_id = 65535;
	CHECK(!omci_backend_execute(&ctx->engine, OMCI_BACKEND_TCONT, &t, sizeof(t), output, &len));
	t.alloc_id = 4096;
	CHECK(omci_backend_execute(&ctx->engine, OMCI_BACKEND_TCONT, &t, sizeof(t), output, &len) == -ERANGE);
	CHECK(!omci_backend_execute(&ctx->engine, OMCI_BACKEND_RESET, NULL, 0, output, &len));
	CHECK(resets == 1);
	CHECK(!omci_backend_execute(&ctx->engine, OMCI_BACKEND_IDENTITY, &id, sizeof(id), output, &len));
	CHECK(!memcmp(ctx->engine.agent.config.serial_number, "TEST0001", 8));
	hardware_error = -EIO;
	CHECK(omci_backend_execute(&ctx->engine, OMCI_BACKEND_RESET, NULL, 0, output, &len) == -EIO);
	CHECK(omci_backend_execute(&ctx->engine, OMCI_BACKEND_TELEMETRY, NULL, 0, output, &len) == -EIO);
	hardware_error = 0;
	CHECK(!omci_backend_execute(&ctx->engine, OMCI_BACKEND_TELEMETRY, NULL, 0, output, &len));
	CHECK(len == sizeof(struct omci_backend_telemetry));
	CHECK(((struct omci_backend_telemetry *)output)->bip_errors == 0x100000002ULL);
	ctx->engine.ops = &missing;
	CHECK(omci_backend_execute(&ctx->engine, OMCI_BACKEND_RESET, NULL, 0, output, &len) == -EOPNOTSUPP);
	CHECK(omci_backend_execute(&ctx->engine, 65535, NULL, 0, output, &len) == -EOPNOTSUPP);
	ctx->engine.ops = &ops;
}

int main(void)
{
	char directory[] = "/tmp/omcid-recovery-XXXXXX", path[128];
	struct omcid original, restored;
	struct omci_mib_object object;
	struct omcid_request q;
	struct omcid_message reply;
	const struct nlattr *tb[OMCI_ATTR_MAX + 1];
	u8 data[] = { 0x80, 0, 1, 0 };
	u16 class = OMCI_CLASS_TCONT, entity = 0x8000;
	unsigned int i, before;
	size_t count;
	int fd;

	CHECK(mkdtemp(directory));
	snprintf(path, sizeof(path), "%s/state", directory);
	init(&original, path);
	backend_tests(&original);
	request(&original, 1, 8, class, entity, data, sizeof(data));
	CHECK(!response[8]);
	request(&original, 2, 9, OMCI_CLASS_OMCI, 0, data, 2);
	CHECK(original.engine.agent.table_snapshot_len);
	original.uni_known = original.uni_enabled = true;
	original.uni_entity = 0x101;
	CHECK(!omcid_checkpoint_save(&original));
	init(&restored, path);
	CHECK(!omcid_checkpoint_load(&restored));
	CHECK(restored.engine.agent.mib.count == original.engine.agent.mib.count);
	CHECK(restored.engine.agent.table_snapshot_len == original.engine.agent.table_snapshot_len);
	CHECK(restored.uni_known && restored.uni_enabled && restored.uni_entity == 0x101);
	CHECK(restored.engine.agent.last_request && restored.engine.agent.last_response);
	before = writes;
	request(&restored, 2, 9, OMCI_CLASS_OMCI, 0, data, 2);
	CHECK(writes == before && restored.engine.agent.duplicates == 1);
	CHECK(!omci_agent_mib_get(&restored.engine, class, entity, &object));
	CHECK(get_unaligned_be16(object.data) == 256);
	count = restored.engine.agent.mib.count;
	for (i = 0; i < 7; i++) {
		CHECK(!omcid_checkpoint_save(&original));
		corrupt(path, i);
		CHECK(omcid_checkpoint_load(&restored) == -EBADMSG);
		CHECK(restored.engine.agent.mib.count == count);
	}
	CHECK(!omcid_checkpoint_save(&original));
	restored.identity.serial_number[7] ^= 1;
	CHECK(omcid_checkpoint_load(&restored) == -EXDEV);
	restored.identity.serial_number[7] ^= 1;
	CHECK(!chmod(path, 0644));
	CHECK(omcid_checkpoint_load(&restored) == -EPERM);
	CHECK(!chmod(path, 0600));
	restored.transport.generation++;
	CHECK(!omcid_checkpoint_load(&restored));
	CHECK(!restored.engine.agent.last_request && !restored.engine.agent.table_snapshot);
	CHECK(!omci_agent_mib_get(&restored.engine, class, entity, &object));
	CHECK(get_unaligned_be16(object.data) == 256);
	CHECK(!omci_device_reconcile_services(&restored.engine));
	omcid_request_init(&q, 37, OMCI_CMD_GET);
	CHECK(!omcid_control(&restored, &q.header, &reply));
	CHECK(!omcid_parse((void *)reply.data, tb, OMCI_ATTR_MAX));
	CHECK(tb[OMCI_ATTR_LOID_STATUS] && !tb[OMCI_ATTR_ALARM_TEXT]);
	{
		u8 diagnostics = 1;

		CHECK(!omcid_put(&q, OMCI_ATTR_DIAGNOSTICS, &diagnostics, sizeof(diagnostics)));
		CHECK(!omcid_control(&restored, &q.header, &reply));
		CHECK(!omcid_parse((void *)reply.data, tb, OMCI_ATTR_MAX));
		CHECK(tb[OMCI_ATTR_ALARM_TEXT]);
	}
	omcid_request_init(&q, 37, OMCI_CMD_MIB_GET);
	CHECK(!omcid_put(&q, OMCI_ATTR_CLASS_ID, &class, sizeof(class)));
	CHECK(!omcid_put(&q, OMCI_ATTR_ENTITY_ID, &entity, sizeof(entity)));
	CHECK(!omcid_control(&restored, &q.header, &reply));
	CHECK(!omcid_parse((void *)reply.data, tb, OMCI_ATTR_MAX));
	CHECK(tb[OMCI_ATTR_ATTR_DATA] && get_unaligned_be16(omcid_data(tb[OMCI_ATTR_ATTR_DATA])) == 256);
	q.generic.cmd = OMCI_CMD_BACKEND;
	CHECK(omcid_control(&restored, &q.header, &reply) == -EOPNOTSUPP);
	q.generic.version--;
	CHECK(omcid_control(&restored, &q.header, &reply) == -EPROTONOSUPPORT);
	fd = open(path, O_WRONLY | O_TRUNC);
	CHECK(fd >= 0 && write(fd, "short", 5) == 5 && !close(fd));
	CHECK(omcid_checkpoint_load(&restored) == -EPERM);
	CHECK(!unlink(path) && !symlink("/dev/null", path));
	CHECK(omcid_checkpoint_load(&restored) == -ELOOP);
	CHECK(!unlink(path) && !rmdir(directory));
	omci_agent_cleanup(&original.engine);
	omci_agent_cleanup(&restored.engine);
	printf("%u typed backend/checkpoint/recovery/control checks passed\n", checks);
	return 0;
}
