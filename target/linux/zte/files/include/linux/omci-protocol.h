/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_OMCI_PROTOCOL_H
#define _LINUX_OMCI_PROTOCOL_H

#include <linux/types.h>
#ifdef __KERNEL__
#include <uapi/linux/omci.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <linux/omci.h>
#endif

/**
 * struct omci_me_class - managed entity class descriptor
 * @class_id: ITU managed entity class identifier
 * @category: enum omci_class_category
 * @support: enum omci_class_support
 * @flags: OMCI_CLASS_F_* bitmap
 * @name: stable human-readable managed entity name
 */
struct omci_me_class {
	__u16 class_id;
	__u8 category;
	__u8 support;
	__u32 flags;
	const char *name;
};

/**
 * struct omci_olt_g - parsed OLT-G identification attributes
 * @vendor_id: four-character OLT vendor identifier
 * @equipment_id: OLT equipment or model identifier
 * @version: OLT software or hardware version identifier
 * @vendor_id_valid: @vendor_id has been received from the OLT
 * @equipment_id_valid: @equipment_id has been received from the OLT
 * @version_valid: @version has been received from the OLT
 * @valid: at least one identification attribute is available
 */
struct omci_olt_g {
	char vendor_id[OMCI_OLT_VENDOR_ID_LEN + 1];
	char equipment_id[OMCI_OLT_EQUIPMENT_ID_LEN + 1];
	char version[OMCI_OLT_VERSION_LEN + 1];
	bool vendor_id_valid;
	bool equipment_id_valid;
	bool version_valid;
	bool valid;
};

/**
 * struct omci_extended_vlan_rule - parsed class 171 table entry
 * @raw: original 16-byte table entry
 * @filter_outer_vid: outer-tag VLAN identifier match
 * @filter_inner_vid: inner-tag VLAN identifier match
 * @treat_outer_vid: outer-tag VLAN identifier treatment
 * @treat_inner_vid: inner-tag VLAN identifier treatment
 * @filter_outer_pbit: outer-tag priority match
 * @filter_outer_tpid_dei: outer-tag TPID/DEI match mode
 * @filter_inner_pbit: inner-tag priority match
 * @filter_inner_tpid_dei: inner-tag TPID/DEI match mode
 * @filter_ethertype: EtherType match mode
 * @tags_to_remove: number of tags removed, or discard mode
 * @treat_outer_pbit: outer-tag priority treatment
 * @treat_outer_tpid_dei: outer-tag TPID/DEI treatment mode
 * @treat_inner_pbit: inner-tag priority treatment
 * @treat_inner_tpid_dei: inner-tag TPID/DEI treatment mode
 * @delete: entry encodes a table deletion
 */
struct omci_extended_vlan_rule {
	__u8 raw[OMCI_EXT_VLAN_RULE_LEN];
	__u16 filter_outer_vid;
	__u16 filter_inner_vid;
	__u16 treat_outer_vid;
	__u16 treat_inner_vid;
	__u8 filter_outer_pbit;
	__u8 filter_outer_tpid_dei;
	__u8 filter_inner_pbit;
	__u8 filter_inner_tpid_dei;
	__u8 filter_ethertype;
	__u8 tags_to_remove;
	__u8 treat_outer_pbit;
	__u8 treat_outer_tpid_dei;
	__u8 treat_inner_pbit;
	__u8 treat_inner_tpid_dei;
	bool delete;
};

/**
 * struct omci_vlan_filter_entry - parsed VLAN tagging filter entry
 * @tci: original VLAN tag control information value
 * @vid: VLAN identifier
 * @pbit: priority code point
 * @dei: drop eligible indicator
 */
struct omci_vlan_filter_entry {
	__u16 tci;
	__u16 vid;
	__u8 pbit;
	__u8 dei;
};

/**
 * struct omci_vlan_tagging_filter - parsed class 84 attributes
 * @entries: valid VLAN filter entries
 * @forward_operation: forwarding operation from the managed entity
 * @num_entries: number of valid entries in @entries
 * @valid: parsed data is available
 */
struct omci_vlan_tagging_filter {
	struct omci_vlan_filter_entry entries[OMCI_VLAN_FILTER_MAX_ENTRIES];
	__u8 forward_operation;
	__u8 num_entries;
	bool valid;
};

/**
 * struct omci_extended_vlan - parsed class 171 attributes and table
 * @rules: parsed VLAN operation table
 * @dscp_to_pbit: DSCP-to-P-bit mapping attribute
 * @input_tpid: input TPID
 * @output_tpid: output TPID
 * @associated_me: associated managed entity pointer
 * @max_table_size: advertised maximum number of table entries
 * @association_type: associated managed entity type
 * @downstream_mode: downstream VLAN processing mode
 * @rule_count: number of valid rules in @rules
 * @valid: parsed data is available
 */
struct omci_extended_vlan {
	struct omci_extended_vlan_rule rules[OMCI_EXT_VLAN_MAX_RULES];
	__u8 dscp_to_pbit[24];
	__u16 input_tpid;
	__u16 output_tpid;
	__u16 associated_me;
	__u16 max_table_size;
	__u8 association_type;
	__u8 downstream_mode;
	__u8 rule_count;
	bool valid;
};

const char *omci_olt_profile_name(__u8 profile);
bool omci_profile_valid(__u8 profile);
bool omci_profile_forceable(__u8 profile);
__u8 omci_profile_detect(const struct omci_olt_g *olt);
__u32 omci_profile_quirks(__u8 profile);
void omci_profile_sanitize_olt_g(struct omci_olt_g *olt, __u32 quirks);
__u16 omci_profile_normalize_uni_entity(__u8 profile, __u16 entity_id);
void omci_profile_normalize_vlan_rule(__u8 profile,
				      struct omci_extended_vlan_rule *rule);
int omci_profile_resolve_multicast_ani(__u8 profile,
				       __u16 bridge_port_entity_id,
				       __u16 *ani_entity_id);

#endif /* _LINUX_OMCI_PROTOCOL_H */
