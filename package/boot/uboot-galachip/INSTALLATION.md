# Installation and rollback gate

## Without a programmer: the bootloader from the stock U-Boot

The RAM-started U-Boot installs the whole boot region, SPL and proper, with
4 KiB erases; a broken SPL recovers over the mask ROM's UART menu (below).
Serve the RAM U-Boot (`…-u-boot.bin` in the OpenWrt image directory;
`xz000-g7-uboot-ram.bin` here) and `…-u-boot-with-spl.bin` (named as
`bootfile_uboot`) by TFTP,
then at the stock `=>` prompt, one line at a time:

```text
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.10
mw.l 9080000c 0
tftpboot 41000000 openwrt-galachip-gc1601-tplink_xz000-g7-u-boot.bin
go 41000000
```

At the `G7>` prompt, `run upgrade_uboot`: `ubcheck` must report both `SPL OK`
and `proper.img OK`, and the compare `Total of 131072 byte(s) were the same`.
Then `reset`. Later updates are the same `run upgrade_uboot` from flash.
Do not write the boot region with the stock U-Boot's `sf`: it erases 64 KiB
blocks.

## With a programmer

**Last resort: the external SPI programmer.** On 2026-11-02 a CH341A clip
gave three identical 4 MiB reads of this unit (SHA256 `8698f068...8335`), and a
full-chip erase, write and verify restored that backup.

## Without a programmer: mask ROM UART recovery

Verified 2026-11-03. When the SPL is broken the mask ROM prints:

```text
Boot from Offset 0: Image header failed! (wrong magic number)
1. SPI-NOR
2. UART(Xmodem)
Please select boot mode[1-2]:
```

The ROM takes XMODEM-CRC but repeats `C` until the first block and cancels on
a repeated block, so plain `sx` fails (it resends block 1 on a stale `C`).
Send with `tools/rom-xmodem.py`; it strips the 16-byte ROM header, since the
ROM runs the upload from its first byte, so `…-u-boot-spl.bin` goes as is.

By hand in picocom:

```sh
picocom -b 115200 --send-cmd tools/rom-xmodem.py /dev/ttyUSB1
```

Type `2` and Enter, then `C-a C-s` and the SPL file name. Or in one step,
which also stops U-Boot's 1 s autoboot:

```sh
tools/rom-xmodem.py /dev/ttyUSB1 openwrt-galachip-gc1601-tplink_xz000-g7-u-boot-spl.bin
```

U-Boot proper then loads from NOR; `run upgrade_uboot` rewrites the boot
region, SPL included.

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

U-Boot may write the boot region (`run upgrade_uboot`) and the firmware
partition (`run upgrade_tftp`). The SPI driver refuses any erase or program
of `misc` and `hwconfig`. No `saveenv` or ROM strap manipulation.
