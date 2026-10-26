# V2802RH PON bring-up log

## 2026-10-25, build d6d62122d0 (RAM boot, Nokia/ALCL OLT)

Works:
- O5 with ZYOT20095605, no PLOAM password.
- omcid stays up (responder userspace, agent operational).
- Profile nokia-alcl configured/effective/forced; MIB sync 4, 163 objects.
- 41 OMCI responses.

Open:
- `backend op 4 -> -122` (set_tcont_qos, T-CONTs 0x8000-0x8007) and
  `backend op 5 -> -122` (set_gem_rate, GEM CTPs 0x100/0x101). -122 is
  EOPNOTSUPP on MIPS: luna_xpon.c lacks these ops. omcid retries on every
  reconcile, flooding the log.
- tx-errors 21 against 42 OMCI tx: some OMCI replies are not sent.
- onu-id 0 and gem-port 0 in status. Stock previously had ONU-ID 5 and
  OMCC GEM 5; assignments can change, so this comparison alone does not
  establish a reporting fault.
- Alarm lan-los on UNI 0x0102 (LAN2): check the uni_link mapping for the
  SerDes port.
- No luna-xpon "service" lines in dmesg: GEM 256/257 services never
  installed. No DHCP/PPPoE test yet.
- The rest of the backend still has to be ported for this board: QoS, GEM
  rate, services into hardware (LAN<->PON offload).

BOSA (GN25L95 "ONU_B+_G", not an RTL8290B):
- `BOSA writes REFUSED ... reg 0x12@0x54 not written`: gpon_provision (or
  the driver's boot path) still tries the RTL8290B register programming on
  this module. The refusal is correct, but the GN25L95 path is missing:
  "RTL boot programming skipped; GN awaits per-unit calibration".
- DDM reads: TX 1.83 mW, bias 14.5 mA, RX 2800 nW (-25.5 dBm). Values move
  between reads (bias 14.6 -> 14.5, TX 1.74 -> 1.83 mW). Port GN25L95
  calibration from the stock rtl-25l95-backup.bin (osk partition) so APC/MOD
  and the RX DDM are set as on stock.

## 2026-10-26, local implementation

The four-UNI identity in the supplied omci-diag output is intentional
emulation of the user's original ONT. Keep the configured identity and
capabilities; physical port count is not grounds to replace them.

The V2802RH GN25L95 sequence was recovered from the exact full backup and
implemented separately from other board profiles. See
[GN25L95-IMPLEMENTATION.md](GN25L95-IMPLEMENTATION.md) for the evidence and
checks. OMCI TX now appends the missing CRC to userspace PDUs, and extended
RX messages retain their complete content. Rebuilding and hardware testing
remain with the user; the earlier live results do not validate these changes.

The follow-up [omcid provider audit](OMCID-RTL9601D.md) removes fixed
reference-service creation from RTL9601D and reports implemented backend
resources. The kernel and OMCI packages now require matching ABI 17 builds.

## 2026-10-26, traffic passes (tag `v2802rh-traffic-ok`)

RAM-booted 7b9623b182 against the Nokia ALCL OLT: O5, full OMCI
provisioning (MIB sync 26-33), GEM 256 / VLAN 100 and GEM 257 / VLAN 660,
and the LAN PC pings the ISP gateway through VLAN 100. What it took:

- AES on the GEM ports the OLT marks encrypted, the OMCC included
  (DS_TRAFFIC_CFG bit 4, stock's dsGemPort aes_en). Without it OMCI went
  dark as soon as Nokia switched encryption on.
- 64 service slots: the OLT maps eight p-bits plus a default to each GEM.
- Each service VLAN in the switch VLAN table (PON + CPU, tagged); the VLAN
  filter otherwise drops it.
- The PON-MAC mode-set keeps omcid's classify SIDs; it used to invalidate
  each GEM flow right after installing it.
- WAN VLAN entries use IVL (bit 10 set; the SDK writes it inverted). With
  SVL a reply to a LAN host was looked up on the host's LAN port, outside
  the VLAN, and dropped.

Debug only: `base-files/etc/uci-defaults/99-v2802rh-debug-isp` bakes in the
bench ISP's settings; delete it before a release image. Still open: LuCI
does not show the FEC/BIP counters (`/proc/gpon` pm line has them),
hardware L2 offload is off, and NOR flashing waits until all of this is
stable.

## 2026-10-26: traffic 36 s after boot (was 64 s)

`omci-diag report` (omci-report) decodes the boot's PLOAM and OMCI both
ways, with answer timing; `omci.main.trace=1` records from boot.

| boot            | laser on | OMCI answer (median) | provisioning | traffic |
|-----------------|----------|----------------------|--------------|---------|
| before          | 34.6 s   | 203 ms               | 23 s, 2 deactivations | 64 s |
| after           | 19.5 s   | 1 ms                 | 4.3 s, none  | 36 s    |

- omcid wrote its 233 KB checkpoint before every answer (~200 ms on this
  CPU). The OLT gave up on the slow early answers: that was the
  deactivate-after-ranging loop. The checkpoint is now deferred.
- The identity is in the kernel from S09 (`/etc/config/omci`, the only
  identity config) and omcid releases the transmitter itself; omci.agent
  no longer re-sets unchanged keys (10 s).
- Remaining before the laser: preinit (~8.5 s on a RAM boot) and init
  scripts up to omcid (S11, ~19 s).
