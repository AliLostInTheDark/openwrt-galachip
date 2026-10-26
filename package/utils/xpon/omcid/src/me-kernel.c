// SPDX-License-Identifier: GPL-2.0-only
/* Kernel state adapters for the shared OMCI catalogue and attribute codec. */
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include "native.h"
#endif

#include "internal.h"

const struct omci_me_desc *omci_me_lookup(const struct omci_agent *agent,
					  u16 class_id)
{
	return omci_me_lookup_profile(agent->profile_effective, class_id);
}

bool omci_me_active(const struct omci_agent *agent, u16 class_id)
{
	return omci_me_active_profile(agent->profile_effective, class_id);
}

bool omci_me_action_allowed(const struct omci_agent *agent, u16 class_id,
			    u8 message_type)
{
	return omci_me_action_allowed_profile(agent->profile_effective, class_id,
					    message_type);
}

int omci_me_decode_create(const struct omci_me_desc *desc,
			  struct omci_mib_object *object,
			  const u8 *data, size_t len)
{
	if (!desc)
		return 0;
	return omci_me_decode_create_data(desc, object->data, &object->attr_mask,
					data, len);
}

int omci_me_decode_set(const struct omci_me_desc *desc,
		       struct omci_mib_object *object, u16 mask,
		       const u8 *data, size_t len)
{
	if (!desc)
		return 0;
	return omci_me_decode_set_data(desc, object->data, &object->attr_mask,
				     mask, data, len);
}

int omci_me_encode_attributes(const struct omci_me_desc *desc,
			      const struct omci_mib_object *object,
			      u16 mask, u8 *data, size_t len,
			      u16 *encoded_mask, size_t *encoded_len)
{
	if (!desc)
		return -ENOENT;
	return omci_me_encode_data(desc, object->data, mask, data, len,
				   encoded_mask, encoded_len);
}

unsigned int omci_me_attr_count(const struct omci_agent *agent)
{
	return omci_me_attr_count_profile(agent->profile_effective);
}

int omci_me_attr_id(const struct omci_agent *agent, u16 class_id,
		    unsigned int attr_index, u16 *attr_id)
{
	if (!agent)
		return -EINVAL;
	return omci_me_attr_id_profile(agent->profile_effective, class_id,
				     attr_index, attr_id);
}

int omci_me_attr_get(const struct omci_agent *agent, u16 attr_id,
		     u16 *class_id, unsigned int *attr_index,
		     const struct omci_me_desc **desc,
		     const struct omci_attr_desc **attr)
{
	if (!agent)
		return -EINVAL;
	return omci_me_attr_get_profile(agent->profile_effective, attr_id,
				      class_id, attr_index, desc, attr);
}

int omci_me_class_get(struct omci_device *odev, u16 class_id,
		      struct omci_me_class *class)
{
	if (!odev)
		return -EINVAL;
	return omci_me_class_get_profile(odev->agent.profile_effective, class_id, class);
}

int omci_me_class_next(struct omci_device *odev, u32 index,
		       struct omci_me_class *class, u32 *next_index)
{
	if (!odev)
		return -EINVAL;
	return omci_me_class_next_profile(odev->agent.profile_effective, index,
				       class, next_index);
}
