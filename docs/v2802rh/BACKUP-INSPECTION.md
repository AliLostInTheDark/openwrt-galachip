# V2802RH backup inspection

Offline inspection of `/home/ali/Desktop/v2802rh-full-backup.bin` on
2026-10-25. Size: 4,194,304 bytes. SHA-256:
`4cc28e901cd311806a9103e61d8c7bd8fe32ac103ee5643db504b6a5b2d06086`.

## OSK files

Two `-OSK1FS-` containers begin at `0x3e3000` and `0x3e7000`.
Their body lengths are `0x420` and `0x2120`. The stored checksums
`0xe212` and `0x9cf1` both match the folded sums of big-endian 16-bit
body words. Entries have a 28-byte name, a big-endian 32-bit length,
and data padded to a 16-byte boundary.

| File | Data offset | Bytes |
| --- | --- | ---: |
| rtl-25l95-backup.bin | 0x3e3030 | 1024 |
| omci_mib.cfg | 0x3e7030 | 7922 |
| omci_custom_opt.conf | 0x3e8f50 | 435 |
| omci_raw_data | 0x3e9130 | 0 |

Extracted files and a SHA-256 manifest are in
`/tmp/v2802rh-backup-inspection/`; these temporary files are not installed
or included in firmware.

## Calibration

The calibration SHA-256 is
`087b4e9f3815363bf36cfbf34a2be548c2a3cf0fe97d4fdae7a68fd4fa6a871b`.
Its identity bytes contain vendor `ONU_B+_G`, part `GN25L95`, revision
`1.0`. Its 1024-byte length matches `GN_CAL_LEN` in the existing loader.
The SFF base checksum is inconsistent (computed `0x2c`, stored `0x2d`);
the extended checksum matches `0x8c`. The enclosing OSK checksum passes,
so retain the original bytes rather than silently correcting the EEPROM.

At the initial inspection the driver requested `rtkbosa_k.bin` and selected
another board's programming sequence. The follow-up recovered the V2802RH
RTOS routine and implemented its own profile; see
[GN25L95-IMPLEMENTATION.md](GN25L95-IMPLEMENTATION.md). The calibration data
remains per unit, and hardware validation is pending.

## OMCI configuration

`omci_mib.cfg` contains 64 class-277 priority queue entries, IDs
65280–65343, spanning T-CONT references 0x8000–0x8007, with queue size
0x0ccc (3276). This is seed configuration, not a capture of the OLT's
current services. `omci_raw_data` is empty.

The custom option tuples are `0 00000080 3`, `3 00100000 c`, and
`3 00080000 0`. Their vendor-specific meanings have not been decoded;
they are not evidence for changing OpenWrt's queue or VLAN programming.

## Scope

Local Binwalk signature inspection and direct binary parsing were used.
REA's firmware scanner was unavailable because its Binwalk provider was
not configured. No firmware was executed, flashed or applied to optics.
This backup does not by itself establish the cause of the live OMCI TX
errors, LAN2 alarm, or the currently assigned ONU-ID/OMCC GEM.
