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

## 2026-10-31: where the earlier open items stand

The debug ISP defaults above are gone (1488beaa3a), LuCI shows FEC, and L2
offload is on by default.

dmesg now separates `luna-gpon: OLT: ...` (PLOAM received, FEC seen in the
OLT's frames) from `luna-gpon: ONU: ...` (state changes, keys sent, T-CONT and
GEM binds, watchdogs). Protocol events run in the FSM timer, so these are queued
and printed only outside O2-O4 and 15 s after O5; each line carries its event's
own time. The full protocol log stays in `/proc/gpon_log`.

FEC showed off for five minutes after boot, whatever the O5 time (first seen on
at 300.1 s with O5 at 41 s, 300.0 s with O5 at 27 s). The driver's once-a-second
PM tick compared jiffies with a zero start value, so it did not run until jiffies
wrapped at 300 s. FEC status and codeword counts now start at O5.

Two first boots after flashing got `Deactivate_ONU-ID` right after Ranging_Time
(3 and 1), and one did not. The boot with one spent 2.36 s in O4 against the
usual 1.16 s, and re-ranged in 2.5 s. The cause is not settled: the NOR driver
sleeps while waiting and PLOAM runs at bottom-half priority, so flash work should
not delay it, and the OLT sends in bursts, so messages in one millisecond prove
nothing. To separate the two, dmesg now reports `ONU: FSM ticks N ms apart` and
`ONU: PLOAM read N ms after its interrupt` from 100 ms, with the worst of each
in `/proc/gpon_timing`. A slow O4 without those lines points at the OLT.

The next boot answered part of it: `FSM ticks 1660 ms apart, in O5` and `PLOAM
read 1603 ms after its interrupt`, ending at network start, then a deactivation.
`rtl9602c_eth_hw_start()` stops the state machine on the first open of any
netdev, and with failsafe disabled nothing opens one before netifd. So a link at
O5 before S20 also had no OMCI path. `gpon_provision` now opens eth0 at S09. An
earlier deactivation on that boot, 5 ms after Assign_Alloc-ID at 31.09 s with no
late-tick line, may be the missing OMCI path, but that is not proven.
