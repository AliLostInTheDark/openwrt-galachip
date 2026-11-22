// SPDX-License-Identifier: GPL-2.0-only
/*
 * The fields of each typed backend operation and their netlink attributes.
 * Shared unchanged between the kernel and omcid.
 */
#ifdef __KERNEL__
#include <linux/array_size.h>
#include <linux/stddef.h>
#include <uapi/linux/omci-backend.h>
#include <net/xpon/omci.h>
#else
#include "native.h"
#include <linux/omci-backend.h>
#endif

#define FIELD(a, s, m, t, l) \
	{ OMCI_BACKEND_A_##a, offsetof(struct omci_backend_##s, m), OMCI_BACKEND_FIELD_##t, l }
#define U32(a, s, m)	FIELD(a, s, m, U32, 4)
#define U64(a, s, m)	FIELD(a, s, m, U64, 8)
#define BIN(a, s, m)	FIELD(a, s, m, BINARY, sizeof_field(struct omci_backend_##s, m))
#define FIELDS(s, f)	{ f, ARRAY_SIZE(f), sizeof(struct omci_backend_##s) }

static const struct omci_backend_field topology_fields[] = {
	U32(TCONT_BASE, topology, tcont_base),
	U32(SCHEDULER_BASE, topology, scheduler_base),
	U32(QUEUE_BASE, topology, queue_base),
	U32(MAX_QUEUE_SIZE, topology, maximum_queue_size),
	U32(ALLOCATED_QUEUE_SIZE, topology, allocated_queue_size),
	U32(TCONT_COUNT, topology, tcont_count),
	U32(QUEUES_PER_TCONT, topology, queues_per_tcont),
	U32(QUEUE_CONFIG_OPTION, topology, queue_config_option),
	U32(SCHEDULER_POLICY, topology, scheduler_policy),
	U32(GEM_PORT_COUNT, topology, gem_port_count),
	U32(QOS_CONFIGURATION, topology, qos_configuration),
	U32(DOWN_QUEUES_PER_UNI, topology, downstream_queues_per_uni),
	U32(FLAGS, topology, flags),
};

static const struct omci_backend_field tcont_fields[] = {
	U32(ENTITY, tcont, entity),
	U32(ALLOC_ID, tcont, alloc_id),
	U32(VALID, tcont, valid),
};

static const struct omci_backend_field gem_fields[] = {
	U32(ENTITY, gem, entity),
	U32(GEM_PORT, gem, gem_port),
	U32(TCONT, gem, tcont),
	U32(DIRECTION, gem, direction),
	U32(VALID, gem, valid),
	U32(ENCRYPTED, gem, encrypted),
};

static const struct omci_backend_field qos_fields[] = {
	U32(ENTITY, qos, entity),
	U32(ENABLED, qos, enabled),
	U32(POLICY, qos, policy),
	BIN(WEIGHT, qos, weight),
};

static const struct omci_backend_field rate_fields[] = {
	U32(GEM_PORT, rate, gem_port),
	U32(UPSTREAM, rate, upstream),
	U32(RATE, rate, rate),
	U32(BURST, rate, burst),
};

static const struct omci_backend_field uni_fields[] = {
	U32(ENTITY, uni, entity),
	U32(ENABLED, uni, enabled),
};

static const struct omci_backend_field uni_attrs_fields[] = {
	U32(ENTITY, uni_attrs, entity),
	U32(PAUSE_QUANTA, uni_attrs, pause_quanta),
	U32(AGEING_S, uni_attrs, ageing_s),
	U32(LEARN_LIMIT, uni_attrs, learn_limit),
	U32(DOWN_KBPS, uni_attrs, down_kbps),
	U32(UP_KBPS, uni_attrs, up_kbps),
	U32(AUTO_DETECTION, uni_attrs, auto_detection),
	U32(MAX_FRAME_SIZE, uni_attrs, max_frame_size),
};

static const struct omci_backend_field service_fields[] = {
	U32(COOKIE, service, cookie),
	U32(UNI_ENTITY, service, uni_entity),
	U32(GEM_ENTITY, service, gem_entity),
	U32(GEM_PORT, service, gem_port),
	U32(TCONT_ENTITY, service, tcont_entity),
	U32(ALLOC_ID, service, alloc_id),
	U32(VLAN, service, vlan),
	U32(PCP, service, pcp),
	U32(QUEUE, service, queue),
	U32(DIRECTION, service, direction),
	U32(UNI_MODE, service, uni_mode),
	U32(UNI_VLAN, service, uni_vlan),
	U32(MULTICAST_ANI, service, multicast_ani),
	U32(FLAGS, service, flags),
	BIN(TREATMENT, service, treatment),
};

static const struct omci_backend_field cookie_fields[] = {
	U32(COOKIE, cookie, cookie),
};

static const struct omci_backend_field link_fields[] = {
	U32(ENTITY, link, entity),
	U32(UP, link, up),
};

static const struct omci_backend_field profile_fields[] = {
	U32(PROFILE_CONFIGURED, profile, configured),
	U32(PROFILE_EFFECTIVE, profile, effective),
	U32(PROFILE_FORCED, profile, forced),
	U32(PROFILE_QUIRKS, profile, quirks),
};

static const struct omci_backend_field operational_fields[] = {
	U32(OPERATIONAL, operational, operational),
};

static const struct omci_backend_field telemetry_fields[] = {
	U32(VALID, telemetry, valid),
	FIELD(TEMPERATURE_MC, telemetry, temperature_mc, S32, 4),
	U32(VOLTAGE_UV, telemetry, voltage_uv),
	U32(BIAS_UA, telemetry, bias_ua),
	U32(TX_POWER_NW, telemetry, tx_power_nw),
	U32(RX_POWER_NW, telemetry, rx_power_nw),
	U32(ALARMS, telemetry, alarms),
	U32(DOWN_FEC, telemetry, downstream_fec),
	U32(UP_FEC, telemetry, upstream_fec),
	U64(BIP_ERRORS, telemetry, bip_errors),
	U64(FEC_CORRECTED_BYTES, telemetry, fec[0]),
	U64(FEC_CORRECTED_CODEWORDS, telemetry, fec[1]),
	U64(FEC_UNCORRECTABLE, telemetry, fec[2]),
	U64(FEC_TOTAL_CODEWORDS, telemetry, fec[3]),
	U64(FEC_SECONDS, telemetry, fec[4]),
};

static const struct omci_backend_field identity_fields[] = {
	U32(VALID, identity, valid),
	BIN(SERIAL, identity, serial),
	BIN(PASSWORD, identity, password),
};

static const struct omci_backend_layout omci_backend_layouts[] = {
	[OMCI_BACKEND_RESET] = { },
	[OMCI_BACKEND_IDENTITY] = { .in = FIELDS(identity, identity_fields) },
	[OMCI_BACKEND_TOPOLOGY] = { .out = FIELDS(topology, topology_fields) },
	[OMCI_BACKEND_TCONT] = { .in = FIELDS(tcont, tcont_fields) },
	[OMCI_BACKEND_GEM] = { .in = FIELDS(gem, gem_fields) },
	[OMCI_BACKEND_TCONT_QOS] = { .in = FIELDS(qos, qos_fields) },
	[OMCI_BACKEND_GEM_RATE] = { .in = FIELDS(rate, rate_fields) },
	[OMCI_BACKEND_UNI] = { .in = FIELDS(uni, uni_fields) },
	[OMCI_BACKEND_UNI_ATTRS] = { .in = FIELDS(uni_attrs, uni_attrs_fields) },
	[OMCI_BACKEND_SERVICE] = { .in = FIELDS(service, service_fields) },
	[OMCI_BACKEND_SERVICE_DELETE] = { .in = FIELDS(cookie, cookie_fields) },
	[OMCI_BACKEND_TELEMETRY] = { .out = FIELDS(telemetry, telemetry_fields) },
	[OMCI_BACKEND_UNI_LINK] = { .in = FIELDS(link, link_fields),
				    .out = FIELDS(link, link_fields) },
	[OMCI_BACKEND_PROFILE] = { .in = FIELDS(profile, profile_fields) },
	[OMCI_BACKEND_OPERATIONAL] = { .in = FIELDS(operational, operational_fields) },
};

/* NULL for an operation this agent does not know */
const struct omci_backend_layout *omci_backend_layout(u16 op)
{
	if (!op || op >= ARRAY_SIZE(omci_backend_layouts))
		return NULL;
	return &omci_backend_layouts[op];
}
