// SPDX-License-Identifier: GPL-2.0-only
/* Existing structured MIB encoding from net/xpon/omci/core.c. */
#include "native.h"
static int omci_put_olt_g(struct sk_buff *msg,
			  const struct omci_mib_object *object)
{
	const struct omci_olt_g *olt = &object->olt_g;
	struct nlattr *nested;

	if (object->class_id != OMCI_CLASS_OLT_G || !olt->valid)
		return 0;

	nested = nla_nest_start(msg, OMCI_ATTR_OLT_G);
	if (!nested)
		return -EMSGSIZE;
	if ((olt->vendor_id_valid &&
	     nla_put_string(msg, OMCI_OLT_G_ATTR_VENDOR_ID, olt->vendor_id)) ||
	    (olt->equipment_id_valid &&
	     nla_put_string(msg, OMCI_OLT_G_ATTR_EQUIPMENT_ID,
			    olt->equipment_id)) ||
	    (olt->version_valid &&
	     nla_put_string(msg, OMCI_OLT_G_ATTR_VERSION, olt->version))) {
		nla_nest_cancel(msg, nested);
		return -EMSGSIZE;
	}
	nla_nest_end(msg, nested);

	return 0;
}

static int omci_put_vlan_filter(struct sk_buff *msg,
				const struct omci_mib_object *object)
{
	const struct omci_vlan_tagging_filter *filter = &object->vlan_filter;
	struct nlattr *entries;
	struct nlattr *nested;
	unsigned int i;

	if (object->class_id != OMCI_CLASS_VLAN_TAGGING_FILTER_DATA || !filter->valid)
		return 0;

	nested = nla_nest_start(msg, OMCI_ATTR_VLAN_TAGGING_FILTER);
	if (!nested)
		return -EMSGSIZE;
	if (nla_put_u8(msg, OMCI_VLAN_FILTER_ATTR_FORWARD_OPERATION,
		       filter->forward_operation) ||
	    nla_put_u8(msg, OMCI_VLAN_FILTER_ATTR_NUMBER_OF_ENTRIES,
		       filter->num_entries))
		goto cancel;

	entries = nla_nest_start(msg, OMCI_VLAN_FILTER_ATTR_ENTRIES);
	if (!entries)
		goto cancel;
	for (i = 0; i < filter->num_entries; i++) {
		const struct omci_vlan_filter_entry *entry = &filter->entries[i];
		struct nlattr *item;

		item = nla_nest_start(msg, i + 1);
		if (!item)
			goto cancel_entries;
		if (nla_put_u8(msg, OMCI_VLAN_FILTER_ENTRY_ATTR_INDEX, i) ||
		    nla_put_u16(msg, OMCI_VLAN_FILTER_ENTRY_ATTR_TCI,
				entry->tci) ||
		    nla_put_u8(msg, OMCI_VLAN_FILTER_ENTRY_ATTR_PBIT,
			       entry->pbit) ||
		    nla_put_u8(msg, OMCI_VLAN_FILTER_ENTRY_ATTR_DEI,
			       entry->dei) ||
		    nla_put_u16(msg, OMCI_VLAN_FILTER_ENTRY_ATTR_VID,
				entry->vid)) {
			nla_nest_cancel(msg, item);
			goto cancel_entries;
		}
		nla_nest_end(msg, item);
	}
	nla_nest_end(msg, entries);
	nla_nest_end(msg, nested);

	return 0;

cancel_entries:
	nla_nest_cancel(msg, entries);
cancel:
	nla_nest_cancel(msg, nested);
	return -EMSGSIZE;
}

static int omci_put_ext_vlan_rule(struct sk_buff *msg,
				  const struct omci_extended_vlan_rule *rule,
				  unsigned int index)
{
	struct nlattr *nested;

	nested = nla_nest_start(msg, index + 1);
	if (!nested)
		return -EMSGSIZE;
	if (nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_INDEX, index) ||
	    nla_put(msg, OMCI_EXT_VLAN_RULE_ATTR_RAW, sizeof(rule->raw),
		    rule->raw) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_DELETE, rule->delete) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_OUTER_PBIT,
		       rule->filter_outer_pbit) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_OUTER_VID, rule->filter_outer_vid) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_OUTER_TPID_DEI,
		       rule->filter_outer_tpid_dei) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_INNER_PBIT,
		       rule->filter_inner_pbit) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_INNER_VID, rule->filter_inner_vid) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_INNER_TPID_DEI,
		       rule->filter_inner_tpid_dei) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_ETHERTYPE,
		       rule->filter_ethertype) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_TAGS_TO_REMOVE,
		       rule->tags_to_remove) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_TREAT_OUTER_PBIT,
		       rule->treat_outer_pbit) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_RULE_ATTR_TREAT_OUTER_VID, rule->treat_outer_vid) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_TREAT_OUTER_TPID_DEI,
		       rule->treat_outer_tpid_dei) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_TREAT_INNER_PBIT,
		       rule->treat_inner_pbit) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_RULE_ATTR_TREAT_INNER_VID, rule->treat_inner_vid) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_TREAT_INNER_TPID_DEI,
		       rule->treat_inner_tpid_dei)) {
		nla_nest_cancel(msg, nested);
		return -EMSGSIZE;
	}
	nla_nest_end(msg, nested);

	return 0;
}

static int omci_put_extended_vlan(struct sk_buff *msg,
				  const struct omci_mib_object *object)
{
	const struct omci_extended_vlan *vlan = &object->extended_vlan;
	struct nlattr *rules;
	struct nlattr *nested;
	unsigned int i;

	if (object->class_id != OMCI_CLASS_EXTENDED_VLAN || !vlan->valid)
		return 0;

	nested = nla_nest_start(msg, OMCI_ATTR_EXTENDED_VLAN);
	if (!nested)
		return -EMSGSIZE;
	if (nla_put_u8(msg, OMCI_EXT_VLAN_ATTR_ASSOCIATION_TYPE,
		       vlan->association_type) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_ATTR_MAX_TABLE_SIZE, vlan->max_table_size) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_ATTR_INPUT_TPID, vlan->input_tpid) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_ATTR_OUTPUT_TPID, vlan->output_tpid) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_ATTR_DOWNSTREAM_MODE,
		       vlan->downstream_mode) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_ATTR_ASSOCIATED_ME, vlan->associated_me) ||
	    nla_put(msg, OMCI_EXT_VLAN_ATTR_DSCP_TO_PBIT,
		    sizeof(vlan->dscp_to_pbit), vlan->dscp_to_pbit))
		goto cancel;

	rules = nla_nest_start(msg, OMCI_EXT_VLAN_ATTR_RULES);
	if (!rules)
		goto cancel;
	for (i = 0; i < vlan->rule_count; i++)
		if (omci_put_ext_vlan_rule(msg, &vlan->rules[i], i))
			goto cancel_rules;
	nla_nest_end(msg, rules);
	nla_nest_end(msg, nested);

	return 0;

cancel_rules:
	nla_nest_cancel(msg, rules);
cancel:
	nla_nest_cancel(msg, nested);
	return -EMSGSIZE;
}

int omci_native_put_mib_object(struct sk_buff *msg,
			       const struct omci_mib_object *object,
			       u32 next_index, const char *name)
{
	if (nla_put_u16(msg, OMCI_ATTR_CLASS_ID, object->class_id) ||
	    nla_put_u16(msg, OMCI_ATTR_ENTITY_ID, object->entity_id) ||
	    nla_put_u16(msg, OMCI_ATTR_ATTR_MASK, object->attr_mask) ||
	    nla_put(msg, OMCI_ATTR_ATTR_DATA, sizeof(object->data),
		    object->data) ||
	    nla_put_u8(msg, OMCI_ATTR_ORIGIN, object->origin) ||
	    nla_put_u32(msg, OMCI_ATTR_INDEX, next_index) ||
	    (name && nla_put_string(msg, OMCI_ATTR_NAME, name)))
		return -EMSGSIZE;
	if (omci_put_olt_g(msg, object) ||
	    omci_put_vlan_filter(msg, object) ||
	    omci_put_extended_vlan(msg, object))
		return -EMSGSIZE;
	return 0;
}
