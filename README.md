# OpenWrt for the TP-Link XZ000-G7 GPON/EPON ONU (GalaChip GC1601)

Open-source OpenWrt firmware with Linux 6.18 for the **TP-Link XZ000-G7**
GPON/EPON SFU ONU, built on the GalaChip GC1601 (GC1601A) SoC (Faraday
FA626TE, ARMv5TE). It replaces the vendor firmware and adds GPON and EPON
with a userspace OMCI daemon, hardware switch offload between the Ethernet
port and the PON at line rate, and a LuCI interface with a PON status page.
It has been tested on GPON with Nokia ISAM OLTs (Airtel Fiber, JioFiber)
and Huawei and ZTE OLTs (Airtel Fiber).

The device is an ONU bridge. Use a separate router for PPPoE/IPoE, NAT, DHCP
and firewalling. OLT provisioning and authentication requirements vary by ISP.

## Supported hardware

| | TP-Link XZ000-G7 |
| --- | --- |
| SoC | GalaChip GC1601 (GC1601A), Faraday FA626TE, ARMv5TE |
| Hardware revision | v1.0 (US1, EU1, IN, BR) |
| Memory | 32 MiB RAM, 4 MiB SPI NOR |
| Network | One Gigabit Ethernet port, GPON and EPON optical interface |
| Not covered | The 8 MiB v1.20 BR/TR variants |

## Contents

- [Status](#status) and [Features](#features)
- [Stock firmware compared](#stock-firmware-compared) and [Measured performance](#measured-performance)
- [Installation](#installation): UART, the OpenWrt U-Boot from RAM, flash backup, U-Boot install, OpenWrt install
- [PON configuration](#pon-configuration): GPON, EPON and VLAN modes
- [Diagnostics](#diagnostics-1), [Updates](#updates), [U-Boot](#u-boot) and [Recovery](#recovery)
- [Building](#building)

## Status

| Area | State |
| --- | --- |
| GPON | Working: activation, ranging, encryption, OMCI provisioning, recovery after fibre outages |
| EPON | Working: MPCP registration, OAM with CTC authentication and VLAN modes |
| Datapath | Hardware switch offload between the LAN port and the PON at line rate |
| Drivers | Vendor-free: the packet engine is a DSA switch (`lan1`, `pon0`), the PON is controlled over generic netlink, upstream-style logging |
| OMCI | Userspace responder (`omcid`), which owns the ONU identity and configuration; the kernel engine has been removed |
| LuCI | Status and configuration for GPON, EPON and the PON mode, using the stock Bootstrap theme |
| U-Boot | U-Boot 2026.10 (TPL, SPL and proper), installed from the stock loader, self-updating, recoverable over serial without a programmer |
| Tested OLTs | GPON: Nokia ISAM (Airtel Fiber, JioFiber); Huawei and ZTE (Airtel Fiber) |

## Features

- GPON and EPON, switchable from LuCI.
- Gigabit Ethernet with hardware bridge offload between the LAN port and the
  PON. Subscriber traffic does not pass through the CPU.
- Tagged, untagged and transparent VLAN operation, or VLANs set by the OLT.
- OMCI in userspace (`omcid`): MIB, transactions, alarms, performance data and
  OLT interoperability profiles, restartable without losing the OLT session.
- Automatic recovery after fibre outages: the service mapping is restored as
  soon as the ONU ranges again.
- ONU identity cloning (serial, vendor ID, versions, equipment ID, SLID/LOID)
  with the field limits the OMCI and CTC formats allow.
- LuCI with the stock Bootstrap theme, showing the PON as a fiber optic port
  with its optical line rate, served by uhttpd's ucode plugin, which
  compiles LuCI once instead of on every request.
- GPON and EPON status pages with receive and transmit power against the
  optical class window, the activation stages in words, what the OLT
  provisioned, and the line state in the header of every page. Settings show
  whether saving applies at once, at the next registration or interrupts
  service. A diagnostic report (identifiers masked) downloads from the status
  page.
- `luci-app-xpon-stats` for the router behind the ONU: the same GPON or EPON
  status in that router's LuCI (any theme), or `xpon-stats` in its shell, read
  from the ONU's read-only status API.
- SSH diagnostics: `omcictl`, `xponctl`, debugfs counters, an on-demand OMCI
  trace and a one-line log of what the OLT did after each reconnect.
- An OpenWrt-built U-Boot 2026.10 (TPL, SPL and proper), installed from the
  stock one without a programmer: it backs up the whole flash, updates itself
  as one image, flashes and restores firmware with one command each, shows
  write progress, and recovers over the serial console even with a broken
  SPL.

## How it works

![Architecture: LuCI and the daemons in userspace, the xPON core and GC1601 drivers in the kernel, and the switch forwarding between the LAN port and the PON](target/linux/galachip/docs/architecture.svg)

Subscriber traffic is forwarded by the switch hardware; `omcid` only
configures it. The kernel keeps what needs hardware access or tight timing
(activation, PLOAM, encryption keys, optical safety, DMA) and checks every
request `omcid` makes.

The packet engine is a DSA switch: `eth0` is the conduit, `lan1` the
Ethernet port and `pon0` the PON port, whose link is up while the ONU is
registered with the OLT. Userspace drives the PON through the `xpon` generic
netlink family (transmitter, EPON control, LAN VLAN rewrite, switch offload,
optical module readings) and `omci`'s typed backend operations; `xponctl`
is its command-line client. Register dumps and counters are in debugfs
(`/sys/kernel/debug/gc1601-pon`, `/sys/kernel/debug/gc1601-eth`).

## Stock firmware compared

The stock firmware is TP-Link's December 2024 release for this board.

### Platform

| | Stock firmware | This firmware |
| --- | --- | --- |
| Base system | Linux 3.16, uClibc, TP-Link web interface | OpenWrt, Linux 6.18, musl, LuCI |
| Source | GPL kernel parts; PON and switch drivers (`xpon_sdk.ko`, `gcnet.ko`) and the OMCI daemon (`gc_omcid`) are binary only | All drivers and daemons are source |
| Management | Web interface, TR-069 | LuCI, ubus, SSH |
| Image | Vendor firmware format | OpenWrt sysupgrade (about 3.2 MiB of the 3.4 MiB image limit) |
| Updates | Vendor images | `sysupgrade`, settings kept |

### PON stack

| | Stock firmware | This firmware |
| --- | --- | --- |
| GPON activation and PLOAM | Inside `xpon_sdk.ko` | `gc1601-gpon` driver, checked against G.984.3 (2014) with amendments 1 and 2 |
| OMCI | `gc_omcid`, closed | `omcid`, checked against G.988 (2022) with amendments 1 and 2 |
| OLT interoperability | Huawei, CTC and Boda private entities | Standard entities plus OLT profiles: auto-detect, Nokia/ALCL, Huawei, ZTE, FiberHome, Dasan, Genexis |
| EPON | `gcoamd` | `epon-oamd` (IEEE 802.3 OAM, CTC authentication and VLAN modes, VSOL inter-authentication) |
| Outage recovery | Vendor state machine | Retained MIB replayed on re-range; tested with short and long fibre outages |
| Upstream FEC | Applied by the MAC when the OLT asks; no status | Same; the state is not reported by the hardware |

### OMCI MIB as seen by the OLT

ONU-created entities, compared with the stock firmware's defaults.

| Entity | Stock firmware | This firmware |
| --- | --- | --- |
| ONU2-G total GEM ports | 256 | 256 |
| ONU2-G priority queues | 144 | 160 (8 upstream per T-CONT, 8 downstream per UNI and VEIP) |
| T-CONTs / traffic schedulers | 15 / 15 | 15 / 15 |
| T-CONT deprecated attribute | 1 | 1 |
| ANI-G status reporting (DBA) | On | On (switch in LuCI) |
| Downstream priority queues | UNI and VEIP | UNI and VEIP |
| PPTP Ethernet UNIs | 1 | 1, the board's port (`omci-uni-ports`); 1-16 configurable |
| ONU-G traffic management option | 2 | 0 by default, configurable |
| Private entities | Huawei, CTC and Boda | Nokia/ALCL, Huawei, ZTE, FiberHome, CTC and Genexis entities |

### Datapath

| | Stock firmware | This firmware |
| --- | --- | --- |
| Forwarding | `gcnet.ko` switch flows | Open SFU offload driver for the same switch |
| Upstream queues | Queue 0 of each T-CONT | One per P-bit (queue 0 as an option) |
| Packet buffers | 1440, with queue limits of 1080/720 | U-Boot's 512, with queue limits sized to them: a T-CONT queue holds at most 256 in and 256 out, so one fast upload cannot drain the pool |
| CPU path | Vendor bridge | `pon0` bridge, used only for traffic without a hardware flow; LAN broadcasts reach the PON once, through it |
| OLT frame size, UNI rate limits, queue weights | Applied | Not applied: their register effects are unverified, so the driver has no such hooks |

The T-CONT queue limits can be tuned live in
`/sys/kernel/debug/gc1601-eth/tcont_depth` and `tcont_out`; a setting that
would exceed the buffer pool is refused.

### Diagnostics

| | Stock firmware | This firmware |
| --- | --- | --- |
| Tools | `gc_omcicli`, vendor CLI | `omcictl`, `xponctl`, `omci-monitor`, `xpon-report`, debugfs counters |
| OMCI capture | Vendor debug | On-demand trace (`SIGUSR1` to `omcid`) |
| Reconnect log | - | One line per reconnect saying what the OLT did |

### Not included

OMCI software download, VoIP, IGMP snooping and TR-069 are not implemented.

## Measured performance

GPON on Airtel Fiber (Nokia ISAM OLT), 300/300 Mbit/s plan, PC directly on the LAN port,
Ookla on one server:

| Test | Download | Upload |
| --- | ---: | ---: |
| Single connection, CUBIC | 202-285 Mbit/s | 157-252 Mbit/s |
| Single connection, BBR | 249-261 Mbit/s | 293 Mbit/s |
| Multiple connections | about 300 Mbit/s | about 300 Mbit/s |

During single-connection tests the ONU's T-CONT queue stayed empty, its
bandwidth reports were never stuck, and no frame was dropped in either
direction: the gap is packet loss on the ISP's path, which CUBIC backs off
from and BBR does not. A single TCP upload also needs more buffering than the
stock queue limits give: with them it backs off at about 70 Mbit/s, or drains
the packet pool and hangs the engine.

GPON on JioFiber (Nokia ISAM OLT), 300/300 Mbit/s plan, PC directly on the LAN port:

| Test | Download | Upload |
| --- | ---: | ---: |
| Ookla, nearest server (3 runs) | 301-306 Mbit/s | 297-310 Mbit/s |
| 2.6 GB sustained download, 1 stream / 4 streams | 193 / 298 Mbit/s | - |
| 2.5 GB sustained upload, 1 stream / 4 streams | - | 215 / 210 Mbit/s |
| Earlier gigabit plan, same OLT | about 900 Mbit/s | about 920 Mbit/s |

In both directions the switch forwarded everything: over more than 5 GB the
CPU handled a few dozen packets, its load stayed below 0.4, and the ONU
counted no uncorrectable FEC codewords or BIP errors. Single-stream results
depend on the server and route: the download test used a speed-test server
about 12 ms away, the upload test Cloudflare's nearest site.

## Standards

- ITU-T G.984.3 (2014) with amendments 1 (2020) and 2 (2025): GPON
  transmission convergence, PLOAM and activation.
- ITU-T G.988 (2022) with amendments 1 (2024) and 2 (2025): OMCI.
- ITU-T G.984.2 (2019): GPON physical layer, within what this board's optics
  support.
- IEEE 802.3: Ethernet OAM and EPON (MPCP in hardware).
- ITU-T G.Sup46 / BBF TP-247: interoperability and conformance test plans used
  as checklists. This firmware is not certified.

The hardware knowledge (register layouts, initialisation order, optics
calibration) comes from the stock firmware; the protocol behaviour comes from
the standards.

## Installation

Everything runs from the serial console: start the OpenWrt U-Boot from RAM
through the stock boot loader, back up the whole flash with it, install it,
then flash OpenWrt from it. The OpenWrt U-Boot is required: it protects the
factory data while flashing, and it is how firmware is updated and restored
later.

### 1. Requirements

- A 3.3 V USB-UART adapter, Ethernet cable and the board's normal power supply.
- A PC with a serial terminal and a TFTP server that also accepts uploads
  (for example `tftpd-hpa` started with `--create`), for the flash backup.
- These three files from the same build:

  - `openwrt-galachip-gc1601-tplink_xz000-g7-u-boot.bin`
  - `openwrt-galachip-gc1601-tplink_xz000-g7-u-boot-with-spl.bin`
  - `openwrt-galachip-gc1601-tplink_xz000-g7-squashfs-sysupgrade.bin`

Verify their checksums and place them in the TFTP root under these names.
Keep the fiber disconnected during installation. Do not upload these images
through the stock firmware's web interface.

### 2. Connect UART and Ethernet

Connect the UART with board power disconnected:

| G7 pad | USB-UART |
| --- | --- |
| GND | GND |
| TX | RX |
| RX | TX |
| VCC | Not connected |

Use the normal power supply. Set the terminal to **115200, 8N1, no flow
control**. Connect the PC directly by Ethernet and set its address to
`192.168.1.10/24`; the G7 uses `192.168.1.1`.

### 3. Start the OpenWrt U-Boot from RAM

Power on and interrupt autoboot to reach the stock U-Boot prompt. Run, **one
line at a time**:

```text
setenv autostart no
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.10
mw.l 9080000c 0
tftpboot 41000000 openwrt-galachip-gc1601-tplink_xz000-g7-u-boot.bin
go 41000000
```

The `mw.l` command stops the stock watchdog. The OpenWrt U-Boot banner
(`U-Boot 2026.10-OpenWrt-…`) and its `=>` prompt follow. Nothing has been
written to flash yet. Do not run `saveenv`.

### 4. Back up the whole flash

At the OpenWrt U-Boot prompt:

```text
run backup_flash
```

It reads the whole 4 MiB flash and uploads it to the TFTP server as
`xz000-g7-flash.bin`. Rename it on the PC, run `backup_flash` a second time
and compare the two: they must be identical and exactly **4194304 bytes**.

```sh
sha256sum xz000-g7-flash-1.bin xz000-g7-flash.bin
wc -c xz000-g7-flash.bin
```

Keep it as the stock backup, off the device and private: it contains this
unit's calibration and device and ISP identifiers. Cut the stock firmware
partition out of it now, for a later return to stock:

```sh
dd if=xz000-g7-flash.bin of=g7-stock-firmware.bin bs=64k skip=2 count=59
```

`g7-stock-firmware.bin` must be **3866624 bytes**. The flash layout:

| Partition | Offset | Size |
| --- | --- | ---: |
| `u-boot` | `0x000000` | 128 KiB |
| `firmware` | `0x020000` | 3776 KiB |
| `misc` | `0x3d0000` | 128 KiB |
| `hwconfig` | `0x3f0000` | 64 KiB |

Stop here if the backup fails or the two copies differ.

### 5. Install the OpenWrt U-Boot

Still at the prompt of the U-Boot started from RAM:

```text
run upgrade_uboot
```

It checks the TPL the way the chip's mask ROM does and the SPL and U-Boot
proper by their CRCs before writing, rewrites only the blocks that changed,
and reads them back to compare. Do not power off while it writes. Then:

```text
reset
```

Press a key within 3 s to stop autoboot at the new U-Boot's `=>` prompt;
left alone, it boots the stock firmware still in flash. Details in
[INSTALLATION.md](package/boot/uboot-galachip/INSTALLATION.md).

### 6. Install OpenWrt

At the installed U-Boot's prompt:

```text
run upgrade_tftp
boot
```

`upgrade_tftp` loads the sysupgrade image, checks its tag and size, writes
only the firmware partition and never touches `misc` or `hwconfig`. OpenWrt
starts with default settings; open `http://192.168.1.1` and set a root
password.

To try a build before flashing it, put
`openwrt-galachip-gc1601-tplink_xz000-g7-initramfs-zImage` in the TFTP root
and run `run boot_tftp`: it boots from RAM and writes nothing.

## PON configuration

Select **Network -> PON Mode** and reboot after changing technology.
Enter only your ISP-authorized identity and authentication details.

### GPON

Open **Network -> GPON -> Configuration -> General settings**.

- Set the GPON serial and password/LOID required by the ISP. Each field
  shows its length limit.
- Start with automatic OLT detection and no forced profile.
- Leave ONU type, UNI count, OMCC version and traffic-management overrides
  at their defaults unless required by the ISP. Each choice in these lists
  says what it means.
- **Profiles** fills the identity of a known ISP ONT in one step.
- **Report queue status for DBA** is on by default, as on the stock firmware.
  Turn it off only if the OLT's upstream bandwidth allocation misbehaves.

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
# The PON: transmitter, EPON state, LAN VLANs, offload, optical readings
xponctl get

# GPON
omcictl status
cat /sys/kernel/debug/gc1601-pon/gpon_state

# EPON
cat /var/run/epon-oamd.json
cat /sys/kernel/debug/gc1601-pon/epon_state
cat /sys/kernel/debug/gc1601-pon/epon_offload_status

# Ethernet and offload
ethtool lan1
ethtool -S pon0
cat /sys/kernel/debug/gc1601-eth/sfu
dmesg | grep "SFU offload"
```

GPON should reach `O5`. EPON needs MPCP registration and an operational OAM
link. The kernel log lists each flow the switch forwards, with its GEM port
and T-CONT, whenever the set changes; an enabled offload setting alone does
not confirm that traffic matches a hardware flow.

After each reconnect, `omcid` logs one line saying what the OLT did, for
example `omcid: OLT after 33 s dark: MIB reset and full reprovision; …`:

```sh
logread | grep "OLT after"
```

To capture OMCI, start and stop a trace with the same command, then read it.
The trace contains raw OMCI including identity data; keep it private.

```sh
kill -USR1 $(pidof omcid)
omci-monitor -r /tmp/omcid-trace.txt
```

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
From the U-Boot prompt, `run upgrade_tftp` does the same with default
settings.
The 4 MiB flash leaves little room for additional packages.

## U-Boot

The build also publishes an upstream U-Boot 2026.10 for the G7, under
upstream U-Boot's file names, documented in
[package/boot/uboot-galachip](package/boot/uboot-galachip/README.md). It is
written for the GalaChip GC1601 SoC: the G7 adds only its DDR settings, its
device tree and its file names, so another GC1601 ONU or SFP stick needs only
those. The SPL unpacks U-Boot in 0.12 s and U-Boot loads the kernel in
0.1 s, with the caches on and the flash read over two lanes at 100 MHz;
autoboot waits 3 s for a key.

What it does differently from the stock loader, all tested on the board:

- **Upstream boot chain.** The mask ROM starts U-Boot TPL in SRAM, which
  sets up DDR and loads U-Boot SPL, which unpacks U-Boot proper. Each stage
  checks the next one's CRC and takes it over YMODEM on the serial console
  when the flash copy is bad, so a broken loader recovers without a
  programmer; a broken TPL recovers through the mask ROM's UART menu.
- **Same device tree as Linux.** The SoC and board `.dts` files are shared
  with the kernel; U-Boot reads the flash layout, the factory MAC and the
  protected ranges from them instead of hardcoding addresses.
- **Faster boot.** Dual-output flash reads at 100 MHz and the caches on: the
  whole 4 MiB flash reads in 1.05 s (1.64 s before), and U-Boot proper
  unpacks in 0.12 s instead of 2.5 s without the D-cache.
- **Upstream drivers and conventions.** FTWDT010 watchdog with reset through
  it, FTTMR010 timer, upstream ARM926 CPU code, the default `=> ` prompt, and
  `boot` for the default boot.
- **Protected factory data.** The flash driver refuses to program or erase
  `misc` and `hwconfig` and refuses a chip erase. The flashing scripts check
  the image tag and size first, rewrite only the 4 KiB blocks that changed,
  and read back to compare.
- **Self-update and stock restore.** `run upgrade_uboot` replaces the whole
  loader in place; `run restore_stock` puts the stock firmware back from
  this unit's backup and keeps the new loader.

| File (`openwrt-galachip-gc1601-tplink_xz000-g7-…`) | NOR | Use |
| --- | --- | --- |
| `…-u-boot-with-spl.bin` | `0`, 128 KiB | The whole boot loader, TPL, SPL and proper: `run upgrade_uboot` |
| `…-u-boot.bin` | none, runs from RAM | Chainloaded with `tftpboot 41000000` and `go 41000000`, for the first install |
| `…-u-boot-tpl.bin` | `0`, up to 8 KiB | TPL with the mask ROM's header: recovery over the ROM's serial menu, or an SPI programmer |
| `…-u-boot-spl.img` | `0x2000`, 32 KiB | SPL; the TPL takes it over YMODEM when the flash copy is broken |
| `…-u-boot.img` | `0xa000`, 88 KiB | U-Boot proper, LZMA; the SPL takes it over YMODEM when the flash copy is broken |

It is installed in
[steps 3 to 5 of the installation](#3-start-the-openwrt-u-boot-from-ram). Later updates are `run upgrade_uboot` from the installed
U-Boot, with the new `…-u-boot-with-spl.bin` in the TFTP root.

Useful commands at the `=>` prompt: `boot` (the default boot from flash),
`run boot_tftp` (RAM boot of the initramfs), `run upgrade_tftp` (flash the sysupgrade image), `run backup_flash`
(TFTP the whole 4 MiB NOR to the PC as `xz000-g7-flash.bin`) and
`run restore_stock` (write this unit's raw `firmware.bin` backup, served as
`g7-stock-firmware.bin`, verify it and boot stock). `sf erase`, `sf write`
and `sf update` show a progress bar with the speed. The flash driver refuses to
program or erase `misc` and `hwconfig`, and to erase the whole chip; elsewhere
`sf` writes wherever it is told, so never type a write address by hand.
Sysupgrade never touches the boot loader.

## Recovery

From the OpenWrt U-Boot prompt (press a key within 3 s of power-on),
`run boot_tftp` boots a RAM image and `run upgrade_tftp` reflashes OpenWrt.

To return to the stock firmware, put this unit's `g7-stock-firmware.bin`
from step 4 (exactly **3866624 bytes**; a vendor update file is not
equivalent) in the TFTP root, disconnect the fiber and run:

```text
run restore_stock
```

It checks the tag and size, writes the firmware partition, reads it back and
compares; the OpenWrt U-Boot stays and boots the stock firmware. The stock
boot loader itself is the first 128 KiB of `xz000-g7-flash.bin`.

If the boot loader itself is broken: with a broken SPL or U-Boot proper, the
TPL or SPL waits for `…-u-boot-spl.img` or `…-u-boot.img` over YMODEM
(`sb` from lrzsz). For one that loads but hangs, hold any key down in the
terminal while powering on: the TPL and the SPL then take YMODEM first. With a broken TPL, the chip's mask ROM stops at a
`1. SPI-NOR / 2. UART(Xmodem)` menu. Send the TPL over the serial console
with [rom-xmodem.py](package/boot/uboot-galachip/tools/rom-xmodem.py)
(standard XMODEM senders fail with this ROM). Then `run upgrade_uboot`:

```sh
python3 package/boot/uboot-galachip/tools/rom-xmodem.py /dev/ttyUSB0 openwrt-galachip-gc1601-tplink_xz000-g7-u-boot-tpl.bin
```

Damage to `misc` or `hwconfig` can only be repaired from this unit's own
backup with an external SPI programmer.

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

The seed selects the same packages as the released images. Save any
existing `.config` before replacing it. Images are generated in
`bin/targets/galachip/gc1601/`. Do not publish private ISP settings or factory
backups.

## Credits and license

Based on OpenWrt, Linux and LuCI.

The GPON parts of `xpon-agent` and `luci-app-xpon` (formerly `omci-agent` and
`luci-app-omci`) are based on the original Airoha GPON OMCI packages by [@Sirherobrine23](https://github.com/Sirherobrine23)
(**Matheus Sampaio Queiroga**).

The optical power gauges, activation stages, apply-cost badges, line status in
the header and diagnostic report download on the GPON and EPON pages follow
ideas from [odi-ui](https://github.com/AndreZiviani/odi-ui) by
[@AndreZiviani](https://github.com/AndreZiviani) (**Andre Ziviani**). No code
was copied.

Original authorship and file-level license notices are retained.
OpenWrt is licensed under GPL-2.0; component licenses remain applicable.
