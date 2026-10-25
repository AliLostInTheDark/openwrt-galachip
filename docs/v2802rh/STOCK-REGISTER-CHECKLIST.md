# V2802RH: stock vs OpenWrt switch registers

Gate for the first NOR flash: every register that differs between stock and
OpenWrt is accounted for below.

- **Stock:** `~/Desktop/register.log`, the RTOS diag `register get all`, 3234
  registers.
- **Ours:** `~/Desktop/register-owrt.log`, devmem over swcore 0x1b000000,
  2039 registers. Both dumps were taken on 2026-10-25, ours at 12:49.
- **Diff:** 236 registers differ across the 2039 addresses both dumps cover.

Register names come from Realtek's own RTL9602C register list,
`rtk_rtl9602c_reg_list` in the XZ000-G6 GPL vmlinux (1530 registers). Each
entry points at a `<NAME>_RTL9602C_FIELDS` table, and the vmlinux symbol table
names it. Field names are not in that kernel's debug info, so bit-level
differences are given by position.

**Our dump predates** the commits that brought up LAN2, the LEDs and the
2.5G SerDes (e63c66a472, 990f869f43). A re-dump of the differing addresses
on the current build (`~/Desktop/register-dump-owrt.log`, 2026-10-25)
confirmed section 2; every other configuration row reads as before.

Status key:
- **ok:** runtime state or an intended difference; no action.
- **re-dump:** fixed since our dump; confirm.
- **flash:** decide or fix before the first NOR flash.
- **PON:** belongs to the GPON or BOSA stage; the fibre must be connected.

## 1. Runtime state (no action)

These are counters, latches, scratch registers of the last indirect access,
and packet-header capture buffers. They differ between any two reads.

| Address | Register | Note |
|---|---|---|
| 0x00000–0x00008 | GPHY_IND_WD / CMD / RD | last internal-PHY access |
| 0x0001c | EFUSE_IND_CMD | last eFuse access |
| 0x000a0 | CHIP_DEBUG_OUT | debug bus |
| 0x000b4, 0x000bc, 0x000c4 | I2C_IND_WD / ADR / CMD (bus 1) | last I2C transaction |
| 0x000f4, 0x000f8 | CFG_FE_POLL_WD_1 / RD_0 | PHY poll scratch |
| 0x0013c, 0x00140 | THERMAL_STS_0/1 | die temperature |
| 0x00164 | CRASH_INTR | latched status |
| 0x00198, 0x0019c | P_ABLTY port 0/1 | read-only link status; port 0 was 100M then, before the SerDes fix |
| 0x12000–0x12024 | TBL_ACCESS_CTRL / STS / WR_DATA / RD_DATA | last table access |
| 0x17014, 0x1701c | L2_LRN_CNT, L2_SYS_LRN_CNT | learned-MAC counts |
| 0x17024 | L2_TBL_FLUSH_CTRL | flush status |
| 0x1a004, 0x1a008 | RLDP_CHK/LP_STS_CTRL | loop-detect status |
| 0x1d010, 0x1d014, 0x1d020 | INTR_IMS, INTR_STAT, L2_LRN_OVER_STS | latched interrupts |
| 0x20008 | P_CUR_RATE | measured port rate |
| 0x23100–0x23118, 0x2d024–0x2d098 | FC_*_USED_PAGE_CNT, FC_TOTAL_PAGE_CNT | buffer occupancy |
| 0x28040–0x280ac, 0x28140–0x28178 | HSB_DATA, HSB_PARSER, HSA_DATA | last-packet header capture |
| 0x2a070–0x2a0a8 | HSD_DATA | last-packet header capture |
| 0x2a104 | RSVD_ALE_HSA | reserved |
| 0x360f4 | EPON_LOCAL_TIME | free-running clock |

## 2. Fixed after our first dump (confirmed by the re-dump)

| Address | Register | Stock | First dump | Re-dump |
|---|---|---|---|---|
| 0x00180 | FORCE_P_ABLTY port 0 | 0x76 | 0x16 | 0x76, SerDes forced 2.5G with pause (990f869f43) |
| 0x00198, 0x0019c | P_ABLTY port 0/1 | 0x76, 0x60 | 0x15, 0xe0 | 0x76, 0x60: same as stock |
| 0x001b4 | ABLTY_FORCE_MODE port 0 | 0xfff | 0xfec | 0xfff; the GPON modeset no longer clears it |
| 0x1e008 | DATA_LED_CFG LED1 | 0x20f78 | 0 | 0x20f78, LAN1 link/activity |
| 0x1e05c | LED_EN | 0x10084 | 0 | 0x4: bit 2 (LED1); bits 7 and 16 are the PON rows below |
| 0x23014 | IO_LED_EN | 0x8042 | 0 | 0x2: bit 1 (LED1); bits 6 and 15 are the PON rows below |

## 3. Decide or fix before flashing

These change how the board behaves as an L2 bridge today.

**Resolved 2026-10-25.** `realtek,stock-switch-config` (rtl9602c_eth.c,
`rtl9602c_sw_stock_config()`) applies:
- stock's storm control, including meters 8–15 read from stock at 0x25004
  (rate 0x100, packet mode on meter 10);
- a 1600-byte frame limit;
- LUT_CFG bits 28/26/23;
- IGR_BWCTRL bit 1;
- FIB_REG0 power-down.

A RAM boot read all of these back as stock, with LAN1, LAN2 and the LEDs
working and a 5 MB transfer through LuCI.

Kept ours, with reasons:
- **Learning:** stock sets the per-port learn limit to 0, traps over-limit
  frames to the CPU and floods unknown unicast only to the CPU, so it bridges
  the LAN in software. Ours learns in hardware.
- **Aging:** ours ages out learned entries.
- **IGMP and unknown-multicast:** ours floods them; there is no snooping
  daemon.
- **Source-port permit:** ours clears it on purpose.
- **Interrupt mask:** ours unmasks only what the driver handles.
- **HSG_SDS_EXT_REG0–3:** they read as status or counters (ours 0x4b, then
  0x4a across boots), and LAN2 runs at 2.5G.

The blink period moves to the PON section with the PON/LOS LEDs.

| Address | Register | Stock | Ours | Reading | Proposal |
|---|---|---|---|---|---|
| 0x17000 | LUT_CFG | 0x14c00bb8 | 0x00400bb8 | Bit 21 (BCAM_DIS) is already cleared as stock; bits 28, 26 and 23 are set only on stock. Fields are unnamed; luna_gpon.c sets bits 22 and 21 by name. | Set 28/26/23 as stock and check LAN1↔LAN2 forwarding |
| 0x17004 | LUT_AGEOUT_CTRL | 0 | 0xf | Ours enables per-port aging (luna_gpon.c l2_init). | Keep ours: entries age out when a host moves |
| 0x17008, 0x1700c | LUT_LRN_LIMITNO | 0 | 0x3fffff | Per-port learn limit. | Keep ours, then confirm stock's meaning: together with L2_LRN_OVER_STS=1 it may not learn on those ports at all |
| 0x17018 | LUT_SYS_LRN_LIMITNO | 0x440 (1088) | 0x7ff (2047) | Learn limit for the whole system. | Keep ours |
| 0x1c00c | LUT_LEARN_OVER_CTRL | 0xaa | 0 | Action when over the limit (2 bits per port). | Follows the row above |
| 0x1c088 | L2_SRC_PORT_PERMIT | 0x8 | 0 | Bit 3 = CPU port. | Match stock (CPU port only) |
| 0x1703c–0x1706c | STORM_CTRL_UM / UC / BC / DHCP (CTRL, METER_IDX) | on, with meters | off | Stock rate-limits unknown multicast, unknown unicast, broadcast and DHCP floods. | Adopt: protects the 32 MB CPU from LAN floods. The meter rates it points at (0x25000 block) are not in either dump; read them from stock first |
| 0x11008, 0x1100c, 0x11014 | ACCEPT_MAX_LEN_CTRL ports 0, 1, 3 | 0 | 3 | Selects a max-length profile. | See next row |
| 0x23034, 0x11018 | MAX_LENGTH_CFG0 / CFG1 | 0x640 (1600) / 0 | 0x3ff0 / 0x3ff0 | Ours accepts 16k jumbo frames on LAN and CPU; stock caps at 1600. | Match stock: GEM frames cannot carry jumbo frames, and the CPU path has 2k buffers |
| 0x1105c–0x11068 | IGMP_P_CTRL ports 0–3 | 0x6aa | 0x400 | Stock traps IGMP/MLD to the CPU for snooping; ours forwards it. | Keep ours: no snooping daemon. Revisit in PON for IPTV |
| 0x1c010–0x1c01c | UNKN_L2_MC / IP4_MC / IP6_MC, UNKN_MC_CFG | 0x80 / 0x95 / 0x95, 0x20 | 0 / 0 / 0, 0x28 | Unknown-multicast actions. | Keep ours (flood) for a plain bridge; revisit with IGMP |
| 0x1c020–0x1c028 | LUT_BC / UNKN_MC / UNKN_UC_FLOOD | 0xf / 0x8 / 0x8 | 0xb / 0xb / 0xb | Ours floods to LAN1, LAN2 and CPU, never the PON. luna_gpon.c writes stock's 0x8 at GPON init. | Keep ours until the PON bridge exists |
| 0x1c148 | LUT_UNMATCHED_VLAN_CTRL | 0xf | 0 | Action for frames whose VLAN misses. | Investigate on the re-dump |
| 0x1c094 | MIR_CTRL | 0x33 | 0 | Port mirroring config. | Investigate: likely stock's disabled encoding |
| 0x1c098, 0x1c09c | STAT_PRIVATE_REASON | 0x181, 0xf7 | 0 | MIB counter selection. | ok |
| 0x1d00c | INTR_IMR | 0x2481 | 0x400 | Ours unmasks only what the driver handles. | ok |
| 0x1e050 | LED_BLINK_RATE_CFG | 0x3249 | 0x4249 (was 0x249) | Bits 14:12 force-blink period: stock 3, ours 4 since the LED commit. | Match stock with the PON/LOS LEDs |
| 0x21a00–0x21a0c, 0x21a68 | HSG_SDS_EXT_REG0–3, 26 | 0xffff / 0xff00 / 0xffff / 0xff00 / 3 | 0x4a / 0 / 0x4a / 0 / 1 | 2.5G SerDes (LAN2) extension registers: still differ though LAN2 carries traffic at 2.5G. | Investigate: read stock's bits against ours before deciding |
| 0x20010 | IGR_BWCTRL_P_CTRL | 0x1ffffe | 0x1ffffc | Bit 1 differs (port 0 ingress rate control). | Match stock |
| 0x22c00 | FIB_REG0 | 0x1940 | 0x1140 | BMCR layout: stock sets bit 11 (power-down) on the unused fibre PHY. | Match stock: power it down |
| 0x23018 | IO_MODE_EN | 0x01024040 | 0x010a4040 | Bit 19 = OEM_EN (optics pads), which luna_gpon.c sets. | PON: set only while the optics are in use |

## 4. GPON / BOSA stage

Stock's GPON bridge and QoS configuration. Most of it is the hardware LAN↔PON
path that the L2 offload needs. Whether stock was ranged on an OLT when
dumped is not recorded; the PON PVID (1015) suggests an OMCI-provisioned
VLAN.

| Address | Register | Stock | Ours | Note |
|---|---|---|---|---|
| 0x1300c–0x1301c | VLAN_PB_VID, VLAN_EXT_VID | LAN 4005, PON 1015, ext 4005 / 1 | LAN 1, PON 4025 | Equivalent for untagged LAN; the PON PVID comes from OMCI |
| 0x13020–0x1306c | VLAN_PPB_VLAN_VAL, VLAN_PORT_PPB_VLAN | set | 0 | Protocol-based VLAN (an ethertype selects the VID) |
| 0x14000–0x1400c | SVLAN_EP_DMAC_CTRL, SVLAN_P_SVID, SVLAN_CTRL | set | 0 | S-tag handling for OMCI VLAN operations |
| 0x15008–0x151c4 | ACL_TEMPLATE_CTRL, ACL_EN, ACL_PERMIT, ACL_ACTION | 4 ACLs on | off | Stock traps or steers traffic with ACLs; the rule table is not in the dump |
| 0x16030, 0x16034 | CF_PATTERN0_TEMPLATE_CFG | 0x16, 0xe | 0 | Classifier templates: the US LAN→GEM path for L2 offload |
| 0x1c0b0–0x1c0f8 | QOS_INTPRI_TO_QID, QOS_PORT_QMAP_CTRL, PRI_SEL_TBL_CTRL | set | 0 | Priority→queue maps for T-CONT queues |
| 0x2d800, 0x2d80c | WFQ_CTRL, EGR_BWCTRL_P_CTRL port 2 | 0x23fff, 0x3fffe | 0x3fff, 0xffffe | PON egress shaping |
| 0x2311c, 0x23120 | FC_PON_GLB_HI_TH / LO_TH | 0x02300224 / 0x02160207 | 0x020001f4 / 0x01e601d7 | PON flow-control thresholds |
| 0x1106c, 0x11130, 0x111b4, 0x2a0c0–0x2a0c4 | L34_GLB_CFG, DSLITE_CTRL, L3MCR_TBL, L34_IPMC_TRAN_TBL | set | mostly 0 | L3/L4 engine; not used by an L2 bridge |
| 0x17090, 0x1c100 | OAM_CTRL_1, OAM_CTRL_0 | 1, 7 | 0, 0 | 802.3ah OAM (EPON); luna_gpon.c clears it on purpose |
| 0x22058–0x220e0 | WSDS_DIG_0A / 0B / 14 / 2C | | | PON SerDes digital enables that luna_gpon.c sets |
| 0x23008 | I2C_CONFIG bus 1 | 0x0234413a | 0x02340270 | Optics I2C (GN25L95): BOSA stage |
| 0x1e01c, 0x1e040; LED_EN bits 7/16; IO_LED_EN bits 6/15 | DATA_LED_CFG LED6, LED15 | 0x1b1000 | 0 | Stock drives PON/LOS from LED engine rows 6 and 15; ours from GPIO 35/26 |

## Re-dump on the current build

Paste this on the OpenWrt console of a RAM-booted current image, and send the
output. It reads only the differing addresses.

```sh
for r in 0:8 28:28 160:160 180:196 244:248 316:320 356:356 384:384 408:412 \
 436:436 69640:69656 69724:69740 69936:69936 70068:70068 73728:73764 \
 77836:77932 81920:81932 86024:86184 86460:86468 90160:90164 94208:94244 \
 94268:94316 94352:94352 106500:106504 114700:114728 114824:114844 \
 114864:114880 114932:114944 115016:115016 118796:118816 122888:122888 \
 122908:122908 122944:122972 131080:131088 137728:137740 137832:137832 \
 139352:139356 139392:139392 139488:139488 142336:142336 143368:143384 \
 143412:143412 143616:143648 163904:163984 164008:164012 164160:164216 \
 172144:172200 172224:172228 172292:172292 184356:184376 184404:184404 \
 184472:184472 186368:186380 221428:221428; do
 a=${r%:*}; e=${r#*:}
 while [ $a -le $e ]; do echo "$a $(devmem $((0x1b000000+a)))"; a=$((a+4)); done
done
```
