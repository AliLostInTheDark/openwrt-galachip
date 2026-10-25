// SPDX-License-Identifier: GPL-2.0-only
/*
 * OMCI identity and configuration store
 *
 * omcid runs the OMCI protocol. The kernel keeps the ONU identity and the
 * configuration omcid starts from; omcid reads it when it binds and owns it
 * from then on.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <net/netlink.h>

#include "../internal.h"
#include "internal.h"

#define OMCI_OMCC_VERSION_G988_2010_BASELINE	0xa0

static void omci_agent_identity_state_locked(const struct omci_agent *agent,
					     struct omci_identity *identity)
{
	memset(identity, 0, sizeof(*identity));
	identity->valid = OMCI_IDENTITY_F_SERIAL_NUMBER |
			  OMCI_IDENTITY_F_VENDOR_ID |
			  OMCI_IDENTITY_F_PASSWORD |
			  OMCI_IDENTITY_F_VERSION |
			  OMCI_IDENTITY_F_EQUIPMENT_ID;
	memcpy(identity->serial_number, agent->config.serial_number,
	       sizeof(identity->serial_number));
	memcpy(identity->vendor_id, agent->config.vendor_id,
	       sizeof(identity->vendor_id));
	memcpy(identity->password, agent->config.password,
	       sizeof(identity->password));
	memcpy(identity->version, agent->config.version,
	       sizeof(identity->version));
	memcpy(identity->equipment_id, agent->config.equipment_id,
	       sizeof(identity->equipment_id));
	identity->serial_source = agent->config.serial_source;
	identity->vendor_source = agent->config.vendor_source;
	identity->password_source = agent->config.password_source;
	identity->version_source = agent->config.version_source;
	identity->equipment_source = agent->config.equipment_source;
}

int omci_agent_init(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;

	mutex_init(&agent->lock);
	agent->enabled = true;
	agent->permissive = false;
	agent->fake_omci = false;
	agent->reboot = true;
	agent->config.reboot_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.uni_count = 4;
	agent->config.onu_type = OMCI_ONU_TYPE_HGU;
	agent->config.onu_type_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.traffic_mgmt_option = 0;
	agent->config.dba_status_reporting = 1;
	agent->config.olt_profile = OMCI_OLT_PROFILE_AUTO;
	agent->config.olt_profile_force = OMCI_OLT_PROFILE_UNSPEC;
	agent->config.olt_profile_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.olt_profile_force_source = OMCI_CONFIG_SOURCE_DEFAULT;
	memcpy(agent->config.serial_number, "OPEN0001", 8);
	memcpy(agent->config.vendor_id, "OPEN", 4);
	strscpy(agent->config.version, "OpenWrt", sizeof(agent->config.version));
	strscpy(agent->config.software_version[0], "OpenWrt",
		sizeof(agent->config.software_version[0]));
	strscpy(agent->config.software_version[1], "OpenWrt",
		sizeof(agent->config.software_version[1]));
	/* The same defaults as the engine's own init; omcid validates them. */
	agent->config.omcc_version = OMCI_OMCC_VERSION_G988_2010_BASELINE;
	strscpy(agent->config.equipment_id, "OpenWrt",
		sizeof(agent->config.equipment_id));
	agent->config.serial_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.vendor_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.password_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.version_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.software_version_source[0] = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.software_version_source[1] = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.omcc_version_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.equipment_source = OMCI_CONFIG_SOURCE_DEFAULT;
	return 0;
}

void omci_agent_cleanup(struct omci_device *odev)
{
	mutex_destroy(&odev->agent.lock);
}

/* The configuration side of the agent status; omcid reports the rest. */
int omci_agent_put_status(struct sk_buff *msg, struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	u8 profile_configured, profile_forced, onu_type;
	bool enabled, permissive, fake_omci;

	mutex_lock(&agent->lock);
	enabled = agent->enabled;
	permissive = agent->permissive;
	fake_omci = agent->fake_omci;
	onu_type = agent->config.onu_type;
	profile_configured = agent->config.olt_profile;
	profile_forced = agent->config.olt_profile_force;
	mutex_unlock(&agent->lock);

	if (nla_put_u8(msg, OMCI_ATTR_AGENT_ENABLED, enabled) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_PERMISSIVE, permissive) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_FAKE_OMCI, fake_omci) ||
	    nla_put_u8(msg, OMCI_ATTR_ONU_TYPE, onu_type) ||
	    nla_put_u8(msg, OMCI_ATTR_OLT_PROFILE_CONFIGURED,
		       profile_configured) ||
	    nla_put_u8(msg, OMCI_ATTR_OLT_PROFILE_FORCED, profile_forced))
		return -EMSGSIZE;
	return 0;
}

int omci_agent_config_get(struct omci_device *odev, u16 key,
			  void *value, size_t *len)
{
	struct omci_agent *agent = &odev->agent;
	const void *source = NULL;
	size_t source_len = 0;
	u8 scalar;

	mutex_lock(&agent->lock);
	switch (key) {
	case OMCI_CONFIG_SERIAL_NUMBER:
		source = agent->config.serial_number;
		source_len = sizeof(agent->config.serial_number);
		break;
	case OMCI_CONFIG_VENDOR_ID:
		source = agent->config.vendor_id;
		source_len = sizeof(agent->config.vendor_id);
		break;
	case OMCI_CONFIG_VERSION:
	case OMCI_CONFIG_HARDWARE_VERSION:
		source = agent->config.version;
		source_len = strnlen(agent->config.version,
				     sizeof(agent->config.version));
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_0:
		source = agent->config.software_version[0];
		source_len = strnlen(agent->config.software_version[0],
				     sizeof(agent->config.software_version[0]));
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_1:
		source = agent->config.software_version[1];
		source_len = strnlen(agent->config.software_version[1],
				     sizeof(agent->config.software_version[1]));
		break;
	case OMCI_CONFIG_EQUIPMENT_ID:
		source = agent->config.equipment_id;
		source_len = strnlen(agent->config.equipment_id,
				     sizeof(agent->config.equipment_id));
		break;
	case OMCI_CONFIG_PASSWORD:
		source = agent->config.password;
		source_len = sizeof(agent->config.password);
		break;
	case OMCI_CONFIG_LOID:
		source = agent->config.loid;
		source_len = strnlen(agent->config.loid,
				     sizeof(agent->config.loid));
		break;
	case OMCI_CONFIG_LOID_PASSWORD:
		source = agent->config.loid_password;
		source_len = strnlen(agent->config.loid_password,
				     sizeof(agent->config.loid_password));
		break;
	case OMCI_CONFIG_TRAFFIC_MGMT_OPTION:
		scalar = agent->config.traffic_mgmt_option;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_DBA_STATUS_REPORTING:
		scalar = agent->config.dba_status_reporting;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_VEIP_SLOT:
		scalar = agent->config.veip_slot;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_ONU_TYPE:
		scalar = agent->config.onu_type;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_UNI_COUNT:
		scalar = agent->config.uni_count;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_AGENT_ENABLED:
		scalar = agent->enabled;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_AGENT_PERMISSIVE:
		scalar = agent->permissive;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_AGENT_FAKE_OMCI:
		scalar = agent->fake_omci;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_AGENT_REBOOT:
		scalar = agent->reboot;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_OMCC_VERSION:
		scalar = agent->config.omcc_version;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_OLT_PROFILE:
		scalar = agent->config.olt_profile;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_OLT_PROFILE_FORCE:
		scalar = agent->config.olt_profile_force;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	default:
		mutex_unlock(&agent->lock);
		return -EINVAL;
	}

	if (*len < source_len) {
		*len = source_len;
		mutex_unlock(&agent->lock);
		return -ENOSPC;
	}
	memcpy(value, source, source_len);
	*len = source_len;
	mutex_unlock(&agent->lock);
	return 0;
}

static int
__omci_agent_config_set_source(struct omci_device *odev, u16 key,
			       const void *value, size_t len, u8 source,
			       bool notify)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_identity identity;
	u8 normalized[OMCI_MAX_CONFIG_VALUE];
	size_t normalized_len = sizeof(normalized);
	u8 scalar;
	bool changed = false;
	int ret = 0;

	if ((key >= OMCI_CONFIG_SERIAL_NUMBER &&
	     key <= OMCI_CONFIG_PASSWORD) ||
	    key == OMCI_CONFIG_HARDWARE_VERSION ||
	    key == OMCI_CONFIG_SOFTWARE_VERSION_0 ||
	    key == OMCI_CONFIG_SOFTWARE_VERSION_1 ||
	    key == OMCI_CONFIG_LOID ||
	    key == OMCI_CONFIG_LOID_PASSWORD) {
		ret = omci_identity_normalize_config(key, value, len,
						     normalized, &normalized_len);
		if (ret)
			return ret;
		value = normalized;
		len = normalized_len;
	}

	mutex_lock(&agent->lock);
	if (odev->userspace_mode) {
		mutex_unlock(&agent->lock);
		return -EBUSY;
	}
	switch (key) {
	case OMCI_CONFIG_SERIAL_NUMBER:
		if (len != sizeof(agent->config.serial_number)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.serial_number, value, len);
		memcpy(agent->config.serial_number, value, len);
		agent->config.serial_source = source;
		memcpy(agent->config.vendor_id, value,
		       sizeof(agent->config.vendor_id));
		agent->config.vendor_source = source;
		break;
	case OMCI_CONFIG_VENDOR_ID:
		if (len != sizeof(agent->config.vendor_id)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.vendor_id, value, len);
		memcpy(agent->config.vendor_id, value, len);
		agent->config.vendor_source = source;
		memcpy(agent->config.serial_number, value,
		       sizeof(agent->config.vendor_id));
		agent->config.serial_source = max(agent->config.serial_source,
						  source);
		break;
	case OMCI_CONFIG_VERSION:
		if (len != sizeof(agent->config.version)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.version, value, len) ||
			  memcmp(agent->config.software_version[0], value, len) ||
			  memcmp(agent->config.software_version[1], value, len);
		memcpy(agent->config.version, value, len);
		memcpy(agent->config.software_version[0], value, len);
		memcpy(agent->config.software_version[1], value, len);
		agent->config.version_source = source;
		agent->config.software_version_source[0] = source;
		agent->config.software_version_source[1] = source;
		break;
	case OMCI_CONFIG_HARDWARE_VERSION:
		if (len != sizeof(agent->config.version)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.version, value, len);
		memcpy(agent->config.version, value, len);
		agent->config.version_source = source;
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_0:
	case OMCI_CONFIG_SOFTWARE_VERSION_1: {
		unsigned int image = key == OMCI_CONFIG_SOFTWARE_VERSION_0 ? 0 : 1;

		if (len != sizeof(agent->config.software_version[image])) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.software_version[image], value, len);
		memcpy(agent->config.software_version[image], value, len);
		agent->config.software_version_source[image] = source;
		break;
	}
	case OMCI_CONFIG_EQUIPMENT_ID:
		if (!len || len > sizeof(agent->config.equipment_id)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.equipment_id, value, len) ||
			  memchr_inv(agent->config.equipment_id + len, 0,
				     sizeof(agent->config.equipment_id) - len);
		memset(agent->config.equipment_id, 0,
		       sizeof(agent->config.equipment_id));
		memcpy(agent->config.equipment_id, value, len);
		agent->config.equipment_source = source;
		break;
	case OMCI_CONFIG_PASSWORD:
		if (len != sizeof(agent->config.password)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.password, value, len);
		memcpy(agent->config.password, value, len);
		agent->config.password_source = source;
		break;
	case OMCI_CONFIG_LOID:
		if (len != sizeof(agent->config.loid)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.loid, value, len);
		memcpy(agent->config.loid, value, len);
		agent->config.loid_source = source;
		break;
	case OMCI_CONFIG_LOID_PASSWORD:
		if (len != sizeof(agent->config.loid_password)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.loid_password, value, len);
		memcpy(agent->config.loid_password, value, len);
		agent->config.loid_password_source = source;
		break;
	case OMCI_CONFIG_OLT_PROFILE:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		if (!omci_profile_valid(scalar)) {
			ret = -EINVAL;
			break;
		}
		changed = agent->config.olt_profile != scalar;
		agent->config.olt_profile = scalar;
		agent->config.olt_profile_source = source;
		break;
	case OMCI_CONFIG_OLT_PROFILE_FORCE:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		if (!omci_profile_forceable(scalar)) {
			ret = -EINVAL;
			break;
		}
		changed = agent->config.olt_profile_force != scalar;
		agent->config.olt_profile_force = scalar;
		agent->config.olt_profile_force_source = source;
		break;
	case OMCI_CONFIG_AGENT_REBOOT:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		changed = agent->reboot != !!scalar;
		agent->reboot = !!scalar;
		agent->config.reboot_source = source;
		break;
	case OMCI_CONFIG_OMCC_VERSION:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		if (!scalar) {
			ret = -EINVAL;
			break;
		}
		changed = agent->config.omcc_version != scalar;
		agent->config.omcc_version = scalar;
		agent->config.omcc_version_source = source;
		break;
	case OMCI_CONFIG_DBA_STATUS_REPORTING:
		if (len != sizeof(scalar) || *(const u8 *)value > 1) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		changed = agent->config.dba_status_reporting != scalar;
		agent->config.dba_status_reporting = scalar;
		break;
	case OMCI_CONFIG_VEIP_SLOT:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		changed = agent->config.veip_slot != scalar;
		agent->config.veip_slot = scalar;
		agent->config.veip_slot_source = source;
		break;
	case OMCI_CONFIG_TRAFFIC_MGMT_OPTION:
	case OMCI_CONFIG_ONU_TYPE:
	case OMCI_CONFIG_UNI_COUNT:
	case OMCI_CONFIG_AGENT_ENABLED:
	case OMCI_CONFIG_AGENT_PERMISSIVE:
	case OMCI_CONFIG_AGENT_FAKE_OMCI:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		if (key == OMCI_CONFIG_TRAFFIC_MGMT_OPTION) {
			changed = agent->config.traffic_mgmt_option != scalar;
			agent->config.traffic_mgmt_option = scalar;
		} else if (key == OMCI_CONFIG_ONU_TYPE) {
			if (scalar > OMCI_ONU_TYPE_CBU) {
				ret = -EINVAL;
				break;
			}
			changed = agent->config.onu_type != scalar;
			agent->config.onu_type = scalar;
			agent->config.onu_type_source = source;
		} else if (key == OMCI_CONFIG_UNI_COUNT) {
			scalar = clamp_t(u8, scalar, 1, 16);
			changed = agent->config.uni_count != scalar;
			agent->config.uni_count = scalar;
		} else if (key == OMCI_CONFIG_AGENT_ENABLED) {
			changed = agent->enabled != !!scalar;
			agent->enabled = !!scalar;
		} else if (key == OMCI_CONFIG_AGENT_PERMISSIVE) {
			changed = agent->permissive != !!scalar;
			agent->permissive = !!scalar;
		} else {
			changed = agent->fake_omci != !!scalar;
			agent->fake_omci = !!scalar;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (!ret && notify && changed)
		omci_agent_identity_state_locked(agent, &identity);
	mutex_unlock(&agent->lock);
	if (!ret && notify && changed && odev->ops->config_changed)
		odev->ops->config_changed(odev, key, &identity);
	return ret;
}

int omci_agent_config_set_source(struct omci_device *odev, u16 key,
				 const void *value, size_t len, u8 source)
{
	return __omci_agent_config_set_source(odev, key, value, len, source,
					      false);
}

int omci_agent_config_set(struct omci_device *odev, u16 key,
			  const void *value, size_t len)
{
	return omci_agent_config_set_userspace(odev, key, value, len,
					       OMCI_CONFIG_SOURCE_NETLINK);
}

int omci_agent_config_set_userspace(struct omci_device *odev, u16 key,
				    const void *value, size_t len, u8 source)
{
	if (source != OMCI_CONFIG_SOURCE_NETLINK &&
	    source != OMCI_CONFIG_SOURCE_SYSFS)
		return -EINVAL;

	return __omci_agent_config_set_source(odev, key, value, len, source,
					      true);
}

int omci_agent_config_source_get(struct omci_device *odev, u16 key, u8 *source)
{
	struct omci_agent *agent = &odev->agent;
	int ret = 0;

	if (!source)
		return -EINVAL;
	mutex_lock(&agent->lock);
	switch (key) {
	case OMCI_CONFIG_SERIAL_NUMBER:
		*source = agent->config.serial_source;
		break;
	case OMCI_CONFIG_VENDOR_ID:
		*source = agent->config.vendor_source;
		break;
	case OMCI_CONFIG_VERSION:
	case OMCI_CONFIG_HARDWARE_VERSION:
		*source = agent->config.version_source;
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_0:
		*source = agent->config.software_version_source[0];
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_1:
		*source = agent->config.software_version_source[1];
		break;
	case OMCI_CONFIG_EQUIPMENT_ID:
		*source = agent->config.equipment_source;
		break;
	case OMCI_CONFIG_PASSWORD:
		*source = agent->config.password_source;
		break;
	case OMCI_CONFIG_LOID:
		*source = agent->config.loid_source;
		break;
	case OMCI_CONFIG_LOID_PASSWORD:
		*source = agent->config.loid_password_source;
		break;
	case OMCI_CONFIG_ONU_TYPE:
		*source = agent->config.onu_type_source;
		break;
	case OMCI_CONFIG_OLT_PROFILE:
		*source = agent->config.olt_profile_source;
		break;
	case OMCI_CONFIG_OLT_PROFILE_FORCE:
		*source = agent->config.olt_profile_force_source;
		break;
	case OMCI_CONFIG_AGENT_REBOOT:
		*source = agent->config.reboot_source;
		break;
	case OMCI_CONFIG_OMCC_VERSION:
		*source = agent->config.omcc_version_source;
		break;
	case OMCI_CONFIG_VEIP_SLOT:
		*source = agent->config.veip_slot_source;
		break;
	default:
		*source = OMCI_CONFIG_SOURCE_UNSPEC;
		ret = -ENOENT;
		break;
	}
	mutex_unlock(&agent->lock);
	return ret;
}


int omci_device_reconcile_services(struct omci_device *odev)
{
	if (!odev)
		return -EINVAL;

	omci_device_notify(odev, OMCI_EVENT_RESOURCE_CHANGE);
	return -EINPROGRESS;
}
EXPORT_SYMBOL_GPL(omci_device_reconcile_services);
