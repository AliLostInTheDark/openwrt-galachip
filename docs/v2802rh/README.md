# OpenWrt for the VSOL V2802RH

OpenWrt with Linux 6.18 for the VSOL V2802RH GPON ONU, built on the Realtek
RTL9601D (MIPS RLX, big-endian). The board has **4 MiB of SPI NOR (Zbit
ZB25VQ32), a GN25L95 optical module, one Gigabit port (LAN1) and one 2.5 Gigabit
port (LAN2)**.

The device is an ONU bridge. Use a separate router for PPPoE/IPoE, NAT, DHCP and
firewalling. OLT provisioning and authentication requirements vary by ISP.

**Before you start:** the only tested way back to a working unit is U-Boot over
serial. Returning to stock and CH341A recovery are written down here, but neither
has been run on this board yet.

## Status

| Area | State |
| --- | --- |
| GPON | Working: activation, ranging, AES, FEC, OMCI provisioning |
| Datapath | Hardware offload between the LAN ports and the PON, with p-bit to GEM mapping |
| OMCI | Userspace `omcid`; the OLT creates services, the firmware seeds none |
| Multicast | MEs and tables stored; the stream reaches the bridge doubled on the one line tested, and IGMP limits are not enforced |
| LuCI | GPON status, configuration, MIB views and the VLAN table |
| Tested OLTs | Nokia ALCL on Airtel (VLANs 100 and 660) and on JioFiber (VLAN 1015, five GEMs) |

### Measured

| Line | Setup | Download | Upload |
| --- | --- | ---: | ---: |
| Airtel, 300/300 plan | USB NIC, untagged on the ONU | 300 Mbit/s | 305 Mbit/s |
| Jio | Router on LAN2 at 2.5 Gbit/s, USB NIC, 6 Ookla runs | 976 Mbit/s | 932 Mbit/s |

The Jio result needs the router to shape its upload bursts (`tbf` at 1160 Mbit/s
with TSO on, in that setup). Unshaped 2.5 Gbit/s bursts overflow the ONU's LAN2
buffer.

### Not tested or not included

- Return to stock and CH341A read or write on this board.
- OLTs that attach ME 171 to VEIP `0x301`. The code learns the VEIP from ME 171,
  but this has not run on such a line.
- QinQ in hardware is off unless `hw_qinq` is set.
- OMCI software download, VoIP on the ONU, TR-069 and IGMP snooping are not
  implemented. Voice works by passing the VLAN to a phone or router.

## Stock firmware compared

| | Stock firmware | This firmware |
| --- | --- | --- |
| System | RTOS `JCO4032_R2.57`, closed | OpenWrt, Linux 6.18, all drivers and daemons in source |
| Management | Vendor web interface | LuCI, ubus, SSH |
| OMCI | Vendor | `omcid`, inspectable with `omcictl` and LuCI |
| Forwarding | Vendor switch rules, single service | Offload built from what the OLT provisions, tagged or untagged per port |
| Updates | Vendor images | `sysupgrade`, settings kept |
| Bootloader | Stock U-Boot | Unchanged, along with its environment, identity and calibration |

## Flash layout

| Partition | Offset | Size | Contents | Touched by install |
| --- | --- | ---: | --- | --- |
| `boot` | `0x000000` | 132 KiB | Preloader and stock U-Boot | No |
| `env`, `env2` | `0x021000`, `0x023000` | 8 KiB each | U-Boot environment | No |
| `config` | `0x025000` | 8 KiB | Serial, MACs | No |
| `firmware` | `0x027000` | 3684 KiB | Kernel, rootfs, overlay | **Yes** |
| `u-boot`, `u-boot-env` | `0x3c0000`, `0x3df000` | 124 KiB, 4 KiB | Reserved for a chainloaded U-Boot | No |
| `osk` | `0x3e0000` | 128 KiB | **Per-unit optics calibration**, OMCI files | No |

Linux keeps everything except `firmware` read-only. Never write one unit's backup
to another: the calibration and identity belong to that unit.

## Requirements

- A 3.3 V USB-UART adapter, Ethernet cable and the normal power supply.
- A PC with a serial terminal and a TFTP server.
- A CH341A programmer with a SOIC-8 clip, for the fastest backup and for recovery.
- These images from the same build, in `bin/targets/realtek-luna/taroko/`:
  - `openwrt-realtek-luna-taroko-vsol_v2802rh-squashfs-firmware.bin`, for the first install
  - `openwrt-realtek-luna-taroko-vsol_v2802rh-squashfs-sysupgrade.bin`, for updates
  - `...-squashfs-kernel.bin` and `...-squashfs-rootfs.bin`, for a RAM test

Serial runs at **115200, 8N1, no flow control**; do not connect the adapter's VCC.
Press a key during the 3-second countdown to reach the `9601D#` prompt.

## 1. Back up the whole flash

Do this before anything else, while stock is intact. Both methods produce the same
4194304-byte file. Keep it off the device and private: it holds the unit's serial,
MACs and calibration.

Do not take this backup from RAM-booted OpenWrt. If it finds a `rootfs_data`
partition its overlay setup could format it, and this has not been ruled out on an
untouched stock flash.

### Option A: CH341A (fastest)

A 4 MiB read typically takes about a minute, against 30 minutes over serial, and
needs no working firmware or bootloader. It is also the only recovery if U-Boot is
ever damaged.

**Wiring and safety**

- **Remove power and fibre.** Never clip onto a powered board.
- **Use 3.3 V.** Many black CH341A boards put about 5 V on the data lines even with
  the 3.3 V jumper set. Measure them, and use a 3.3 V modded board or adapter if so.
- Find the flash by reading its marking (`ZB25VQ32`), and line up clip pin 1 with
  the dot on the chip. A reversed clip can kill the chip.

| Pin | Signal | Pin | Signal |
| ---: | --- | ---: | --- |
| 1 | CS# | 8 | VCC |
| 2 | DO | 7 | HOLD# |
| 3 | WP# | 6 | CLK |
| 4 | GND | 5 | DI |

The programmer powers the board's 3.3 V rail through the clip, so the SoC can also
wake and hold the bus. If detection is unreliable, reseat the clip, or as a last
resort lift the chip's VCC pin or desolder it.

**Read it twice and compare:**

```sh
flashrom -p ch341a_spi
flashrom -p ch341a_spi -r v2802rh-a.bin
flashrom -p ch341a_spi -r v2802rh-b.bin
cmp v2802rh-a.bin v2802rh-b.bin && sha256sum v2802rh-a.bin
wc -c v2802rh-a.bin
```

Stop unless the files match and the size is exactly 4194304. Bad clip contact can
read back consistently wrong, so check that it isn't all `FF` or all `00`.

**If flashrom doesn't detect the chip:** flashrom 1.7 has no entry for the Zbit
ZB25VQ32 and can only use it through its generic SFDP detection, if the chip
answers that. Otherwise use a tool that accepts a manual chip choice, such as
NeoProgrammer or AsProgrammer, set to a 4 MiB 25-series part. The ID should read
`5E 40 16`. Don't force a different chip name for a write without first confirming
that a read matches the serial CRC below.

### Option B: serial from U-Boot (no programmer)

Read-only, about 30 minutes at 115200. Log the session to a file:

```bash
picocom -b 115200 --logfile v2802rh-dump.log /dev/ttyUSB0
```

Test a small read first. If it hangs, stop; don't try other addresses, because a
bad read can hang this chip.

```text
md.b b4000000 40
crc32 b4000000 400000
md.b b4000000 400000
```

Don't type during the dump. Note the CRC, then convert the log on the PC:

```bash
python3 -c "
import re,sys,zlib
out=bytearray()
for l in open(sys.argv[1],errors='ignore'):
    m=re.match(r'\s*([0-9a-f]{8}): ((?:[0-9a-f]{2} ){16})',l)
    if m and int(m[1],16)==0xb4000000+len(out): out+=bytes.fromhex(m[2])
assert len(out)==0x400000,len(out)
open(sys.argv[2],'wb').write(out); print('%08x'%zlib.crc32(out))
" v2802rh-dump.log v2802rh-full-backup.bin
```

It must print the same CRC as the board.

### Check any backup against the board

```text
crc32 b4000000 400000
```

```bash
python3 -c "import sys,zlib;print('%08x'%zlib.crc32(open(sys.argv[1],'rb').read()))" v2802rh-full-backup.bin
```

## 2. Optional: test from RAM

This writes nothing to flash, and a power cycle returns to what is installed. Use it
to check a new line before you commit.

Copy the kernel and rootfs into the TFTP root as `v2802rh-kernel.bin` and
`v2802rh-rootfs.bin`, connect the PC to LAN1 and run:

```text
printenv ipaddr serverip
tftp 81000000 v2802rh-kernel.bin
tftp 81c00000 v2802rh-rootfs.bin
setenv bootargs 'memmap=0x200000$0x1c00000 phram.phram=rootfs,0x1c00000,0x200000 root=/dev/mtdblock0 rootfstype=squashfs'
bootm 81000000
```

If needed, set the two addresses with `setenv ipaddr ...` and `setenv serverip ...`
for this session. Don't run `saveenv`. A RAM-booted system refuses `sysupgrade`.

## 3. Install to flash

Copy the firmware image into the TFTP root as `v2802rh-firmware.bin`, work out its
size and CRC on the PC, and compare both on the board:

```bash
python3 -c "import sys,zlib;d=open(sys.argv[1],'rb').read();print('%x %08x'%(len(d),zlib.crc32(d)))" /srv/tftp/v2802rh-firmware.bin
```

```text
tftp 81000000 v2802rh-firmware.bin
crc32 81000000 ${filesize}
```

Stop unless both match. Replace `<size>` with the size in hex:

```text
sf probe 0
sf erase 27000 399000
sf write 81000000 27000 <size>
crc32 b4027000 <size>
```

The last CRC must match too. Only then run `reset`. If it differs, don't reset: run
the erase and write again from the same RAM copy.

The stock `bootcmd=bootm 9fc27000` boots the new image unchanged. Don't run
`saveenv`. The first boot formats the overlay, which can take a minute or two. Keep
serial connected until it finishes.

Do not upload these images through the stock web interface.

## 4. Configuration

The default address is `http://192.168.1.1`, user `root` with no password. Set a
password first. If your router also uses `192.168.1.0/24`, change the ONU's address.
Keep an address in the ONU's subnet on your PC or router so you can reach it.

Settings are under **Network -> GPON -> Configuration**. Each page marks what a
change costs: applied at once, applied at the next registration, or a service drop.

### Identity

Enter only what your ISP has authorized. Under **General settings -> ONU identity**:

| Setting | What it does | When to set it |
| --- | --- | --- |
| GPON serial number | What the OLT registers the ONU by | Always, if the ISP registered a specific serial |
| SLID / GPON password | PLOAM password | If the ISP uses one |
| LOID, LOID password | Logical ID in ONU-G | If the ISP authenticates by LOID |
| Vendor ID, equipment ID, hardware and software versions | Identity reported over OMCI | When the OLT checks these against the ONT being replaced |

Local files in `files/` are built into an image, so an image built with a
`files/etc/config/omci` carries that serial. Don't share such an image.

### VLANs: read this first

The OLT decides which VLANs exist on your line. These settings only decide how each
one reaches your equipment. **Work out the VLAN IDs first:** ask your ISP, read
**LAN VLANs -> VLAN scan**, or run `omcictl mib show 171`.

The labels describe who does the tagging, and they're easy to misread:

| LuCI label | On the ONU | Your equipment on that port |
| --- | --- | --- |
| **Transparent (router tags)** | Passes the tag unchanged | Sends and receives **tagged** frames |
| **Tagged (router untagged)** | Adds the tag towards the OLT, removes it towards you | Sends and receives **plain** frames |

Only one side may tag. If both do, the frame carries two tags and won't match the service
you meant it for.

Choose under **Configuration -> LAN VLANs -> VLAN control -> Control mode**. Only the
selected mode applies.

| | Simple: one service VLAN | Advanced: VLAN table |
| --- | --- | --- |
| Use it for | One service, such as internet only | Several services, or ports that differ |
| Management | Reached untagged on the LAN ports | Set up by you, see below |
| Risk | Low | You can lock yourself out, so keep serial handy |

### Simple mode

| Mode | ONU | Router or PC |
| --- | --- | --- |
| **Transparent** (default) | Nothing to set | WAN on a VLAN device, for example `wan.<vid>` |
| **Tagged** | Set the VLAN ID, and the priority if the ISP asks | Plain WAN, no VLAN |
| **Control by OLT** | Nothing to set | Plain WAN if the OLT gives untagged traffic a VLAN; otherwise like Transparent |

- **Control by OLT** follows ME 171. If the OLT only provisions tagged traffic, the ONU
  stays transparent, and the router must tag. If the OLT translates VLANs, this mode
  reports an error and leaves the network settings alone: use Tagged or Transparent.
- **Mixing ports:** with Tagged or Control by OLT, set **LAN1 mode** or **LAN2 mode** to
  Transparent for a port whose device tags for itself. It needs exactly one untagged
  service VLAN, and other VLANs pass unchanged.
- **VLAN scan** probes for DHCP and PPPoE replies and offers **Use tagged** or **Use
  transparent** on each result. It can't detect a service that uses a static address or
  IPv6 only, so no reply doesn't rule a VLAN out.

### Advanced mode

Each row of the **VLAN table** is one VLAN. Before you switch, turn on **Untagged
management access** for the port you are using, or you may lose the ONU.

| Column | Choice | Meaning |
| --- | --- | --- |
| PON | Carried, or Not carried | Whether the VLAN goes to the OLT. Service VLANs must be Carried |
| LAN1, LAN2 | Not a member | The port doesn't get this VLAN |
| | Transparent (router tags) | Tagged on the cable. A port can carry many like this |
| | Tagged (router untagged) | Plain on the cable. **One VLAN per port** |
| ONU (CPU) | Ticked | The ONU itself is on this VLAN, used for management |

**ONU management**, in the same page:

| Setting | What it does |
| --- | --- |
| Management device | The VLAN the ONU's address lives on, such as `br-lan.1`. Its row needs ONU (CPU) ticked |
| Untagged management access | Those ports reach the ONU's address with plain frames, even where the port is Tagged on the internet VLAN. Service traffic isn't affected |

Other per-port settings:

- **LAN upstream priority** replaces the 802.1p priority sent to the OLT, or preserves
  it. Downstream priority is kept. Set it only if a service needs a fixed priority.
- Some VLAN IDs are reserved by the device and are refused.
- Keep multicast snooping off on `br-lan`, or the offload stays inactive.

### Setting up what's behind the ONU

**Router:**

| ONU port | Router WAN |
| --- | --- |
| Transparent | VLAN device on WAN (`wan.<vid>`), with PPPoE, DHCP or static on it. One device per VLAN |
| Tagged | Plain `wan`, with PPPoE, DHCP or static on it |

Put extra services such as voice on their own interface with no default route, in
their own firewall zone, with static routes to the ISP's servers if needed. If the
router shapes traffic on the port, one queue covers all VLANs on it.

**Reaching the ONU from the router:**

| Management on the ONU port | Router address goes on |
| --- | --- |
| Plain frames, simple mode or untagged access | Plain `wan` |
| VLAN 1 Transparent | `wan.1` |

Get this wrong and ARP fails. Anything the ONU starts, such as NTP, then stops working,
even while SSH from another port works.

**Managed switch between the ONU and your devices:**

| Switch port | Setting |
| --- | --- |
| To a Transparent ONU port | Tagged member of each VLAN (trunk) |
| To a Tagged ONU port | Untagged member of one local VLAN, with PVID the same |
| To a router or PC that tags | Tagged member of its VLANs |
| To a device that doesn't tag | Untagged member of one VLAN, with PVID the same |
| Management, ONU untagged | Untagged on the ONU port in a separate local VLAN, kept apart from the service VLANs |

**IP phone or other single-service device:** either the switch port is untagged with
that VLAN as PVID and the phone stays untagged, or the port is tagged and the phone
tags. Don't do both.

**PC:** plain NIC on a Tagged port. For a Transparent port, create a VLAN interface
on the NIC.

### Other GPON settings

**General settings -> Agent.** Defaults suit almost every line.

| Setting | What it does | Change it when |
| --- | --- | --- |
| Enable OMCI agent | Runs OMCI | Leave on, unless you're debugging |
| Permissive mode | Accepts OLT behaviour outside G.988 where a workaround exists | Try off only if a workaround misbehaves |
| Fake unsupported OMCI responses | Answers success for unsupported requests | An OLT stalls on an unsupported ME |
| Allow OLT-requested reboot | Lets the OLT reboot the ONU | The ISP needs remote reboots |
| Transmitter | Off keeps the laser off from the next boot | Listening without ranging |
| OLT profile | Vendor quirks; Auto detect is the default | Auto picks the wrong vendor |
| Force OLT profile | Overrides detection | As above, when the detected value is known wrong |

**Advertised capabilities.** These change what the OLT is told and leave the hardware as
it is. Set them only to match an ONT you're replacing or an ISP's instruction.

| Setting | What it does |
| --- | --- |
| OMCC version | OMCI version in ONU2-G. Some OLTs expect that of the old ONT |
| Traffic management option | Priority, rate, or both. Doesn't enable shaping |
| Report queue status for DBA | On sends DBRu reports. Turn off only if upstream allocation misbehaves |
| ONU type | SFU, HGU and so on |
| UNI count | UNIs advertised. It doesn't add ports |
| VEIP slot | Learned from the OLT by default. Fix it only if the OLT rejects the uploaded slot |

**VLAN rewrite.** Takes precedence over the OLT's rules, and the two work independently
of the modes above. Both need your router to tag.

| Setting | Format | What the router uses |
| --- | --- | --- |
| VLAN translation | `network VLAN:LAN VLAN` | The LAN VLAN. The ONU swaps it both ways |
| VLAN stacking (QinQ) | `S-VLAN:C-VLAN` | The C-VLAN. The ONU adds and removes the S-VLAN. Hardware offload is off unless `hw_qinq` is set |

**Module parameters.** `lan_loop_guard` sets what happens when the switch sees a loop: 0
off, 1 log, 2 cut the port (default).

### Clock

The ONU has no default route, so its NTP server must be on its own subnet, usually
the router, with UDP 123 allowed from the ONU in the router's firewall. Set the server
under **System -> System -> Time Synchronization**.

### Connect the fibre

```sh
cat /proc/gpon_health
cat /proc/gpon_prov
cat /sys/class/net/pon0/device/l2_offload
```

You want `onu_state: 5` and `requested=1 active=1 error=0`.

## Updates

```sh
scp -O openwrt-realtek-luna-taroko-vsol_v2802rh-squashfs-sysupgrade.bin root@192.168.100.1:/tmp/v2802rh-sysupgrade.bin
```

On the ONU:

```sh
sysupgrade -T /tmp/v2802rh-sysupgrade.bin
sysupgrade /tmp/v2802rh-sysupgrade.bin
```

Use `-n` for clean settings, and don't use `-F`. The kernel and OMCI packages share
ABI 17 and must come from the same build, which an image always does.

## Back to stock

**This procedure is untested on this board.** Boot, environment, config and
calibration were never changed, so only the firmware area needs restoring.

### From U-Boot

Cut the firmware area out of **this unit's** backup, and get its CRC:

```bash
dd if=v2802rh-full-backup.bin of=/srv/tftp/v2802rh-stock-fw.bin bs=4k skip=39 count=921
python3 -c "import sys,zlib;d=open(sys.argv[1],'rb').read();print('%x %08x'%(len(d),zlib.crc32(d)))" /srv/tftp/v2802rh-stock-fw.bin
```

The size must be `399000`. Disconnect the fibre, then at the `9601D#` prompt:

```text
tftp 81000000 v2802rh-stock-fw.bin
crc32 81000000 ${filesize}
```

Stop unless the size and CRC match. Then:

```text
sf probe 0
sf erase 27000 399000
sf write 81000000 27000 399000
crc32 b4027000 399000
```

The last CRC must match before you `reset`. If it doesn't, keep the power on and
write again.

Once the unit is back on stock, `crc32 b4000000 400000` should match the backup
exactly unless the stock firmware or anything else has since changed flash.

### With a CH341A

Use this if U-Boot won't start, or if you installed a chainloaded U-Boot at
`0x3c0000`. Power off, clip on, then write the full backup and let flashrom verify:

```sh
flashrom -p ch341a_spi -w v2802rh-full-backup.bin
```

## Recovery

| Symptom | What to do |
| --- | --- |
| OpenWrt won't boot, U-Boot prompt works | Repeat step 3 or go back to stock from U-Boot |
| No network but serial works | Run `firstboot -y && reboot` in the console, or reflash from U-Boot |
| U-Boot prompt doesn't appear | Check the serial wiring, then write the full backup with a CH341A |
| Stuck deactivating or no OMCI | Send `xpon-report`, and check `/proc/gpon_log` |

If a flash CRC fails, keep the power on and the serial session open. The RAM copy is
still good to write again.

## Diagnostics

```sh
xpon-report            # one masked file for bug reports
omcictl status
omcictl mib show 171
cat /proc/gpon_health
cat /proc/gpon_log
cat /sys/class/net/pon0/device/l2_offload
logread | grep 'OLT after'
```

`requested=1` alone doesn't mean the offload is active. `active=1 error=0` means
the tables are programmed. Port counters and CPU load during traffic prove the
hardware carried it. Reports mask serials and passwords, but check them before
sharing. Error codes are in [DIAG-CODES.md](DIAG-CODES.md).

## Building

On an OpenWrt build host:

```sh
./scripts/feeds update -a
./scripts/feeds install -a
cp docs/v2802rh/vsol-v2802rh.diffconfig .config
make defconfig
make -j"$(nproc)"
```

Save any existing `.config` first. Don't publish factory backups, logs with serials
or images built with private settings in `files/`.

## More documents

| Topic | Document |
| --- | --- |
| Bring-up history and line results | [PON-BRINGUP-LOG.md](PON-BRINGUP-LOG.md) |
| OMCI provider and limits | [OMCID-RTL9601D.md](OMCID-RTL9601D.md) |
| LAN/PON offload and VLANs | [L2-OFFLOAD.md](L2-OFFLOAD.md) |
| Stock behaviour and open items | [STOCK-PORT-AUDIT.md](STOCK-PORT-AUDIT.md), [STOCK-GPON-REFERENCE.md](STOCK-GPON-REFERENCE.md) |
| Optics | [GN25L95-IMPLEMENTATION.md](GN25L95-IMPLEMENTATION.md) |
| Bootloader and chainload U-Boot experiments | [BOOTLOADER-AUDIT.md](BOOTLOADER-AUDIT.md), [UBOOT-RECOVERY-TESTS.md](UBOOT-RECOVERY-TESTS.md) |

## Credits and license

Based on OpenWrt, Linux and LuCI.

The `realtek-luna` target and the shared GPON core come from the port by **Herman
Brule** ([@alphaonex86](https://github.com/alphaonex86),
[alphaonex86/openwrt](https://github.com/alphaonex86/openwrt)). This fork narrowed it
to the V2802RH and added the board's support, its LAN and PON offload and `omcid`
integration.

The `omci-agent` and `luci-app-omci` packages are based on the original Airoha GPON
OMCI packages by [@Sirherobrine23](https://github.com/Sirherobrine23) (**Matheus
Sampaio Queiroga**).

The optical power gauges, activation stages, apply-cost badges, line status in the
header and diagnostic report download on the GPON pages follow ideas from
[odi-ui](https://github.com/AndreZiviani/odi-ui) by
[@AndreZiviani](https://github.com/AndreZiviani) (**Andre Ziviani**). No code was
copied.

Original authorship and file-level license notices are retained. OpenWrt is licensed
under GPL-2.0; component licenses remain applicable.
