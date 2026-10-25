/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_OMCI_H
#define _NET_OMCI_H

#ifdef __KERNEL__
#include <linux/bits.h>
#include <linux/types.h>
#include <uapi/linux/omci.h>
#else
#include <linux/omci.h>
#endif
#include <linux/omci-protocol.h>

struct device;
struct omci_device;
struct xpon_device;
struct sk_buff;

#define OMCI_IDENTITY_F_SERIAL_NUMBER	BIT(0)
#define OMCI_IDENTITY_F_VENDOR_ID	BIT(1)
#define OMCI_IDENTITY_F_PASSWORD	BIT(2)
#define OMCI_IDENTITY_F_VERSION	BIT(3)
#define OMCI_IDENTITY_F_EQUIPMENT_ID	BIT(4)
#define OMCI_IDENTITY_F_LOID		BIT(5)
#define OMCI_IDENTITY_F_LOID_PASSWORD	BIT(6)

/**
 * struct omci_identity - normalized ONU identity and registration data
 * @valid: OMCI_IDENTITY_F_* bitmap
 * @serial_number: canonical eight-byte GPON serial number
 * @vendor_id: canonical four-byte vendor identifier
 * @password: canonical ten-byte GPON registration password
 * @version: ONU-G version field
 * @equipment_id: ONU2-G equipment identifier
 * @loid: ONU-G logical ONU ID (LOID), separate from the GPON serial number
 * @loid_password: ONU-G logical password, separate from the GPON password
 * @serial_source: enum omci_config_source for @serial_number
 * @vendor_source: enum omci_config_source for @vendor_id
 * @password_source: enum omci_config_source for @password
 * @version_source: enum omci_config_source for @version
 * @equipment_source: enum omci_config_source for @equipment_id
 * @loid_source: enum omci_config_source for @loid
 * @loid_password_source: enum omci_config_source for @loid_password
 */
struct omci_identity {
	u32 valid;
	u8 serial_number[8];
	u8 vendor_id[OMCI_OLT_VENDOR_ID_LEN];
	u8 password[10];
	u8 version[OMCI_OLT_VERSION_LEN];
	u8 equipment_id[OMCI_OLT_EQUIPMENT_ID_LEN];
	u8 loid[OMCI_LOID_LEN];
	u8 loid_password[OMCI_LOID_PASSWORD_LEN];
	u8 serial_source;
	u8 vendor_source;
	u8 password_source;
	u8 version_source;
	u8 equipment_source;
	u8 loid_source;
	u8 loid_password_source;
};

enum omci_gem_port_direction {
	OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI = 1,
	OMCI_GEM_PORT_DIRECTION_ANI_TO_UNI = 2,
	OMCI_GEM_PORT_DIRECTION_BIDIRECTIONAL = 3,
};

/**
 * struct omci_olt_profile_state - resolved OLT interoperability policy
 * @configured: user-selected profile, including auto
 * @effective: active concrete profile
 * @forced: forced concrete profile, or OMCI_OLT_PROFILE_UNSPEC
 * @quirks: OMCI_OLT_QUIRK_* bitmap for @effective
 * @olt: latest parsed OLT-G identity
 */
struct omci_olt_profile_state {
	u8 configured;
	u8 effective;
	u8 forced;
	u32 quirks;
	struct omci_olt_g olt;
};




/**
 * struct omci_telemetry - current PON and optical telemetry
 * @valid: OMCI_TELEMETRY_F_* bitmap
 * @bosa_temperature_mc: BOSA temperature in milli-degrees Celsius
 * @bosa_voltage_uv: optical frontend supply voltage in microvolts
 * @bosa_bias_ua: laser bias current in microamps
 * @bosa_tx_power_nw: transmitted optical power in nanowatts
 * @bosa_rx_power_nw: received optical power in nanowatts
 * @bosa_alarms: OPTICAL_FRONTEND_ALARM_* bitmap
 * @bip_errors: downstream BIP errors counted since boot, only counted in O5
 * @downstream_fec: enum omci_fec_status
 * @upstream_fec: enum omci_fec_status
 */
struct omci_telemetry {
	u32 valid;
	s32 bosa_temperature_mc;
	u32 bosa_voltage_uv;
	u32 bosa_bias_ua;
	u32 bosa_tx_power_nw;
	u32 bosa_rx_power_nw;
	u32 bosa_alarms;
	u64 bip_errors;
	/* Downstream FEC totals since boot, for FEC PM history data */
	u64 fec_corrected_bytes;
	u64 fec_corrected_codewords;
	u64 fec_uncorrectable_codewords;
	u64 fec_total_codewords;
	u64 fec_seconds;
	u8 downstream_fec;
	u8 upstream_fec;
};

/**
 * struct omci_ani_topology - pre-existing upstream traffic resources
 * @tcont_base: first T-CONT managed entity ID
 * @scheduler_base: first Traffic Scheduler managed entity ID
 * @queue_base: first Priority Queue managed entity ID
 * @maximum_queue_size: maximum queue size reported in 2048-byte blocks
 * @allocated_queue_size: initial allocated queue size in 2048-byte blocks
 * @tcont_count: number of upstream T-CONT resources
 * @queues_per_tcont: number of upstream priority queues per T-CONT
 * @queue_config_option: Priority Queue configuration option
 * @scheduler_policy: initial Traffic Scheduler policy
 *
 * The OMCI agent uses this description to seed complete Priority Queue and
 * Traffic Scheduler managed entities before the first MIB upload. Entity IDs
 * are allocated contiguously. Queue entity IDs are grouped by T-CONT.
 */
/**
 * struct omci_service_config - normalized upstream OMCI service
 * @cookie: stable identifier used to replace or delete one service rule
 * @uni_entity_id: normalized PPTP Ethernet UNI or VEIP entity
 * @gem_ctp_entity_id: GEM port network CTP managed entity
 * @gem_port_id: GEM port identifier programmed in the GPON MAC
 * @tcont_entity_id: T-CONT managed entity associated with the GEM port
 * @alloc_id: Alloc-ID of that T-CONT, so a backend whose entity map was
 *	cleared by a GPON restart can still resolve the channel
 * @vlan_id: VLAN identifier used for upstream classification
 * @pcp: IEEE 802.1p priority used for classification and queue selection
 * @queue: hardware upstream queue
 * @direction: enum omci_gem_port_direction
 * @vlan_treatment: raw class 171 treatment words for the backend
 * @multicast_ani_entity_id: Dasan WAN bridge port used as multicast ANI
 * @vlan_valid: @vlan_id participates in classification
 * @pcp_valid: @pcp participates in classification
 * @vlan_treatment_valid: @vlan_treatment requires backend processing
 * @multicast_ani_valid: @multicast_ani_entity_id was profile-resolved
 * @multicast: service represents a multicast GEM path
 * @default_service: fallback service when no VLAN/PCP rule matches
 */
/*
 * What the service's VLAN looks like on the UNI side, from the class 171
 * rule: the network (PON) side always carries @vlan_id.
 */
enum omci_uni_vlan_mode {
	OMCI_UNI_VLAN_SAME,		/* the UNI carries @vlan_id too */
	OMCI_UNI_VLAN_UNTAGGED,		/* untagged on the UNI, tagged upstream */
	OMCI_UNI_VLAN_TRANSLATE,	/* @uni_vlan_id on the UNI */
	OMCI_UNI_VLAN_STACKED,		/* @vlan_id stacked over @uni_vlan_id */
};

struct omci_service_config {
	u32 cookie;
	u16 uni_entity_id;
	u16 gem_ctp_entity_id;
	u16 gem_port_id;
	u16 tcont_entity_id;
	u16 alloc_id;
	u16 vlan_id;
	u8 pcp;
	u8 queue;
	u8 direction;
	u8 vlan_treatment[8];
	u8 uni_vlan_mode;		/* enum omci_uni_vlan_mode */
	u16 uni_vlan_id;
	u16 multicast_ani_entity_id;
	bool vlan_valid;
	bool pcp_valid;
	bool vlan_treatment_valid;
	bool multicast_ani_valid;
	bool multicast;
	bool default_service;
};

struct omci_ani_topology {
	u16 tcont_base;
	u16 scheduler_base;
	u16 queue_base;
	u16 maximum_queue_size;
	u16 allocated_queue_size;
	u8 tcont_count;
	u8 queues_per_tcont;
	u8 queue_config_option;
	u8 scheduler_policy;
};

/**
 * struct omci_uni_attrs - G.988 attributes the OLT sets for an Ethernet UNI
 * @pause_quanta: PPTP Ethernet UNI (ME 11) pause time, in pause quanta of
 *	512 bit times; 0 disables pause frames
 * @ageing_s: MAC bridge service profile (ME 45) dynamic filtering ageing
 *	time in seconds; 0 selects the default
 * @learn_limit: MAC learning depth of the UNI bridge port (ME 47), else of
 *	its bridge (ME 45); 0 means no limit
 * @down_kbps: peak rate of the bridge port's outbound traffic descriptor
 *	(ME 280, towards the UNI) in kbit/s; 0 means unlimited
 * @up_kbps: peak rate of its inbound traffic descriptor, in kbit/s
 * @auto_detection: G.988 PPTP Ethernet UNI auto-detection configuration
 * @max_frame_size: maximum Ethernet frame size; zero restores the board default
 */
struct omci_uni_attrs {
	u16 pause_quanta;
	u32 ageing_s;
	u16 learn_limit;
	u32 down_kbps;
	u32 up_kbps;
	u8 auto_detection;
	u16 max_frame_size;
};

struct omci_tcont_qos {
	bool enabled;
	u8 policy;		/* G.988: 1 = strict priority, 2 = WRR */
	u8 weight[8];		/* indexed by related-port priority */
};

/**
 * struct omci_device_ops - hardware transport and provisioning operations
 * @backend_begin: acquire nonblocking session serialization
 * @backend_end: release session serialization
 * @backend_identity: apply registration identity with session serialization held
 * @backend_reset: clear OMCI-owned resources, preserving PLOAM and the OMCC
 * @start: acquire and start the OMCI transport independently of netdev state
 * @stop: stop and release the OMCI transport
 * @xmit: transmit an OMCI PDU; consumes @skb only on success
 * @get_ani_topology: describe pre-existing ANI scheduling resources
 * @set_tcont: configure a T-CONT mapping
 * @set_tcont_qos: apply explicitly configured T-CONT scheduling
 * @set_gem_port: configure a GEM port
 * @set_gem_rate: apply a GEM's peak-rate descriptor, in bytes/s and bytes
 * @set_uni: enable or disable a UNI
 * @set_uni_attrs: apply a UNI's pause, MAC table and rate attributes
 * @replace_service: atomically add or replace a normalized upstream service
 * @delete_service: remove a normalized upstream service by cookie
 * @get_telemetry: refresh PON FEC and optical telemetry
 * @get_uni_link: report whether a PPTP Ethernet UNI has carrier
 * @set_olt_profile: apply a resolved OLT interoperability profile
 * @set_operational: report whether the active OMCI agent is operational
 * @config_changed: report a normalized runtime OMCI configuration change
 */
struct omci_device_ops {
	/* Nonblocking session serialization for netlink provisioning. */
	int (*backend_begin)(struct omci_device *odev);
	void (*backend_end)(struct omci_device *odev);
	int (*backend_identity)(struct omci_device *odev,
				const struct omci_identity *identity);
	int (*backend_reset)(struct omci_device *odev);
	int (*start)(struct omci_device *odev);
	void (*stop)(struct omci_device *odev);
	int (*xmit)(struct omci_device *odev, struct sk_buff *skb,
		    u16 gem_port_id);
	int (*get_ani_topology)(struct omci_device *odev,
				struct omci_ani_topology *topology);
	int (*set_tcont)(struct omci_device *odev, u16 entity_id,
			 u16 alloc_id, bool valid);
	int (*set_tcont_qos)(struct omci_device *odev, u16 entity_id,
			     const struct omci_tcont_qos *qos);
	int (*set_gem_port)(struct omci_device *odev, u16 entity_id,
			    u16 gem_port_id, u16 tcont_entity_id,
			    u8 direction, bool valid, bool encrypted);
	int (*set_gem_rate)(struct omci_device *odev, u16 gem_port_id,
			    bool upstream, u32 rate, u32 burst);
	int (*set_uni)(struct omci_device *odev, u16 entity_id, bool enable);
	int (*set_uni_attrs)(struct omci_device *odev, u16 entity_id,
			     const struct omci_uni_attrs *attrs);
	int (*replace_service)(struct omci_device *odev,
			       const struct omci_service_config *service);
	int (*delete_service)(struct omci_device *odev, u32 cookie);
	int (*get_telemetry)(struct omci_device *odev,
			     struct omci_telemetry *telemetry);
	int (*get_uni_link)(struct omci_device *odev, u16 entity_id,
			    bool *link_up);
	int (*set_olt_profile)(struct omci_device *odev,
			       const struct omci_olt_profile_state *state);
	void (*set_operational)(struct omci_device *odev, bool operational);
	void (*config_changed)(struct omci_device *odev, u16 key,
			       const struct omci_identity *identity);
};

struct omci_device *
omci_device_register(struct xpon_device *xpon, u32 capabilities,
		     const struct omci_device_ops *ops, void *priv);
void omci_device_unregister(struct omci_device *odev);
int omci_device_start(struct omci_device *odev);
void omci_device_stop(struct omci_device *odev);

void *omci_device_priv(const struct omci_device *odev);
u32 omci_device_id(const struct omci_device *odev);

int omci_identity_load(struct device *dev, struct omci_identity *identity);
void gpon_random_serial_number(const u8 vendor[4], struct omci_identity *identity);
void omci_device_set_identity_info(struct omci_device *odev,
				   const struct omci_identity *identity);
void omci_device_set_identity(struct omci_device *odev,
			      const u8 serial_number[8],
			      const u8 password[10]);
void omci_device_set_onu_id(struct omci_device *odev, u16 onu_id);
void omci_device_set_channel(struct omci_device *odev, u16 gem_port_id,
			     bool valid);
void omci_device_set_state(struct omci_device *odev, u8 state);
void omci_device_reset_session(struct omci_device *odev);
void omci_device_receive(struct omci_device *odev, struct sk_buff *skb,
			 u16 gem_port_id, u32 flags);
int omci_device_reconcile_services(struct omci_device *odev);
bool omci_device_userspace_agent(const struct omci_device *odev);

#endif /* _NET_OMCI_H */
