// SPDX-License-Identifier: GPL-2.0-only
/* Typed hardware operations. Ownership and session checks belong to core.c. */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <uapi/linux/omci-backend.h>
#include "internal.h"
#else
#include "native.h"
#include <linux/omci-backend.h>
#endif

int omci_backend_execute(struct omci_device *odev, u16 op, const void *data,
			 size_t len, void *output, size_t *output_len)
{
	const struct omci_device_ops *ops = odev->ops;

	*output_len = 0;
	switch (op) {
	case OMCI_BACKEND_RESET:
		if (len)
			return -EINVAL;
		return ops->backend_reset ? ops->backend_reset(odev) : -EOPNOTSUPP;
	case OMCI_BACKEND_IDENTITY: {
		const struct omci_backend_identity *q = data;
		struct omci_identity identity = {};
		struct omci_agent_config *config = &odev->agent.config;
		int ret;

		if (len != sizeof(*q) || !q->valid || (q->valid & ~3) ||
		    q->reserved[0] || q->reserved[1])
			return -EINVAL;
		if (!ops->backend_identity)
			return -EOPNOTSUPP;
		if (q->valid & OMCI_BACKEND_ID_SERIAL) {
			identity.valid |= OMCI_IDENTITY_F_SERIAL_NUMBER;
			memcpy(identity.serial_number, q->serial, sizeof(identity.serial_number));
		}
		if (q->valid & OMCI_BACKEND_ID_PASSWORD) {
			identity.valid |= OMCI_IDENTITY_F_PASSWORD;
			memcpy(identity.password, q->password, sizeof(identity.password));
		}
		ret = ops->backend_identity(odev, &identity);
		if (ret)
			return ret;
		/* Keep the normalized registration readback coherent for daemon restart. */
		mutex_lock(&odev->agent.lock);
		if (q->valid & OMCI_BACKEND_ID_SERIAL) {
			memcpy(config->serial_number, q->serial, sizeof(config->serial_number));
			memcpy(config->vendor_id, q->serial, sizeof(config->vendor_id));
			config->serial_source = OMCI_CONFIG_SOURCE_NETLINK;
			config->vendor_source = OMCI_CONFIG_SOURCE_NETLINK;
		}
		if (q->valid & OMCI_BACKEND_ID_PASSWORD) {
			memcpy(config->password, q->password, sizeof(config->password));
			config->password_source = OMCI_CONFIG_SOURCE_NETLINK;
		}
		mutex_unlock(&odev->agent.lock);
		return 0;
	}
	case OMCI_BACKEND_TOPOLOGY: {
		struct omci_backend_topology *out = output;
		struct omci_ani_topology t = {};
		int ret;

		if (len)
			return -EINVAL;
		if (!ops->get_ani_topology)
			return -EOPNOTSUPP;
		ret = ops->get_ani_topology(odev, &t);
		if (ret)
			return ret;
		*out = (struct omci_backend_topology) {
			.tcont_base = t.tcont_base,
			.scheduler_base = t.scheduler_base,
			.queue_base = t.queue_base,
			.maximum_queue_size = t.maximum_queue_size,
			.allocated_queue_size = t.allocated_queue_size,
			.tcont_count = t.tcont_count,
			.queues_per_tcont = t.queues_per_tcont,
			.queue_config_option = t.queue_config_option,
			.scheduler_policy = t.scheduler_policy,
			.gem_port_count = t.gem_port_count,
			.qos_configuration = t.qos_configuration,
			.downstream_queues_per_uni = t.downstream_queues_per_uni,
			.flags = t.flags,
		};
		*output_len = sizeof(*out);
		return 0;
	}
	case OMCI_BACKEND_TCONT: {
		const struct omci_backend_tcont *q = data;

		if (len != sizeof(*q))
			return -EINVAL;
		if (q->entity > 0xffff || q->alloc_id > 0xffff || q->valid > 1 ||
		    (q->valid && q->alloc_id > 0xfff && q->alloc_id != 0xffff))
			return -ERANGE;
		if (!ops->set_tcont)
			return -EOPNOTSUPP;
		return ops->set_tcont(odev, q->entity, q->alloc_id, q->valid);
	}
	case OMCI_BACKEND_GEM: {
		const struct omci_backend_gem *q = data;

		if (len != sizeof(*q))
			return -EINVAL;
		if (q->entity > 0xffff || q->gem_port > 0xfff || q->tcont > 0xffff ||
		    q->valid > 1 || q->encrypted > 1 || q->direction < 1 || q->direction > 3)
			return -ERANGE;
		if (!ops->set_gem_port)
			return -EOPNOTSUPP;
		return ops->set_gem_port(odev, q->entity, q->gem_port, q->tcont,
					 q->direction, q->valid, q->encrypted);
	}
	case OMCI_BACKEND_TCONT_QOS: {
		struct omci_tcont_qos qos;
		const struct omci_backend_qos *q = data;

		if (len != sizeof(*q))
			return -EINVAL;
		if (q->entity > 0xffff || q->enabled > 1 ||
		    (q->enabled && q->policy != 1 && q->policy != 2))
			return -ERANGE;
		qos.enabled = q->enabled;
		qos.policy = q->policy;
		memcpy(qos.weight, q->weight, sizeof(qos.weight));
		if (!ops->set_tcont_qos)
			return -EOPNOTSUPP;
		return ops->set_tcont_qos(odev, q->entity, &qos);
	}
	case OMCI_BACKEND_GEM_RATE: {
		const struct omci_backend_rate *q = data;

		if (len != sizeof(*q))
			return -EINVAL;
		if (q->gem_port > 0xfff || q->upstream > 1)
			return -ERANGE;
		if (!ops->set_gem_rate)
			return -EOPNOTSUPP;
		return ops->set_gem_rate(odev, q->gem_port, q->upstream, q->rate, q->burst);
	}
	case OMCI_BACKEND_UNI: {
		const struct omci_backend_uni *q = data;

		if (len != sizeof(*q))
			return -EINVAL;
		if (q->entity > 0xffff || q->enabled > 1)
			return -ERANGE;
		if (!ops->set_uni)
			return -EOPNOTSUPP;
		return ops->set_uni(odev, q->entity, q->enabled);
	}
	case OMCI_BACKEND_UNI_ATTRS: {
		struct omci_uni_attrs a;
		const struct omci_backend_uni_attrs *q = data;

		if (len != sizeof(*q))
			return -EINVAL;
		if (q->entity > 0xffff || q->pause_quanta > 0xffff ||
		    q->learn_limit > 0xffff || q->auto_detection > 0xff ||
		    q->max_frame_size > 0xffff)
			return -ERANGE;
		a = (struct omci_uni_attrs) {
			.pause_quanta = q->pause_quanta, .ageing_s = q->ageing_s,
			.learn_limit = q->learn_limit, .down_kbps = q->down_kbps,
			.up_kbps = q->up_kbps, .auto_detection = q->auto_detection,
			.max_frame_size = q->max_frame_size,
		};
		if (!ops->set_uni_attrs)
			return -EOPNOTSUPP;
		return ops->set_uni_attrs(odev, q->entity, &a);
	}
	case OMCI_BACKEND_SERVICE: {
		struct omci_service_config s;
		const struct omci_backend_service *q = data;

		if (len != sizeof(*q))
			return -EINVAL;
		if (q->uni_entity > 0xffff || q->gem_entity > 0xffff || q->gem_port > 0xfff ||
		    q->tcont_entity > 0xffff || q->alloc_id > 0xfff || q->vlan > 4095 ||
		    q->pcp > 7 || q->queue > 7 || q->direction < 1 || q->direction > 3 ||
		    q->uni_mode > OMCI_UNI_VLAN_STACKED || q->uni_vlan > 4095 ||
		    q->multicast_ani > 0xffff || (q->flags & ~0x3f))
			return -ERANGE;
		s = (struct omci_service_config) {
			.cookie = q->cookie, .uni_entity_id = q->uni_entity,
			.gem_ctp_entity_id = q->gem_entity, .gem_port_id = q->gem_port,
			.tcont_entity_id = q->tcont_entity, .alloc_id = q->alloc_id,
			.vlan_id = q->vlan, .pcp = q->pcp, .queue = q->queue,
			.direction = q->direction, .uni_vlan_mode = q->uni_mode,
			.uni_vlan_id = q->uni_vlan, .multicast_ani_entity_id = q->multicast_ani,
			.vlan_valid = !!(q->flags & OMCI_BACKEND_SVC_VLAN),
			.pcp_valid = !!(q->flags & OMCI_BACKEND_SVC_PCP),
			.vlan_treatment_valid = !!(q->flags & OMCI_BACKEND_SVC_TREATMENT),
			.multicast_ani_valid = !!(q->flags & OMCI_BACKEND_SVC_MULTICAST_ANI),
			.multicast = !!(q->flags & OMCI_BACKEND_SVC_MULTICAST),
			.default_service = !!(q->flags & OMCI_BACKEND_SVC_DEFAULT),
		};
		memcpy(s.vlan_treatment, q->treatment, sizeof(s.vlan_treatment));
		if (!ops->replace_service)
			return -EOPNOTSUPP;
		return ops->replace_service(odev, &s);
	}
	case OMCI_BACKEND_SERVICE_DELETE: {
		const struct omci_backend_cookie *q = data;

		if (len != sizeof(*q))
			return -EINVAL;
		if (!ops->delete_service)
			return -EOPNOTSUPP;
		return ops->delete_service(odev, q->cookie);
	}
	case OMCI_BACKEND_TELEMETRY: {
		struct omci_backend_telemetry *out = output;
		struct omci_telemetry t = {};
		int ret;

		if (len)
			return -EINVAL;
		if (!ops->get_telemetry)
			return -EOPNOTSUPP;
		ret = ops->get_telemetry(odev, &t);
		if (ret)
			return ret;
		*out = (struct omci_backend_telemetry) {
			.valid = t.valid, .temperature_mc = t.bosa_temperature_mc,
			.voltage_uv = t.bosa_voltage_uv, .bias_ua = t.bosa_bias_ua,
			.tx_power_nw = t.bosa_tx_power_nw, .rx_power_nw = t.bosa_rx_power_nw,
			.alarms = t.bosa_alarms, .downstream_fec = t.downstream_fec,
			.upstream_fec = t.upstream_fec, .bip_errors = t.bip_errors,
			.fec = { t.fec_corrected_bytes, t.fec_corrected_codewords,
				 t.fec_uncorrectable_codewords, t.fec_total_codewords,
				 t.fec_seconds },
		};
		*output_len = sizeof(*out);
		return 0;
	}
	case OMCI_BACKEND_UNI_LINK: {
		struct omci_backend_link *out = output;
		bool up = false;
		int ret;
		const struct omci_backend_link *q = data;

		if (len != sizeof(*q))
			return -EINVAL;
		if (q->entity > 0xffff || q->up)
			return -ERANGE;
		if (!ops->get_uni_link)
			return -EOPNOTSUPP;
		ret = ops->get_uni_link(odev, q->entity, &up);
		if (ret)
			return ret;
		*out = (struct omci_backend_link) { q->entity, up };
		*output_len = sizeof(*out);
		return 0;
	}
	case OMCI_BACKEND_PROFILE: {
		struct omci_olt_profile_state state;
		const struct omci_backend_profile *q = data;

		if (len != sizeof(*q))
			return -EINVAL;
		if (q->configured > OMCI_OLT_PROFILE_ZTE || q->effective > OMCI_OLT_PROFILE_ZTE ||
		    q->forced > OMCI_OLT_PROFILE_ZTE || !omci_profile_forceable(q->forced) ||
		    !omci_profile_valid(q->configured) ||
		    !omci_profile_forceable(q->effective) ||
		    !q->effective || q->quirks != omci_profile_quirks(q->effective))
			return -ERANGE;
		state = (struct omci_olt_profile_state) {
			.configured = q->configured, .effective = q->effective,
			.forced = q->forced, .quirks = q->quirks,
		};
		if (!ops->set_olt_profile)
			return -EOPNOTSUPP;
		return ops->set_olt_profile(odev, &state);
	}
	case OMCI_BACKEND_OPERATIONAL: {
		const struct omci_backend_operational *q = data;

		if (len != sizeof(*q))
			return -EINVAL;
		if (q->operational > 1)
			return -ERANGE;
		if (!ops->set_operational)
			return -EOPNOTSUPP;
		ops->set_operational(odev, q->operational);
		return 0;
	}
	default:
		return -EOPNOTSUPP;
	}
}
