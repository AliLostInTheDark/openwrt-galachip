# Installation and rollback gate

**Installation is blocked.** Independent power-on ROM UART recovery has not
been demonstrated. The package intentionally contains no installer, automatic
bootloader update or environment-saving command. Its NOR controller is
read-only in this release.

This is the later review procedure, not flash-write instructions.

1. Validate the current unit's complete 4 MiB backup and independent ROM marker
   recovery. Keep the installed SPL and an independently recoverable original
   boot region. A valid-but-hung SPL must not be the only recovery route.
2. Complete the RAM proper/wrapper/UART/TFTP/Linux tests with the exact manifest.
   Verify the actual installed boot chain and keep factory data from this unit.
3. Before an SPL-preserving proper trial, prove **4 KiB erase** operation in the
   exact flashing implementation to be used. The legacy 64 KiB erase path can
   include SPL when touching the first proper block. Chip datasheet support
   alone is not evidence that the software emits the required opcode.
4. If that 4 KiB path is unavailable or unverified, **stop the proper-only trial**.
   Do not round offsets, enlarge the partition or erase the first 64 KiB.
5. Only after recovery, erase geometry and a separately authorized write method
   are established, install the proper allocation while preserving SPL.
   Compare loaded sizes/CRCs/hashes before writing, then read back and compare
   every byte before resetting. Trial U-Boot proper first; SPL last.
6. Replacing SPL needs its own explicit authorization and independent rollback
   demonstration. A DDR failure cannot use the DRAM-backed UART receiver.
7. After any permitted write, verify that the full protected regions below
   remain identical. Record complete UART logs and finish the 20+20 boot matrix.

| Region | Offset | Length |
| --- | --- | --- |
| ROM/SPL allocation | `0` | `2000` |
| Compressed proper allocation | `2000` | `1e000` |
| Firmware — preserve | `20000` | `3b0000` |
| Misc/factory — preserve | `3d0000` | `20000` |
| Optical calibration — preserve | `3f0000` | `10000` |

Rollback must use the exact original bytes for this unit's boot region, with
the same verified erase/write/readback method. Do not restore a different
unit's factory areas or overwrite firmware as part of a bootloader rollback.
The absence of a CH341A makes the independent ROM gate essential; it does not
make a warm U-Boot serial receiver an independent recovery mechanism.

No `sf erase/write/update`, `mtd`, `saveenv` or ROM strap manipulation is
authorized by this document. Once the gate passes, prepare the exact command
sequence against the then-current hardware evidence instead of reusing a
stale generic recipe.
