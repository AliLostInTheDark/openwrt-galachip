# OpenWrt for the TP-Link XZ000-G7

OpenWrt/Linux 6.18 bring-up for the GalaChip GC1601-based XZ000-G7.
This is a development fork, not an official OpenWrt release or a universal
replacement for every XZ000-G7 hardware revision.

The installation below keeps the stock boot loader and the unit's factory
data. **Back up first, test from RAM, then install with `sysupgrade`.**
Replacing U-Boot or installing an SPL is not required.

## Supported hardware and current status

This guide covers the tested **32 MiB RAM / 4 MiB SPI NOR / one Gigabit
Ethernet port** board with the GC1601 SoC. Check the board and flash layout,
not just the product name. The 8 MiB variants, including reported v1.20 BR/TR
units, are outside this installation procedure.

Status as of 15 October 2026:

| Area | Validation so far |
| --- | --- |
| Ethernet and GPON SFU offload | Approximately 900/900 Mbit/s reported on the tested Nokia/ALCL GPON connection, including flash-boot testing. This is not an OLT compatibility guarantee. |
| EPON | Registration, OAM/authentication, and full offload reported working in untagged mode on a CDATA OLT. |
| EPON ONU-side VLAN tagging | Not yet validated on a known-good OLT configuration. The CDATA tagged test was inconclusive because the LCO reported an OLT-side problem. |
| VSOL EPON OLT | Testing pending; existing inter-authentication code is not proof of compatibility. |
| Recovery | Serial/TFTP RAM boot and firmware-region stock restoration through the stock U-Boot have been exercised. |

The device is intended as an ONU bridge. The small-flash profile does not
provide a normal home-router package set: DHCP/DNS serving, firewalling,
Wi-Fi, and PPPoE termination are not included. Use your router for PPPoE or
IPoE, routing, NAT, and firewalling.

## 1. Before you start

You need:

- A **3.3 V logic-level USB-UART adapter**, an Ethernet cable, and the board's
  normal power supply. Do not use a 5 V UART.
- A PC with a serial terminal, a TFTP server, and an SCP client. Keep the PC
  directly connected to the G7 during installation.
- Both images from the **same build**:

  - `openwrt-galachip-gc1601-tplink_xz000-g7-initramfs-zImage`
  - `openwrt-galachip-gc1601-tplink_xz000-g7-squashfs-sysupgrade.bin`
- Your own ISP-provisioned identity and authentication details: GPON serial
  and password/LOID, or the EPON MAC/LOID as applicable, plus the service VLAN
  and whether tagging belongs on the ONU or your router.

For prebuilt images, obtain the exact source revision and checksums from the
build's publisher. Otherwise, build this source tree as described below; the
official OpenWrt Firmware Selector is not the download source for this fork.

Do not upload the sysupgrade image through the stock web UI. It is not a
vendor update image. Do not use an image for the XZ000-G3 or another ONU.

Disconnect the fiber while installing and making backups. Reconnect it after
selecting the correct PON mode and configuring your authorized identity.

### Flash layout: 4 MiB board only

Offsets below are hexadecimal, with the end address excluded.

| Range | OpenWrt partition | Size | Contents |
| --- | --- | ---: | --- |
| `0x000000` - `0x020000` | `u-boot` | 128 KiB | Stock boot chain |
| `0x020000` - `0x3d0000` | `firmware` | 3776 KiB | Kernel, root filesystem, and writable overlay |
| `0x3d0000` - `0x3f0000` | `misc` | 128 KiB | Factory configuration, including the MAC address |
| `0x3f0000` - `0x400000` | `hwconfig` | 64 KiB | Unit-specific optical calibration |

Normal `sysupgrade` targets `firmware`; the other three partitions are
read-only in the board DTS. The stock kernel exposes separate `kernel` and
`rootfs` partitions inside the same firmware range, so its partition names
and MTD numbers differ. Never identify a write target by an assumed MTD number.

**Never erase or replace `u-boot`, `misc`, or `hwconfig` during installation.**
Someone else's factory backup is not a substitute for yours. Do not run
vendor calibration initialization commands such as `gccli bob init`.

## 2. Connect the UART and stop autoboot

With board power disconnected, connect the labelled UART pads:

| G7 | USB-UART adapter |
| --- | --- |
| GND | GND |
| TX | RX |
| RX | TX |
| VCC | **Not connected** |

Power the board with its normal supply, not through the UART header. Set the
terminal to **115200 baud, 8 data bits, no parity, 1 stop bit, no flow control**.
Only one terminal program should open the serial port.

Stop autoboot when the stock U-Boot prints `Hit any key to stop autoboot`.
The prompt is `=>`. Paste **one command at a time**; long pastes can lose
characters in the loader's serial input buffer.

All U-Boot environment changes below are temporary. **Do not run `saveenv`.**
Do not change `bootcmd`, replace the boot loader, or invoke vendor flash/update
aliases to perform a RAM test.

## 3. Prepare Ethernet and TFTP on the PC

Set the PC's wired interface manually:

```text
PC address:   192.168.1.10
Subnet mask:  255.255.255.0
G7 address:   192.168.1.1
Gateway:      leave blank
```

There is no DHCP server in the bridge image. Make sure another interface or
VPN is not routing `192.168.1.0/24` elsewhere.

On Linux, configure the address in your network manager and serve the image
from your TFTP server's configured root, commonly `/srv/tftp`. On Windows,
set a manual IPv4 address and select `192.168.1.10` as the TFTP server's
listening interface. Allow TFTP through the firewall on this direct/private
connection; do not disable the firewall globally.

For shorter serial commands, place a copy of the initramfs image in the TFTP
root named **`g7-initramfs.zImage`**. This is only a filename change; do not
modify the file contents. Verify downloaded images against the build's
published `sha256sums` on the PC.

## 4. Boot OpenWrt from RAM

At the **stock U-Boot `=>` prompt**, run each line separately:

```text
setenv autostart no
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.10
mw.l 9080000c 0
setenv bootargs console=ttyS0,115200n8 earlycon
tftpboot 41000000 g7-initramfs.zImage
```

The `mw.l` command disables the watchdog for this temporary loader session;
it is not a flash write. The temporary boot arguments avoid inheriting the
stock root-device and partition arguments for this initramfs boot.

Check that TFTP finishes successfully and that `Bytes transferred` matches
the initramfs file's size on the PC. Do not boot after an aborted or incomplete
transfer. Then run:

```text
bootz 41000000
```

Use the current initramfs image with its appended device tree; no separate
DTB or custom U-Boot is needed for this procedure. A successful RAM boot does
not install the image to NOR. Power-cycling returns to the existing firmware.

Wait for OpenWrt to finish starting. At its **Linux shell**, inspect:

```sh
cat /proc/version
cat /proc/cmdline
cat /proc/mtd
```

Confirm the four named regions and their sizes match the table above. Extra
`kernel`, `rootfs`, or `rootfs_data` child partitions are normal. If the main
layout differs, stop before any flash write.

Set a temporary root password from the serial console before using SSH/SCP:

```sh
passwd
```

The RAM session's settings, including this password, do not survive a power
cycle or a clean installation.

## 5. Back up the stock flash and copy it off the G7

Do this **before the first OpenWrt installation**, while the stock contents
are still in NOR. On the RAM-booted G7, run:

```sh
(
    set -e
    mkdir /tmp/g7-stock-backup
    for p in u-boot firmware misc hwconfig; do
        d=$(awk -v label="\"$p\"" '
            $4 == label { sub(/:$/, "", $1); print $1 }
        ' /proc/mtd)
        [ -n "$d" ] || { echo "Missing partition: $p"; exit 1; }
        dd if="/dev/$d" of="/tmp/g7-stock-backup/$p.bin" bs=65536 || exit 1
    done
    wc -c /tmp/g7-stock-backup/*.bin
)
```

Do not continue after a read error. Expected file sizes:

| File | Bytes |
| --- | ---: |
| `u-boot.bin` | 131072 |
| `firmware.bin` | 3866624 |
| `misc.bin` | 131072 |
| `hwconfig.bin` | 65536 |
| Total | 4194304 |

Copy **all four files to the PC**. `/tmp` is RAM, not a safe backup location.
For a Linux/OpenSSH PC, run locally:

```sh
mkdir -p g7-stock-backup
scp -O 'root@192.168.1.1:/tmp/g7-stock-backup/*.bin' g7-stock-backup/
cd g7-stock-backup
wc -c *.bin
sha256sum u-boot.bin firmware.bin misc.bin hwconfig.bin > SHA256SUMS
cat u-boot.bin firmware.bin misc.bin hwconfig.bin > g7-stock-full.bin
wc -c g7-stock-full.bin
```

`g7-stock-full.bin` must be exactly **4194304 bytes**. The concatenation order
matters. Keep both the individual partitions and the full-chip file privately,
with a second copy on another storage device.

On Windows, select **SCP**, not SFTP, in your file-transfer client and download
`/tmp/g7-stock-backup/`. The minimal image does not include an SFTP server.
Check each file's size and record hashes on the PC. A full-chip file can also
be assembled from these four files in the same order using binary concatenation.

These backups contain device identifiers, calibration, and potentially ISP
credentials. **Do not commit them to this repository or attach them to public
issues.** If you repeat this procedure after installing OpenWrt, the firmware
backup contains OpenWrt, not stock.

## 6. Install OpenWrt to NOR

First confirm the RAM image boots reliably and management Ethernet works.
Keep the fiber disconnected. Use the matching **sysupgrade** image, not the
initramfs image or a vendor firmware download.

Copy the image to `/tmp` on the G7. For a Linux/OpenSSH PC:

```sh
scp -O openwrt-galachip-gc1601-tplink_xz000-g7-squashfs-sysupgrade.bin \
    root@192.168.1.1:/tmp/g7-sysupgrade.bin
```

On the **G7 Linux shell**, test it first:

```sh
sysupgrade -T /tmp/g7-sysupgrade.bin
```

Only after this succeeds, with verified off-device backups and stable power:

```sh
sysupgrade -n /tmp/g7-sysupgrade.bin
```

This is the first installation's intentional flash write. It replaces the
firmware region, discards the RAM session's settings, and reboots. Do not
interrupt power. Do not use `-F` to bypass image checks.

LuCI's **System -> Backup / Flash Firmware** is an alternative if included in
your build: select the sysupgrade image and disable **Keep settings** for the
first installation. Never upload the initramfs image there.

After reboot, keep the PC on its static address, open `http://192.168.1.1`,
and set a root password again. Management HTTP is intended for the trusted
direct LAN connection, not exposure to the public internet.

## 7. Configure GPON or EPON

Select the network's actual technology under **Network -> PON Mode**, then
save and reboot if changing modes. GPON is the default; it does not register
on an EPON OLT, and vice versa. Mode changes on a RAM image are not durable
across a power cycle.

### GPON

Under **Network -> OMCI -> Configuration**:

1. Enter your ISP-authorized GPON serial and password or LOID. Apply only the
   additional identity fields required by your provider.
2. Start with **OLT profile: Auto detect** and **Force OLT profile: Do not
   force**. Do not carry a forced profile from another connection into a new
   OLT test.
3. Leave traffic-management capability overrides, ONU type, and UNI count at
   their board defaults unless your provider requires otherwise. Advertising
   an HGU or extra UNIs does not create physical ports or increase throughput.
4. Select the LAN VLAN behavior below, then **Save & Apply**.

| GPON LAN VLAN mode | Router-facing behavior |
| --- | --- |
| Transparent | The G7 passes service tags through; configure the required VLAN on your router. |
| Tagged | The G7 adds the chosen VLAN upstream and removes it downstream; the router sends untagged traffic. |
| Control by OLT | Uses the OLT-provisioned mapping for untagged traffic. Availability depends on that service's provisioning. |

Do not tag the same service on both devices unintentionally. Keep the default
`eth0`/`pon0` bridge rather than adding `pon0.<VID>` as a second tagging layer.
The optional VLAN scan sends discovery probes and temporarily pauses offload;
it is not a passive scan or a way to discover unprovisioned services.

Connect the fiber and check **Network -> OMCI -> Status**, or use:

```sh
omcictl status
cat /sys/class/net/pon0/device/gpon_state
```

`O5` means GPON registration succeeded. Internet service additionally needs
the correct OMCI service mappings, VLAN, and your router's PPPoE/IPoE setup.

### EPON

Under **Network -> EPON -> Configuration**, enter the authorized ONU MAC and
LOID/password if required. Leave active OAM discovery off initially; enable
it only if MPCP registers but OAM remains waiting because the OLT never starts
discovery. Do not guess vendor identity fields or an inter-authentication secret.

Start with **Set by the OLT** for VLAN configuration when the OLT provisions
CTC Port VLAN correctly. Use **Transparent** for a service explicitly arranged
to pass frames without ONU-side VLAN rewriting. Use **Tagged** only when the
OLT service requires the ONU to insert/remove that VLAN. These settings are
separate from GPON's OMCI VLAN settings.

Connect the fiber and inspect **Network -> EPON -> Status**, or run:

```sh
cat /var/run/epon-oamd.json
cat /sys/class/net/pon0/device/epon_state
```

Look for MPCP registration, `oam_link=1`/OAM `send_any`, and successful CTC/auth
where the OLT requires them. MPCP registration alone is not a complete service.
Tagged EPON performance and VSOL OLT compatibility still need validation; do
not treat the known-good untagged CDATA result as proof for those paths.

### Management behind your router

The default management address is `192.168.1.1`. If your router's own LAN uses
the same subnet, change the G7 management subnet while directly connected.
Access through the router generally needs an appropriate WAN-side secondary
address and firewall policy; a route alone may not be sufficient. Do not bridge
your trusted LAN into the ISP-facing network just to reach the management UI.

## 8. Updating and checking performance

Back up your OpenWrt configuration before updating. Transfer the new matching
sysupgrade image as in step 6, then run on the G7:

```sh
sysupgrade -T /tmp/g7-sysupgrade.bin
```

If the test succeeds, a normal update that retains compatible settings is:

```sh
sysupgrade /tmp/g7-sysupgrade.bin
```

Use `-n` instead when the release notes require a clean configuration or when
deliberately discarding settings. Never carry unrelated ISP profiles into a
clean test. With only 4 MiB flash, avoid adding packages unless space and
dependencies have been checked; keep the diagnostic tools in the device profile.

After installation, test a **power-off cold boot**, not only a RAM boot or
software reboot. Compare the same PC/router path, server, plan, and VLAN mode.
The historical strict-priority and loader-handoff issues mean an old fast
initramfs result does not validate a different flashed build.

Useful read-only checks on the G7:

```sh
cat /sys/class/net/eth0/speed
cat /sys/class/net/pon0/device/sfu_offload
cat /sys/bus/platform/devices/90e00000.packet-engine/sfu_snapshot
cat /proc/net/dev
```

`1000` confirms Ethernet link speed. `sfu_offload=1` is an enable setting,
**not proof that the traffic matches an installed hardware flow**. Include
counter deltas and CPU load during an upload/download when reporting a slowdown.

## 9. Recovery and returning to stock

### OpenWrt does not boot

If the stock boot loader and Ethernet still work, interrupt autoboot and
repeat the RAM-boot procedure in steps 2-4. From the working initramfs, inspect
the failure and reinstall a validated sysupgrade image if appropriate.

Serial/TFTP recovery depends on a working boot chain; it is not guaranteed
after bootloader damage or a hardware failure. Holding reset at power-on has
not established a mask-ROM recovery procedure for this board. The stock
`httpd` command is bootloader firmware recovery, not a replacement for a
working boot loader. Do not rely on it to recover an overwritten SBL/U-Boot.

### Restore your own stock firmware region through stock U-Boot

This method has been exercised on the 4 MiB board. It writes only
`0x020000 through 0x3cffff`, preserving the boot chain and factory regions.
It requires **your original raw `firmware.bin` backup from step 5**, exactly
**3866624 bytes (`0x3b0000`)**. A TP-Link downloadable update file, an OpenWrt
sysupgrade image, and a full-chip dump are not interchangeable inputs here.

Disconnect the fiber. Put that raw backup in the PC's TFTP root as
`g7-stock-firmware.bin`. At the **stock U-Boot prompt**, run separately:

```text
setenv autostart no
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.10
mw.l 9080000c 0
tftpboot 41000000 g7-stock-firmware.bin
printenv filesize
sf probe
```

**Stop unless** the transfer completed with `3866624` bytes, `filesize` is
`3b0000`, and `sf probe` identifies the expected **4 MiB** flash. Confirm again
that the backup belongs to this unit and that its boot chain/layout is the
one described here. The next commands erase and replace the firmware region:

```text
sf erase 20000 3b0000
sf write 41000000 20000 3b0000
sf read 40c00000 20000 3b0000
cmp.b 41000000 40c00000 3b0000
```

Require `Total of 3866624 byte(s) were the same` before power-cycling. If
verification fails, keep power on and retain the UART session; do not try
broader erases. This procedure does not repair damaged factory data or a
missing boot loader. An external SPI programmer may still be needed for those
failures. A full-chip dump is valuable recovery material, not permission to
overwrite the entire flash during routine installation.

## Building from this source tree

The GalaChip kernel additions and patches are included under
[`target/linux/galachip`](target/linux/galachip); a separate kernel checkout is
not required to build this OpenWrt tree. PON control-plane packages are under
[`package/utils/xpon`](package/utils/xpon).

Use a supported OpenWrt build host with the usual build dependencies. From a
clean checkout, the device seed configuration is:

```sh
./scripts/feeds update -a
./scripts/feeds install -a
cp target/linux/galachip/tplink_xz000-g7.diffconfig .config
make defconfig
make -j"$(nproc)"
```

The `cp` step replaces `.config`; save any existing build configuration first.
Images appear in `bin/targets/galachip/gc1601/`. Do not add private ISP defaults,
factory dumps, passwords, or per-unit overlays to a published source tree.

## Credits and license

XZ000-G7 port maintained by Ifran Ali, building on OpenWrt, Linux, LuCI, and
the credited xPON/OMCI work in the source files. Existing authorship and
file-level license notices remain applicable. OpenWrt is licensed under
GPL-2.0; individual components may use their own compatible licenses.

This is a development fork. Report board/OLT-specific issues here with the
exact build revision, board flash size, OLT model/software, PON mode, VLAN
arrangement, and redacted logs. Do not post credentials or factory dumps.
