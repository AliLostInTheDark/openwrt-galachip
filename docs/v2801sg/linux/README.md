# VSOL V2801SG / ZX279125

This target is for an ONU with hardware-offloaded LAN-to-GPON bridging.
Subscriber traffic belongs in the packet engine; Linux handles management,
OMCI and control exceptions. Management-LAN tests do not establish subscriber
offload or optical registration. Firmware installation remains outside the
current RAM-only validation scope.

The stock UI spells the vendor `VSOL`. The display model is `VSOL V2801SG`;
the stable DT board ID is `v-sol,v2801sg`. U-Boot reports a600MHz Cortex-A9,
32MiB DRAM and8MiB ZB25VQ64 SPI NOR (JEDEC5e4017).

The current [bridge-only profiles](ONU-BRIDGE-PROFILE.md) remove routing/NAT
applications and local IPv6, retain IPv4 management, and separate the lean RAM
LAN test from the LuCI/OMCI development profile. Historical dual-stack test
results below describe an older image; current subscriber L2 IPv6 forwarding
still requires its own optical validation.

## LAN port

`kmod-zx279125-ethernet` loads automatically and binds to the Ethernet DT
node. It provides `lan0`, NAPI reception across all16 CPU queues, a shared
packet-engine control transport, factory MAC loading, PHY carrier tracking,
and management bridge MAC notifications. Manual bring-up
arguments and the old startup script are no longer needed.

The factory reader validates the TAGH0201 partition and each record checksum.
LAN1 MAC tag0101 takes precedence over base MAC tag0100. Invalid identity
does not fall back to a random MAC or a shared U-Boot address.

Hardware IRQ-only operation passed a normal RAM boot,5000 full-MTU pings
with0% loss, live IP/MAC changes verified through TCP/SSH, interface
stop/reopen, module removal/reload, and payload-checked duplex TCP.
See [Ethernet evidence](ETHERNET-BRINGUP.md) for exact images, hashes,
measurements and subsequent cold-start validation.

The packet engine keeps running independently of the UNI administrative
state so its control transport can serve PON. PON is currently enabled in the
board DTS, with laser permission defaulting off; the xPON port remains unfinished.
Management delivery uses a MAC-selective comparator and has passed primary/secondary
IPv4 and IPv6 payload tests. IPv6 neighbor recovery after LAN stop/start remains
unresolved; see the latest Ethernet checkpoint before calling the LAN port complete.

## Platform and memory

The custom UART is not16550: data+04, flags+14, baud divisors+24/+28 and
interrupt mask/status/clear+40/+48/+4c. It supports1152008N1 without hardware
flow control. MDIO discovered PHY1 with ID03815c11; PHY model-specific reset
or RGMII-delay changes are not inferred from a name.

The GIC uses distributor00801000 and CPU interface00800100. Routing writes
at00803050/54 precede GIC initialization. Event/source timers are94401000
and94402000; the event IRQ is SPI17. Normal reboot writes1 to9400003c.
Clock gates retain the stock nominal rates; PLL retuning is not implemented.

Packet buffers, rings and auxiliary DMA occupy reserved memory starting at
41000000. The whole41000000..42000000 region remains `no-map` until the
auxiliary extent is established. This leaves16MiB for Linux before kernel
and runtime allocations. The retained pool is never returned to the Linux
allocator on interface stop or driver removal.

The LAN driver adopts only the verified, idle U-Boot layout or initializes
the documented hardware reset state. It performs no shared reset or optical
programming. The DT handoff cursor address41fef75c is verified for this
board's March2025 U-Boot; another bootloader requires fresh verification.

## Build and RAM boot

The bring-up configuration omits LuCI/PON daemons. The RAM root is an XZ
squashfs embedded in a minimal initramfs; a loop mount and tmpfs overlay run
normal OpenWrt init without expanding the entire root into scarce RAM.

```sh
cp .config /tmp/openwrt-before-v2801sg.config
cp target/linux/zte/vsol_v2801sg-bringup.diffconfig .config
make defconfig
make -j4
```

The RAM bring-up profile includes kernel `/dev/mem` support, BusyBox `devmem`,
`hexdump`, `base64`, `timeout`, netcat listening/timeouts, TFTP get/put,
IP neighbor display, process-aware `netstat`, and the small `ethtool` package.
These avoid uploading ad-hoc register and transfer helpers during UART tests.
Register reads should use the established physical mappings; FIFO/mailbox
reads can consume hardware state and are not general-purpose dump targets.

The output is
`bin/targets/zte/zx279125/openwrt-zte-zx279125-vsol_v2801sg-initramfs-uImage`.
Validate its header/data CRCs, appended DTB, embedded root, and placement
before transferring it. At40600000 the whole file must end below41000000;
the decompressed kernel, BSS and bootstrap must also fit below the pool.

Serve the verified image from the existing TFTP server192.168.1.100. For
example, with its server-side filename `image.uImage`:

```text
tftp 0x40600000 image.uImage
setenv bootargs console=ttyS0,115200n8 mem=32M panic=0
bootm 0x40600000
```

The environment change is RAM-only. Never `saveenv` during these tests.
Direct `bootm` avoids the stock `bootcmd` restoring flash-root arguments.
The stock container offset40600140 does not apply to this plain uImage.
`panic=0` prevents a failed test automatically rebooting into stock boot
actions. The UART adapter is `/dev/ttyUSB1`; only one process may own it.

For reset-state initialization, UART `xmodem 0x40600000` can transfer the
same image without U-Boot networking touching the Ethernet blocks. The
receiver accepts CRC/XMODEM-1K and writes RAM only, bounded to5MiB. `bootm`
then verifies the transferred image CRC. No install command is involved.

## Evidence

Reference firmware2026-05-26 SHA256:
`bd6e8c813dc0d2d879c36392c093fc0cd24c7fd34660256d16b2ca4e35e97257`.
Decompressed kernel SHA256:
`b00093288a4cc655b8e872f3399ef5bbb966cf80b58e42930779bf9fedd722dd`.
23,208 kallsyms entries were recovered. Complete `netdriver.ko`, `switch.ko`
and `tm.ko` hashes matched board readbacks; this does not prove all optical
modules or the whole kernel identical.

[ETHERNET-BRINGUP.md](ETHERNET-BRINGUP.md) retains the register/instruction
evidence and historical experiments. [VERIFICATION.md](VERIFICATION.md)
records the previous management checks and original xPON gaps.
[XPON-INTEGRATION.md](XPON-INTEGRATION.md) includes design intentions and
historical claims; it must not be treated as proof of implemented optics.
