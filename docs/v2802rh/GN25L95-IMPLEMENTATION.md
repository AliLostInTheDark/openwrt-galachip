# V2802RH GN25L95 and RTL9601D OMCI

Local implementation, 2026-10-26. No device access or optical programming
was performed during development.

## Exact stock evidence

The input is `v2802rh-full-backup.bin`, SHA-256
`4cc28e901cd311806a9103e61d8c7bd8fe32ac103ee5643db504b6a5b2d06086`.
The RTOS LZMA stream starts at flash offset `0x61160`; its properties byte
is `0x6f` (lc=3, lp=2, pb=2), and its declared output size is 12,777,612
bytes. Local 7-Zip recovered that many bytes; the carved input also
contained trailing flash data, which 7-Zip reported. No integrity checksum
for the decompressed application was established. Its SHA-256 is
`8174f6378ae21b239b68b13e2eccf76d5212db8d386e823a865661ee7cbf74d2`.

Ghidra 12.1.4 decoded the relevant routines as MIPS16e, based at
`0x80000000`. The explicit raw-image mapping is supported by the firmware
header's load address and matching literal-pool pointers. This was focused
static analysis, not a whole-firmware audit or execution of stock code.

- `0x80470404`: GN25L95 initialization from the 1024-byte calibration file.
- `0x80470354`: temperature correction selected by MIB key `0xc018`.
- MIB table entry at `0x8066a1c8` maps `0xc018` to `LAN_TYPE`.
- The checksum-validated hardware MIB in this backup has `LAN_TYPE=0x3`.

The sequence restores all 256 A0 bytes, loads tables 4/5/6/2, repeats
page-2 D2–E7, restores lower alarms and sparse bytes, sets page-2 B6 bit 2,
enables alarms, pulses soft TX disable, sets safe startup, and writes the
password bytes. Both page-1 F8 and FC use calibration offset `0x1f8` in
stock's instructions. The final correction writes D2/D3=`01/c9`, followed
by D4/D5=`aa/00` when LAN_TYPE is 1, otherwise `a9/ca`.

Analysis files, pseudocode, disassembly, and local test helpers are under
`tmp/v2802rh/bosa-audit/` (ignored by Git).

## Implemented path

The V2802RH DT declares `semtech,gn25l95`. Together with the exact board
compatible, this selects the V2802RH sequence and skips RTL8290B identity
register probes. Physical Semtech identification is still required before
calibration writes.

`gpon_provision` reads this unit's `rtl-25l95-backup.bin` from its
checksum-validated OSK container and `LAN_TYPE` from its hardware MIB. It
stages these separately as:

- `/lib/firmware/vsol/v2802rh-gn25l95.bin`: the original 1024 bytes.
- `/lib/firmware/vsol/v2802rh-gn25l95-lan-type`: decimal LAN_TYPE plus newline.

Missing/invalid data prevents serial-triggered activation and withdraws
staged files. The driver checks both inputs and stops on the first failed
I2C operation. Existing TX_DISABLE inhibition covers calibration and is
released only after successful completion. The runtime success log names
`V2802RH RTOS` and reports 898 operations.

The RTL9601D OMCI backend appends the AAL5 CRC to userspace PDUs before
NIC submission; baseline replies therefore reach the NIC as 48 bytes.
It also preserves extended RX content, clears stale services after OMCC
reinstallation, and accepts resets of unconfigured QoS/rate state. Explicit
unsupported scheduling or rate requests remain errors through userspace.
The user's ONT emulation identity and four-UNI setting are preserved.
The subsequent [RTL9601D provider-model audit](OMCID-RTL9601D.md) moves
resource limits and profile-service policy into the backend and updates
the OMCI ABI to version 17.

## Verification and remaining work

The changed kernel sources compile with the configured MIPS cross compiler.
The existing omcid test suite passes, as does an additional local backend
check that active unsupported policies are not silently acknowledged.
Checks against the real backup confirm byte-identical calibration extraction,
LAN_TYPE=3, the 898-operation sequence, both temperature branches, and
length/capacity rejection. An injected I2C error stops the executor at the
first failed write. Shell staging checks cover successful publication,
missing LAN_TYPE, and missing OSK data; failure withdraws both staged files.

A full firmware build, RAM boot, cold boot, optical measurements, and
LAN-to-PON forwarding validation were pending when this was written. Explicit
multi-queue QoS, per-GEM policing and hardware LAN-to-PON offload were not part
of this change; see [L2-OFFLOAD.md](L2-OFFLOAD.md) for offload. ONU-ID/OMCC values and the LAN2 alarm require live evidence;
the supplied output alone does not establish their cause.
