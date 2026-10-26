/* SPDX-License-Identifier: GPL-2.0 */
/* The Luna GPON<->NIC contract */
#ifndef _LUNA_GPON_NIC_H
#define _LUNA_GPON_NIC_H

struct omci_onu;
/* @uni_admin_set */
int luna_omci_attach(struct omci_onu *onu, void *cookie, u8 seed,
		    int (*tx)(void *, const u8 *, unsigned int),
		    void (*tx_fence)(void *),
		    int (*uni_admin_set)(void *, unsigned int, bool));

/* Ask the common owner to drive whatever the model has accepted and not yet
 * had applied.  Safe from any context: it only schedules. */
void luna_uni_apply_kick(void);
void luna_omci_detach(void *cookie);
int luna_omci_enqueue(void *cookie, const u8 *msg, unsigned int len);
void luna_omci_set_sn(const u8 sn[8]);
void luna_omci_set_optical(u16 rx, u16 tx);
/* The WAN netdev's MAC, reported in ME 134: call wherever it is set. */
void luna_omci_set_wan_mac(const u8 *mac);
void luna_omci_rx_errors(u32 *bad_mic, u32 *runt);
void luna_omci_report_oper_up(void);
bool luna_gpon_data_ready(void);
/* Normalized SFU services, independent of the in-kernel OMCI responder. */
#define LUNA_L2_SERVICES 16
struct luna_l2_service {
	u16 vid;
	u8 flow;
	u8 lan_mask;
	bool untagged;
};
void rtl9601d_l2_changed(void);

/* omcid in place of the in-kernel responder (luna_xpon.c). */
struct device;
struct net_device;
struct sk_buff;
struct led_classdev;
#if IS_ENABLED(CONFIG_XPON_OMCI)
int luna_xpon_l2_services(struct luna_l2_service *services);
void luna_xpon_service_vids(unsigned long *vids);
bool luna_xpon_wanted(void);
bool luna_xpon_active(void);
bool luna_xpon_data_ready(void);
/* @pon_led and @los_led: the board's lamps, NULL for none; net/xpon drives
 * them from the registration state and the optical LOS. */
int luna_xpon_attach(struct device *dev, struct net_device *wan, void *cookie,
		     int (*tx)(void *, const u8 *, unsigned int),
		     int (*uni_admin_set)(void *, unsigned int, bool),
		     bool (*uni_link)(void *, unsigned int),
		     struct led_classdev *pon_led, struct led_classdev *los_led);
void luna_xpon_detach(void *cookie);
void luna_xpon_omci_rx(const u8 *msg, unsigned int len);
int luna_xpon_tx_flow(struct sk_buff **pskb);
void luna_xpon_rx_fixup(struct sk_buff *skb);
#else
static inline int luna_xpon_l2_services(struct luna_l2_service *services)
{
	return -EOPNOTSUPP;
}
static inline void luna_xpon_service_vids(unsigned long *vids) { }
static inline bool luna_xpon_wanted(void) { return false; }
static inline bool luna_xpon_active(void) { return false; }
static inline bool luna_xpon_data_ready(void) { return false; }
static inline int luna_xpon_attach(struct device *dev, struct net_device *wan,
				   void *cookie,
				   int (*tx)(void *, const u8 *, unsigned int),
				   int (*uni_admin_set)(void *, unsigned int, bool),
				   bool (*uni_link)(void *, unsigned int),
				   struct led_classdev *pon_led,
				   struct led_classdev *los_led)
{
	return -EOPNOTSUPP;
}
static inline void luna_xpon_detach(void *cookie) { }
static inline void luna_xpon_omci_rx(const u8 *msg, unsigned int len) { }
static inline int luna_xpon_tx_flow(struct sk_buff **pskb) { return -ENOENT; }
static inline void luna_xpon_rx_fixup(struct sk_buff *skb) { }
#endif
void luna_gpon_vlan_filter_ok(bool ok);
/* The CPU's WAN control frames ride their own flow on the data T-CONT's next queue
 * (luna_gpon.c ctrl_queue): an offloaded flood fills the data queue, and the PPP LCP
 * echo replies queued behind it were lost (X100DG, V2801RGW, 2026-10-04). */
bool luna_gpon_ctrl_flow_ready(void);

/* A WAN frame the session's life depends on: PPPoE discovery, a PPP control protocol
 * (LCP, PAP, CHAP, IPCP, IPv6CP: protocol >= 0x8000) or ARP -- one 802.1Q tag allowed.
 * Byte math only: the Luna CPUs are big-endian. */
static inline bool luna_wan_control_frame(const u8 *d, unsigned int len)
{
	unsigned int off = 12;
	unsigned int type;

	if (len < off + 2)
		return false;
	type = (d[off] << 8) | d[off + 1];
	if (type == 0x8100 && len >= off + 6) {
		off += 4;
		type = (d[off] << 8) | d[off + 1];
	}
	if (type == 0x0806 || type == 0x8863)
		return true;
	return type == 0x8864 && len >= off + 10 && ((d[off + 8] << 8) | d[off + 9]) >= 0x8000;
}
void luna_gpon_nic_reset_begin(void);
void luna_gpon_nic_reset_end(void);

/* Arm the GMAC OMCI trap for downstream stream-id @sid (the OMCC). Provided by
 * rtl9602c_eth.c; called from the GPON Configure_Port-ID handler. */
void rtl9602c_eth_set_omci_sid(unsigned int sid);

/* Provision the ONU identity (G.984.3 ONU-SN: 4 ASCII vendor + 4 serial bytes)
 * so the eth driver's OMCI responder reports an ONU-G Vendor-ID/Serial matching
 * the PLOAM Serial_Number the OLT ranged. Provided by rtl9602c_eth.c. */
void rtl9602c_eth_set_omci_identity(const u8 *sn8);

/* DS OMCI frames that reached the CPU NIC ring */
#define GPON_OMCI_RX_UNAVAIL	0xffffffffu
u32 rtl9602c_eth_omci_rx_count(void);

/* gpon0 (WAN) RX packet count, used by the GPON */
u32 rtl9602c_eth_wan_rx_count(void);

/* US-OMCI TX-ring reclaim cursor ("dirty"): count of OMCC descriptors the HW has
 * consumed (OWN cleared). Non-zero => the OMCC TX ring is being fetched. Defined
 * in rtl9602c_eth.c; surfaced for the periodic O5 serial diagnostic. */
u32 rtl9602c_eth_omci_tx_dirty(void);

/* US-OMCI responses the shell's TX ring REFUSED. */
static inline u32 rtl9602c_eth_omci_tx_dropped(void) { return 0; }

/* OLT-INDEPENDENT US-OMCI datapath self-test: inject a synthetic OMCI frame
 * through the US-OMCI TX path so RX_SID_GOOD_CNT_US[4] can be checked at O5
 * without the OLT sending DS OMCI. Defined in rtl9602c_eth.c. */
void rtl9602c_eth_omci_selftest(void);

/* Full PON US/DS-NIC + PBO bring-up (defined in luna_gpon.c). Non-__init: also
 * re-run from rtl9602c_eth_open() after the GMAC IP-block reset so the US-NIC RX
 * engine re-latches against the freshly-reset GMAC (stock order: GMAC reset -> NIC). */
void gpon_pbo_init(void);

/* Faithful port of the stock SDK rtk_all_module_init() GPON datapath bring-up,
 * run on the quiescent switch in the eth reset path (after the GMAC reset + swcore
 * resync, before the GMAC is programmed/armed). Defined in luna_gpon.c. */
void rtl9602c_datapath_tables_init(void);

/* WAN data-GEM datapath. */
#define GPON_DATA_FLOW		1
#define GPON_DATA_GEM_DEFAULT	193u

/* The OLT also provisions a MULTICAST/broadcast GEM (OMCI ME268 inst=1 Port-ID=0x0fff,
 * paired with ME281 Multicast GEM IWTP). Broadcast DS (e.g. the DHCP OFFER the server
 * can only broadcast to an IP-less client) may ride this GEM, not the unicast data GEM.
 * Wire it to its own internal DS flow so broadcast DS de-encapsulates to the CPU/gpon0. */
#define GPON_MCAST_FLOW	2
#define GPON_CTRL_FLOW		3
#define GPON_MCAST_GEM	0xfffu

/* Timer-owned data installation consumes the accepted common OMCI binding. */
int gpon_install_data_gem(void);

/* Publish the cached live DDM optical levels into ME */
#if IS_ENABLED(CONFIG_RTL9602C_ETH)
void rtl9602c_eth_omci_set_optical(s16 rx_level, s16 tx_level);
#else
static inline void rtl9602c_eth_omci_set_optical(s16 rx_level, s16 tx_level)
{
}
#endif

/* Emit an OMCI Attribute-Value-Change reporting the HGU WAN-egress (VEIP ME329) operational,
 * so the OLT un-gates downstream user-data forwarding. The OLT never polls the data MEs after
 * creating them; it waits for this AVC. Defined in rtl9602c_eth.c; called from the GPON FSM a
 * few seconds after O5 (config-apply done). */
void rtl9602c_eth_omci_report_oper_up(void);


#endif /* _LUNA_GPON_NIC_H */
