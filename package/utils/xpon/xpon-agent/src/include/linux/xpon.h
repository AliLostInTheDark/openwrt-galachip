/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_XPON_H
#define _UAPI_LINUX_XPON_H

#define XPON_GENL_NAME		"xpon"
#define XPON_GENL_VERSION	3

#include <linux/types.h>

enum xpon_mode {
	XPON_MODE_GPON,
	XPON_MODE_EPON,
	XPON_MODE_XGSPON,

	__XPON_MODE_MAX,
};
#define XPON_MODE_MAX (__XPON_MODE_MAX - 1)

enum xpon_registration_state {
	XPON_REGISTRATION_DOWN,
	XPON_REGISTRATION_DISCOVERY,
	XPON_REGISTRATION_REGISTERING,
	XPON_REGISTRATION_OPERATIONAL,
};

enum xpon_cmd {
	XPON_CMD_UNSPEC,
	XPON_CMD_GET,
	XPON_CMD_GET_CAPS,
	XPON_CMD_GET_STATE,
	XPON_CMD_SET_MODE,
	XPON_CMD_EVENT,
	XPON_CMD_SET,

	__XPON_CMD_MAX,
};
#define XPON_CMD_MAX (__XPON_CMD_MAX - 1)

enum xpon_attr {
	XPON_ATTR_UNSPEC,
	XPON_ATTR_IFINDEX,
	XPON_ATTR_MODE,
	XPON_ATTR_AVAILABLE_MODES,
	XPON_ATTR_REGISTRATION,
	XPON_ATTR_CARRIER,
	XPON_ATTR_SIGNAL_DETECT,
	XPON_ATTR_LOS,
	XPON_ATTR_CHANGED,
	XPON_ATTR_TX_ALLOW,		/* u8: the ONU may answer the OLT */
	XPON_ATTR_TX_HOLD,		/* u8: the laser is held off, whatever TX_ALLOW says */
	XPON_ATTR_TX_HOLD_SECONDS,	/* u32: hold off for this long, 0 to release */
	XPON_ATTR_IDENTITY_WAIT,	/* u8: ranging waits for the ONU identity; set 0 */
	XPON_ATTR_EPON_MAC,		/* binary, 6: the LLID 0 MAC; all zero for the default */
	XPON_ATTR_EPON_OAM_LINK,	/* u8: the OAM link is up */
	XPON_ATTR_EPON_GENERATION,	/* u32: the registration an OAM link-up belongs to */
	XPON_ATTR_EPON_REGISTER,	/* u8: LLID 0 may register in the discovery window */
	XPON_ATTR_EPON_KEY_LLID,	/* u8: churning key: the LLID, */
	XPON_ATTR_EPON_KEY_INDEX,	/* u8: its key slot, 0 or 1, */
	XPON_ATTR_EPON_KEY,		/* u32: and the 24-bit key */
	XPON_ATTR_EPON_HOLDOVER_MS,	/* u32: CTC holdover, 0 for off */
	XPON_ATTR_EPON_SYNC,		/* u8: downstream is in sync */
	XPON_ATTR_EPON_NACK_SILENT,	/* u8: the OLT refused registration; quiet backoff */
	XPON_ATTR_EPON_REGISTERED,	/* u32: the registered LLIDs, bit 0 for LLID 0 */
	XPON_ATTR_MODULE_DIAG,		/* binary: SFF-8472 A2h 0x60-0x71, the live readings */
	XPON_ATTR_MODULE_THRESHOLDS,	/* binary: SFF-8472 A2h 0x18-0x27, the power limits */
	XPON_ATTR_SFU_OFFLOAD,		/* u8: forward the services in the switch */
	XPON_ATTR_SFU_UNTAG_VLAN,	/* u16: the VLAN the LAN side sends untagged, 0: none */
	XPON_ATTR_SFU_VLAN_MAP,		/* binary: struct xpon_vlan_map[], empty to clear */

	__XPON_ATTR_MAX,
};
#define XPON_ATTR_MAX (__XPON_ATTR_MAX - 1)

/* A manual LAN-side VLAN: translated to @wan, or stacked under it. */
#define XPON_MODULE_DIAG_LEN		18
#define XPON_MODULE_THRESHOLDS_LEN	16

#define XPON_VLAN_MAP_STACKED	(1 << 0)
#define XPON_VLAN_MAP_MAX	8

struct xpon_vlan_map {
	__u16 wan;
	__u16 lan;
	__u32 flags;
};

#endif /* _UAPI_LINUX_XPON_H */
