/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XPON_NL_H
#define XPON_NL_H

#include <stdint.h>
#include <linux/xpon.h>

/* A PON interface's state, as one XPON_CMD_GET returns it. */
struct xpon_nl_state {
	uint64_t present;		/* 1 << XPON_ATTR_* for each one reported */
	uint8_t mode, registration, carrier, signal_detect, los;
	uint8_t tx_allow, tx_hold, identity_wait;
	uint32_t tx_hold_seconds;
	uint8_t epon_mac[6];
	uint8_t epon_oam_link, epon_register, epon_sync, epon_nack_silent;
	uint32_t epon_holdover_ms, epon_generation, epon_registered;
	uint8_t module_diag[XPON_MODULE_DIAG_LEN];
	uint8_t module_thresholds[XPON_MODULE_THRESHOLDS_LEN];
	uint8_t sfu_offload;
	uint16_t sfu_untag_vlan;
	int sfu_vlan_maps;
	struct xpon_vlan_map sfu_vlan_map[XPON_VLAN_MAP_MAX];
};

#define XPON_NL_HAS(st, attr)	(((st)->present >> (attr)) & 1)

/* 0 or a negative errno. */
int xpon_nl_query(const char *ifname, struct xpon_nl_state *st);

/* One scalar attribute; -EOPNOTSUPP when the driver does not report it. */
int xpon_nl_get(const char *ifname, int attr, uint32_t *value);
int xpon_nl_get_raw(const char *ifname, int attr, void *data, int len);

/* Several attributes in one SET. @data with @len for a binary one. */
struct xpon_nl_attr {
	int type;
	uint32_t value;		/* a scalar's value */
	const void *data;	/* or its bytes, when set */
	int len;		/* and their length */
};

int xpon_nl_setv(const char *ifname, const struct xpon_nl_attr *attrs, int n);
int xpon_nl_set(const char *ifname, int attr, uint32_t value);

/*
 * A socket on the family's "events" group: readable whenever a PON
 * interface's state changes. Drain it, then query. A negative errno.
 */
int xpon_nl_event_open(void);
void xpon_nl_event_drain(int fd);

#endif
