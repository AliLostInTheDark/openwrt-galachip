# XZ000-G7 U-Boot development port

U-Boot **v2026.10** for the GC1601/FA626TE XZ000-G7 with 32 MiB DDR and
4 MiB EN25QH32B NOR. **U-Boot proper runs from NOR behind the stock SPL and
updates itself (`run upgrade_uboot`); the replacement SPL and ROM recovery are
untested.** See [HARDWARE-TESTS.md](HARDWARE-TESTS.md).
The experimental `gc1601-loader` package is separate and unchanged.

## Scope

Driver-model NS16550 UART, SCU clocks, FTTMR010 timer, FTSPI020 SPI memory,
MDIO/PHY and packet-engine Ethernet; `ping`, TFTP, upstream `loadx/loady`,
`bootz`, and `fwload`/`zdtb` for the existing tagged OpenWrt format.
The packet engine establishes SRAM, pool, rings, switch defaults and CPU/UNI
queue profiles rather than requiring V5's inherited ring-base signature.

Environment defaults are volatile and live in
`files/board/tplink/xz000-g7/xz000-g7.env`: 115200n8, device `192.168.1.1`,
server `192.168.1.10`. Read the factory MAC at absolute NOR `0x3ef100`; an invalid
MAC requires a runtime `setenv ethaddr`. No random/shared fallback address.

NOR runs single-lane, mode 0, at up to 50 MHz with 256-byte PIO bursts, as
the Linux driver does. The SPI driver accepts program and erase only for
U-Boot proper and the firmware partition (`0x2000`-`0x3d0000`), with erases
aligned to their own block size. Everything else is refused below every
command and script: chip erase, status-register writes, any erase or program
touching the SPL (`0`-`0x2000`), `misc` and `hwconfig`. NOR uses 4 KiB
erases, so updating proper never erases the SPL block. The SPL can only be
replaced with the SPI programmer. Probe does not unlock the chip.
There is no persistent environment, HTTP recovery, mtest or optical
calibration operation.

## Commands

| Command | Use |
| --- | --- |
| `run boot_flash` | Default `bootcmd`: `fwload` the tagged kernel from NOR, then `bootz`. |
| `run boot_tftp` | `tftpboot ${bootfile}` (the OpenWrt initramfs zImage), `zdtb`, `bootz`. |
| `run backup_flash` | Read the whole 4 MiB NOR and `tftpput` it as `xz000-g7-flash.bin`. |
| `run upgrade_uboot` | `tftpboot ${ubootfile}` (proper.img), `ubcheck`, `sf update` at `0x2000`, read back and compare. |
| `ubcheck <addr> <size>` | Check a loaded proper.img: size, footer and wrapper CRC32s, entry. |
| `run upgrade_tftp` | `tftpboot ${upgradefile}` (sysupgrade.bin), check tag and size, `sf update` at `0x20000`. |
| `fwload <addr>` | Load the tagged kernel; sets `filesize` and `fdt_addr`. |
| `zdtb <addr>` | Set `fdt_addr` to the DTB appended to a loaded zImage. |
| `ethdiag [verbose]` | Packet-engine handoff registers. |

`run` stops at the first failing step. `setenv baudrate 230400` doubles the
console rate (the 33 MHz UART clock also allows an exact, non-standard
1031250); 460800 and 921600 are not reachable. The wrapper's XMODEM recovery
stays at 115200.

## Build bootloader components

From the OpenWrt checkout, using an existing cross compiler:

```sh
export STAGING_DIR=/path/to/toolchain
python3 package/boot/uboot-galachip/tools/build.py \
    --archive /path/to/dl/u-boot-2026.10.tar.bz2 \
    --output /path/to/g7-uboot-build \
    --cross-compile "$STAGING_DIR/bin/arm-openwrt-linux-muslgnueabi-" \
    --revision "$(git rev-parse HEAD)" --jobs 4
```

Needs Python 3.12+ with lzma, make, a host C compiler, bison, flex, dtc and
the normal upstream U-Boot host build dependencies. The archive hash is pinned
in both the OpenWrt recipe and standalone builder. Reusing an output directory
with a different patch identity is rejected.

In OpenWrt the package is built by default for `tplink_xz000-g7` and
publishes one device artifact,
`openwrt-galachip-gc1601-tplink_xz000-g7-u-boot-proper.img`, which is the
default `ubootfile` for `run upgrade_uboot`. The SPL and full boot-region
images stay in the package build directory until the SPL passes a cold-boot
trial. Sysupgrade never touches the bootloader.

## Artifacts and formats

| File | Purpose |
| --- | --- |
| `xz000-g7-uboot-ram.bin` | Raw warm-entry U-Boot, load and enter at `0x41000000`. |
| `xz000-g7-uboot-proper.img` | Exactly `0x1e000` bytes: raw executable wrapper, checked LZMA payload and integrity footer, compatible with the installed SPL's raw entry. |
| `xz000-g7-spl-rom.bin` | 16-byte ROM header followed by at most `0x1c00` bytes of SRAM first-stage code. |
| `xz000-g7-bootregion.bin` | Exactly `0x20000` bytes: ROM/SPL, erased padding, then proper.img. **Not an installation authorization.** |
| `xz000-g7-uboot-uart.pkg` | Checked RAM U-Boot package for the wrapper's XMODEM failure path. |
| `manifest.json` | Revision, configuration hashes, artifact sizes/hashes, entries and limits. |
| `host-inspection.json` | ISA, relocation, command, stack and SRAM assertions. |

The installed SPL **does not consume a legacy image header** at flash `0x2000`.
It copies `0x60000` raw bytes to `0x40100000` and calls that address.
A legacy header prepended there would execute as instructions and break boot.
The new wrapper preserves the raw ARM entry; the new SPL checks the wrapper's
footer CRC before entering it. The wrapper validates the internal header,
compressed CRC, LZMA profile/length, decoder result, end marker and expanded CRC.

The existing firmware tag has magic `0x03000003`, a 512-byte header and kernel
length at `0x78`. It has **no CRC field**. `fwload` bounds that length to
the firmware partition, checks zImage bounds and the appended DTB. RAM
variants stop at the prompt; the production variant offers a three-second
interrupt window before `run boot_flash`.

## Memory and startup contracts

| Use | Range, end exclusive |
| --- | --- |
| Proper expansion/output | `0x40010000–0x400f0000` maximum; BSS must end before wrapper. |
| Wrapper and compressed input | `0x40100000–0x4011e000` file allocation. |
| LZMA probability heap | `0x40200000–0x40210000` |
| Wrapper stack | `0x40300000–0x40304000` |
| DDR scratch | `0x407f0000–0x407f0010` |
| Fixed packet-engine DMA | `0x40800000–0x40912000` |
| Wrapper UART staging | Starts `0x40fffff0`; payload starts `0x41000000`. |
| Proper initial stack top | `0x40f00000`; full runtime relocates near the top of DDR. |
| ROM-loaded code plus SPL BSS | `0x80000000–0x80001c00` |
| Reserved ROM metadata | `0x80001c00–0x80001d00` |
| SPL stack | `0x80001d00–0x80002000` |

Both proper variants preserve initialized DDR. Startup is ARMv5TE, masks
IRQ/FIQ, rejects enabled MMU/D-cache rather than discarding dirty cache lines,
and leaves caches off. There is no ARM926 test-clean loop or ARMv7 barrier.
Without VBAR, vectors at zero belong to ROM: vector relocation is suppressed.
Unexpected synchronous exceptions still depend on the ROM handlers; their
behavior is an unresolved hardware gate.

SPL uses the installed 36-entry DDR table unless the controller already reports
ready. As in the GPL, a not-ready controller is always programmed and either
lock bit is accepted. Ready state is preserved, and scratch accesses are verified
on both paths. Never manually launch this
SPL over a running Linux/U-Boot session as a DDR test.

TFTP, serial and NOR command destinations exclude DMA and relocated U-Boot.
A timed-out TX buffer remains hardware-owned; unmatched RX ownership is an
error. Linux handoff stops RX/TX, settles tracked ownership and drains known
descriptors. An ownership failure blocks handoff. The existing DMA reservation
is retained in the outgoing FDT; no Linux UAPI changes are introduced.

## Host checks

```sh
python3 package/boot/uboot-galachip/tests/test_images.py
UBOOT_SRC=/path/to/g7-uboot-build/u-boot-v2026.10 \
    python3 package/boot/uboot-galachip/tests/test_decoder.py
python3 package/boot/uboot-galachip/tests/test_drivers.py
```

The tests execute the production C validators, decoder and receiver, plus the
unmodified SPI/Ethernet driver C against mocked MMIO/driver-model interfaces.
They cover corrupt/truncated images, lengths/overflow, CRC retries, duplicates,
sequence wrap, line purge, timeout/cancellation, FIFO tails, SPI abort failures, descriptor
bounds and TX ownership. They do not emulate PHY timing, actual DMA arbitration,
NOR electrical behavior or DDR training.

Read [AUDIT.md](AUDIT.md), [HARDWARE-TESTS.md](HARDWARE-TESTS.md),
[INSTALLATION.md](INSTALLATION.md) and [HANDOFF.md](HANDOFF.md) before testing.

## Source and authorship

Upstream U-Boot retains its original notices. Clock, timer and SPI sources retain
the Faraday/Bing-Yao Luo/Bo-Cun Chen/Po-Yu Chuang credits where derived.
Ethernet/descriptor work retains Ifran Ali's authorship from the Linux/V5 port.
The decoder is upstream's Igor Pavlov LZMA SDK implementation and license.
Private flash dumps, calibration data, live MAC/serial identities and old
experimental artifacts are not part of this package.

The GC1601 console ignores an empty Enter instead of repeating the last
command. Explicitly entered commands and command-line history remain available.
