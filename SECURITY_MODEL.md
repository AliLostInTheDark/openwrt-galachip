# Threat model: openwrt-galachip (TP-Link XZ000-G7 ONU firmware)

## Scope
This model covers only the code in this fork that upstream OpenWrt does not have:
- `target/linux/galachip` (kernel drivers, `net/xpon`, DTS `dts/gc1601-tplink-xz000-g7.dts`, base-files, image)
- `package/utils/xpon` (omcid, libomci-protocol, omci-agent, omci-monitor, omci-vlanscan, omci-management, epon-oamd, LuCI apps, xpon-report, xpon-diagnostics)
- `package/boot/uboot-galachip`, `package/boot/gc1601-loader`
- `package/luci/luci-theme-slate`, `package/luci/luci-app-onu-system`

**Out of scope (user):** upstream OpenWrt packages that the fork has not modified.

## System overview
The firmware runs on a GPON/EPON ONU bridge with a GC1601 (ARMv5TE) SoC, 32 MiB RAM and 4 MiB SPI NOR. The switch hardware forwards subscriber traffic between the LAN port and the PON. The CPU handles the control plane: activation, PLOAM, OMCI, OAM, optics, and management (LuCI over uhttpd, ubus/rpcd, dropbear SSH). The kernel owns timing- and hardware-critical work (activation, PLOAM, encryption keys, optical safety, DMA). It also checks every request that the userspace `omcid` makes over generic netlink. The board has no routing or NAT, and the router behind it does the firewalling.

## Components
| Component | Code |
| --- | --- |
| **GPON kernel** | `target/linux/galachip/files/drivers/soc/galachip/` (gc1601-gpon, gc1601-pon, gc1601-xpon, *-diag, BOSA optics); `files/net/xpon/` (core, genl, sysfs, leds, `omci/`) |
| **Datapath** | `files/drivers/net/ethernet/galachip/gc1601-eth.c` (switch/SFU offload, T-CONT/GEM queues, buffer pool), `drivers/net/phy/galachip.c`, `drivers/net/mdio/mdio-gc1601.c` |
| **omcid** | `package/utils/xpon/omcid`, `libomci-protocol`, `omci-agent` (ucode/ubus `omci.agent`), `omci-monitor`, `omci-vlanscan`, `omci-management`, `luci-app-omci/root/usr/sbin/omci-vlan` |
| **EPON** | `package/utils/xpon/epon-oamd`, `drivers/soc/galachip/gc1601-epon*.c`, `files/net/xpon/oam/` |
| **LuCI/web** | `package/utils/xpon/luci-app-*` (views, rpcd ACLs, `rpcd/ucode/xpon.public`), `luci-app-xpon-stats`, `xpon-report`, `xpon-diagnostics`, `package/luci/luci-theme-slate`, `package/luci/luci-app-onu-system` |
| **Boot** | `package/boot/uboot-galachip` (SPL + proper, TFTP/serial/NOR update commands, `ubcheck`, `fwload`), `package/boot/gc1601-loader`, `target/linux/galachip/image`, `base-files/lib/upgrade/platform.sh` |
| **Platform** | `files/arch/arm`, clk/clocksource/irqchip/i2c/spi/mtd drivers, `dts/`, other `base-files` (init scripts, board.d, uci-defaults) |

## Trust boundaries and entry points
1. **PON / OLT side (untrusted, user confirmed).** PLOAM and GTC framing (gc1601-gpon), OMCI messages (kernel `net/xpon/omci`, then omcid and libomci-protocol), EPON MPCP/OAM and CTC extensions (gc1601-epon, `net/xpon/oam`, epon-oamd), and GEM/LLID data frames into the datapath. Anyone in this position can reach these: a malicious or spoofed OLT, an ISP-side attacker, and another ONU or a tap on the same shared PON tree (downstream broadcast, upstream contention).
2. **LAN side (untrusted, user confirmed).** Devices on or beside the router can reach the management IP (default `192.168.1.1`). The board's `lan` bridge is `eth0 pon0` (`base-files/etc/board.d/02_network`). `pon0` carries CPU-path traffic that has no hardware flow, and `omci-management` builds the `br-onu` management bridge with isolated ports.
   - **User: the management plane (LuCI/uhttpd, ubus, SSH) must be reachable only from the LAN port.** Any path that lets PON/OLT-side traffic reach it (through `pon0` in the `lan` bridge, the CPU path, or the management VLAN/`br-onu`) is a bug.
3. **Unauthenticated web/ubus (user confirmed as an attacker).** uhttpd serves `/ubus` and LuCI via the ucode handler (`uci-defaults/50_luci-ucode-handler`). The `unauthenticated` ACLs are `xpon-public.json` (`xpon.public status`) and `omci-public.json` (`omci.agent status/status_live`, `omci.vlan status`).
4. **Authenticated LuCI/rpcd.** These ACLs grant write access that reaches hardware: `luci-app-omci.json` (`omci.agent` `mib_set/config_set/apply`, exec `/usr/sbin/omci-vlanscan *`), `luci-app-epon.json` (write `tx_hold`, `/etc/xpon-rogue`, `file.remove`), and `luci-app-xpon.json` (PON mode, diagnostic report).
5. **Local userspace to kernel.** Generic netlink (`xpon`, `omci` families). `XPON_CMD_GET`, `XPON_CMD_GET_STATE` and `OMCI_CMD_GET` are unprivileged, and the rest require `GENL_ADMIN_PERM`. Writable sysfs exposes `tx_hold`, `bosa_program`, `pon_start`, `serdes_init`, `gmac_init` and the OMCI config attributes.
6. **Physical (user confirmed as an attacker).** UART console to U-Boot and Linux, and the SPI NOR. U-Boot can TFTP or serial-load images and write NOR.
7. **Firmware update.** `sysupgrade` (`platform.sh`), plus the U-Boot `upgrade_tftp`, `upgrade_uboot` and `restore_stock` commands, which check tag, size and CRC.

## Assets (all named critical by the user)
- **ISP credentials and cloned identity:** GPON serial and password, LOID and LOID password, CTC auth, vendor and equipment ID (UCI `omci`/`epon`, kernel `omci/identity.c`). `xpon-report` masks them by default.
- **Device integrity:** root shell, firmware, U-Boot and SPL in NOR.
- **Service availability:** staying ranged, recovering after outages, not hanging the switch, DMA or the 512-buffer packet pool, omcid restartability.
- **Optical/laser safety:** not transmitting outside granted bursts. A rogue ONU disrupts every subscriber on the PON tree (`tx_hold`, `allow_tx`, rogue detection).
- **Subscriber traffic confidentiality:** GEM AES keys and key exchange (PLOAM), and correct GEM-to-UNI mapping (no leaking other subscribers' downstream).

## Attackers (all confirmed relevant by the user)
1. Malicious or spoofed OLT, or the ISP side (crafted PLOAM, OMCI, OAM)
2. Another ONU or a tap on the same PON tree
3. Devices on the LAN side
4. Unauthenticated web/ubus clients that reach the management IP
5. Physical attacker with UART or SPI access

## Existing controls
- Kernel validates omcid requests, and privileged genl ops use `GENL_ADMIN_PERM`.
- Unauthenticated ubus is limited to read-only status, and `xpon.public` filters EPON OAM fields to an allow-list (`oam_fields`) that excludes credentials.
- `xpon-report` masks serials, passwords and LOIDs unless `-u` is given.
- U-Boot checks image tags, sizes and CRCs, and excludes DMA and relocated U-Boot from load destinations.
- Unverified register features ship disabled behind module parameters (`uni_omci_attrs`, `gem_sfu_shaping`, `tcont_qos`), and `tcont_depth`/`tcont_out` refuse settings that exceed the buffer pool.
- The `br-onu` management bridge sets its ports to isolated.

## Accepted risk / out of scope (user)
- Unmodified upstream OpenWrt packages.
- The unauthenticated read-only status API exposing status data. Leaking a credential or identity through it is still in scope.
- No root password until first login, as on stock OpenWrt.
- **Unsigned firmware:** U-Boot, SPL and sysupgrade check integrity (tag, size, CRC) but not authenticity. Missing signature verification is accepted (hobbyist/open firmware). Memory-safety bugs in those parsers are still in scope.

## Open questions
- None outstanding.

No infrastructure repository applies (embedded firmware).
