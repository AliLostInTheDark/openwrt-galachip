/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_OMCI_ATTRIBUTES_H
#define _NET_OMCI_ATTRIBUTES_H

#include <linux/omci-protocol.h>

#define OMCI_VLAN_FILTER_LIST_MASK	0x8000
#define OMCI_VLAN_FILTER_FORWARD_MASK	0x4000
#define OMCI_VLAN_FILTER_COUNT_MASK	0x2000

#define OMCI_EXT_VLAN_ASSOC_TYPE_MASK	0x8000
#define OMCI_EXT_VLAN_MAX_SIZE_MASK	0x4000
#define OMCI_EXT_VLAN_INPUT_TPID_MASK	0x2000
#define OMCI_EXT_VLAN_OUTPUT_TPID_MASK	0x1000
#define OMCI_EXT_VLAN_DOWNSTREAM_MASK	0x0800
#define OMCI_EXT_VLAN_TABLE_MASK	0x0400
#define OMCI_EXT_VLAN_ASSOC_PTR_MASK	0x0200
#define OMCI_EXT_VLAN_DSCP_MAP_MASK	0x0100

#define OMCI_OLT_G_VENDOR_ID_MASK	0x8000
#define OMCI_OLT_G_EQUIPMENT_ID_MASK	0x4000
#define OMCI_OLT_G_VERSION_MASK	0x2000
#define OMCI_OLT_G_TIME_OF_DAY_MASK	0x1000
#define OMCI_OLT_G_TIME_OF_DAY_LEN	14

/* Decode into staged attributes; failure can follow earlier valid fields. */
int omci_attr_olt_g_set(struct omci_olt_g *olt, __u16 mask,
			const __u8 *data, size_t len);
int omci_attr_vlan_filter_create(struct omci_vlan_tagging_filter *filter,
				 const __u8 *data, size_t len);
int omci_attr_vlan_filter_set(struct omci_vlan_tagging_filter *filter,
			      __u16 mask, const __u8 *data, size_t len);
int omci_attr_extended_vlan_create(struct omci_extended_vlan *vlan,
				   const __u8 *data, size_t len);
int omci_attr_extended_vlan_set(struct omci_extended_vlan *vlan, __u16 mask,
				const __u8 *data, size_t len);
int omci_attr_vlan_rule_update(struct omci_extended_vlan *vlan,
			       const __u8 *data, size_t len);

#endif /* _NET_OMCI_ATTRIBUTES_H */
