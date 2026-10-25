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
- onu-id 0 and gem-port 0 in status. Stock had ONU-ID 5, OMCC GEM 5, so the
  ONU-ID/channel report to the framework is wrong or stale.
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
