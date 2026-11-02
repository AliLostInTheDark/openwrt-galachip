# Installation and rollback gate

**Recovery is by external SPI programmer.** On 2026-11-02 a CH341A clip
gave three identical 4 MiB reads of this unit (SHA256 `8698f068...8335`), and a
full-chip erase, write and verify restored that backup. Independent ROM UART
recovery is still unproven and is no longer required for a proper trial. The
package contains no bootloader installer; all boot-region writes use the
programmer, not `sf` from U-Boot.

Before each write:

1. Read the chip three times with the programmer; all hashes must match the
   last verified backup, or take a new verified backup.
2. Build the image with `tools/flash_image.py <backup> <proper.img> <out>`.
   It checks the proper footer and wrapper CRC, and rejects any change
   outside `0x2000`-`0x20000`. Add `--spl` only for the final SPL step.
3. Write the whole file with flashrom, which verifies after writing. Full-chip
   writes avoid the 4 KiB erase question: flashrom rewrites every block it
   erases from the file.
4. On failure, rewrite the verified backup the same way.

Order: proper first, keeping stock SPL; then `run boot_flash`, the timer, `reset` and
the 20 cold/20 reset matrix; SPL last, after its own explicit authorization.
Record complete UART logs for every trial.

| Region | Offset | Length |
| --- | --- | --- |
| ROM/SPL allocation | `0` | `2000` |
| Compressed proper allocation | `2000` | `1e000` |
| Firmware — preserve | `20000` | `3b0000` |
| Misc/factory — preserve | `3d0000` | `20000` |
| Optical calibration — preserve | `3f0000` | `10000` |

Rollback uses this unit's own verified backup. Never restore a different
unit's factory or calibration areas.

U-Boot's `sf` may write only the firmware partition (`run upgrade_tftp`);
the SPI driver refuses the boot region, `misc` and `hwconfig`. Boot-region
changes use the programmer. No `saveenv` or ROM strap manipulation.
