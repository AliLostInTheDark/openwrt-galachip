// SPDX-License-Identifier: GPL-2.0-only
/* Shared parsed OMCI attributes; callers own staging and MIB commit. */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#include "attributes.h"
#include "me.h"

static __u16 omci_attr_be16(const __u8 *data)
{
	return ((__u16)data[0] << 8) | data[1];
}

static __u32 omci_attr_be32(const __u8 *data)
{
	return ((__u32)data[0] << 24) | ((__u32)data[1] << 16) |
	       ((__u32)data[2] << 8) | data[3];
}

static const __u8 *omci_attr_consume(const __u8 **data, size_t *len,
				     size_t size)
{
	const __u8 *value;

	if (*len < size)
		return NULL;
	value = *data;
	*data += size;
	*len -= size;

	return value;
}

static void omci_olt_g_copy_string(char *dest, size_t dest_len,
				   const __u8 *source, size_t source_len)
{
	size_t len = source_len < dest_len - 1 ? source_len : dest_len - 1;

	memcpy(dest, source, len);
	dest[len] = '\0';
}

int omci_attr_olt_g_set(struct omci_olt_g *olt, __u16 mask,
			const __u8 *data, size_t len)
{
	const __u8 *value;

	if (!olt || (!data && len))
		return -EINVAL;
	if (mask & OMCI_OLT_G_VENDOR_ID_MASK) {
		value = omci_attr_consume(&data, &len,
					  OMCI_OLT_VENDOR_ID_LEN);
		if (!value)
			return -EINVAL;
		omci_olt_g_copy_string(olt->vendor_id, sizeof(olt->vendor_id),
				       value, OMCI_OLT_VENDOR_ID_LEN);
		olt->vendor_id_valid = true;
	}
	if (mask & OMCI_OLT_G_EQUIPMENT_ID_MASK) {
		value = omci_attr_consume(&data, &len,
					  OMCI_OLT_EQUIPMENT_ID_LEN);
		if (!value)
			return -EINVAL;
		omci_olt_g_copy_string(olt->equipment_id,
				       sizeof(olt->equipment_id), value,
				       OMCI_OLT_EQUIPMENT_ID_LEN);
		olt->equipment_id_valid = true;
	}
	if (mask & OMCI_OLT_G_VERSION_MASK) {
		value = omci_attr_consume(&data, &len, OMCI_OLT_VERSION_LEN);
		if (!value)
			return -EINVAL;
		omci_olt_g_copy_string(olt->version, sizeof(olt->version),
				       value, OMCI_OLT_VERSION_LEN);
		olt->version_valid = true;
	}
	if (mask & OMCI_OLT_G_TIME_OF_DAY_MASK) {
		value = omci_attr_consume(&data, &len,
					  OMCI_OLT_G_TIME_OF_DAY_LEN);
		if (!value)
			return -EINVAL;
	}
	olt->valid = olt->vendor_id_valid || olt->equipment_id_valid ||
		     olt->version_valid;

	return 0;
}

static void omci_vlan_filter_parse_entries(struct omci_vlan_tagging_filter *filter,
					   const __u8 *data)
{
	unsigned int i;

	for (i = 0; i < OMCI_VLAN_FILTER_MAX_ENTRIES; i++) {
		__u16 tci = omci_attr_be16(data + i * sizeof(tci));

		filter->entries[i].tci = tci;
		filter->entries[i].vid = tci & OMCI_GENMASK(11, 0);
		filter->entries[i].dei = (tci >> 12) & 0x1;
		filter->entries[i].pbit = (tci >> 13) & 0x7;
	}
}

int omci_attr_vlan_filter_create(struct omci_vlan_tagging_filter *filter,
				 const __u8 *content, size_t len)
{
	if (!filter || !content || len < 26)
		return -EINVAL;

	omci_vlan_filter_parse_entries(filter, content);
	filter->forward_operation = content[24];
	filter->num_entries = content[25] < OMCI_VLAN_FILTER_MAX_ENTRIES ?
		content[25] : OMCI_VLAN_FILTER_MAX_ENTRIES;
	filter->valid = true;

	return 0;
}

int omci_attr_vlan_filter_set(struct omci_vlan_tagging_filter *filter,
			      __u16 mask, const __u8 *data, size_t len)
{
	if (!filter || (!data && len))
		return -EINVAL;

	if (mask & OMCI_VLAN_FILTER_LIST_MASK) {
		if (len < 24)
			return -EINVAL;
		omci_vlan_filter_parse_entries(filter, data);
		data += 24;
		len -= 24;
	}
	if (mask & OMCI_VLAN_FILTER_FORWARD_MASK) {
		if (!len)
			return -EINVAL;
		filter->forward_operation = *data++;
		len--;
	}
	if (mask & OMCI_VLAN_FILTER_COUNT_MASK) {
		if (!len)
			return -EINVAL;
		filter->num_entries = *data < OMCI_VLAN_FILTER_MAX_ENTRIES ?
			*data : OMCI_VLAN_FILTER_MAX_ENTRIES;
	}
	filter->valid = true;

	return 0;
}

static bool omci_ext_vlan_rule_is_delete(const __u8 *raw)
{
	unsigned int i;

	for (i = 8; i < OMCI_EXT_VLAN_RULE_LEN; i++)
		if (raw[i] != 0xff)
			return false;

	return true;
}

static void omci_ext_vlan_parse_rule(struct omci_extended_vlan_rule *rule,
				     const __u8 *raw)
{
	__u32 word;

	memcpy(rule->raw, raw, sizeof(rule->raw));
	word = omci_attr_be32(raw);
	rule->filter_outer_pbit = (word >> 28) & 0xf;
	rule->filter_outer_vid = (word >> 15) & 0x1fff;
	rule->filter_outer_tpid_dei = (word >> 12) & 0x7;

	word = omci_attr_be32(raw + 4);
	rule->filter_inner_pbit = (word >> 28) & 0xf;
	rule->filter_inner_vid = (word >> 15) & 0x1fff;
	rule->filter_inner_tpid_dei = (word >> 12) & 0x7;
	rule->filter_ethertype = word & 0xf;

	word = omci_attr_be32(raw + 8);
	rule->tags_to_remove = (word >> 30) & 0x3;
	rule->treat_outer_pbit = (word >> 16) & 0xf;
	rule->treat_outer_vid = (word >> 3) & 0x1fff;
	rule->treat_outer_tpid_dei = word & 0x7;

	word = omci_attr_be32(raw + 12);
	rule->treat_inner_pbit = (word >> 16) & 0xf;
	rule->treat_inner_vid = (word >> 3) & 0x1fff;
	rule->treat_inner_tpid_dei = word & 0x7;
	rule->delete = omci_ext_vlan_rule_is_delete(raw);
}

int omci_attr_vlan_rule_update(struct omci_extended_vlan *vlan,
			       const __u8 *raw, size_t len)
{
	unsigned int index;
	bool delete;

	if (!vlan || !raw || len < OMCI_EXT_VLAN_RULE_LEN ||
	    vlan->rule_count > OMCI_EXT_VLAN_MAX_RULES)
		return -EINVAL;

	delete = omci_ext_vlan_rule_is_delete(raw);
	for (index = 0; index < vlan->rule_count; index++)
		if (!memcmp(vlan->rules[index].raw, raw, 8))
			break;

	if (delete) {
		if (index == vlan->rule_count)
			return 0;
		memmove(&vlan->rules[index], &vlan->rules[index + 1],
			(vlan->rule_count - index - 1) * sizeof(vlan->rules[0]));
		vlan->rule_count--;
		memset(&vlan->rules[vlan->rule_count], 0,
		       sizeof(vlan->rules[0]));
		return 0;
	}

	if (index == vlan->rule_count) {
		if (vlan->rule_count >= OMCI_EXT_VLAN_MAX_RULES)
			return -ENOSPC;
		vlan->rule_count++;
	}
	omci_ext_vlan_parse_rule(&vlan->rules[index], raw);

	return 0;
}

int omci_attr_extended_vlan_create(struct omci_extended_vlan *vlan,
				   const __u8 *content, size_t len)
{
	if (!vlan || !content || len < 26)
		return -EINVAL;

	vlan->association_type = content[0];
	vlan->associated_me = omci_attr_be16(content + 24);
	vlan->max_table_size = OMCI_EXT_VLAN_MAX_RULES;
	vlan->valid = true;

	return 0;
}

int omci_attr_extended_vlan_set(struct omci_extended_vlan *vlan, __u16 mask,
				const __u8 *data, size_t len)
{
	if (!vlan || (!data && len))
		return -EINVAL;

	if (mask & OMCI_EXT_VLAN_ASSOC_TYPE_MASK) {
		const __u8 *value = omci_attr_consume(&data, &len, 1);

		if (!value)
			return -EINVAL;
		vlan->association_type = value[0];
	}
	if (mask & OMCI_EXT_VLAN_MAX_SIZE_MASK) {
		const __u8 *value = omci_attr_consume(&data, &len, 2);

		if (!value)
			return -EINVAL;
		vlan->max_table_size = omci_attr_be16(value);
	}
	if (mask & OMCI_EXT_VLAN_INPUT_TPID_MASK) {
		const __u8 *value = omci_attr_consume(&data, &len, 2);

		if (!value)
			return -EINVAL;
		vlan->input_tpid = omci_attr_be16(value);
	}
	if (mask & OMCI_EXT_VLAN_OUTPUT_TPID_MASK) {
		const __u8 *value = omci_attr_consume(&data, &len, 2);

		if (!value)
			return -EINVAL;
		vlan->output_tpid = omci_attr_be16(value);
	}
	if (mask & OMCI_EXT_VLAN_DOWNSTREAM_MASK) {
		const __u8 *value = omci_attr_consume(&data, &len, 1);

		if (!value)
			return -EINVAL;
		vlan->downstream_mode = value[0];
	}
	if (mask & OMCI_EXT_VLAN_TABLE_MASK) {
		const __u8 *value = omci_attr_consume(&data, &len,
						      OMCI_EXT_VLAN_RULE_LEN);
		int ret;

		if (!value)
			return -EINVAL;
		ret = omci_attr_vlan_rule_update(vlan, value, OMCI_EXT_VLAN_RULE_LEN);
		if (ret)
			return ret;
	}
	if (mask & OMCI_EXT_VLAN_ASSOC_PTR_MASK) {
		const __u8 *value = omci_attr_consume(&data, &len, 2);

		if (!value)
			return -EINVAL;
		vlan->associated_me = omci_attr_be16(value);
	}
	if (mask & OMCI_EXT_VLAN_DSCP_MAP_MASK) {
		const __u8 *value = omci_attr_consume(&data, &len,
						      sizeof(vlan->dscp_to_pbit));

		if (!value)
			return -EINVAL;
		memcpy(vlan->dscp_to_pbit, value, sizeof(vlan->dscp_to_pbit));
	}
	vlan->valid = true;

	return 0;
}
