# V2802RH: what can still be ported from stock

Audited 2026-10-31 against the stock register dump
(`~/Desktop/register-stock-online.log`), the live board and the named functions
in the XZ005-G6 reference kernel. Register fields come from
`rtk_rtl9602c_reg_list` in that kernel. Nothing here has been tested with
traffic: the Airtel OLT uses a VEIP and sends none of these settings.

## Done

UNI settings from the OLT (ME 11, 45, 47), through `rtl9602c_uni_l2_set()`:

| Setting | Register | Evidence |
|---|---|---|
| MAC ageing | `0x17000` [20:0], 0.1 s units | `l2_aging_set`; stock's 3000 is its 300 s |
| Learning limit | `0x17008` onward, 11 bits per port | `l2_portLimitLearningCnt_set` |
| Over-limit action | `0x1c00c`, 2 bits per port: 0 forward, 1 drop, 2 trap | `l2_portLimitLearningCntAction_set` |
| Max frame | `0x11018`, shared by LAN and CPU ports | `switch_maxPktLenByPort_set` |

Ageing and the frame limit are chip-wide, so the shortest ageing and largest
frame win. The frame limit stays between 1600 and 2031: an OLT's usual 1518
would drop tagged frames, and the PON profile is 2031.

PON flow-control watermarks `0x2311c`/`0x23120` use stock's values.

## Already matches stock

- PON scheduling type, `0xf02194` bit 19: stock and ours both read `0x00066000`.
  `WFQ_CTRL` (`0x2d800`) is the LAN-side `qos_schedulingType`, not the PON one.
- MAC ageing default: `0x17000` reads `0x14c00bb8` on both.
- Dying gasp: the template is armed in hardware slot 5. Stock's ISR only
  notifies. Whether it fires on a power cut needs an OLT-side check.

## Kept different on purpose

- `0x2d80c` port 2 egress rate: stock's field is `0x1ffff`, about 1 Gbit/s. Ours
  is unlimited, which suits the 1.24 Gbit/s upstream.

## Added beyond stock

- LAN jacks run on phylink. A PHY driver in `rtl9602c_eth.c` keeps stock's RTL8221B
  SerDes sequence. LAN2 pause follows negotiation, where stock forces it on. Link
  messages are phylink's. EEE stays on the driver's own code.
- RX uses a page pool, one 4k page per buffer.
- RLDP loop detection, off in stock. Registers: enable `0x1a000`, TX mask `0x1a01c`,
  enter and leave status `0x1a020`/`0x1a024` (write 1 to clear), looped `0x1a028`,
  peer port `0x1a02c`. `lan_loop_guard`: 0 off, 1 log, 2 cut (default). See LAN-015 to 017.
- The kernel has no built-in identity. Password, serial, vendor and versions come from
  userspace. Airtel's Nokia OLT accepted an empty password.

## Still open

| Item | What is known | What is missing |
|---|---|---|
| PCP to different GEMs | One upstream rule per p-bit on field (8,3). Measured 2026-11-01: frames sent with p-bit 5 passed a rule requiring 5 and stopped at one requiring 0. This can't separate the tag's p-bit from an internal priority that follows it | A split-GEM OLT such as JioFiber's to test |
| VLAN translation and QinQ in hardware | Classifier rules in both directions, see L2-OFFLOAD.md. Translation measured at 302/296 Mbit/s with the CPU idle; its action values hold. QinQ's are inferred from `cfg2raw` tables | QinQ: an OLT that carries a stacked service; off unless `hw_qinq` is set |
| Multicast GEM, IGMP/MLD snooping | `l2_ipMcastAddr_*`, `l2_ipmcMode_*`. The Airtel OLT creates classes 309 and 310 with every value zero and no multicast GEM | Not started; needs an IPTV service to test |
| eFuse memory patches | Read and logged at bring-up | Written only if a chip turns out to carry any: the addresses are unchecked fuse data |

## Dying gasp and pause time

- Dying gasp: interrupt type 13 is bit 13 of `0x1d00c`/`0x1d010`, on the GPON interrupt's line (stock `dying_gasp_isr`). The driver logs it, masks it, counts it in `/proc/gpon`, and restarts after 100 ms only with `dying_gasp_reset`, where stock always resets the chip. Seen firing on a forced power-off (`[PON-011]`, 2026-11-01). Whether the OLT received the gasp needs an OLT-side check.
- Pause time: ME 11's value sends one 802.3x PAUSE frame on a change. No hardware register is involved.

## Downstream GEM rates

The chip has no per-GEM meter and no per-VLAN policing (`rtk_vlan_policing*` has no RTL9602C implementation), so a limit is a share meter (7 down to 4; stock's storm controls hold 8-15) and an ACL rule on the PON port matching the GEM's service VLAN. At most 4. Template 0 is stock's; ACL matching stays off until a rate exists. Taken from the kernel's `_raw_rtl9602c_acl_rule_set`, `acl_igrRuleAction_set` and `acl_template_set`, seen limiting a real download to 9.66 Mbit/s of a requested 10, with upload untouched, and 302 Mbit/s again once removed (2026-11-01). Raw template code 9 is the C-tag, field 6 of template 0, and 8 is the S-tag. The bucket must stay at stock's `0x3fff`: at `0xffff` the rule hit but the meter never reported exceeded and nothing was limited. The policing action is enum `ACL_IGR_LOG_ACT` (3), so control bit 3, log mode (35,2) = 0 and meter (30,5).

`/sys/class/net/pon0/device/l2_sw_debug` reads and writes these registers and tables live, and `s <ms>` samples which rule wins each action under traffic; the commands are listed above `l2_sw_debug_store`.

## Decoded for the remaining work

- Share meters `0x25004`, 16 entries: rate [50:32] in 8 kbit/s units, bucket [16:1], IFG [0].
- CF action tables, (lsb, width). Upstream: CS_ACT 0,3; SVID 3,12; SPRI 15,3; CSPRI_ACT 18,3; CSVID_ACT 21,2; SID_ACT 23,1; SID 24,7; CACT 33,2; CVID 35,12; CPRI 47,3; CVID_ACT 50,3; CPRI_ACT 53,3. These names are inferred from field order and the known CPRI and SID fields.
- CF per-rule enables `0x16038`, bits 0-5.
- ACL actions `0x15048`, 96 entries, bits 0-7; ACL action table fields (66,1) (62,4) (61,1) (59,2) (53,6) (52,1) (49,3) (43,6) (41,2) (37,4) (35,2) (30,5) (27,3) (15,12) (12,3) (0,12). Control bits are the `rtk_acl_igr_act_type_e` values: CVLAN 0, SVLAN 1, PRI 2, LOG 3, FORWARD 4, INTR 5, ROUTE 6, and bit 7 inverts the match. `0x15000` holds, per action type, the rule that won it for the last frame looked up.

## LAN PHYs

Compared with `switch_init` and `port_init` in the XZ005 kernel. The patch table is byte-identical in the board's RTOS image.

| Stock | Driver |
|---|---|
| `phyPatchArray_20160413`, 55 writes | Applied at bring-up, then a power cycle as `phyPower_set` does |
| `_switch_default_patch`, eFuse PHY writes | Applied at bring-up |
| Autonegotiation ability and enable | `ethtool -s`, and ME 11 auto-detection as an advertisement |
| Pause | `ethtool -A`; LAN2's forced MAC bits follow |
| EEE advertisement, page 0xa5d | `ethtool --set-eee`; stock's power-on default is kept |
| RTCT cable test, green, `switch_eeePatch` | Not ported: RTCT and green are empty stubs in stock, and `switch_eeePatch` has no direct caller |
