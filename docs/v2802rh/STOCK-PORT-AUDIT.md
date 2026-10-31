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

## Still open

| Item | What is known | What is missing |
|---|---|---|
| PCP to different GEMs | Rule fields (lsb, width): UNI 0,3; C-tag 3,1; VID 11,12; (5,3) and (8,3) unnamed | Which field is the priority; a planner that allows two flows per VID; an OLT or fixture to test |
| VLAN translation and QinQ in hardware | `svlan_*` functions, `0x14000`–`0x1400c` | Tracing them; runs correctly on the CPU today |
| Downstream GEM rates | Share meters at `0x25004`: rate [50:32], bucket [16:1] | How a GEM binds to a meter |
| Multicast GEM, IGMP/MLD snooping | `l2_ipMcastAddr_*`, `l2_ipmcMode_*` | Not started; needs an IPTV service to test |
| UNI pause time | ME 11 value reaches the driver and is logged | No control for the MAC's pause quanta |
| eFuse memory patches | Read and logged at bring-up | Written only if a chip turns out to carry any: the addresses are unchecked fuse data |

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
