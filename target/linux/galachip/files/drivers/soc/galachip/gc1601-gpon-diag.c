// SPDX-License-Identifier: GPL-2.0-only
/*
 * GalaChip GC1601 GPON diagnostics: load it on an ONU that does not come up
 * (or not fully) on some OLT and send the dmesg back. It changes nothing.
 *
 * On load: activation state, Alloc-IDs, GEM ports, the OMCI T-CONT map,
 * every OMCI service (VLAN -> GEM/T-CONT, UNI VLAN treatment), the switch
 * flows and the LAN VLAN rewrite. Then, as they happen: O-state changes,
 * PLOAMs in both directions by name, OMCI messages (type, ME class and,
 * for creates and sets, the contents), T-CONT/GEM/service changes, switch
 * flow updates, the first data frames, and upstream frames no service
 * carries. Serial numbers and passwords are masked unless mask=0.
 */

#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/module.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/xpon/omci.h>

#include "gc1601-pon.h"

static unsigned int ploam_log = 300;
module_param(ploam_log, uint, 0644);
MODULE_PARM_DESC(ploam_log, "PLOAMs to log, No_message excluded (default 300)");

static unsigned int omci_log = 1000;
module_param(omci_log, uint, 0644);
MODULE_PARM_DESC(omci_log, "OMCI messages to log (default 1000)");

static bool omci_dump = true;
module_param(omci_dump, bool, 0644);
MODULE_PARM_DESC(omci_dump, "Hex dump of OMCI creates, sets and their responses (default on)");

static unsigned int rx_log = 32;
module_param(rx_log, uint, 0644);
MODULE_PARM_DESC(rx_log, "Downstream data frames to log (default 32)");

static unsigned int tx_log = 32;
module_param(tx_log, uint, 0644);
MODULE_PARM_DESC(tx_log, "Upstream data frames to log, refused ones included (default 32)");

static bool mask = true;
module_param(mask, bool, 0644);
MODULE_PARM_DESC(mask, "Mask serial numbers and passwords (default on)");

static unsigned int summary_secs = 30;
module_param(summary_secs, uint, 0644);
MODULE_PARM_DESC(summary_secs, "Seconds between summaries, 0 for none (default 30)");

static struct gc1601_pon *diag_pon;
static struct delayed_work diag_summary;
static atomic_t ploams_logged, omci_logged, rx_logged, tx_logged;

/* ---- names ---- */

static const char *ploam_down_name(u8 id)
{
	static const char * const names[] = {
		[1] = "Upstream_Overhead", [2] = "Serial_number_mask",
		[3] = "Assign_ONU-ID", [4] = "Ranging_Time",
		[5] = "Deactivate_ONU-ID", [6] = "Disable_serial_number",
		[8] = "Encrypted_Port-ID", [9] = "Request_password",
		[10] = "Assign_Alloc-ID", [11] = "No_message", [12] = "POPUP",
		[13] = "Request_Key", [14] = "Configure_Port-ID",
		[15] = "Physical_Equipment_Error", [16] = "Change_Power_Level",
		[17] = "PST", [18] = "BER_Interval", [19] = "Key_Switching_Time",
		[20] = "Extended_Burst_Length", [21] = "PON-ID",
		[22] = "Swift_POPUP", [23] = "Ranging_Adjustment",
		[24] = "Sleep_Allow",
	};

	return id < ARRAY_SIZE(names) && names[id] ? names[id] : "unknown";
}

static const char *ploam_up_name(u8 id)
{
	static const char * const names[] = {
		[1] = "Serial_number_ONU", [2] = "Password", [3] = "Dying_Gasp",
		[4] = "No_message", [5] = "Encryption_key",
		[6] = "Physical_Equipment_Error", [7] = "PST", [8] = "REI",
		[9] = "Acknowledge", [10] = "Sleep_Request",
	};

	return id < ARRAY_SIZE(names) && names[id] ? names[id] : "unknown";
}

static const char *omci_action_name(u8 action)
{
	static const char * const names[] = {
		[4] = "Create", [6] = "Delete", [8] = "Set", [9] = "Get",
		[11] = "Get_all_alarms", [12] = "Get_all_alarms_next",
		[13] = "MIB_upload", [14] = "MIB_upload_next", [15] = "MIB_reset",
		[16] = "Alarm", [17] = "AVC", [18] = "Test",
		[19] = "Start_SW_download", [20] = "Download_section",
		[21] = "End_SW_download", [22] = "Activate_image",
		[23] = "Commit_image", [24] = "Synchronize_time", [25] = "Reboot",
		[26] = "Get_next", [27] = "Test_result",
		[28] = "Get_current_data", [29] = "Set_table",
	};

	return action < ARRAY_SIZE(names) && names[action] ? names[action] : "unknown";
}

static const char *me_name(u16 class)
{
	switch (class) {
	case 2: return "ONT_data";
	case 5: return "Cardholder";
	case 6: return "Circuit_pack";
	case 7: return "Software_image";
	case 11: return "PPTP_Ethernet_UNI";
	case 45: return "MAC_bridge_service_profile";
	case 46: return "MAC_bridge_configuration_data";
	case 47: return "MAC_bridge_port_config_data";
	case 48: return "MAC_bridge_port_designation_data";
	case 49: return "MAC_bridge_port_filter_table";
	case 79: return "MAC_bridge_port_filter_preassign";
	case 84: return "VLAN_tagging_filter_data";
	case 130: return "802.1p_mapper_service_profile";
	case 131: return "OLT-G";
	case 133: return "ONU_power_shedding";
	case 134: return "IP_host_config_data";
	case 136: return "TCP/UDP_config_data";
	case 137: return "Network_address";
	case 148: return "Authentication_security_method";
	case 158: return "ONU_remote_debug";
	case 171: return "Extended_VLAN_tagging_operation";
	case 256: return "ONU-G";
	case 257: return "ONU2-G";
	case 262: return "T-CONT";
	case 263: return "ANI-G";
	case 264: return "UNI-G";
	case 266: return "GEM_interworking_TP";
	case 268: return "GEM_port_network_CTP";
	case 272: return "GAL_Ethernet_profile";
	case 277: return "Priority_queue";
	case 278: return "Traffic_scheduler";
	case 280: return "Traffic_descriptor";
	case 281: return "Multicast_GEM_interworking_TP";
	case 287: return "OMCI";
	case 290: return "Dot1X_port_extension";
	case 297: return "Port_mapping_package";
	case 309: return "Multicast_operations_profile";
	case 310: return "Multicast_subscriber_config_info";
	case 329: return "VEIP";
	case 330: return "Generic_status_portal";
	case 332: return "Enhanced_security_control";
	case 334: return "Ethernet_frame_extended_PM";
	case 340: return "BBF_TR-069_management_server";
	case 347: return "IPv6_host_config_data";
	case 425: return "Ethernet_frame_extended_PM_64bit";
	default: return class >= 240 && class <= 255 ? "vendor_specific" : "?";
	}
}

/* ---- live hooks ---- */

static void diag_state(struct gc1601_pon *pon, int from, int to)
{
	dev_info(pon->dev, "gpon-diag: O%d -> O%d (ONU-ID %u)\n", from, to,
		 READ_ONCE(pon->gpon.onu_id));
}

static void diag_ploam(struct gc1601_pon *pon, bool up, const u8 *m)
{
	u8 msg[12];

	if (atomic_read(&ploams_logged) >= ploam_log)
		return;
	memcpy(msg, m, sizeof(msg));
	if (mask) {
		if (up && msg[1] == 1)			/* Serial_number_ONU: keep the vendor */
			memset(msg + 6, 0xee, 4);
		else if (up && msg[1] == 2)		/* Password */
			memset(msg + 2, 0xee, 10);
		else if (!up && msg[1] == 3)		/* Assign_ONU-ID: serial after the ID */
			memset(msg + 7, 0xee, 4);
	}
	atomic_inc(&ploams_logged);
	dev_info(pon->dev, "gpon-diag: PLOAM %s ONU %u %s(%u) %*ph\n", up ? "up  " : "down",
		 msg[0], up ? ploam_up_name(msg[1]) : ploam_down_name(msg[1]), msg[1],
		 10, msg + 2);
}

static void diag_ploam_rx(struct gc1601_pon *pon, const u8 *msg)
{
	if (msg[1] != 11)	/* No_message floods the downstream */
		diag_ploam(pon, false, msg);
}

static void diag_ploam_tx(struct gc1601_pon *pon, const u8 *msg, int ret)
{
	if (msg[1] != 4)
		diag_ploam(pon, true, msg);
	if (ret)
		dev_info(pon->dev, "gpon-diag: upstream PLOAM %s not sent: %d\n",
			 ploam_up_name(msg[1]), ret);
}

static void diag_omci(struct gc1601_pon *pon, bool tx, const u8 *d, unsigned int len,
		      int ret)
{
	u8 type, action;
	u16 class;
	bool dump;

	if (len < 8 || atomic_inc_return(&omci_logged) > omci_log)
		return;
	type = d[2];
	action = type & 0x1f;
	class = get_unaligned_be16(d + 4);
	dev_info(pon->dev,
		 "gpon-diag: OMCI %s tci=%04x %s%s%s dev=%02x ME %u %s inst=%#06x%s len=%u%s\n",
		 tx ? "tx" : "rx", get_unaligned_be16(d), omci_action_name(action),
		 type & 0x40 ? " AR" : "", type & 0x20 ? " AK" : "", d[3], class,
		 me_name(class), get_unaligned_be16(d + 6),
		 type & 0x20 && d[3] == 0x0a && len > 8 ? (d[8] ? " result!=0" : " ok") : "",
		 len, ret ? " (send failed)" : "");
	if (type & 0x20 && d[3] == 0x0a && len > 8 && d[8])
		dev_info(pon->dev, "gpon-diag: OMCI result %u (1 processing error, 2 not supported, 3 parameter error, 4 unknown ME, 5 unknown instance, 6 device busy, 7 instance exists, 9 attribute failed)\n",
			 d[8]);

	/* Contents of what configures the data path; ONU-G and auth hold secrets. */
	dump = omci_dump && (action == 4 || action == 8 || action == 6) &&
	       !(mask && (class == 256 || class == 148));
	if (dump)
		print_hex_dump(KERN_INFO, "gpon-diag:   ", DUMP_PREFIX_OFFSET, 16, 1,
			       d + 8, min(len - 8, 40U), false);
}

static void diag_rx(struct gc1601_pon *pon, u16 gem, const u8 *d, unsigned int len)
{
	u16 type = len >= ETH_HLEN ? get_unaligned_be16(d + 2 * ETH_ALEN) : 0;

	if (atomic_inc_return(&rx_logged) > rx_log)
		return;
	if (type == ETH_P_8021Q && len >= VLAN_ETH_HLEN)
		dev_info(pon->dev, "gpon-diag: rx GEM %u len=%u %pM <- %pM VLAN %u pcp %u type %04x\n",
			 gem, len, d, d + ETH_ALEN, get_unaligned_be16(d + 14) & VLAN_VID_MASK,
			 d[14] >> 5, get_unaligned_be16(d + 16));
	else
		dev_info(pon->dev, "gpon-diag: rx GEM %u len=%u %pM <- %pM untagged type %04x\n",
			 gem, len, d, d + ETH_ALEN, type);
}

static void diag_tx(struct gc1601_pon *pon, u16 gem, int tcont, const u8 *d,
		    unsigned int len, int ret)
{
	u16 type = len >= ETH_HLEN ? get_unaligned_be16(d + 2 * ETH_ALEN) : 0;
	int vid = type == ETH_P_8021Q && len >= VLAN_ETH_HLEN ?
		  get_unaligned_be16(d + 14) & VLAN_VID_MASK : -1;

	if (atomic_inc_return(&tx_logged) > tx_log)
		return;
	if (ret == -ENOENT)
		dev_info(pon->dev, "gpon-diag: tx refused: no OMCI service for %s %d (type %04x)\n",
			 vid < 0 ? "untagged," : "VLAN", vid < 0 ? 0 : vid, type);
	else
		dev_info(pon->dev, "gpon-diag: tx GEM %u T-CONT %d len=%u VLAN %d type %04x\n",
			 gem, tcont, len, vid, type);
}

static void diag_tcont(struct gc1601_pon *pon, u16 entity, u16 alloc_id, bool valid)
{
	dev_info(pon->dev, "gpon-diag: OMCI T-CONT %#06x -> Alloc-ID %u%s\n", entity,
		 alloc_id, valid ? "" : " (released)");
}

static void diag_gem(struct gc1601_pon *pon, u16 entity, u16 port, u16 tcont_entity,
		     u8 direction, bool valid, bool encrypted)
{
	static const char * const dirs[] = { "?", "up", "down", "both" };

	dev_info(pon->dev, "gpon-diag: OMCI GEM CTP %#06x port %u T-CONT %#06x %s%s%s\n",
		 entity, port, tcont_entity, dirs[direction & 3], encrypted ? " AES" : "",
		 valid ? "" : " (deleted)");
}

static void diag_service(struct gc1601_pon *pon, const struct omci_service_config *s)
{
	static const char * const uni[] = { "same", "untagged", "translate", "stacked" };

	dev_info(pon->dev,
		 "gpon-diag: service %u: UNI %#06x GEM %u (CTP %#06x) Alloc-ID %u queue %u VLAN %s%u pcp %s%u LAN %s %u%s%s\n",
		 s->cookie, s->uni_entity_id, s->gem_port_id, s->gem_ctp_entity_id,
		 s->alloc_id, s->queue, s->vlan_valid ? "" : "untagged/", s->vlan_id,
		 s->pcp_valid ? "" : "any/", s->pcp,
		 s->uni_vlan_mode < ARRAY_SIZE(uni) ? uni[s->uni_vlan_mode] : "?",
		 s->uni_vlan_id, s->multicast ? " multicast" : "",
		 s->default_service ? " default" : "");
	if (s->vlan_treatment_valid)
		dev_info(pon->dev, "gpon-diag:   VLAN rule (ME 171) %8ph\n", s->vlan_treatment);
}

static void diag_service_delete(struct gc1601_pon *pon, u32 cookie)
{
	dev_info(pon->dev, "gpon-diag: service %u deleted\n", cookie);
}

static const char *lan_name(u8 lan)
{
	static const char * const names[] = { "same", "untagged", "translate", "stacked" };

	return lan < ARRAY_SIZE(names) ? names[lan] : "?";
}

static void diag_flows(struct gc1601_pon *pon, const struct gc1601_sfu_flow *f,
		       unsigned int n, int ret)
{
	unsigned int i;

	dev_info(pon->dev, "gpon-diag: switch flows: %u, packet engine %s (%d)\n", n,
		 ret ? "refused" : "accepted", ret);
	for (i = 0; i < n; i++)
		dev_info(pon->dev, "gpon-diag:   flow %u: %s %u GEM %u T-CONT %u LAN %s %u\n",
			 i, f[i].untagged ? "untagged" : "VLAN", f[i].vlan_id,
			 f[i].gem_port, f[i].tcont, lan_name(f[i].lan), f[i].lan_vid);
}

static const struct gc1601_gpon_diag_ops diag_ops = {
	.state = diag_state,
	.ploam_rx = diag_ploam_rx,
	.ploam_tx = diag_ploam_tx,
	.omci = diag_omci,
	.rx = diag_rx,
	.tx = diag_tx,
	.tcont = diag_tcont,
	.gem = diag_gem,
	.service = diag_service,
	.service_delete = diag_service_delete,
	.flows = diag_flows,
};

/* ---- snapshot ---- */

static void diag_snapshot(struct gc1601_pon *pon)
{
	struct gc1601_gpon *g = &pon->gpon;
	struct gc1601_xpon *xp = &pon->xp;
	unsigned int i, n = 0;
	char gems[256];
	size_t len = 0;

	dev_info(pon->dev, "gpon-diag: mode=%s running=%d allow_tx=%d state=O%d onu_id=%u ranged=%d los=%d lof=%d omcc GEM %u%s\n",
		 pon->mode == GC1601_PON_EPON ? "epon" : "gpon", g->running, g->allow_tx,
		 g->state, g->onu_id, g->ranged, g->los, g->lof, g->omcc_port,
		 g->omcc_valid ? "" : " (not set)");
	dev_info(pon->dev, "gpon-diag: PLOAM rx=%llu tx=%llu repeats=%llu refused=%llu, LOS events %llu, LOF events %llu\n",
		 g->ploam_rx, g->ploam_tx, g->ploam_repeats, g->tx_refused, g->los_events,
		 g->lof_events);

	for (i = 0; i < GPON_TCONTS; i++)
		if (g->alloc[i] != U16_MAX)
			dev_info(pon->dev, "gpon-diag: T-CONT %u: Alloc-ID %u (PLOAM)\n", i, g->alloc[i]);
	for (i = 0; i < GPON_TCONTS; i++)
		if (xp->tcont_alloc[i] != U16_MAX)
			dev_info(pon->dev, "gpon-diag: OMCI T-CONT %#06x: Alloc-ID %u\n",
				 xp->tcont_entity[i], xp->tcont_alloc[i]);

	for (i = 0; i < 4096; i++) {
		if (!test_bit(i, g->gem))
			continue;
		n++;
		if (len < sizeof(gems) - 16)
			len += scnprintf(gems + len, sizeof(gems) - len, " %u%s", i,
					 test_bit(i, g->gem_aes) ? "(AES)" : "");
	}
	dev_info(pon->dev, "gpon-diag: GEM ports: %u:%s\n", n, n ? gems : " none");

	spin_lock_bh(&xp->service_lock);
	dev_info(pon->dev, "gpon-diag: services: %u\n", xp->services);
	for (i = 0; i < GPON_SERVICES; i++) {
		const struct gc1601_service *s = &xp->service[i];

		if (!s->used)
			continue;
		dev_info(pon->dev, "gpon-diag:   service %u: VLAN %s%u GEM %u Alloc-ID %u queue %u LAN %s %u\n",
			 s->cookie, s->vlan_valid ? "" : "untagged/", s->vlan_id, s->gem_port,
			 s->alloc_id, s->queue, lan_name(s->uni_mode), s->uni_vid);
	}
	spin_unlock_bh(&xp->service_lock);

	diag_flows(pon, xp->sfu, xp->sfu_count == U32_MAX ? 0 : xp->sfu_count, 0);
	len = 0;
	for (i = 0; i < xp->vlan_maps; i++)
		len += scnprintf(gems + len, sizeof(gems) - len, " %c%u:%u",
				 xp->vlan_map[i].mode == GC1601_SFU_LAN_STACKED ? 's' : 't',
				 xp->vlan_map[i].wan, xp->vlan_map[i].lan);
	dev_info(pon->dev, "gpon-diag: LAN VLAN: untagged VLAN %u, map:%s\n",
		 xp->sfu_untag_vlan, xp->vlan_maps ? gems : " none");
	dev_info(pon->dev, "gpon-diag: OMCI rx=%lu to_core=%lu errors=%lu crc_errors=%lu, tx=%lu failures=%lu last_error=%d\n",
		 xp->omci_rx_seen, xp->omci_rx_to_core, xp->omci_rx_errors, xp->omci_crc_errors,
		 xp->omci_tx_attempts, xp->omci_tx_failures, xp->omci_tx_last_error);
}

static void diag_summary_work(struct work_struct *w)
{
	struct gc1601_pon *pon = diag_pon;
	struct gc1601_gpon *g = &pon->gpon;

	/* Quiet while the other PON mode runs. */
	if (READ_ONCE(pon->mode) != GC1601_PON_GPON)
		goto again;
	dev_info(pon->dev, "gpon-diag: summary O%d onu_id=%u los=%d lof=%d PLOAM rx=%llu tx=%llu OMCI rx=%lu tx=%lu failures=%lu services=%u flows=%u\n",
		 g->state, g->onu_id, g->los, g->lof, g->ploam_rx, g->ploam_tx,
		 pon->xp.omci_rx_seen, pon->xp.omci_tx_attempts, pon->xp.omci_tx_failures,
		 pon->xp.services, pon->xp.sfu_count == U32_MAX ? 0 : pon->xp.sfu_count);
again:
	if (summary_secs)
		schedule_delayed_work(&diag_summary, secs_to_jiffies(summary_secs));
}

static int __init gc1601_gpon_diag_init(void)
{
	struct gc1601_pon *pon = gc1601_gpon_diag_register(&diag_ops);

	if (IS_ERR(pon))
		return PTR_ERR(pon);
	diag_pon = pon;

	dev_info(pon->dev, "gpon-diag: loaded%s\n", mask ? ", serials and passwords masked" : "");
	diag_snapshot(pon);
	INIT_DELAYED_WORK(&diag_summary, diag_summary_work);
	if (summary_secs)
		schedule_delayed_work(&diag_summary, secs_to_jiffies(summary_secs));
	return 0;
}

static void __exit gc1601_gpon_diag_exit(void)
{
	cancel_delayed_work_sync(&diag_summary);
	gc1601_gpon_diag_unregister();
	dev_info(diag_pon->dev, "gpon-diag: unloaded\n");
}

module_init(gc1601_gpon_diag_init);
module_exit(gc1601_gpon_diag_exit);

MODULE_DESCRIPTION("GalaChip GC1601 GPON diagnostics");
MODULE_LICENSE("GPL");
