# V2802RH diagnostic codes

Every fault line in dmesg carries a code such as `[PON-012]`. Codes are fixed: each is one place in the source, so `grep` finds it there. The rest of the line carries the values.

```sh
dmesg | grep -E "\[[A-Z0-9]+-[0-9]+\]"
```

`EVENT` lines come from the PON event queue and print late, outside O2-O4 and 15 s after O5; each ends with the time it happened. A single `EVENT` or `INFO` line is often normal: a `Link is Down` when a cable is pulled, or one deactivation on a first boot. Repeats are the fault.

New messages take the next free number in their group, and a removed code is not reused. `SYS-010` is unused.

## PON: PON activation and PLOAM

| Code | Level | Message |
|---|---|---|
| `PON-001` | EVENT | OLT: Deactivate_ONU-ID, was ONU %u |
| `PON-002` | EVENT | OLT: serial number given a new ONU-ID, restarting at O1 |
| `PON-003` | EVENT | ONU: kept downstream lock through deactivation, %u ticks at O5 |
| `PON-004` | EVENT | ONU: stopped keeping lock after %u ticks, state %u |
| `PON-005` | EVENT | ONU: re-ranging after %u LOS ticks |
| `PON-006` | EVENT | ONU: no OLT answer in O3, %u ticks, %u serial number offers |
| `PON-007` | EVENT | ONU: no Ranging_Time after %u ticks in O4 |
| `PON-008` | EVENT | ONU: O5 watchdog, %u ticks with no WAN RX |
| `PON-009` | EVENT | ONU: no upstream grant at O5, %u ticks, streak %u |
| `PON-010` | EVENT | ONU: PLOAM read %u ms after its interrupt, in O%u |
| `PON-011` | EMERG | dying gasp interrupt |
| `PON-012` | EVENT | ONU: FSM ticks %u ms apart, in O%u |
| `PON-013` | EVENT | OLT: unhandled PLOAM type %#x for ONU %u |

## OPT: Optics, laser, SerDes clock recovery

| Code | Level | Message |
|---|---|---|
| `OPT-001` | ERR | GN calibration refused: err=%d op=%u reg=0x%02x |
| `OPT-002` | WARN | PONIC_INITRDY after CDR re-pulse: %s |
| `OPT-003` | WARN | DS CDR wedged (GTC_DS_STS=0x%08x); SP_SDS_EN_RX re-acquire #%u |
| `OPT-004` | WARN | ESD-recover: DS PLEND/LOM fail=%u byte-locked at O%u (BWmap mis-phased) -> RX-CDR re-lock #%u |
| `OPT-005` | ERR | /pon-optics realtek,i2c-bus=%u: this die has buses 0 and 1 -- keeping 0 |
| `OPT-006` | ERR | %s GPIO %d level %u failed: %d |
| `OPT-007` | ERR | optical activation failed and TX inhibit readback failed |
| `OPT-008` | ERR | transmitter %s failed (%d); activation_ready=%u |
| `OPT-009` | WARN | family-lib SerDes not ready (0x%08x) -> inline fallback |
| `OPT-010` | WARN | SerDes analog-ready not seen (%s, FIB_EXT_REG21=0x%08x) |
| `OPT-011` | WARN | RST_DONE not seen (reset=0x%08x) |
| `OPT-012` | ERR | TX inhibit failed during unload |
| `OPT-013` | INFO | downstream optical signal lost; waiting for signal |
| `OPT-014` | WARN | optical diagnostics unavailable (%d) |
| `OPT-015` | ERR | realtek,ext-mdio-gpios without a valid realtek,ext-phy-addr |

## OMC: OMCI channel, omcid backend, GEM install

| Code | Level | Message |
|---|---|---|
| `OMC-001` | ERR | OMCC DS GEM install timeout |
| `OMC-002` | ERR | gpon_chip table declares no US port map -- OMCC gem %u not stamped |
| `OMC-003` | ERR | uni_test REFUSED: instance %#06x is not in this board's declared panel |
| `OMC-004` | ERR | /omci-uni REFUSED: %s -- keeping the single-UNI default |
| `OMC-005` | WARN | /omci-uni has no usable ethernet-uni-ports (%d bytes for %d UNIs): the administrative state is modelled and NEVER APPLIED |
| `OMC-006` | ERR | onu_capacity '%s' REFUSED: %s -- the board's counts stay |
| `OMC-007` | WARN | ANI-G Test Result not sent (OMCI TX error) |
| `OMC-008` | ERR | DATA GEM DS install timeout |
| `OMC-009` | ERR | gpon_chip table declares no US port map -- data gem %u not stamped |
| `OMC-010` | ERR | MCAST GEM DS install timeout |
| `OMC-011` | ERR | DATA GEM classify readback timeout |
| `OMC-012` | EVENT | ONU: deactivated %u ms after O5, %u OMCI frames reached the CPU |
| `OMC-013` | WARN | DS GEM-port CAM not cleared after the reset: the first Configure_Port-ID clears it |
| `OMC-014` | WARN | OMCI TX failed: %d (len=%u GEM=%u) |
| `OMC-015` | INFO | OMCI agent not operational |
| `OMC-016` | ERR | omcid backend not registered: %d |
| `OMC-101` | INFO | omcid: OLT after %s: no OMCI; 0 messages |

## QOS: T-CONTs, queues, buffers, rates

| Code | Level | Message |
|---|---|---|
| `QOS-001` | WARN | gpon_pbo_init called before this driver probed (ponip_base is NULL) -- skipped |
| `QOS-002` | WARN | US PBO DRAM pool alloc failed; US-NIC RX may not work |
| `QOS-003` | WARN | DS PBO DRAM pool alloc failed; DS stays SRAM-only and DS OMCI may be dropped |
| `QOS-004` | WARN | US PLOAM queue %u still full after 2 ms: type 0x%02x dropped |
| `QOS-005` | ERR | T-CONT alloc bind timeout |
| `QOS-006` | ERR | qid %u drain-out failed (%d): drain_cmd=%08x US_PBUF=%08x state=O%u activation_ready=%u |
| `QOS-007` | ERR | control qid %u drain-out failed (%d) |
| `QOS-008` | WARN | SIDVALID[%u] re-issued with arm INCOMPLETE (tcont_en=%u sch_qmap=%u pir=0x%x) -> binding committed against un-armed queue |
| `QOS-009` | WARN | data reconciliation failed rc=%d; ownership retained |
| `QOS-010` | WARN | channel reinstall retains quarantined queues %#x until reboot |
| `QOS-011` | WARN | queue %u cleanup failed: %d; quarantining |
| `QOS-012` | WARN | T-CONT %#x queue cleanup incomplete; quarantined=%#x |
| `QOS-013` | ERR | T-CONT %#x QoS rollback failed: %d (update %d); queues %#x quarantined until reboot |
| `QOS-014` | WARN | T-CONT %#x QoS not applied: %d |
| `QOS-015` | WARN | too many rate-limited GEMs, GEM %u unlimited |
| `QOS-016` | ERR | downstream rate rules failed: %d |

## L2: L2 offload and switch VLANs

| Code | Level | Message |
|---|---|---|
| `L2-001` | WARN | L2 offload cannot represent the installed OMCI service set; retaining CPU forwarding |
| `L2-002` | WARN | CPU fallback storm control readback failed |
| `L2-003` | WARN | UNI L2 settings replay failed |
| `L2-004` | WARN | swcore VLAN 4k-table engine still BUSY after %u tries (rc=%d): default VLAN %u may not be installed, LAN egress may stay filtered |
| `L2-005` | WARN | swcore VLAN table BUSY writing the PON VLAN %u: WAN broadcasts may reach the LAN ports |
| `L2-006` | WARN | swcore VLAN table BUSY writing steering VLAN %u for %s: in VLAN mode its frames would reach no jack |
| `L2-007` | WARN | V2802RH L2 offload unavailable: %d |
| `L2-008` | WARN | service VLAN %u is a jack's steering VLAN: lanN frames go plain while it is provisioned |
| `L2-009` | ERR | L2 offload rollback incomplete |
| `L2-010` | ERR | L2 fallback storm control readback failed: %d |
| `L2-011` | ERR | L2 offload teardown incomplete; data isolation retained |

## LAN: LAN jacks and UNI administrative state

| Code | Level | Message |
|---|---|---|
| `LAN-001` | WARN | UNI slot %u has no %s; its administrative state is modelled and not applied |
| `LAN-002` | ERR | /omci-uni %s REFUSED: %s -- that part keeps its default |
| `LAN-003` | WARN | UNI %#x: ageing, learn limit or max frame not applied |
| `LAN-004` | WARN | UNI %#x: auto-detection %#x not applied |
| `LAN-005` | WARN | UNI %#x: pause time %u not sent |
| `LAN-006` | WARN | UNI port %u rate replay failed |
| `LAN-007` | WARN | the jack's link could not be held down (%d): it keeps forwarding while this netdev is down |
| `LAN-008` | ERR | ethernet-ports/%pOFn: needs reg (one of this board's %u copper ports, once) and a label |
| `LAN-009` | ERR | %s (switch port %u) not registered: %d |
| `LAN-010` | ERR | UNI port %u is not one of this board's %u copper ports -- REFUSED, nothing is written |
| `LAN-011` | ERR | UNI port %u: the MAC gate did not take -- FORCE_P_ABLTY %08x, ABLTY_FORCE_MODE %08x, so the administrative state is NOT applied |
| `LAN-012` | INFO | lan%u: Link is Down |
| `LAN-013` | INFO | lan%u: link dropped and recovered within a second (%u drops) |
| `LAN-014` | INFO | lan2: link dropped and recovered within a second (%u drops) |

## PHY: LAN PHYs and SerDes

| Code | Level | Message |
|---|---|---|
| `PHY-001` | WARN | LAN1 PHY tuning failed: %d |
| `PHY-002` | WARN | SoC never raised RDY_FOR_PATCH after %d us: the GMAC<->switch resync below runs against a block that did not announce itself |
| `PHY-003` | ERR | GPHY analog patch did not complete (%d): the 0x6485 variant fixup is NOT applied, so the patch-done bit is NOT asserted and this bring-up FAILS |
| `PHY-004` | ERR | gphy: phy %u BMCR %04x did not land (%d); its power state is UNKNOWN, not assumed |
| `PHY-005` | ERR | gphy: phy %u is administratively LOCKED and reads BMCR %04x -- the power-down did not hold across the reset; refusing to bring the interface up over it |
| `PHY-006` | ERR | gphy: phy %u is not locked and reads BMCR %04x -- it is powered DOWN, so that socket would link and carry nothing |
| `PHY-007` | ERR | UNI port %u: the SerDes link gate did not take -- FORCE_P_ABLTY %08x |
| `PHY-008` | ERR | UNI port %u: the MAC gate took but this die guards port 0's PHY power on %#05x, which reads %08x -- the administrative state is NOT fully applied and stays owed |
| `PHY-009` | ERR | UNI port %u: its BMCR could not be read (%d); nothing is written back and the state is owed |
| `PHY-010` | ERR | UNI port %u: the power-down half of the unlock pulse did not land (BMCR %04x) -- the far end would never re-negotiate |
| `PHY-011` | ERR | UNI port %u: the PHY did not reach %s (%d, BMCR %04x); the administrative state is owed |
| `PHY-012` | WARN | RTL8221B LED setup read failed: %d |
| `PHY-013` | WARN | RTL8221B LED setup read back %04x, not 00f8 |
| `PHY-014` | ERR | external PHY at MDIO address %u did not answer |
| `PHY-015` | WARN | RTL8221B SerDes setup pending; link polling will retry |
| `PHY-016` | WARN | LAN2 PHY SerDes option %u failed: %d; retrying |

## NAT: L34 hardware NAT

| Code | Level | Message |
|---|---|---|
| `NAT-001` | WARN | L34 hw-nat init failed; software forwarding |
| `NAT-002` | WARN | L34: no address notifier; a WAN that changes address keeps its stale tables |
| `NAT-003` | WARN | L34: the common TC lifecycle could not be created; software forwarding |
| `NAT-004` | WARN | L34: could not bind the TC lifecycle to the device; software forwarding |
| `NAT-005` | ERR | rtl9602c-l34: table %u entry %u write failed (%d) |
| `NAT-006` | WARN | rtl9602c-l34: neighbour L2 slot %u changed before unpin |
| `NAT-007` | ERR | rtl9602c-l34: WAN classifier setup failed (%d) |
| `NAT-008` | WARN | rtl9602c-l34: WAN rows kept: owners neigh=%u out_reserved=%u in_reserved=%u |

## SYS: Bring-up, board description, identity

| Code | Level | Message |
|---|---|---|
| `SYS-001` | ERR | gponmon netdev not allocated -- no control-plane capture |
| `SYS-002` | ERR | gponmon netdev not registered -- no control-plane capture |
| `SYS-003` | ERR | realtek,port-leds holds %d cells, not <index type> pairs (max %zu) -- no port LED driven |
| `SYS-004` | WARN | LED %s not exposed at /sys/class/leds (%d) |
| `SYS-005` | ERR | panel LEDs not driven (%s: no LED-controller table for this die) |
| `SYS-006` | ERR | panel LEDs not driven (%s: the DT gpon node declares no valid PON/LOS lamp and no port LED) |
| `SYS-007` | WARN | datapath_tables_init called before this driver probed (swcore_base is NULL) -- skipped; the switch fabric is NOT initialised |
| `SYS-008` | ERR | memory-region %d is not a %lu-byte pool -- allocating instead |
| `SYS-009` | WARN | ONU-SN "%s" is not 4 ID chars + 8 hex digits -- keeping the serial in force |
| `SYS-011` | WARN | GPON interrupt %d refused (%d): downstream PLOAM waits for the FSM tick |
| `SYS-012` | ERR | realtek-luna's struct gpon_ploam_ops leaves the MANDATORY op ->%s NULL -- refusing to probe (nothing has been touched) |
| `SYS-013` | ERR | ioremap 0x%08x failed |
| `SYS-014` | ERR | ioremap 0x%08x failed |
| `SYS-015` | ERR | ioremap 0x%08x failed |
| `SYS-016` | WARN | scratch R/W failed — MAC may be gated |
| `SYS-017` | ERR | gpon_ploam_init refused: mandatory op ->%s is NULL |
| `SYS-018` | WARN | onu_password '%s' is not a G.984.3 password; sending an empty one |
| `SYS-019` | WARN | cannot expose cached health status |
| `SYS-020` | WARN | TX-DMA parked (head=%u dirty=%u otx=%u/%u cdo0=%u txok=%u IO_CMD=%08x ISR1=%08x): IP-block power-cycle recovery #%u |
| `SYS-021` | WARN | eFuse asks for %04x%04x = %04x%04x; not applied |
| `SYS-022` | WARN | %u eFuse memory patches not applied |
| `SYS-023` | WARN | request_irq(%d) failed (%d); pure-poll fallback |
| `SYS-024` | ERR | switch bring-up failed (%d): eth0 stays DOWN rather than up on a half-programmed fabric |
| `SYS-025` | ERR | realtek,rx-ring-entries %u is outside 64..%u |
| `SYS-026` | WARN | gpon0 (WAN) register failed; WAN datapath disabled |
