# GalaChip GC1601 OpenWrt target

OpenWrt target for the GalaChip GC1601 xPON SoC (Faraday FA626TE, ARMv5TE),
with Linux 6.18. It supports the TP-Link XZ000-G7 GPON/EPON ONU: 32 MiB RAM,
4 MiB SPI NOR and one Gigabit Ethernet port.

The full firmware, installation guide, measured performance and the
userspace OMCI and EPON packages live in
[openwrt-galachip](https://github.com/AliLostInTheDark/openwrt-galachip)
(branch `xz000-g7-wip`), where this directory is `target/linux/galachip`.
The boot loader is in
[galachip-uboot](https://github.com/AliLostInTheDark/galachip-uboot).

## Contents

| Path | What it holds |
| --- | --- |
| `files/drivers/soc/galachip/` | PON core and GPON/EPON MACs: activation, PLOAM, ranging, encryption, MPCP, optics (BOSA) and the xPON device that `omcid` and `epon-oamd` drive |
| `files/drivers/net/ethernet/galachip/` | Packet engine: Ethernet, the shared buffer pool and queues, `pon0`, and the hardware SFU offload between the LAN port and the PON |
| `files/drivers/net/{mdio,phy}/` | MDIO bus and the internal Gigabit PHY |
| `files/drivers/{clk,clocksource,irqchip,spi,i2c,mtd}/` | SCU clocks, timer, FTINTC030 interrupts, FTSPI020 NOR, FTIIC010 I2C and the firmware partition parser |
| `files/include/` | Device tree bindings and the xPON/OMCI kernel interfaces |
| `patches-6.18/` | Hooks into upstream files (Kconfig, Makefiles, ARM platform) plus a small OF fix |
| `dts/` | GC1601 SoC and XZ000-G7 device trees |
| `image/` | sysupgrade image and the U-Boot artifacts |
| `base-files/`, `gc1601/` | Board scripts, LuCI additions and the default package set |
| `tests/` | Host tests that compile driver code against stubs |
| `tools/pe-dump.sh` | Read-only packet-engine and GPON register dump, the same on stock and OpenWrt |

## Use

Copy this directory into an OpenWrt tree as `target/linux/galachip` (it also
needs the `package/utils/xpon` packages from openwrt-galachip), then:

```sh
cp target/linux/galachip/tplink_xz000-g7.diffconfig .config
make defconfig
make -j"$(nproc)"
```

Images are written to `bin/targets/galachip/gc1601/`.

## Tests

```sh
cd tests
python3 test-gpon-services.py
python3 test-rei.py
python3 test-uni-control.py
```

Each extracts the current function from the driver source and runs it against
stubbed kernel APIs. They need only a host C compiler.

## License

GPL-2.0, as Linux and OpenWrt. File-level notices are retained.
