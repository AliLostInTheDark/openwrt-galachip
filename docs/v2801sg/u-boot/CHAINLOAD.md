# VSOL V2801SG RAM chainload test — 2026-10-23

## Further user results and DDR startup evidence

The user completed three TFTP downloads of the 3316056-byte LAN image, each
about 2 MiB/s, with CRC32 475efa17 every time. SPI read of the first 256 flash
bytes into RAM also succeeded and showed the stock ARM vector/header. These
are warm Ethernet RX and read-only SPI results, not cold-start validation.

The stock flash image contains the DDR startup code. Raw ARM disassembly of
`v2801sg-mtd1.bin` at linked addresses 41f0078c..41f007c8 emits `ddr init`
one character at a time through 41f0063c, then calls 41f004a8 at 41f007cc.
The called routine programs PLL/clock and controller/timing registers,
including 94300000 and 94302000, and polls completion status. The dispatcher
at 41f00778..41f00788 skips this initialization when already executing in
the 4xxxxxxx DRAM range. This establishes a software DDR startup path in the
vendor flash bootloader; the message's absence from strings output was due
to individual character writes, not evidence that it came from mask ROM.

The exact reset-ROM dispatch/XIP or SRAM execution mechanism remains to be
traced. The open-source port currently has no equivalent DDR sequence;
its dram_init only reports memory. Porting and testing the flash startup
sequence is a distinct prerequisite for a standalone power-on replacement.
Do not rerun DDR initialization while executing from DDR.

## User-reported first hardware result

The user supplied a successful 2026.10 RAM-chainload banner, working UART,
relocation to 40fa9000, FDT at 40b987c0 and stack at 40b987a0. Environment
startup reported NULL storage, bootcmd was absent and bootdelay was -1.
After explicitly setting the factory MAC, ping to 192.168.1.100 succeeded.
Read-only `sf probe` identified zb25vq64, 8 MiB, 4 KiB erase size.

This establishes warm chainload and initial management connectivity, not DDR
initialization, sustained TFTP RX or Linux handoff. The initial auto-adopted
MAC was incorrect (00:50:00:00:00:00); explicitly setting the factory MAC is
still necessary. bdinfo also exposes 41a00000..41ffffff as a second RAM bank
without an LMB reservation, including the inherited stock cursor table. Do
not treat that range as free scratch RAM or boot Linux until the handoff is
resolved. The current prompt and flash are still usable; no hang was reported.

This is a separate test variant, not a flash replacement. It uses upstream
U-Boot 2026.10 and the source drivers in this package. It links no proprietary
firmware blob, but depends on the installed stock loader to initialize DDR
and the inherited platform state. The weak s_init does no DDR setup and
dram_init only reports memory size. A working chainload does not prove a
working power-on boot. ROM boot packaging and standalone DDR initialization
remain replacement prerequisites, as does fitting the 256 KiB partition.

## Artifact and checks

- File: `bin/targets/zte/zx279125/v2801sg-uboot-chainload-c2d60c34.bin`.
- Size: 280288 bytes (`0x446e0`), 18144 bytes larger than the boot partition.
- SHA256: `c2d60c3428ff3221f81d3ea98594e69018e391867a2212165328b1e21520b575`.
- Raw entry/link address: 40400000. BSS end: 40446b28, below the initial
  stack at 40700000. Stock U-Boot resides above this test load region.
- Startup enters the normal ARMv7 vector/reset path and CP15 initialization.
- Config verified: ENV_IS_NOWHERE=y, ENV_IS_IN_SPI_FLASH=n, CMD_SAVEENV=n,
  BOOTDELAY=-1, BOOTCOMMAND empty, SPI_FLASH_UNLOCK_ALL=n.
- Build passed; `/tmp/v2801sg-uboot-chainload-build-final.log`. The build's
  ignored install-directory cleanup warning is unchanged from the base package.
- This variant has not yet been run on hardware.

Rebuild without changing the selected OpenWrt LAN RAM profile:

```sh
make package/boot/uboot-zte/compile CONFIG_PACKAGE_u-boot-vsol_v2801sg-chainload=y -j4 V=s
```

The rebuilt package output is under `u-boot-vsol_v2801sg-chainload/`; recheck
its hash before using a previously named copy.

## PC staging

Use the existing 192.168.1.100 TFTP server and keep fiber disconnected:

```sh
sudo install -m 0644 /home/ali/openwrt-galachip/bin/targets/zte/zx279125/v2801sg-uboot-chainload-c2d60c34.bin /srv/tftp/
sha256sum /srv/tftp/v2801sg-uboot-chainload-c2d60c34.bin
```

## Stock U-Boot

Reset and interrupt autoboot. Transfer the raw `.bin`, not the legacy `.img`:

```text
setenv serverip 192.168.1.100
setenv ipaddr 192.168.1.1
tftp 0x40400000 v2801sg-uboot-chainload-c2d60c34.bin
go 0x40400000
```

Run go only after transfer succeeds with 280288 bytes. No SPI write, erase,
upgrade or environment save is needed. This replaces the running monitor in
RAM; it is not expected to return to the old go command.

## New U-Boot

Expect a 2026.10 banner containing `RAM-chainload` and a `ZX279125 #` prompt.
Check serial/relocation first, then the management Ethernet path:

```text
version
bdinfo
printenv bootcmd bootdelay
setenv ethaddr 70:b6:4f:01:a6:7a
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.100
ping 192.168.1.100
```

The MAC above is this board's observed factory LAN MAC. Keep the entire UART
output, including relocation addresses and Ethernet errors. If the device
becomes unresponsive, stop; do not try other jump addresses. Reset recovers
the unchanged stock bootloader when the user is at the desk.

After this baseline, controlled TFTP RX/repeated network commands and SPI
read-only verification remain. Do not boot Linux from the new monitor yet:
warm stock-cursor adoption and standalone/new-cursor handoff need separate
validation. Do not infer that a successful warm Ethernet test proves cold
packet-engine initialization, DDR power-on behavior or flash boot readiness.
