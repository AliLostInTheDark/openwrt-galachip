# OpenWrt for the TP-Link XZ000-G7

OpenWrt with Linux 6.18 for the GalaChip GC1601-based XZ000-G7.
This development fork supports the **32 MiB RAM / 4 MiB SPI NOR / one
Gigabit Ethernet port** board. The 8 MiB v1.20 BR/TR variants are not
covered by this installation guide.

## Features

- Gigabit Ethernet and GPON hardware bridge offload, with approximately
  900/900 Mbit/s throughput.
- EPON registration, OAM/authentication and hardware bridge offload.
- Tagged, untagged and transparent VLAN operation.
- LuCI configuration and status pages for GPON, EPON and PON mode.
- Optical monitoring and driver diagnostics.
- RAM boot and installation using the stock U-Boot.

The device operates as an ONU bridge. Use a separate router for PPPoE/IPoE,
routing, NAT, DHCP and firewalling. OLT provisioning and authentication
requirements vary.

## Installation

Back up the unit, boot OpenWrt from RAM, then install with `sysupgrade`.
Replacing U-Boot or installing an SPL is not required.

### 1. Requirements

- A 3.3 V USB-UART adapter, Ethernet cable and the board's normal power supply.
- A PC with a serial terminal, TFTP server and SCP client.
- These two images from the same build:

  - `openwrt-galachip-gc1601-tplink_xz000-g7-initramfs-zImage`
  - `openwrt-galachip-gc1601-tplink_xz000-g7-squashfs-sysupgrade.bin`

Verify image checksums. Keep the fiber disconnected during installation.
Do not upload these images through the stock firmware's web interface.

### 2. Connect UART and Ethernet

Connect the UART with board power disconnected:

| G7 pad | USB-UART |
| --- | --- |
| GND | GND |
| TX | RX |
| RX | TX |
| VCC | Not connected |

Use the normal power supply. Set the terminal to **115200, 8N1, no flow
control**. Interrupt autoboot to reach the stock U-Boot `=>` prompt.

Connect the PC directly by Ethernet and set its address to
`192.168.1.10/24`. The G7 uses `192.168.1.1`; no DHCP server is included.
Place the initramfs image in the TFTP root as `g7-initramfs.zImage`.

### 3. Boot from RAM

Run these at the stock U-Boot prompt, **one line at a time**:

```text
setenv autostart no
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.10
mw.l 9080000c 0
setenv bootargs console=ttyS0,115200n8 earlycon
tftpboot 41000000 g7-initramfs.zImage
```

The `mw.l` command disables the watchdog for this loader session.
After a successful transfer, confirm the byte count matches the file, then:

```text
bootz 41000000
```

This does not write flash. Do not run `saveenv` or change `bootcmd`.

### 4. Back up the stock flash

In the RAM-booted OpenWrt shell, check:

```sh
cat /proc/mtd
```

The main regions must match this layout:

| Partition | Offset | Size |
| --- | --- | ---: |
| `u-boot` | `0x000000` | 128 KiB |
| `firmware` | `0x020000` | 3776 KiB |
| `misc` | `0x3d0000` | 128 KiB |
| `hwconfig` | `0x3f0000` | 64 KiB |

Child partitions such as `kernel`, `rootfs` and `rootfs_data` are normal.
Stop if the main layout differs. Never erase `u-boot`, `misc` or `hwconfig`;
they contain the boot chain and factory data.

Create backups while stock firmware is still in flash:

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

Expected sizes in bytes: `u-boot.bin` 131072, `firmware.bin` 3866624,
`misc.bin` 131072 and `hwconfig.bin` 65536. Do not continue after a read error.

Set a password on the G7 with `passwd`, then run on a Linux PC:

```sh
mkdir -p g7-stock-backup
scp -O 'root@192.168.1.1:/tmp/g7-stock-backup/*.bin' g7-stock-backup/
cd g7-stock-backup
wc -c *.bin
sha256sum u-boot.bin firmware.bin misc.bin hwconfig.bin > SHA256SUMS
cat u-boot.bin firmware.bin misc.bin hwconfig.bin > g7-stock-full.bin
wc -c g7-stock-full.bin
```

The full backup must be **4194304 bytes**. Keep copies off the device and
private; they contain calibration and device/ISP identifiers. Windows
clients should use SCP rather than SFTP.

### 5. Install to flash

Copy the matching sysupgrade image from the PC:

```sh
scp -O openwrt-galachip-gc1601-tplink_xz000-g7-squashfs-sysupgrade.bin \
    root@192.168.1.1:/tmp/g7-sysupgrade.bin
```

On the G7, validate it:

```sh
sysupgrade -T /tmp/g7-sysupgrade.bin
```

Only after validation succeeds and backups are stored on the PC:

```sh
sysupgrade -n /tmp/g7-sysupgrade.bin
```

This replaces the firmware region and reboots with default settings.
Do not interrupt power or use `-F`. After reboot, open
`http://192.168.1.1` and set a root password.

## PON configuration

Select **Network -> PON Mode** and reboot after changing technology.
Enter only your ISP-authorized identity and authentication details.

### GPON

Open **Network -> GPON -> Configuration -> General settings**.

- Set the GPON serial and password/LOID required by the ISP.
- Start with automatic OLT detection and no forced profile.
- Leave ONU type, UNI count and traffic-management overrides at their defaults
  unless required by the ISP.

### EPON

Open **Network -> EPON -> Configuration**.

- Set the authorized ONU MAC and LOID/password if required.
- Leave active OAM discovery disabled unless registration succeeds but OAM
  remains waiting for discovery.

### VLAN mode

| Mode | Behavior |
| --- | --- |
| Transparent | Passes tags through; configure the service VLAN on the router if required. |
| Tagged | Adds the service VLAN upstream and removes it downstream; the router sends untagged traffic. |
| OLT-controlled | Applies the VLAN configuration supplied by the OLT. |

GPON labels the last option **Control by OLT**; EPON uses **Set by the OLT**.
Keep the default Ethernet/PON bridge and avoid tagging the same service twice.

Connect the fiber and check the corresponding GPON or EPON status page.
Use a different management subnet if your router also uses `192.168.1.0/24`.

## Diagnostics

```sh
# GPON
omcictl status
cat /sys/class/net/pon0/device/gpon_state

# EPON
cat /var/run/epon-oamd.json
cat /sys/class/net/pon0/device/epon_state
cat /sys/class/net/pon0/device/epon_offload_status

# Ethernet and offload
cat /sys/class/net/eth0/speed
cat /sys/class/net/pon0/device/sfu_offload
cat /sys/bus/platform/devices/90e00000.packet-engine/sfu_snapshot
cat /proc/net/dev
```

GPON should reach `O5`. EPON needs MPCP registration and an operational OAM
link. An enabled offload setting alone does not confirm that traffic matches
a hardware flow.

## Updates

Back up your configuration, transfer the new sysupgrade image to `/tmp`,
and run:

```sh
sysupgrade -T /tmp/g7-sysupgrade.bin
```

If validation succeeds, update while retaining compatible settings:

```sh
sysupgrade /tmp/g7-sysupgrade.bin
```

Use `-n` for a clean configuration. Check operation after a power-off cold boot.
The 4 MiB flash leaves little room for additional packages.

## Recovery

With a working stock U-Boot, repeat the RAM-boot procedure and reinstall
OpenWrt. To return to stock, use **this unit's original raw `firmware.bin`
backup**, exactly **3866624 bytes**. A vendor update file is not equivalent.

Place the backup in the TFTP root as `g7-stock-firmware.bin`. Disconnect the
fiber and run at the stock U-Boot prompt:

```text
setenv autostart no
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.10
mw.l 9080000c 0
tftpboot 41000000 g7-stock-firmware.bin
printenv filesize
sf probe
```

Stop unless the transfer succeeded, `filesize` is `3b0000` and the flash
is **4 MiB**. The following commands erase and replace only the firmware region:

```text
sf erase 20000 3b0000
sf write 41000000 20000 3b0000
sf read 40c00000 20000 3b0000
cmp.b 41000000 40c00000 3b0000
```

Require `Total of 3866624 byte(s) were the same` before power-cycling.
If verification fails, keep power on and retain the serial session.
Bootloader or factory-data damage may require an external SPI programmer.

## Building

Kernel additions are included in [target/linux/galachip](target/linux/galachip).
PON packages are in [package/utils/xpon](package/utils/xpon).
No separate kernel checkout is required.

On an OpenWrt build host:

```sh
./scripts/feeds update -a
./scripts/feeds install -a
cp target/linux/galachip/tplink_xz000-g7.diffconfig .config
make defconfig
make -j"$(nproc)"
```

Save any existing `.config` before replacing it. Images are generated in
`bin/targets/galachip/gc1601/`. Do not publish private ISP settings or factory
backups.

## Credits and license

Maintained by Ifran Ali, based on OpenWrt, Linux, LuCI and the credited
xPON/OMCI work.

The `omci-agent` and `luci-app-omci` packages are based on the original Airoha
GPON OMCI packages by [@Sirherobrine23](https://github.com/Sirherobrine23)
(**Matheus Sampaio Queiroga**).

Original authorship and file-level license notices are retained.
OpenWrt is licensed under GPL-2.0; component licenses remain applicable.
