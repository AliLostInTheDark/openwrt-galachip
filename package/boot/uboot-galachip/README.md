# GalaChip GC1601 U-Boot

U-Boot **v2026.10** for GalaChip GC1601 (FA626TE) ONUs and SFP sticks. The
boot chain is U-Boot TPL in SRAM, U-Boot SPL in DDR and U-Boot proper, as on
Rockchip. **They ship as one boot-region image, `u-boot-with-spl.bin`, which
`run upgrade_uboot` installs, also from the stock U-Boot, without a
programmer ([INSTALLATION.md](INSTALLATION.md)).**

Everything but a board's DDR settings, device tree and file names is SoC
code; what differs between boards comes from the device tree, as Linux reads
it. Boards: TP-Link XZ000-G7 (32 MiB DDR, 4 MiB EN25QH32B NOR), tested on
the hardware. In the OpenWrt port
([openwrt-galachip](https://github.com/AliLostInTheDark/openwrt-galachip)) this
repository is the package `package/boot/uboot-galachip`; it also builds on its
own with `tools/build.py`.

## Scope

Driver-model NS16550 UART, SCU clocks, FTTMR010 timer, FTWDT010 watchdog
(reset through `wdt-reboot`), FTSPI020 SPI memory, MDIO/PHY and
packet-engine Ethernet; `ping`, TFTP, upstream `loadx/loady`, `boot`, `bootz`, and
`fwload`/`zdtb` for the existing tagged OpenWrt format. The prompt is
upstream's `=> `.
The Ethernet driver sets up the packet engine's SRAM, buffer pool, rings,
switch defaults and CPU/UNI queue profiles itself; it needs nothing an earlier
loader left behind.

## Boot chain and flash layout

| NOR offset | Content | Runs at |
| --- | --- | --- |
| `0x0` | U-Boot TPL with the mask ROM's 16-byte header, at most `0x1c00` bytes (`u-boot-tpl.bin`) | SRAM `0x80000000` |
| `0x2000` | U-Boot SPL as a legacy image, at most `0x7fc0` bytes of data (`u-boot-spl.img`) | DDR `0x40100000` |
| `0xa000` | U-Boot proper as an LZMA legacy image (`u-boot.img`), up to `0x20000` | DDR `0x40010000` |

The mask ROM checks the TPL's header and word sum and runs it from SRAM. The
TPL (Thumb, no driver model) stops the watchdog, sets up DDR from the board's
register table, then moves its stack and BSS to DDR. It reads the SPL from
`0x2000` with a register-level FTSPI020 loader (`mach-galachip/spl_spi.c`, as
sunxi does), checks its legacy header and data CRC and enters it; if that
fails it receives `u-boot-spl.img` over YMODEM. The SPL (driver model,
FTSPI020, Thumb) loads `u-boot.img` from `0xa000`, checks the data CRC of the
compressed payload and decompresses it; if that fails it receives the same
`u-boot.img` over YMODEM. Neither boots a raw image from flash. So a broken
SPL or proper recovers over the serial console, and a broken TPL over the
mask ROM's UART menu.

## Source layout

U-Boot proper follows upstream's layout; the files in `files/` are copied over
the patched v2026.10 tree.

| Path | Contents |
| --- | --- |
| `arch/arm/mach-galachip/` | `ARCH_GALACHIP` and the board choice; entry checks (`save_boot_params`), the no-op `relocate_vectors`, the D-cache (`cache.S`, `mmu.c`), `print_cpuinfo`; board hooks from the device tree (`board.c`); `fwload`/`zdtb`/`ubcheck` (`boot.c`), `ethdiag`; TPL: `tpl.c`, DDR setup (`sdram.c`), SPI loader (`spl_spi.c`) and linker script; SPL: `spl.c`; both: boot order (`boot-device.c`) and reset (`reset.c`). |
| `arch/arm/include/asm/arch-galachip/gc1601.h` | Register bases, load-range bounds and the Ethernet DMA area. |
| `arch/arm/dts/` | `gc1601.dtsi` and `gc1601-tplink-xz000-g7.dts`, identical to the Linux ones in the OpenWrt tree's `target/linux/galachip/dts/`; U-Boot additions in `gc1601-u-boot.dtsi` and `gc1601-tplink-xz000-g7-u-boot.dtsi`. |
| `board/tplink/xz000-g7/` | The board's DDR register table (`sdram.c`) and its file names (`xz000-g7.env`). |
| `configs/` | `tplink_xz000_g7_defconfig` (flash, `0x40010000`) and `tplink_xz000_g7_ram_defconfig` (includes it; `0x41000000`, no autoboot). |
| `include/configs/gc1601.h` | DDR base, the TPL's console and timer, baud table. |
| `include/env/galachip/gc1601.env` | The default environment's scripts, included by each board's `.env`. |
| `drivers/clk/clk-gc1601.c` | SCU clocks: PLL0 from the oscillator, CPU, AHB/APB and SPI dividers. |
| `drivers/timer/fttmr010_timer.c` | FTTMR010 timer 2, free-running down-counter on APB. |
| `drivers/spi/ftspi020.c`, `drivers/net/gc1601_*.c` | SPI memory, Ethernet and MDIO. |

The CPU code is upstream `arm926ejs` with `SKIP_LOWLEVEL_INIT`: its
`cpu_init_crit` cleans the D-cache with the ARM926 test-and-clean loop, which
is not verified on the FA626TE (`tools/verify.py` rejects that instruction).
`save_boot_params` in `mach-galachip` does what that init would: masks
interrupts, refuses an entry with the MMU or D-cache on, then turns off the
I-cache, alignment checks and high vectors and invalidates the caches and TLBs.
The SPL runs with the I-cache and, through a flat section map, the D-cache on:
LZMA took 2.5 s to unpack U-Boot without it and takes 0.12 s with it. Before
it jumps, `mach-galachip/cache.S` cleans the D-cache with the FA626TE's
clean-and-invalidate-all operation (`c7, c14, 0`, as Linux's `cache-fa.S`)
and turns it and the MMU off; a board test matched U-Boot's CRC32 after that.
U-Boot proper caches DDR write-back through upstream's `cache-cp15.c`, with
the same operation as its `flush_dcache_all` (patch 207 makes the ARM926 one
weak). `mach-galachip/mmu.c` leaves the packet engine's DMA area uncached,
since the Ethernet driver reaches it with plain loads, stores and `memcpy`,
and `go` cleans and turns off the caches first, as `bootz` does.
The watchdog is not started; `board_init` stops one left running by an
earlier stage. `CONFIG_SYS_CLK_FREQ` is the watchdog's PCLK (APB, 133.33 MHz),
used only for `wdt start` timeouts.

Environment defaults are volatile: the scripts in
`include/env/galachip/gc1601.env` (115200n8, device `192.168.1.1`, server
`192.168.1.10`) and the board's file names. `misc_init_r()` sets `fw_offset`
and `fw_size` from the firmware partition (`galachip,gc1601-fw`) and
`flash_size` from the flash, and reads the factory MAC from the Ethernet
node's `mac-address` NVMEM cell (on the XZ000-G7 at NOR `0x3ef100`). An invalid
MAC requires a runtime `setenv ethaddr`. No random/shared fallback address.

NOR runs in mode 0 with 256-byte PIO bursts and the Linux driver's speeds,
from the same device tree: reads of 64 bytes or more use dual-output reads
(1-1-2) at 100 MHz, the 200 MHz SPI clock divided by 2; identification,
status, programming and erasing stay at 25 MHz (`galachip,safe-max-frequency`).
The driver switches the SCU's SPI clock from 50 to 200 MHz; the TPL does the
same and reads the SPL with the same 1-1-2 command at 100 MHz. The SPI driver passes the commands of the SPI memory
layer to the controller, with one exception: it refuses to program or erase
the ranges in the flash node's `galachip,protected-ranges` (`misc` and
`hwconfig`, `0x3d0000`-`0x3fffff`) and refuses a chip erase. The ranges come
from the device tree, not from the driver. The scripts that program the flash
(`upgrade_uboot`, `upgrade_tftp`, `restore_stock`) only touch the boot region
and the firmware partition (`0`-`0x3d0000`) and check the tag and size of what
they write; `sf` at the prompt can still write anywhere else it is told.
NOR uses 4 KiB erases and `sf update` skips identical blocks, so
an unchanged SPL is never rewritten; a broken one recovers over the mask
ROM's UART menu. Probe does not unlock the chip.
There is no persistent environment, HTTP recovery, mtest or optical
calibration operation.

## Commands

| Command | Use |
| --- | --- |
| `boot` or `run boot_flash` | Default `bootcmd`: `fwload` the tagged kernel from NOR, then `bootz`. |
| `run boot_tftp` | `tftpboot ${bootfile}` (the OpenWrt initramfs zImage), `zdtb`, `bootz`. |
| `run backup_flash` | Read the whole NOR and `tftpput` it as `${bootfile_backup}`. |
| `run restore_stock` | `tftpboot ${bootfile_stock}` (this unit's raw `firmware` backup, exactly `${fw_size}` bytes), check tag and size, `sf update` at `${fw_offset}`, read back and compare. U-Boot stays; stock firmware boots with `boot_flash`. |
| `run upgrade_uboot` | `tftpboot ${bootfile_uboot}` (u-boot-with-spl.bin), `ubcheck`, `sf update` of `0`-`0x20000`, read back and compare. |
| `ubcheck <addr> <size>` | Check a loaded u-boot-with-spl.bin (size `0x20000`: the TPL's header and word sum as the ROM checks them, then `u-boot-spl.img` and `u-boot.img` as legacy images: header and data CRC32, load address, type, compression) or a `u-boot.img` alone. |
| `run upgrade_tftp` | `tftpboot ${bootfile_upg}` (sysupgrade.bin), check tag and size, `sf update` at `${fw_offset}`. |
| `fwload <addr>` | Load the kernel of the tagged firmware partition; sets `filesize` and `fdt_addr`. |
| `zdtb <addr>` | Set `fdt_addr` to the DTB appended to a loaded zImage. |
| `ethdiag [verbose]` | Packet-engine handoff registers. |

`run` stops at the first failing step. `setenv baudrate 230400` doubles the
console rate (the 33 MHz UART clock also allows an exact, non-standard
1031250); 460800 and 921600 are not reachable. The TPL's and the SPL's
YMODEM recovery stay at 115200.

## Adding a board

A board needs, besides its device in the OpenWrt tree:

1. `arch/arm/dts/gc1601-<vendor>-<board>.dts`, the Linux one unchanged, with
   its memory node, flash partitions (the firmware partition with
   `compatible = "galachip,gc1601-fw"`) and the Ethernet node's `mac-address`
   NVMEM cell; plus `…-u-boot.dtsi` with the `ethernet0` alias and the flash's
   `galachip,protected-ranges` for factory data.
2. `board/<vendor>/<board>/`: `sdram.c` with the DDR controller settings the
   board's stock loader writes, `<board>.env` including
   `<env/galachip/gc1601.env>` and naming the board's files, `Kconfig`,
   `Makefile` and `MAINTAINERS`.
3. A `TARGET_` entry in `arch/arm/mach-galachip/Kconfig`, the two defconfigs
   and a `U-Boot/<vendor>_<board>` entry in the package `Makefile`.

The DDR settings must come from the board itself (its stock loader or a
register dump); wrong ones stop the TPL at `DDR setup failed`, which the
mask ROM's UART menu recovers.

## Build bootloader components

From this repository, using an existing cross compiler (for example an
OpenWrt toolchain):

```sh
export STAGING_DIR=/path/to/toolchain
python3 tools/build.py \
    --archive /path/to/dl/u-boot-2026.10.tar.bz2 \
    --output /path/to/g7-uboot-build \
    --cross-compile "$STAGING_DIR/bin/arm-openwrt-linux-muslgnueabi-" \
    --revision "$(git rev-parse HEAD)" --jobs 4
```

Needs Python 3.12+ with lzma, make, a host C compiler, bison, flex, dtc and
the normal upstream U-Boot host build dependencies. The archive hash is pinned
in both the OpenWrt recipe and standalone builder. Reusing an output directory
with a different patch identity is rejected. Outside an OpenWrt tree the
manifest's OpenWrt revision reads `unknown`.

In OpenWrt the package (`u-boot-tplink_xz000_g7`) is built by default for
`tplink_xz000-g7` and installs these device artifacts. Sysupgrade never touches the bootloader.

| File | Purpose |
| --- | --- |
| `…-u-boot-with-spl.bin` | Exactly `0x20000` bytes: the whole boot region, the default `bootfile_uboot` for `run upgrade_uboot`. |
| `…-u-boot.bin` | RAM U-Boot, load and enter at `0x41000000` from any U-Boot (`go`). |
| `…-u-boot.img` | U-Boot proper for NOR `0xa000`: `mkimage` legacy image, LZMA, load and entry `0x40010000`. Also what the SPL takes over YMODEM. |
| `…-u-boot-spl.img` | U-Boot SPL for NOR `0x2000`: `mkimage` legacy image, uncompressed, load and entry `0x40100000`. Also what the TPL takes over YMODEM. |
| `…-u-boot-tpl.bin` | U-Boot TPL with its ROM header, for NOR `0x0`, a programmer or the mask ROM's UART menu (without its 16-byte header). |

The build output also holds `manifest.json` (revision, configuration hashes,
artifact sizes and hashes, layout) and `host-inspection.json` (ISA,
relocation, command, stack and free-space checks from `tools/verify.py`).
U-Boot is built with LTO, the TPL and SPL in Thumb, and the CRC-32 and
CRC-16 tables are built at run time (`DYNAMIC_CRC_TABLE`); that is what fits
the TPL with its YMODEM recovery into the ROM's 0x1c00 bytes (about 100 bytes
spare) and the SPL and proper into the boot region.

`tools/rom-xmodem.py` sends the TPL to the mask ROM's UART menu, standalone or
as picocom's send command ([INSTALLATION.md](INSTALLATION.md)). `sf erase`,
`sf write` and `sf update` draw a progress bar and report time and KiB/s.

The existing firmware tag has magic `0x03000003`, a 512-byte header and kernel
length at `0x78`. It has **no CRC field**. `fwload` bounds that length to
the firmware partition, checks zImage bounds and the appended DTB. RAM
variants stop at the prompt; the production variant offers a three-second
interrupt window before `run boot_flash`.

## Memory and startup contracts

| Use | Range, end exclusive |
| --- | --- |
| Proper, decompressed by the SPL | `0x40010000`; BSS must end before `0x40100000`. |
| SPL code, BSS and DTB | `0x40100000–0x40108000`; its legacy header lands at `0x400fffc0`. |
| SPL early heap (LZMA buffers) and stack | `0x402c0000–0x40300000` |
| DDR scratch | `0x407f0000–0x407f0010` |
| Fixed packet-engine DMA, uncached | `0x40800000–0x40912000` (the 1 MiB sections `0x40800000–0x40a00000`) |
| SPL YMODEM staging | From `0x41000000` (`CONFIG_SYS_LOAD_ADDR`). |
| Proper initial stack top | `0x40f00000`; full runtime relocates near the top of DDR. |
| TPL code and data | `0x80000000–0x80001c00` |
| Reserved ROM metadata | `0x80001c00–0x80001d00` |
| TPL stack and global data until DDR is up | `0x80001d00–0x80002000` |
| TPL BSS (YMODEM state, CRC table) | From `0x40200000` |
| TPL stack after DDR setup | Below `0x40300000` |
| SPL section table | `0x40300000–0x40304000` |

Both proper variants preserve initialized DDR. Startup is ARMv5TE, masks
IRQ/FIQ, rejects enabled MMU/D-cache rather than discarding dirty cache lines,
and starts with the D-cache off; proper turns it on after relocation. There is no ARM926 test-clean loop
or ARMv7 barrier.
Without VBAR, vectors at zero belong to ROM: vector relocation is suppressed.
Unexpected synchronous exceptions still depend on the ROM handlers; their
behavior is an unresolved hardware gate.

The TPL uses the board's DDR table (36 entries on the XZ000-G7), the one the installed boot
loader writes, unless the controller already reports ready. As in the GPL, a not-ready controller is always programmed and either
lock bit is accepted. Ready state is preserved, and scratch accesses are verified
on both paths. Never manually launch the TPL
over a running Linux/U-Boot session as a DDR test.

TFTP, serial and NOR command destinations exclude DMA and relocated U-Boot.
LMB also reserves the Linux DT's `reserved-memory` regions (packet-engine
pool `0x40a00000–0x40d32000`, ramoops `0x40f80000–0x40fc0000`).
A timed-out TX buffer remains hardware-owned; unmatched RX ownership is an
error. Linux handoff stops RX/TX, settles tracked ownership and drains known
descriptors. An ownership failure blocks handoff. The existing DMA reservation
is retained in the outgoing FDT; no Linux UAPI changes are introduced.

## Host checks

```sh
python3 tests/test_images.py
python3 tests/test_drivers.py
```

The tests execute the image tools (ROM header, legacy images, boot region,
programmer image), plus the unmodified SPI, Ethernet, MDIO and clock driver C
against mocked MMIO/driver-model interfaces.
They cover corrupt/truncated images, lengths/overflow, CRC retries, duplicates,
sequence wrap, line purge, timeout/cancellation, FIFO tails, SPI abort failures, descriptor
bounds and TX ownership. They do not emulate PHY timing, actual DMA arbitration,
NOR electrical behavior or DDR training.

Read [INSTALLATION.md](INSTALLATION.md) before installing.

## Source and authorship

Upstream U-Boot retains its original notices. Clock, timer and SPI sources retain
the Faraday/Bing-Yao Luo/Bo-Cun Chen/Po-Yu Chuang credits where derived.
Ethernet/descriptor work retains Ifran Ali's authorship from the Linux/V5 port.
LZMA decompression is upstream U-Boot's (Igor Pavlov's LZMA SDK) in the SPL.
Private flash dumps, calibration data, live MAC/serial identities and old
experimental artifacts are not part of this package.

The GC1601 console ignores an empty Enter instead of repeating the last
command. Explicitly entered commands and command-line history remain available.
