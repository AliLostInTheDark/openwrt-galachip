/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_OMCI_BACKEND_H
#define _UAPI_LINUX_OMCI_BACKEND_H
#include <linux/types.h>

/*
 * Native-endian netlink payloads, fixed-width fields, no pointers or C bools.
 * Every opcode requires its exact payload length; unknown opcodes fail.
 */
enum omci_backend_op {
	OMCI_BACKEND_TOPOLOGY = 1,
	OMCI_BACKEND_TCONT,
	OMCI_BACKEND_GEM,
	OMCI_BACKEND_TCONT_QOS,
	OMCI_BACKEND_GEM_RATE,
	OMCI_BACKEND_UNI,
	OMCI_BACKEND_UNI_ATTRS,
	OMCI_BACKEND_SERVICE,
	OMCI_BACKEND_SERVICE_DELETE,
	OMCI_BACKEND_TELEMETRY,
	OMCI_BACKEND_UNI_LINK,
	OMCI_BACKEND_PROFILE,
	OMCI_BACKEND_OPERATIONAL,
	OMCI_BACKEND_IDENTITY,
	OMCI_BACKEND_RESET,
};

struct omci_backend_topology {
	__u32 tcont_base, scheduler_base, queue_base;
	__u32 maximum_queue_size, allocated_queue_size;
	__u32 tcont_count, queues_per_tcont, queue_config_option, scheduler_policy;
	__u32 gem_port_count, qos_configuration, downstream_queues_per_uni, flags;
};

struct omci_backend_tcont {
	__u32 entity, alloc_id, valid;
};

struct omci_backend_gem {
	__u32 entity, gem_port, tcont, direction, valid, encrypted;
};

struct omci_backend_qos {
	__u32 entity, enabled, policy; __u8 weight[8];
};

struct omci_backend_rate {
	__u32 gem_port, upstream, rate, burst;
};

struct omci_backend_uni {
	__u32 entity, enabled;
};

struct omci_backend_uni_attrs {
	__u32 entity, pause_quanta, ageing_s, learn_limit, down_kbps, up_kbps;
	__u32 auto_detection, max_frame_size;
};

#define OMCI_BACKEND_SVC_VLAN (1U << 0)
#define OMCI_BACKEND_SVC_PCP (1U << 1)
#define OMCI_BACKEND_SVC_TREATMENT (1U << 2)
#define OMCI_BACKEND_SVC_MULTICAST_ANI (1U << 3)
#define OMCI_BACKEND_SVC_MULTICAST (1U << 4)
#define OMCI_BACKEND_SVC_DEFAULT (1U << 5)
struct omci_backend_service {
	__u32 cookie, uni_entity, gem_entity, gem_port, tcont_entity, alloc_id;
	__u32 vlan, pcp, queue, direction, uni_mode, uni_vlan, multicast_ani, flags;
	__u8 treatment[8];
};

struct omci_backend_cookie {
	__u32 cookie;
};

struct omci_backend_link {
	__u32 entity, up;
};

struct omci_backend_profile {
	__u32 configured, effective, forced, quirks;
};

struct omci_backend_operational {
	__u32 operational;
};

struct omci_backend_telemetry {
	__u32 valid;
	__s32 temperature_mc;
	__u32 voltage_uv, bias_ua, tx_power_nw, rx_power_nw, alarms;
	__u32 downstream_fec, upstream_fec, reserved;
	__aligned_u64 bip_errors;
	__aligned_u64 fec[5];
};

#define OMCI_BACKEND_MAX_DATA 96

#define OMCI_BACKEND_ID_SERIAL (1U << 0)
#define OMCI_BACKEND_ID_PASSWORD (1U << 1)
struct omci_backend_identity {
	__u32 valid;
	__u8 serial[8];
	__u8 password[10];
	__u8 reserved[2];
};
#endif
