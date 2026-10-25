// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2026  Realtek RTL8192FE clean-room contributors */

#include "../wifi.h"
#include "../pci.h"
#include "../base.h"
#include "../stats.h"
#include "reg.h"
#include "def.h"
#include "phy.h"
#include "trx.h"
#include "led.h"
#include "dm.h"
#include "fw.h"

static u8 _rtl92fe_map_hwqueue_to_fwqueue(struct sk_buff *skb, u8 hw_queue)
{
	__le16 fc = rtl_get_fc(skb);

	if (unlikely(ieee80211_is_beacon(fc)))
		return QSLT_BEACON;
	if (ieee80211_is_mgmt(fc) || ieee80211_is_ctl(fc))
		return QSLT_MGNT;

	return skb->priority;
}

/* The RTL8192F PHY-status report is a PAGED 28-byte block, byte 0 low nibble = page:
 *   page 0 (CCK)  byte 1 PWDB (dBm = PWDB - 110: the new CCK AGC, 0xa9c[17]), byte 12 SQ
 *   page 1 (OFDM) bytes 1..4 PWDB per path, 16..19 EVM (s8, half dB), 20..23 CFO tail (s8),
 *                 24..27 SNR (s8, half dB)
 *   page 2        bytes 1..4 PWDB per path only -- no EVM, no CFO
 * The legacy AGC-report layout this port carried until 2026-09-28 read a valid page-1 block
 * as -110 dBm with CFO 0 -- the "-100 dBm at 1 m" -- and fed that to the DM. Explicit byte
 * math: no struct overlay (dev/rtl9607c-test/rtl8192fe_physts_test.c). */
static bool _rtl92fe_query_rxphystatus(struct ieee80211_hw *hw,
				       struct rtl_stats *pstatus, u8 *pdesc,
				       struct rx_fwinfo *p_drvinfo,
				       bool bpacket_match_bssid,
				       bool bpacket_toself,
				       bool packet_beacon)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	const u8 *b = (const u8 *)p_drvinfo;
	u8 page = b[0] & 0x0f;
	bool valid = page <= 2;
	s8 rx_pwr_all = -110, rx_pwr;
	u8 pwdb_all = 0, rf_rx_num = 0, i, max_ss;
	u32 total_rssi = 0;

	/* `rtl_stats` lives OUTSIDE pci.c's RX loop: every field a page may not
	 * write is cleared here, or the previous frame's level is inherited. */
	pstatus->packet_matchbssid = bpacket_match_bssid;
	pstatus->packet_toself = bpacket_toself;
	pstatus->packet_beacon = packet_beacon;
	pstatus->rx_mimo_signalquality[0] = -1;
	pstatus->rx_mimo_signalquality[1] = -1;
	pstatus->signalquality = 0;
	pstatus->rxpower = -110;
	memset(pstatus->rx_pwr, 0, sizeof(pstatus->rx_pwr));
	memset(pstatus->rx_mimo_signalstrength, 0,
	       sizeof(pstatus->rx_mimo_signalstrength));

	/* Reject positive dBm and signed wrap before either RSSI consumer. */
	if (page == 0)
		valid = b[1] <= 110;
	else if (page == 1 || page == 2)
		for (i = RF90_PATH_A; i < RF6052_MAX_PATH; i++)
			if (rtlpriv->dm.rfpath_rxenable[i] && b[1 + i] > 110)
				valid = false;

	if (valid && page == 0) {
		u8 sq_rpt = b[12];

		rx_pwr_all = (s8)((int)b[1] - 110);
		pwdb_all = rtl_query_rxpwrpercentage(rx_pwr_all);
		if (bpacket_match_bssid) {
			u8 sq = sq_rpt >= 64 ? 0 :
				sq_rpt <= 20 ? 100 : (u8)((64 - sq_rpt) * 9 / 4);

			pstatus->signalquality = sq;
			pstatus->rx_mimo_signalquality[0] = sq;
		}
	} else if (valid && (page == 1 || page == 2)) {
		for (i = RF90_PATH_A; i < RF6052_MAX_PATH; i++) {
			if (!rtlpriv->dm.rfpath_rxenable[i])
				continue;
			rf_rx_num++;
			rx_pwr = (s8)((int)b[1 + i] - 110);
			pstatus->rx_pwr[i] = rx_pwr;
			pstatus->rx_mimo_signalstrength[i] =
				rtl_query_rxpwrpercentage(rx_pwr);
			total_rssi += pstatus->rx_mimo_signalstrength[i];
			if (rx_pwr > rx_pwr_all)
				rx_pwr_all = rx_pwr;
		}
		pwdb_all = rtl_query_rxpwrpercentage(rx_pwr_all);
		pstatus->rxpower = rx_pwr_all;
		if (page == 1) {
			max_ss = (pstatus->rate >= DESC_RATEMCS8 &&
				  pstatus->rate <= DESC_RATEMCS15) ? 2 : 1;
			for (i = 0; i < max_ss; i++) {
				/* -128 is the PHY's "no EVM" sentinel: quality 0, never 100 */
				s8 evm_half_db = (s8)b[16 + i];
				u8 evm = evm_half_db == -128 ? 0 :
					 rtl_evm_db_to_percentage(evm_half_db / 2);

				if (bpacket_match_bssid) {
					if (i == 0)
						pstatus->signalquality = evm;
					pstatus->rx_mimo_signalquality[i] = evm;
				}
			}
			if (bpacket_match_bssid) {
				for (i = RF90_PATH_A; i <= RF90_PATH_B; i++)
					rtlpriv->dm.cfo_tail[i] = (s8)b[20 + i];
				if (rtlpriv->dm.packet_count == 0xffffffff)
					rtlpriv->dm.packet_count = 0;
				else
					rtlpriv->dm.packet_count++;
			}
		}
	}
	pstatus->rx_pwdb_all = pwdb_all;
	pstatus->bt_rx_rssi_percentage = pwdb_all;
	pstatus->recvsignalpower = rx_pwr_all;
	if (page == 0 || rf_rx_num == 0)
		pstatus->signalstrength =
			(u8)rtl_signal_scale_mapping(hw, pwdb_all);
	else
		pstatus->signalstrength =
			(u8)rtl_signal_scale_mapping(hw, total_rssi / rf_rx_num);
	return valid;
}

static bool _rtl92fe_translate_rx_signal_stuff(struct ieee80211_hw *hw,
					       struct sk_buff *skb,
					       struct rtl_stats *pstatus,
					       u8 *pdesc,
					       struct rx_fwinfo *p_drvinfo)
{
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	struct rtl_efuse *rtlefuse = rtl_efuse(rtl_priv(hw));
	struct ieee80211_hdr *hdr;
	u8 *tmp_buf;
	u8 *praddr;
	u8 *psaddr;
	__le16 fc;
	bool packet_matchbssid, packet_toself, packet_beacon;

	tmp_buf = skb->data + pstatus->rx_drvinfo_size +
		  pstatus->rx_bufshift + 24;

	hdr = (struct ieee80211_hdr *)tmp_buf;
	fc = hdr->frame_control;
	praddr = hdr->addr1;
	psaddr = ieee80211_get_SA(hdr);
	ether_addr_copy(pstatus->psaddr, psaddr);

	packet_matchbssid = (!ieee80211_is_ctl(fc) &&
			     (ether_addr_equal(mac->bssid,
					ieee80211_has_tods(fc) ?
					hdr->addr1 :
					ieee80211_has_fromds(fc) ?
					hdr->addr2 : hdr->addr3)) &&
			      (!pstatus->hwerror) && (!pstatus->crc) &&
			      (!pstatus->icv));

	packet_toself = packet_matchbssid &&
			(ether_addr_equal(praddr, rtlefuse->dev_addr));

	if (ieee80211_is_beacon(fc))
		packet_beacon = true;
	else
		packet_beacon = false;

	if (packet_beacon && packet_matchbssid)
		rtl_priv(hw)->dm.dbginfo.num_qry_beacon_pkt++;

	if (packet_matchbssid && ieee80211_is_data_qos(hdr->frame_control) &&
	    !is_multicast_ether_addr(ieee80211_get_DA(hdr))) {
		struct ieee80211_qos_hdr *hdr_qos =
					(struct ieee80211_qos_hdr *)tmp_buf;
		u16 tid = le16_to_cpu(hdr_qos->qos_ctrl) & 0xf;

		if (tid != 0 && tid != 3)
			rtl_priv(hw)->dm.dbginfo.num_non_be_pkt++;
	}

	/* an unknown page carries no level: nothing is fed to the DM as a
	 * zero-strength sample, and the caller marks the frame NO_SIGNAL_VAL */
	if (!_rtl92fe_query_rxphystatus(hw, pstatus, pdesc, p_drvinfo,
					packet_matchbssid, packet_toself,
					packet_beacon))
		return false;
	rtl_process_phyinfo(hw, tmp_buf, pstatus);
	return true;
}

static void _rtl92fe_insert_emcontent(struct rtl_tcb_desc *ptcb_desc,
				      u8 *virtualaddress8)
{
	u32 dwtmp;
	__le32 *virtualaddress = (__le32 *)virtualaddress8;

	memset(virtualaddress, 0, 8);

	set_earlymode_pktnum(virtualaddress, ptcb_desc->empkt_num);
	if (ptcb_desc->empkt_num == 1) {
		dwtmp = ptcb_desc->empkt_len[0];
	} else {
		dwtmp = ptcb_desc->empkt_len[0];
		dwtmp += ((dwtmp % 4) ? (4 - dwtmp % 4) : 0) + 4;
		dwtmp += ptcb_desc->empkt_len[1];
	}
	set_earlymode_len0(virtualaddress, dwtmp);

	if (ptcb_desc->empkt_num <= 3) {
		dwtmp = ptcb_desc->empkt_len[2];
	} else {
		dwtmp = ptcb_desc->empkt_len[2];
		dwtmp += ((dwtmp % 4) ? (4 - dwtmp % 4) : 0) + 4;
		dwtmp += ptcb_desc->empkt_len[3];
	}
	set_earlymode_len1(virtualaddress, dwtmp);
	if (ptcb_desc->empkt_num <= 5) {
		dwtmp = ptcb_desc->empkt_len[4];
	} else {
		dwtmp = ptcb_desc->empkt_len[4];
		dwtmp += ((dwtmp % 4) ? (4 - dwtmp % 4) : 0) + 4;
		dwtmp += ptcb_desc->empkt_len[5];
	}
	set_earlymode_len2_1(virtualaddress, dwtmp & 0xF);
	set_earlymode_len2_2(virtualaddress, dwtmp >> 4);
	if (ptcb_desc->empkt_num <= 7) {
		dwtmp = ptcb_desc->empkt_len[6];
	} else {
		dwtmp = ptcb_desc->empkt_len[6];
		dwtmp += ((dwtmp % 4) ? (4 - dwtmp % 4) : 0) + 4;
		dwtmp += ptcb_desc->empkt_len[7];
	}
	set_earlymode_len3(virtualaddress, dwtmp);
	if (ptcb_desc->empkt_num <= 9) {
		dwtmp = ptcb_desc->empkt_len[8];
	} else {
		dwtmp = ptcb_desc->empkt_len[8];
		dwtmp += ((dwtmp % 4) ? (4 - dwtmp % 4) : 0) + 4;
		dwtmp += ptcb_desc->empkt_len[9];
	}
	set_earlymode_len4(virtualaddress, dwtmp);
}

/* handshake spy: a permanent, always-on in-tree instrument ... -- dev/MEASURED-trx.c.md sec 1. */
#ifdef CONFIG_RTLWIFI_DEBUG
static bool rtl92f_spy_on = true;
module_param_named(spy, rtl92f_spy_on, bool, 0644);
MODULE_PARM_DESC(spy, "1=log mgmt+EAPOL frames rx/tx to dmesg for 4-way debug (default on)");
#else
static const bool rtl92f_spy_on;
#endif

/* When on, also log EVERY unicast DATA frame (encrypted or not) at rx/tx -- the
 * definitive "does unicast downstream data reach tx_fill_desc" probe (run an
 * ONU->client unicast ICMP flood). Default off: too chatty for normal traffic. */
#ifdef CONFIG_RTLWIFI_DEBUG
static bool rtl92f_spy_data;
module_param_named(spy_data, rtl92f_spy_data, bool, 0644);
MODULE_PARM_DESC(spy_data, "1=also log every unicast DATA frame rx/tx (default off)");
#else
static const bool rtl92f_spy_data;
#endif

static const char *rtl92f_eapol_msg(u16 ki)
{
	if (!(ki & BIT(3)))			return (ki & BIT(7)) ? "G1" : "G2";
	if ((ki & BIT(7)) && !(ki & BIT(8)))	return "M1";
	if ((ki & BIT(7)) &&  (ki & BIT(8)))	return "M3";
	return (ki & BIT(9)) ? "M4" : "M2";	/* M4 = MIC set, no Ack */
}

static void rtl92f_spy(const char *dir, struct ieee80211_hdr *hdr, u32 len,
		       u8 rate, u16 crc, u16 icv)
{
	__le16 fc = hdr->frame_control;
	const u8 *p = (const u8 *)hdr;
	u32 hlen;

	if (!rtl92f_spy_on)
		return;
	if (ieee80211_is_mgmt(fc)) {
		u16 reason = 0;

		if (!(ieee80211_is_auth(fc) || ieee80211_is_assoc_req(fc) ||
		      ieee80211_is_assoc_resp(fc) || ieee80211_is_reassoc_req(fc) ||
		      ieee80211_is_deauth(fc) || ieee80211_is_disassoc(fc) ||
		      ieee80211_is_action(fc)))
			return;			/* beacons/probes: too chatty */
		/* auth/assoc are unicast by 802.11: a broadcast one received is a
		 * foreign or malformed frame (a neighbour's beacon read one byte late
		 * logged ~10 lines/s on the serial console, 2026-09-23) */
		if (dir[0] == 'R' && is_multicast_ether_addr(hdr->addr1) &&
		    !(ieee80211_is_deauth(fc) || ieee80211_is_disassoc(fc)))
			return;
		if (crc)
			return;			/* skip neighbours' bad-CRC mgmt flood
						 * (check-BSSID is off for probe-req RX,
						 * so the AP now hears every nearby net;
						 * our own client is close = crc=0). The
						 * EAPOL 4-way is DATA below, unfiltered,
						 * so a genuinely-broken M2 still shows. */
		if (ieee80211_is_auth(fc) && len >= 30) {
			/* auth body: alg(0=open,2=shared,3=SAE), seq, status.
			 * alg=3 => phone trying WPA3/SAE against our WPA2-only AP.
			 * status!=0 in our TX => we rejected the phone's auth. */
			pr_info("92f-spy %s AUTH alg=%u seq=%u status=%u %pM->%pM rate=0x%02x crc=%u\n",
				dir, p[24] | (p[25] << 8), p[26] | (p[27] << 8),
				p[28] | (p[29] << 8), hdr->addr2, hdr->addr1, rate, crc);
			return;
		}
		if (ieee80211_is_assoc_resp(fc) && len >= 28)
			reason = p[26] | (p[27] << 8);	/* assoc status code */
		else if ((ieee80211_is_deauth(fc) || ieee80211_is_disassoc(fc)) &&
			 len >= 26)
			reason = p[24] | (p[25] << 8);
		pr_info("92f-spy %s mgmt st=%u %pM->%pM rate=0x%02x crc=%u icv=%u rsn=%u\n",
			dir, (le16_to_cpu(fc) >> 4) & 0xf, hdr->addr2, hdr->addr1,
			rate, crc, icv, reason);
		return;
	}
	if (!ieee80211_is_data(fc))
		return;
	/* Unicast DATA probe: log every unicast data frame BEFORE parsing the
	 * (maybe-encrypted) payload, so an ONU->client unicast test -- ICMP flood or
	 * the unicast DHCP OFFER -- shows whether unicast downstream data reaches
	 * tx_fill_desc at all (the data-queue TX-ring stall check). */
	if (rtl92f_spy_data && !is_multicast_ether_addr(hdr->addr1))
		pr_info_ratelimited("92f-spy %s DATA-UC %pM->%pM prot=%u rate=0x%02x len=%u\n",
				    dir, hdr->addr2, hdr->addr1,
				    !!ieee80211_has_protected(fc), rate, len);
	hlen = ieee80211_hdrlen(fc);
	if (ieee80211_has_protected(fc))
		hlen += 8;			/* CCMP/TKIP IV sits before the LLC/SNAP */
	if (len < hlen + 8 + 17)		/* need up to the replay-ctr byte */
		return;
	p += hlen;				/* LLC/SNAP header (past the IV if encrypted) */
	if (p[0] != 0xaa || p[1] != 0xaa)
		return;				/* not LLC/SNAP encapsulated */
	if (p[6] == 0x88 && p[7] == 0x8e) {	/* EAPOL (the 4-way) */
		pr_info("92f-spy %s EAPOL %s ki=0x%04x rc=%u %pM->%pM rate=0x%02x crc=%u icv=%u pm=%u\n",
			dir, rtl92f_eapol_msg((p[13] << 8) | p[14]),
			(u16)((p[13] << 8) | p[14]), p[24],
			hdr->addr2, hdr->addr1, rate, crc, icv,
			!!ieee80211_has_pm(fc));
		return;
	}
	/* DHCP (IPv4 0x0800 / UDP 17 / port 67|68): shows whether the AP actually
	 * TXes the OFFER downstream, to broadcast or unicast, at what rate -- the
	 * "4-way done but no IP" case. bcast=1 + not delivered => group-key/PS. */
	if (p[6] == 0x08 && p[7] == 0x00 && len >= hlen + 8 + 32 && p[17] == 17) {
		u16 dport = (p[30] << 8) | p[31];

		if (dport == 67 || dport == 68)
			pr_info("92f-spy %s DHCP dport=%u %pM->%pM rate=0x%02x crc=%u bcast=%d pm=%u\n",
				dir, dport, hdr->addr2, hdr->addr1, rate, crc,
				is_multicast_ether_addr(hdr->addr1),
				!!ieee80211_has_pm(fc));
	}
}

bool rtl92fe_rx_query_desc(struct ieee80211_hw *hw,
			   struct rtl_stats *status,
			   struct ieee80211_rx_status *rx_status,
			   u8 *pdesc8, struct sk_buff *skb)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rx_fwinfo *p_drvinfo;
	struct ieee80211_hdr *hdr;
	__le32 *pdesc = (__le32 *)pdesc8;
	u32 phystatus = get_rx_desc_physt(pdesc);
	bool level = false;
	u8 wake_match;

	if (get_rx_status_desc_rpt_sel(pdesc) == 0)
		status->packet_report_type = NORMAL_RX;
	else
		status->packet_report_type = C2H_PACKET;
	status->length = (u16)get_rx_desc_pkt_len(pdesc);
	status->rx_drvinfo_size = (u8)get_rx_desc_drv_info_size(pdesc) *
				  RX_DRV_INFO_SIZE_UNIT;
	status->rx_bufshift = (u8)(get_rx_desc_shift(pdesc) & 0x03);
	status->icv = (u16)get_rx_desc_icv(pdesc);
	status->crc = (u16)get_rx_desc_crc32(pdesc);
	status->hwerror = (status->crc | status->icv);
	status->decrypted = !get_rx_desc_swdec(pdesc);
	status->rate = (u8)get_rx_desc_rxmcs(pdesc);
	status->isampdu = (bool)(get_rx_desc_paggr(pdesc) == 1);
	status->timestamp_low = get_rx_desc_tsfl(pdesc);
	status->is_cck = RTL92FE_RX_HAL_IS_CCK_RATE(status->rate);
	/* Tag HT (MCS) frames so the RX rate maps to an HT MCS index instead of
	 * collapsing to legacy rate_idx 0. Without this, mac80211/iwinfo report the
	 * station RX rate as 0 Mbit/s and the width as 0 MHz (an empty rate_info).
	 * The AP runs HT20, so the 20 MHz width is correct once the rate is set. */
	status->is_ht = IS_HT_RATE(status->rate);

	status->macid = get_rx_desc_macid(pdesc);
	if (get_rx_status_desc_pattern_match(pdesc))
		wake_match = BIT(2);
	else if (get_rx_status_desc_magic_match(pdesc))
		wake_match = BIT(1);
	else if (get_rx_status_desc_unicast_match(pdesc))
		wake_match = BIT(0);
	else
		wake_match = 0;
	if (wake_match)
		rtl_dbg(rtlpriv, COMP_RXDESC, DBG_LOUD,
			"Get Wakeup Packet!! WakeMatch=%d\n", wake_match);
	rx_status->freq = hw->conf.chandef.chan->center_freq;
	rx_status->band = hw->conf.chandef.chan->band;

	hdr = (struct ieee80211_hdr *)(skb->data + status->rx_drvinfo_size +
				       status->rx_bufshift + 24);

	rtl92f_spy("RX", hdr, status->length, status->rate,
		   status->crc, status->icv);

	if (status->crc)
		rx_status->flag |= RX_FLAG_FAILED_FCS_CRC;

	if (status->rx_is40mhzpacket)
		rx_status->bw = RATE_INFO_BW_40;

	if (status->is_ht)
		rx_status->encoding = RX_ENC_HT;

	rx_status->flag |= RX_FLAG_MACTIME_START;

	/* The RTL8192F reports HW-decrypted frames with the 802.11 ...
	 * dev/MEASURED-trx.c.md sec 2. */
	if (status->decrypted) {
		if (_ieee80211_is_robust_mgmt_frame(hdr) &&
		    !ieee80211_has_protected(hdr->frame_control))
			rx_status->flag &= ~RX_FLAG_DECRYPTED;
		else
			rx_status->flag |= RX_FLAG_DECRYPTED;
	}

	rx_status->rate_idx = rtlwifi_rate_mapping(hw, status->is_ht,
						   false, status->rate);

	rx_status->mactime = status->timestamp_low;
	if (phystatus && status->rx_drvinfo_size >= 28) {
		/* SHIFT pads the frame after driver info, not the PHY report. */
		p_drvinfo = (struct rx_fwinfo *)(skb->data + 24);

		level = _rtl92fe_translate_rx_signal_stuff(hw, skb, status,
							   pdesc8, p_drvinfo);
	}
	/* the paged report is dBm already (PWDB - 110); the legacy +10 went with
	 * the legacy decoder */
	if (level)
		rx_status->signal = status->recvsignalpower;
	else
		rx_status->flag |= RX_FLAG_NO_SIGNAL_VAL;
	if (status->packet_report_type == TX_REPORT2) {
		status->macid_valid_entry[0] =
			get_rx_rpt2_desc_macid_valid_1(pdesc);
		status->macid_valid_entry[1] =
			get_rx_rpt2_desc_macid_valid_2(pdesc);
	}
	return true;
}

void rtl92fe_rx_check_dma_ok(struct ieee80211_hw *hw, u8 *header_desc8,
			     u8 queue_index)
{
	u8 first_seg = 0;
	u8 last_seg = 0;
	u16 total_len = 0;
	u16 read_cnt = 0;
	__le32 *header_desc = (__le32 *)header_desc8;

	if (!header_desc)
		return;

	total_len = (u16)get_rx_buffer_desc_total_length(header_desc);
	first_seg = (u8)get_rx_buffer_desc_fs(header_desc);
	last_seg = (u8)get_rx_buffer_desc_ls(header_desc);

	/* The buffer descriptor may not yet be visible to the host the
	 * instant the RX interrupt is taken; spin a bounded number of times
	 * waiting for the segment fields to populate, then give up.
	 */
	while (total_len == 0 && first_seg == 0 && last_seg == 0) {
		read_cnt++;
		total_len = (u16)get_rx_buffer_desc_total_length(header_desc);
		first_seg = (u8)get_rx_buffer_desc_fs(header_desc);
		last_seg = (u8)get_rx_buffer_desc_ls(header_desc);

		if (read_cnt > 20)
			break;
	}
}

u16 rtl92fe_rx_desc_buff_remained_cnt(struct ieee80211_hw *hw, u8 queue_index)
{
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u16 read_point, write_point, remind_cnt;
	u32 tmp_4byte;

	tmp_4byte = rtl_read_dword(rtlpriv, REG_RXQ_TXBD_IDX);
	read_point = (u16)((tmp_4byte >> 16) & 0x7ff);
	write_point = (u16)(tmp_4byte & 0x7ff);

	if (write_point != rtlpci->rx_ring[queue_index].next_rx_rp) {
		rtl_dbg(rtlpriv, COMP_RXDESC, DBG_DMESG,
			"write point is 0x%x, reg value is 0x%x\n",
			write_point, tmp_4byte);
		tmp_4byte = rtl_read_dword(rtlpriv, REG_RXQ_TXBD_IDX);
		read_point = (u16)((tmp_4byte >> 16) & 0x7ff);
		write_point = (u16)(tmp_4byte & 0x7ff);
	}

	/* RX indices delimit completed slots; unlike TX free space, no slot
	 * is reserved. Native RX drains until host and hardware indices match.
	 */
	remind_cnt = (read_point + RTL_PCI_MAX_RX_COUNT - write_point) %
		     RTL_PCI_MAX_RX_COUNT;

	if (remind_cnt == 0)
		return 0;

	rtlpci->rx_ring[queue_index].next_rx_rp = write_point;

	return remind_cnt;
}

static u16 get_desc_addr_fr_q_idx(u16 queue_index)
{
	u16 desc_address;

	switch (queue_index) {
	case BK_QUEUE:
		desc_address = REG_BKQ_TXBD_IDX;
		break;
	case BE_QUEUE:
		desc_address = REG_BEQ_TXBD_IDX;
		break;
	case VI_QUEUE:
		desc_address = REG_VIQ_TXBD_IDX;
		break;
	case VO_QUEUE:
		desc_address = REG_VOQ_TXBD_IDX;
		break;
	case BEACON_QUEUE:
		desc_address = REG_BEQ_TXBD_IDX;
		break;
	case TXCMD_QUEUE:
		desc_address = REG_BEQ_TXBD_IDX;
		break;
	case MGNT_QUEUE:
		desc_address = REG_MGQ_TXBD_IDX;
		break;
	case HIGH_QUEUE:
		desc_address = REG_HI0Q_TXBD_IDX;
		break;
	case HCCA_QUEUE:
		desc_address = REG_BEQ_TXBD_IDX;
		break;
	default:
		desc_address = REG_BEQ_TXBD_IDX;
		break;
	}
	return desc_address;
}

u16 rtl92fe_get_available_desc(struct ieee80211_hw *hw, u8 q_idx)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u16 point_diff = 0;
	u16 current_tx_read_point, current_tx_write_point;
	u32 tmp_4byte;

	tmp_4byte = rtl_read_dword(rtlpriv, get_desc_addr_fr_q_idx(q_idx));
	current_tx_read_point = (u16)((tmp_4byte >> 16) & 0x0fff);
	current_tx_write_point = (u16)((tmp_4byte) & 0x0fff);

	point_diff = calc_fifo_space(current_tx_read_point,
				     current_tx_write_point,
				     TX_DESC_NUM_92F);

	return point_diff;
}

static void rtl92fe_pre_fill_tx_bd_desc(struct ieee80211_hw *hw,
					u8 *tx_bd_desc8, u8 *desc8,
					u8 queue_index,
					struct sk_buff *skb,
					dma_addr_t addr)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	u32 pkt_len = skb->len;
	u16 desc_size = 40; /* tx desc header size */
	u32 psblen = 0;
	u16 tx_page_size;
	u32 total_packet_size;
	u16 current_bd_desc;
	u8 i;
	u16 real_desc_size = 0x28;
	u16 append_early_mode_size = 0;
	u8 segmentnum = 1 << (RTL8192FE_SEG_NUM + 1);
	dma_addr_t desc_dma_addr;
	bool dma64 = rtlpriv->cfg->mod_params->dma64;
	__le32 *desc = (__le32 *)desc8;
	__le32 *tx_bd_desc = (__le32 *)tx_bd_desc8;

	tx_page_size = 2;
	current_bd_desc = rtlpci->tx_ring[queue_index].cur_tx_wp;

	total_packet_size = desc_size + pkt_len;

	if (rtlpriv->rtlhal.earlymode_enable) {
		if (queue_index < BEACON_QUEUE) {
			append_early_mode_size = 8;
			total_packet_size += append_early_mode_size;
		}
	}

	if (tx_page_size > 0) {
		psblen = (pkt_len + real_desc_size + append_early_mode_size) /
			 (tx_page_size * 128);

		if (psblen * (tx_page_size * 128) < total_packet_size)
			psblen += 1;
	}

	/* tx desc DMA address (where the chip fetches the 64-byte header) */
	desc_dma_addr = rtlpci->tx_ring[queue_index].dma +
			(current_bd_desc * TX_DESC_SIZE);

	/* Reset header slot. */
	set_tx_buff_desc_len_0(tx_bd_desc, 0);
	set_tx_buff_desc_psb(tx_bd_desc, 0);
	set_tx_buff_desc_own(tx_bd_desc, 0);

	for (i = 1; i < segmentnum; i++) {
		set_txbuffer_desc_len_with_offset(tx_bd_desc, i, 0);
		set_txbuffer_desc_amsdu_with_offset(tx_bd_desc, i, 0);
		set_txbuffer_desc_add_low_with_offset(tx_bd_desc, i, 0);
		set_txbuffer_desc_add_high_with_offset(tx_bd_desc, i, 0, dma64);
	}

	/* Clear all status in the txdesc header. */
	clear_pci_tx_desc_content(desc, TX_DESC_SIZE);

	if (rtlpriv->rtlhal.earlymode_enable) {
		if (queue_index < BEACON_QUEUE)
			set_tx_buff_desc_len_0(tx_bd_desc, desc_size + 8);
		else
			set_tx_buff_desc_len_0(tx_bd_desc, desc_size);
	} else {
		set_tx_buff_desc_len_0(tx_bd_desc, desc_size);
	}
	set_tx_buff_desc_psb(tx_bd_desc, psblen);
	set_tx_buff_desc_addr_low_0(tx_bd_desc, desc_dma_addr);
	set_tx_buff_desc_addr_high_0(tx_bd_desc, ((u64)desc_dma_addr >> 32),
				     dma64);

	/* seg1 = packet payload */
	set_txbuffer_desc_len_with_offset(tx_bd_desc, 1, pkt_len);
	/* extension/AMSDU mode not used */
	set_txbuffer_desc_amsdu_with_offset(tx_bd_desc, 1, 0);
	set_txbuffer_desc_add_low_with_offset(tx_bd_desc, 1, addr);
	set_txbuffer_desc_add_high_with_offset(tx_bd_desc, 1,
					       ((u64)addr >> 32), dma64);

	set_tx_desc_pkt_size(desc, (u16)(pkt_len));
	set_tx_desc_tx_buffer_size(desc, (u16)(pkt_len));
}

void rtl92fe_tx_fill_desc(struct ieee80211_hw *hw,
			  struct ieee80211_hdr *hdr, u8 *pdesc8,
			  u8 *pbd_desc_tx,
			  struct ieee80211_tx_info *info,
			  struct ieee80211_sta *sta,
			  struct sk_buff *skb,
			  u8 hw_queue, struct rtl_tcb_desc *ptcb_desc)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	struct rtl_hal *rtlhal = rtl_hal(rtlpriv);
	struct rtlwifi_tx_info *tx_info = rtl_tx_skb_cb_info(skb);
	u16 seq_number;
	__le16 fc = hdr->frame_control;
	u8 fw_qsel = _rtl92fe_map_hwqueue_to_fwqueue(skb, hw_queue);
	bool firstseg = ((hdr->seq_ctrl &
			  cpu_to_le16(IEEE80211_SCTL_FRAG)) == 0);
	bool lastseg = ((hdr->frame_control &
			 cpu_to_le16(IEEE80211_FCTL_MOREFRAGS)) == 0);
	dma_addr_t mapping;
	u8 bw_40 = 0;
	__le32 *pdesc = (__le32 *)pdesc8;

	if (mac->opmode == NL80211_IFTYPE_STATION) {
		bw_40 = mac->bw_40;
	} else if (mac->opmode == NL80211_IFTYPE_AP ||
		   mac->opmode == NL80211_IFTYPE_ADHOC) {
		if (sta)
			bw_40 = sta->deflink.ht_cap.cap &
				IEEE80211_HT_CAP_SUP_WIDTH_20_40;
	}
	seq_number = (le16_to_cpu(hdr->seq_ctrl) & IEEE80211_SCTL_SEQ) >> 4;
	rtl_get_tcb_desc(hw, info, sta, skb, ptcb_desc);
	/* spy: the resolved HW rate is now in ptcb_desc -- log our mgmt/EAPOL TX
	 * (M1/M3 out) so we can see the AP's side of the 4-way. */
	rtl92f_spy("TX", hdr, skb->len, ptcb_desc->hw_rate, 0, 0);
	/* reserve 8 bytes for AMPDU early mode */
	if (rtlhal->earlymode_enable) {
		skb_push(skb, EM_HDR_LEN);
		memset(skb->data, 0, EM_HDR_LEN);
	}
	mapping = dma_map_single(&rtlpci->pdev->dev, skb->data, skb->len,
				 DMA_TO_DEVICE);
	if (dma_mapping_error(&rtlpci->pdev->dev, mapping)) {
		rtl_dbg(rtlpriv, COMP_SEND, DBG_TRACE, "DMA mapping error\n");
		return;
	}

	if (pbd_desc_tx)
		rtl92fe_pre_fill_tx_bd_desc(hw, pbd_desc_tx, pdesc8, hw_queue,
					    skb, mapping);

	if (ieee80211_is_nullfunc(fc) || ieee80211_is_ctl(fc)) {
		firstseg = true;
		lastseg = true;
	}
	if (firstseg) {
		if (rtlhal->earlymode_enable) {
			set_tx_desc_pkt_offset(pdesc, 1);
			set_tx_desc_offset(pdesc,
					   USB_HWDESC_HEADER_LEN + EM_HDR_LEN);
			if (ptcb_desc->empkt_num) {
				rtl_dbg(rtlpriv, COMP_SEND, DBG_TRACE,
					"Insert 8 byte.pTcb->EMPktNum:%d\n",
					ptcb_desc->empkt_num);
				_rtl92fe_insert_emcontent(ptcb_desc,
							  (u8 *)(skb->data));
			}
		} else {
			set_tx_desc_offset(pdesc, USB_HWDESC_HEADER_LEN);
		}

		set_tx_desc_tx_rate(pdesc, ptcb_desc->hw_rate);

		if (ieee80211_is_mgmt(fc)) {
			ptcb_desc->use_driver_rate = true;
		} else {
			if (rtlpriv->ra.is_special_data) {
				ptcb_desc->use_driver_rate = true;
				set_tx_desc_tx_rate(pdesc, DESC_RATE11M);
			} else {
				/* Data rate is the firmware RA's: the peer macid is
				 * CONNECTed with its RA mask (hw.c).  The CCK 11M pin
				 * it replaces: dev/MEASURED-trx.c.md sec 3-4. */
				ptcb_desc->use_driver_rate = false;
			}
		}

		if (info->flags & IEEE80211_TX_CTL_AMPDU) {
			set_tx_desc_agg_enable(pdesc, 1);
			set_tx_desc_max_agg_num(pdesc, 0x14);
		}
		set_tx_desc_seq(pdesc, seq_number);
		set_tx_desc_rts_enable(pdesc,
				       ((ptcb_desc->rts_enable &&
					 !ptcb_desc->cts_enable) ? 1 : 0));
		set_tx_desc_hw_rts_enable(pdesc, 0);
		set_tx_desc_cts2self(pdesc,
				     ((ptcb_desc->cts_enable) ? 1 : 0));

		set_tx_desc_rts_rate(pdesc, ptcb_desc->rts_rate);
		set_tx_desc_rts_sc(pdesc, ptcb_desc->rts_sc);
		set_tx_desc_rts_short(pdesc,
				((ptcb_desc->rts_rate <= DESC_RATE54M) ?
				 (ptcb_desc->rts_use_shortpreamble ? 1 : 0) :
				 (ptcb_desc->rts_use_shortgi ? 1 : 0)));

		if (ptcb_desc->tx_enable_sw_calc_duration)
			set_tx_desc_nav_use_hdr(pdesc, 1);

		if (bw_40) {
			if (ptcb_desc->packet_bw == HT_CHANNEL_WIDTH_20_40) {
				set_tx_desc_data_bw(pdesc, 1);
				set_tx_desc_tx_sub_carrier(pdesc, 3);
			} else {
				set_tx_desc_data_bw(pdesc, 0);
				set_tx_desc_tx_sub_carrier(pdesc,
							   mac->cur_40_prime_sc);
			}
		} else {
			set_tx_desc_data_bw(pdesc, 0);
			set_tx_desc_tx_sub_carrier(pdesc, 0);
		}

		set_tx_desc_linip(pdesc, 0);
		if (sta) {
			u8 ampdu_density = sta->deflink.ht_cap.ampdu_density;

			set_tx_desc_ampdu_density(pdesc, ampdu_density);
		}
		if (info->control.hw_key) {
			struct ieee80211_key_conf *key = info->control.hw_key;

			switch (key->cipher) {
			case WLAN_CIPHER_SUITE_WEP40:
			case WLAN_CIPHER_SUITE_WEP104:
			case WLAN_CIPHER_SUITE_TKIP:
				set_tx_desc_sec_type(pdesc, 0x1);
				break;
			case WLAN_CIPHER_SUITE_CCMP:
				set_tx_desc_sec_type(pdesc, 0x3);
				break;
			default:
				set_tx_desc_sec_type(pdesc, 0x0);
				break;
			}
		}

		set_tx_desc_queue_sel(pdesc, fw_qsel);
		set_tx_desc_data_rate_fb_limit(pdesc, 0x1F);
		set_tx_desc_rts_rate_fb_limit(pdesc, 0xF);
		set_tx_desc_disable_fb(pdesc,
				       ptcb_desc->disable_ratefallback ? 1 : 0);
		set_tx_desc_use_rate(pdesc, ptcb_desc->use_driver_rate ? 1 : 0);

		if (ieee80211_is_data_qos(fc)) {
			if (mac->rdg_en) {
				rtl_dbg(rtlpriv, COMP_SEND, DBG_TRACE,
					"Enable RDG function.\n");
				set_tx_desc_rdg_enable(pdesc, 1);
				set_tx_desc_htc(pdesc, 1);
			}
		}
		/* tx report */
		rtl_set_tx_report(ptcb_desc, pdesc8, hw, tx_info);
	}

	set_tx_desc_first_seg(pdesc, (firstseg ? 1 : 0));
	set_tx_desc_last_seg(pdesc, (lastseg ? 1 : 0));
	set_tx_desc_tx_buffer_address(pdesc, mapping);
	if (rtlpriv->dm.useramask) {
		set_tx_desc_rate_id(pdesc, ptcb_desc->ratr_index);
		set_tx_desc_macid(pdesc, ptcb_desc->mac_id);
	} else {
		set_tx_desc_rate_id(pdesc, 0xC + ptcb_desc->ratr_index);
		set_tx_desc_macid(pdesc, ptcb_desc->ratr_index);
	}

	set_tx_desc_more_frag(pdesc, (lastseg ? 0 : 1));
	if (is_multicast_ether_addr(ieee80211_get_DA(hdr)) ||
	    is_broadcast_ether_addr(ieee80211_get_DA(hdr)))
		set_tx_desc_bmc(pdesc, 1);

	/* TX-FINAL probe (spy_data): read back the FINAL descriptor ...
	 * dev/MEASURED-trx.c.md sec 5. */
	if (rtl92f_spy_data && ieee80211_is_data(hdr->frame_control) &&
	    !is_multicast_ether_addr(hdr->addr1))
		pr_info("92f-spy TX-FINAL %pM sta=%s hw_key=%s tx_rate=0x%02x agg=%u sec=%u\n",
			hdr->addr1,
			sta ? "SET" : "NULL",
			info->control.hw_key ? "SET" : "NULL",
			(u32)le32_get_bits(*(pdesc + 4), GENMASK(6, 0)),
			(u32)le32_get_bits(*(pdesc + 2), BIT(12)),
			(u32)le32_get_bits(*(pdesc + 1), GENMASK(23, 22)));

	rtl_dbg(rtlpriv, COMP_SEND, DBG_TRACE, "\n");
}

void rtl92fe_tx_fill_cmddesc(struct ieee80211_hw *hw, u8 *pdesc8,
			     struct sk_buff *skb)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	u8 fw_queue = QSLT_BEACON;
	dma_addr_t mapping = dma_map_single(&rtlpci->pdev->dev, skb->data,
					    skb->len, DMA_TO_DEVICE);
	u8 txdesc_len = 40;
	__le32 *pdesc = (__le32 *)pdesc8;

	if (dma_mapping_error(&rtlpci->pdev->dev, mapping)) {
		rtl_dbg(rtlpriv, COMP_SEND, DBG_TRACE, "DMA mapping error\n");
		return;
	}
	rtl92fe_pre_fill_tx_bd_desc(hw,
				    (u8 *)rtlpci->tx_ring[BEACON_QUEUE].buffer_desc,
				    pdesc8, BEACON_QUEUE, skb, mapping);

	set_tx_desc_offset(pdesc, txdesc_len);

	set_tx_desc_tx_rate(pdesc, DESC_RATE1M);

	set_tx_desc_seq(pdesc, 0);

	set_tx_desc_linip(pdesc, 0);

	set_tx_desc_queue_sel(pdesc, fw_queue);

	set_tx_desc_first_seg(pdesc, 1);
	set_tx_desc_last_seg(pdesc, 1);

	set_tx_desc_tx_buffer_size(pdesc, (u16)(skb->len));

	set_tx_desc_tx_buffer_address(pdesc, mapping);

	set_tx_desc_rate_id(pdesc, 7);
	set_tx_desc_macid(pdesc, 0);

	set_tx_desc_own(pdesc, 1);

	set_tx_desc_pkt_size(pdesc, (u16)(skb->len));

	set_tx_desc_first_seg(pdesc, 1);
	set_tx_desc_last_seg(pdesc, 1);

	set_tx_desc_offset(pdesc, 40);

	set_tx_desc_use_rate(pdesc, 1);

	RT_PRINT_DATA(rtlpriv, COMP_CMD, DBG_LOUD,
		      "H2C Tx Cmd Content\n", pdesc, txdesc_len);
}

void rtl92fe_set_desc(struct ieee80211_hw *hw, u8 *pdesc8, bool istx,
		      u8 desc_name, u8 *val)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u8 q_idx = *val;
	bool dma64 = rtlpriv->cfg->mod_params->dma64;
	__le32 *pdesc = (__le32 *)pdesc8;

	/* The chip may never be told to write more than the mapping below it
	 * is long: pci.c maps RTL_PCI_RX_BUFFER_SIZE, the RX_PREPARE case
	 * below programs MAX_RECEIVE_BUFFER_SIZE + RX_DESC_SIZE.
	 */
	BUILD_BUG_ON(MAX_RECEIVE_BUFFER_SIZE + RX_DESC_SIZE >
		     RTL_PCI_RX_BUFFER_SIZE);

	if (istx) {
		switch (desc_name) {
		case HW_DESC_TX_NEXTDESC_ADDR:
			set_tx_desc_next_desc_address(pdesc, *(u32 *)val);
			break;
		case HW_DESC_TXBUFF_ADDR: {
			/* The beacon's payload address, in BOTH places the fill writes it:
			 * the TXBD's segment 1 and the tx desc (pci.c's fixed beacon copy). */
			struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
			u32 addr = *(u32 *)val;

			set_txbuffer_desc_add_low_with_offset(pdesc, 1, addr);
			set_txbuffer_desc_add_high_with_offset(pdesc, 1, 0,
							       rtlpriv->cfg->mod_params->dma64);
			set_tx_desc_tx_buffer_address(
				(__le32 *)rtlpci->tx_ring[BEACON_QUEUE].desc, addr);
			break;
		}
		case HW_DESC_OWN: {
			struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
			struct rtl8192_tx_ring *ring = &rtlpci->tx_ring[q_idx];
			u16 max_tx_desc = ring->entries;

			if (q_idx == BEACON_QUEUE) {
				ring->cur_tx_wp = 0;
				ring->cur_tx_rp = 0;
				set_tx_buff_desc_own(pdesc, 1);
				return;
			}

			/* caller has ensured a tx desc is available */
			ring->cur_tx_wp =
				((ring->cur_tx_wp + 1) % max_tx_desc);

			rtl_write_word(rtlpriv,
				       get_desc_addr_fr_q_idx(q_idx),
				       ring->cur_tx_wp);
		}
		break;
		}
	} else {
		switch (desc_name) {
		case HW_DESC_RX_PREPARE:
			set_rx_buffer_desc_ls(pdesc, 0);
			set_rx_buffer_desc_fs(pdesc, 0);
			set_rx_buffer_desc_total_length(pdesc, 0);

			set_rx_buffer_desc_data_length(pdesc,
						       MAX_RECEIVE_BUFFER_SIZE +
						       RX_DESC_SIZE);

			set_rx_buffer_physical_low(pdesc,
						   (*(dma_addr_t *)val) &
						   DMA_BIT_MASK(32));
			set_rx_buffer_physical_high(pdesc,
						    ((u64)(*(dma_addr_t *)val)
						     >> 32),
						    dma64);
			break;
		case HW_DESC_RXERO:
			set_rx_desc_eor(pdesc, 1);
			break;
		default:
			WARN_ONCE(true,
				  "rtl8192fe: ERR rxdesc :%d not processed\n",
				  desc_name);
			break;
		}
	}
}

u64 rtl92fe_get_desc(struct ieee80211_hw *hw,
		     u8 *pdesc8, bool istx, u8 desc_name)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u64 ret = 0;
	bool dma64 = rtlpriv->cfg->mod_params->dma64;
	__le32 *pdesc = (__le32 *)pdesc8;

	if (istx) {
		switch (desc_name) {
		case HW_DESC_OWN:
			ret = get_tx_desc_own(pdesc);
			break;
		case HW_DESC_TXBUFF_ADDR:
			ret = get_txbuffer_desc_addr_low(pdesc, 1);
			ret |= (u64)get_txbuffer_desc_addr_high(pdesc, 1,
								dma64) << 32;
			break;
		default:
			WARN_ONCE(true,
				  "rtl8192fe: ERR txdesc :%d not processed\n",
				  desc_name);
			break;
		}
	} else {
		switch (desc_name) {
		case HW_DESC_OWN:
			ret = get_rx_desc_own(pdesc);
			break;
		case HW_DESC_RXPKT_LEN:
			ret = get_rx_desc_pkt_len(pdesc);
			break;
		case HW_DESC_RXBUFF_ADDR:
			ret = get_rx_desc_buff_addr(pdesc);
			break;
		default:
			WARN_ONCE(true,
				  "rtl8192fe: ERR rxdesc :%d not processed\n",
				  desc_name);
			break;
		}
	}
	return ret;
}

bool rtl92fe_is_tx_desc_closed(struct ieee80211_hw *hw, u8 hw_queue, u16 index)
{
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u16 read_point, write_point;
	bool ret = false;
	struct rtl8192_tx_ring *ring = &rtlpci->tx_ring[hw_queue];

	{
		u16 cur_tx_rp;
		u32 tmpu32;

		tmpu32 = rtl_read_dword(rtlpriv,
					get_desc_addr_fr_q_idx(hw_queue));
		cur_tx_rp = (u16)((tmpu32 >> 16) & 0x0fff);

		/* don't need to update ring->cur_tx_wp */
		ring->cur_tx_rp = cur_tx_rp;
	}

	read_point = ring->cur_tx_rp;
	write_point = ring->cur_tx_wp;

	if (write_point > read_point) {
		if (index < write_point && index >= read_point)
			ret = false;
		else
			ret = true;
	} else if (write_point < read_point) {
		if (index > write_point && index < read_point)
			ret = true;
		else
			ret = false;
	} else {
		if (index != read_point)
			ret = true;
	}

	if (hw_queue == BEACON_QUEUE)
		ret = true;

	if (rtlpriv->rtlhal.driver_is_goingto_unload ||
	    rtlpriv->psc.rfoff_reason > RF_CHANGE_BY_PS)
		ret = true;

	return ret;
}

/* The PCIe engine's own word: arm the self-check and read it back. */
static bool rtl92fe_pcie_tx_stuck(struct rtl_priv *rtlpriv)
{
	rtl_write_byte(rtlpriv, REG_PCIE_STUCK_CHK, PCIE_STUCK_CHK_ARM);
	return rtl_read_byte(rtlpriv, REG_PCIE_STUCK_CHK) == PCIE_STUCK_CHK_TX;
}

/* Vendor check_hangup, re-expressed: on every watchdog tick a queue with
 * frames pending whose HW read index has not moved for TX_HANG_PENDING_S is
 * hung -- unless ANY queue moved or drained this tick, which restarts every
 * clock. Independent of the TXDMA error counter, which stays a diagnostic.
 * The silicon witness declares a hang on its own.
 */
bool rtl92fe_tx_hang_detect(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci *rtlpci = rtl_pcidev(rtl_pcipriv(hw));
	unsigned long now = jiffies ? jiffies : 1;
	bool stuck = false, alive = false;
	u8 q;

	for (q = 0; q < RTL_PCI_MAX_TX_QUEUE_COUNT; q++) {
		struct rtl8192_tx_ring *ring = &rtlpci->tx_ring[q];
		u16 rp;

		if (q == BEACON_QUEUE)
			continue;
		if (!skb_queue_len(&ring->queue)) {
			if (rtlpci->hang_since[q])
				alive = true;			/* it drained */
			rtlpci->hang_since[q] = 0;
			continue;
		}
		rp = (rtl_read_dword(rtlpriv, get_desc_addr_fr_q_idx(q)) >> 16) &
		     0x0fff;
		if (rtlpci->hang_since[q] && rp == rtlpci->hang_rp[q]) {
			if (time_after_eq(now, rtlpci->hang_since[q] +
					       TX_HANG_PENDING_S * HZ))
				stuck = true;
			continue;
		}
		if (rtlpci->hang_since[q])
			alive = true;				/* the index moved */
		rtlpci->hang_since[q] = now;
		rtlpci->hang_rp[q] = rp;
	}
	if (alive)
		memset(rtlpci->hang_since, 0, sizeof(rtlpci->hang_since));
	rtlpci->pcie_tx_stuck = rtl92fe_pcie_tx_stuck(rtlpriv);
	return rtlpci->pcie_tx_stuck || (stuck && !alive);
}

void rtl92fe_tx_polling(struct ieee80211_hw *hw, u8 hw_queue)
{
	/* The RTL8192F PCIe engine advances its own write pointer ...
	 * dev/MEASURED-trx.c.md sec 6. */
}
