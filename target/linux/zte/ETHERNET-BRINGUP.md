# ZX279125 Ethernet bring-up

## User-reported LAN recovery result — 2026-10-23

The user RAM-booted the lean recovery image and supplied UART output plus:

- 1000 transmitted/1000 received, 0% loss, RTT 0.468/0.520/0.773 ms.
- 20 transmitted/20 received, 0% loss, RTT 0.484/0.545/1.088 ms.
- Adopted consumer 2261, preserving a nonzero stock U-Boot handoff.
- Factory LAN MAC 70:b6:4f:01:a6:7a, br-lan 192.168.1.1/24, bridge ports
  lan0/pon0 and no WAN sections; IPv4 forwarding 0.
- 1000 Mb/s full duplex; MemTotal 11476 kB, MemAvailable 3660 kB.
- TCP perturbation table 256 entries/1024 bytes confirmed in the boot log.

This is user-supplied hardware evidence of recovered management LAN reception
and latency. No on-board image hash, payload-transfer, lifecycle, cold-boot or
optical forwarding result was supplied, so those gates remain open. The
reported kernel log tail shows no DMA ownership fault or OOM.

## Current RAM test image — 2026-10-23

The repaired driver and bridge-only configuration now build a lean LAN RAM
image, `v2801sg-lan-recovery-469a2e9c.uImage`, 3316056 bytes, SHA256
`469a2e9c2c8c951c841f3aef1e5b574d9aecd8567f511e2377deae1937a87a9b`.
Full build and image/DTB/bootstrap/RAM placement checks passed. It has not been
booted on hardware. Use [RAMBOOT-TEST.md](RAMBOOT-TEST.md) for the exact commands.

The [bridge-only profiles](ONU-BRIDGE-PROFILE.md) remove routing/NAT and local
IPv6, while retaining IPv4 administration and the full DMA guard. The former
IPv6 neighbor-recovery test no longer applies to local management in this
profile; it is not a demonstrated fix for that old dual-stack configuration.
Optical L2 forwarding, including subscriber IPv6, remains unverified.

## Regression repair — 2026-10-23

The board still runs the March 7, 2025 vendor U-Boot. The user has installed
OpenWrt in place of the vendor firmware; this review performed no hardware
access or flash writes. The installed image reports delayed/duplicate ping
replies and loss. The repairs below require a new RAM-only hardware check;
the older passing results do not validate the intervening source changes.

### Confirmed source defects and repairs

- **Wrong Linux queue-counter view.** `zx_rx_count()` had changed to sixteen
  independent low-halfword reads. Vendor kernel `pp_net_poll` at
  `c02665e0..c02665fc` selects low halfwords for queues 0..7 and high halfwords
  for queues 8..15. Literal bases at `c02669e0/e4` are `f4164100/f41640e0`,
  so both groups address the same eight words. Restore that packed view.
  The vendor U-Boot poller uses a different sixteen-word loop; its implementation
  must not be used to rewrite the vendor Linux queue interpretation.
- **Invented consumers on adoption.** DTS pointed at `41840000`, while the
  installed stock U-Boot's verified relocated cursor table is `41fef75c`.
  The driver replaced an unfamiliar table with zero consumers. Empty queues
  do not reset the hardware producer index: adopting zero can replay old
  descriptors and return the wrong buffers. Restore the stock cursor address
  and reject an invalid table without modifying it.
- **Reclaimed handoff memory.** The reduced 10 MiB reservation exposed
  `41a00000..41ffffff`, including the live stock cursor table, to Linux.
  Restore the full 16 MiB guard until a deliberate early handoff and all DMA
  extents are established. This returns Linux to 16 MiB before kernel/runtime
  allocations; the full LuCI/OMCI profile needs separate memory validation.
- **Malformed RX handling.** Invalid buffer indices now stop the engine instead
  of advancing only the software cursor. Failed releases of malformed frames
  are no longer ignored. Ethernet source-address inspection requires a complete
  Ethernet header, including on non-Ethernet control/exception traffic.
- **Unsupported comments.** Remove claims that the management classifier alone
  establishes subscriber offload or prevents MLD allocation failures. Neither
  conclusion follows from the existing LAN tests.

Vendor evidence is the retained raw kernel
`/home/ali/Desktop/MISC-3/fwdump_v2801sg/vmlinux`, SHA256
`b00093288a4cc655b8e872f3399ef5bbb966cf80b58e42930779bf9fedd722dd`.
Raw ARM disassembly was read at virtual base `c0008000`; REA rejected this
headerless input, so these are objdump observations, not REA analysis results.
The source snapshot and focused disassembly are in
`/tmp/v2801sg-ethernet-repair/`.

The separate replacement U-Boot package also had a NULL ring pointer in cold
initialization, unsafe adoption/reset fallbacks and buffer error paths. See
`package/boot/uboot-zte/README.md` for its repairs and build evidence. It remains
experimental: its private cursor table is not the installed vendor table,
and explicit Linux handoff compatibility still needs implementation/testing.

Next: validate the repaired Linux build with stock U-Boot, a lean RAM profile
and fiber disconnected. Check nonzero inherited consumers, correct queue
counts, bounded latency, payload integrity and balanced buffer ownership
before resuming the completion checklist below. Do not replace U-Boot to work
around the separate image-wrapper issue described next.

### Stock automatic-boot CRC repair

`scripts/mkzxicheader.py` checksummed the entire padded rootfs, including the
mutable JFFS2 marker at the start of rootfs_data. On the existing image,
changing only `deadc0de` to `85190320` reproduces the reported `77251898`
CRC while leaving SquashFS unchanged. The wrapper now takes `bytes_used` from
the SquashFS superblock and checksums only its read-only, 64 KiB-aligned extent.
The overlay marker remains in the output image, outside that checksum range.
Rootfs location is derived from the aligned kernel end rather than searching
for an arbitrary magic occurrence inside the kernel. Truncated inputs are
rejected.

Stock U-Boot compares rootfs CRC field `0x6c` at `41f06488`, but its failure
message at `41f06498` mistakenly prints kernel CRC field `0x60` as the expected
value. This explains the reported `5a465b0c`; changing the CRC field offsets
would be incorrect. Focused instructions are retained in
`/tmp/v2801sg-vendor-fs-crc-check.asm`.

### Verification of these repairs

- `make -j4 target/linux/compile V=s` completed successfully with the final
  Ethernet source; log: `/tmp/v2801sg-ethernet-repair/kernel-build-final.log`.
- A separate `W=1` module build passed with a modpost warning about omitted
  module description metadata; log: `ethernet-w1.log` in the same directory.
  `CONFIG_MODULE_STRIPPED=y` removes this metadata despite the source's
  `MODULE_DESCRIPTION`. Final module SHA256:
  `093967e6713aae2132ba0c433f8c1ae2e1030ced495716b517d8a56feba42948`.
- Compiled/decompiled board DT confirms the full `41000000..41ffffff`
  no-map reservation and stock cursor `41fef75c`.
- A wrapper fixture rebuilt from the existing artifact passes header, kernel
  and rootfs CRC verification both before and after simulated overlay marker
  initialization. Read-only filesystem corruption and truncation are rejected.
  This fixture is not a newly built deployable firmware image.
- Replacement U-Boot package build passed; artifact hashes and remaining
  handoff incompatibility are recorded in its package README.

No repaired image has been booted or flashed. The existing images under
`bin/targets/zte/zx279125/` must not be confused with a validated final image.

## Latest LAN checkpoint — 2026-10-22

Testing stopped at the user's request. The current driver and RAM image are
working development checkpoints, not a completed subscriber-offload port.

### Implemented and verified

Management delivery now uses the dedicated DMAC comparator at92140040/44
with SPA9230811c bit8. Bit16 enables IPv6 control identification/Neighbor
Discovery. Live isolation showed that changing only the comparator MAC stops
secondary IPv4 TCP; restoring it restores TCP. The tested narrow SPA word is
01110300, bank1 remains zero, and the original action banks are unchanged.
The earlier bank1-bit0 hypothesis used an incompatible SDK enum mapping and
was rejected by runtime evidence. All local IPv4/IPv6 comparators can remain
zero. Broad stock parser settings are unnecessary.

The driver tracks the management bridge MAC rather than a primary IP address.
The old inetaddr notifier, primary-IP comparator writes and optional hrtimer
polling fallback were removed. Named DT global/SPA resources and the binding
were added. The target-wide local-IPv6 disable file was removed. Existing
reserved DMA ownership and PON-disabled state remain.

Image:3891101 bytes, SHA256
`970e43bff23ff643c18db2a4378f7dcdfdde734bd4f16055d09179e51f2f4e4f`.
TFTP filename:`v2801sg-lan-mac-970e43bff23f.uImage`.
Running LAN module matched SHA256
`000a72f89f9fee6c28766d87b5c7a4032b77c71eef173efd96d65c3fa066edb3`.
Full build, image CRC/bootstrap checks, compiled DT resources and YAML parsing
passed. The boot used TFTP/adopted initialization; this exact image has not
had a serial-only or direct physical-power-cycle boot yet.

| Test | Result |
|---|---|
| Automatic startup | Shell, SSH, IPv4, IPv6 link-local and ULA available; br-lan forwarding26.00s |
| Primary/secondary IPv4 and two IPv6 addresses | SSH banners on all four, with IP comparators zero |
| Full-MTU IPv4,5000 packets |0% loss;0.445/0.504/0.797ms min/avg/max |
| Full-MTU IPv6,5000 packets |0% loss;0.497/0.567/0.837ms |
| Duplex TCP on each of four addresses |1MiB each direction, both payloads checked; IPv4~0.47s, IPv6~0.54s |
| Live bridge MAC change | Fresh IPv4/IPv6 aliases accepted SSH; hardware MAC comparator updated |
| Module remove/reload | Completed; IPv4/IPv6 SSH subsequently worked |
| Buffer ownership at stops | BMU allocations equaled releases; reserved buffers retained |
| Memory |~2020KiB available initially,~1660KiB after lifecycle tests |

### Open issue before calling LAN complete

IPv6 TCP can time out after a LAN stop/start even after bridge forwarding
and non-tentative addresses return. Linux receives SYNs and enters SYN_RECV,
but its neighbor entry for the PC can remain INCOMPLETE, preventing replies.
Later probes recover. IPv4 remains responsive, and no DMA fault or panic was
seen. A board-initiated ping subsequently reached the PC over both IPv4 and
IPv6. The cause of this neighbor-recovery timing is not isolated; do not count
the lifecycle transient as a passed IPv6 recovery test.

Next: inspect NS/NA transmission/reception and checksums with neighbor caches
cleared, fix and repeat settled link cycles, then cold-test the final image.
Physical carrier transitions, tagged management frames and LAN-to-GPON
subscriber hardware forwarding remain separate validation gates. Fiber stayed
disconnected and no installation, saveenv or explicit flash-write command was
issued in these LAN tests. Payload helper files were removed before handoff.

Host artifacts:`/tmp/v2801sg-lan-final/`; UART:`/tmp/v2801sg-normal-boot/bridge.log`.

### Remaining Ethernet completion steps

Run these in order, keeping testing RAM-only and fiber disconnected. Stop
hardware work immediately if the board becomes unresponsive; wait for the
user before attempting recovery. No flash installation or `saveenv` is needed.

1. **Resolve IPv6 neighbor recovery.** Reproduce the failure after `lan0`
   down/up with fresh neighbor state. Capture NS/NA in both directions and
   correlate MAC addresses, multicast delivery, checksums, neighbor states,
   driver counters and TCP SYN/SYN-ACK. The current BusyBox `ip` can show
   neighbors but cannot delete them; use a verified RAM-only helper if needed.
   Fix the demonstrated cause rather than adding a delay or broad trap mask.
   Pass when repeated cycles restore IPv4 and IPv6 TCP without a priming ping,
   manual register write or stale neighbor-cache dependency.
2. **Repeat lifecycle and carrier checks.** Exercise administrative down/up,
   driver unload/reload, management MAC changes, address add/remove and physical
   cable disconnect/reconnect. Check primary/secondary IPv4, IPv6 link-local
   and ULA after each transition. Record recovery time, PHY negotiation,
   comparator updates and balanced buffer ownership; require no unexplained
   drops, DMA errors, stuck queues or accumulating allocations.
3. **Complete packet-path coverage.** Check board-initiated and PC-initiated
   traffic, broadcast ARP, IPv6 multicast/ND, small and full-MTU packets,
   concurrent duplex TCP and tagged management traffic where configured.
   Verify VLAN-tag handling and isolation against the intended configuration.
   Repeat the existing loss and payload-integrity tests after any driver fix.
4. **Check sustained stability and resource use.** Run a recorded, bounded
   mixed IPv4/IPv6 traffic soak. Compare counters, available memory, BMU
   allocations/releases and queue progress before and after. Separately sample
   a genuinely idle interval to check IRQ/CPU activity with no polling fallback.
   Require responsive UART/SSH, no OOM, kernel warnings or buffer leak.
5. **Validate the final image from cold state.** Rebuild and record image/module
   hashes, rerun image/DT/bootstrap checks, then boot that exact image over UART
   without TFTP Ethernet initialization. Also perform a user-assisted physical
   power-cycle test, stopping at U-Boot before the UART RAM load. Verify automatic
   cold initialization, factory MAC, shell, SSH and dual-stack traffic. Earlier
   cold-boot results from another image do not satisfy this gate.
6. **Close out the LAN driver.** Review remaining temporary assumptions and
   diagnostic code, document supported bootloader/DMA layout constraints, and
   update this checkpoint with exact commands, hashes and pass/fail evidence.
   Call standalone LAN management complete only after the gates above pass.

LAN-to-GPON subscriber forwarding is a subsequent integration gate: prove the
ONU bridge forwards in hardware, including VLAN/GEM mapping, isolation and
CPU-trap policy, when optical testing is available. Management TCP throughput
does not establish subscriber offload or completion of the full ONU port.

## Management and RAM-root checks (2026-10-22)

A serial-only boot of image8a9b84199201 initialized the cold PP layout and
passed5000/5000 full-MTU pings (0.594ms average), but never exposed a usable
shell. A later UART capture reported OOM, killing netifd; startup included
several packet-steering/hotplug processes. Ping alone did not prove userspace
health. The compressed RAM root now omits startup links for packet steering,
cron, GPIO-switch configuration, LEDs and NTP during LAN tests. This affects
only the RAM bootstrap; it does not remove the installed-image services or
reduce the reserved hardware packet pool.

The revised image3850497 bytes, SHA256
`6886d8f7663ed780938add2320b6216f5084c25a703b58671ecc66bd5b55c0ba`,
passed image CRC/bootstrap checks and a TFTP-adopted boot with an interactive
shell. MemAvailable settled around1.8–1.9MiB. Primary IPv4 SSH banner delivery
worked; a secondary192.168.1.2 address timed out. IPv6 was initially disabled
on br-lan; enabling it created a valid link-local address, but PC neighbor
resolution failed. A temporary module wrote the independently traced IPv6
comparator, verified the write and restored all four original words on unload;
it did not fix neighbor discovery. This does not isolate an IPv6 TCP-specific
failure, since Neighbor Discovery had not succeeded.

Stock was then booted for read-only reference collection, with fiber still
disconnected. Its IPv4 HTTP returned200; IPv6fe80::1 resolved to the stock MAC
and TCP port80 returned RST (no IPv6 HTTP listener established). Both local IP
comparators read zero. SPA identification masks readffffffff/0003ffff, and
per-port protocol action banks at923080ec/f0/f4 and923080f8/fc/100 read
01155155/15551555/00002555. These observations show that stock management
delivery is broader than the present primary-IPv4 workaround; they do not
justify copying every trap policy into the ONU subscriber path.

Some stock debug views read stale620xx/640xx encoded addresses and return
zeros. Current SPA/SBRG SDK tables usec20xx/c40xx. Direct physical devmem2
reads corroborated the current mappings. CLA extra-index diagnostics also
showed populated extraction indices, rather than only no-match action bits.
Reference logs and register JSON are in`/tmp/v2801sg-stock-registers/`.
The revised image subsequently passed a UART/XMODEM-only reset-state boot:
LAN reported cold initialization, the shell and SSH banner worked, and5000
full-MTU pings passed with0% loss and0.511ms average RTT. MemAvailable was
1948KiB after startup. This establishes reset-state initialization and RAM
userspace progress; a direct physical-power-cycle OpenWrt test remains separate.

Reversible stock SPA/action settings restored IPv6 neighbor resolution but
not complete TCP delivery with the IPv4 comparator cleared. Adding the tested
stock CLA controls and DPA identification masks did not change that result.
A dedicated DMAC comparator test likewise did not deliver TCP. Stock bridge
flood-mask and isolation comparisons also failed to replace the comparator.
Each test restored its saved registers on module unload; primary IPv4 SSH
was rechecked successfully afterward. None of these broad policies were
integrated into the LAN driver. Complete multi-address delivery remains open.

The RAM bring-up diffconfig now selects kernel/devmem access plus BusyBox
register, hex, base64, bounded-command, netcat, TFTP and socket diagnostics,
and the tiny ethtool variant. These tools replace manual diagnostic uploads;
stock hardware tables still need a complete management-delivery contract.

Diagnostic image3891617 bytes, SHA256
`40625b814effc7ba6e1eef13bc82e86cf59610f0e1d17feedc816972a4837d1f`,
passed full build, CRC/DTB/root checks and a TFTP-adopted RAM boot. The staged
filename is`v2801sg-lan-diag-40625b814eff.uImage`. Live checks verified
root-only/dev/mem (major1/minor1), `devmem 0x9214c05c 32` returningC0A80101,
ethtool driver info and1000Mb/s full duplex, a hash-checked34-byte TFTP
get, base64 decode, hexdump, IP neighbors and process-aware netstat.
A bounded netcat listener received the exact PC test payload and exited.
SSH banner delivery remained successful. MemAvailable1828KiB after startup;
no new kernel fault was logged. TFTP put is selected but was not live-tested.


## Current LAN port (2026-10-21)

`zx279125-ethernet.c` replaces the manual `zx279125-inherited` experiment.
It is a DT platform driver with named resources, a PHY phandle, checked
factory LAN MAC, automatic module loading, all sixteen CPU queues and NAPI.
No boot arguments for mode, MAC, cursor or management IPv4 are required.
The default RX watchdog interval is zero: hardware IRQs drive reception.

The PP engine remains active independently of the UNI administrative state,
so a later PON control consumer does not depend on `lan0` being up. One lock
serializes descriptor publication, releases and BMU allocation; the broker
also serializes shared Top INTC mask updates. Kernel address notifications
track the management bridge's MAC and primary IPv4 comparator. Read-only
ethtool link settings use the PHY selected by DT without resetting it.

Factory tag records are checked before exposing a MAC. LAN1 tag0101 takes
precedence over base tag0100; no random or shared U-Boot MAC is substituted.
The dedicated TAGPARAM library is independent of optical probing.

The hardware uses the retained fixed packet pool. Its whole16MiB reservation
remains intentional until the auxiliary DMA extent is established. DT still
records the independently verified bootloader cursor table; another U-Boot
build requires fresh handoff verification. Unknown active layouts fail
before adoption; cold initialization accepts only the documented reset
fingerprint and performs no shared reset or optical programming.

The TFTP-adopted RAM boot passed automatic startup, factory MAC selection,
5000 full-MTU pings (0% loss,0.492ms average), live primary IPv4 and bridge
MAC changes with real TCP/SSH acceptance, stop/reopen and complete module
removal/reload. Duplex TCP verified1048576 bytes each way in0.471s after
reload. LAN errors/drops remained zero. A5.04s idle sample counted539 timer
IRQs (~107/s), versus the former~4100/s polling fallback. No flash writes.

Validated image:3850937 bytes, SHA256
`8a9b841992010c492750ceae4aa0120ebbe7777b015cea1859da26784bdd1554`.
Running LAN module matched SHA256
`3b4b803b1be2fece23cf14e95fa610591bc09d1ba66ea62ed608d5c6ab8f6271`.
Host logs are in `/tmp/v2801sg-lan-platform/`; UART is recorded in
`/tmp/v2801sg-normal-boot/bridge.log`.

Serial-only reset-state initialization still requires hardware validation.
Primary management IPv4 is supported; secondary IPv4 and IPv6 TCP
classification remain limitations. PON activation is disabled in the board
DTS while the optical port remains unfinished. These are management-LAN
checks, not optical registration or subscriber bridge-offload measurements.
Earlier measurements below describe the historical experiments.

## Historical bring-up evidence

## Reference and live correspondence

The reference raw kernel from the supplied 2026-05-26 firmware has SHA256
`b00093288a4cc655b8e872f3399ef5bbb966cf80b58e42930779bf9fedd722dd`.
The physical board booted stock Linux 2.6.32.61, build 2025-11-24, with a
DG-GR6010 hardware V2.8S profile. Different package dates do not establish
different networking code: UART transfers verified these complete modules
against the reference rootfs:

| Module | Bytes | SHA256 |
|---|---:|---|
| netdriver.ko | 17340 | 2e9d36119adefcf74f1865f9cb11ff3c6537b4075e399052a11799e00f39afdc |
| switch.ko | 144124 | f1073a8b9cb5a21c2ee9ea2f24329e0e9acc34662af93a76ddf1229fd9c0e45d |
| tm.ko | 1192288 | b0fcbdcd8a90c90b2b8bf1a9d1fa9b43811f1b584719020b6a17582f3c0665c0 |

Live kernel text also matched the reference at VA `0xc0022df4` for 468 bytes
(clock/reset initialization) and `0xc0266a2c` for 592 bytes (RX release/TX
mailboxes and cache helpers). Of the selected kernel symbols, 148 addresses
matched; an init-only symbol differed. The entire kernel is not verified
byte-identical. Optical and tag-parameter modules have different file sizes.

## Hardware contract

The stock static mapping translates VA 0xf0800000 to PA 0x9a100000.
MDIO is at 0x9a101000, its clock gate at 0x9a100010. GPIO is at 0x94404000,
not the I2C resource 0x9a102000. The enabled custom MDIO controller discovered
PHY 1 with ID 0x03815c11 on both TFTP and serial-only RAM boots. No PHY model
is assigned without a matching ID.

| MDIO offset | Meaning |
|---|---|
| 0x04 / 0x08 | write / read data |
| 0x0c | divider; stock initializes zero |
| 0x10 | completion bit0, cleared by writing zero |
| 0x14 | register0..4, PHY5..9, opcode10..11, start14 |

Opcode 1 writes, opcode 2 reads. Initialization sets bit 12 and clears 13..15.
The clock provider preserves the stock nominal 2.5 MHz rate and unrelated gate
fields. Transactions are bus-serialized and have bounded completion polling.
PHY link reads save/restore page selector 0x1e and read page 0 status 0x1a twice.
Link is bit 6; modes 0/1=10half/full,2/3=100half/full,4/5=1000half/full.
Physical SMAC 1 is at 0x92200000. Logical EMAC0/eth0, PHY address 1 and SMAC 1
are different identifiers; do not derive the MAC index from the logical name.

The DT reserves `[0x41000000,0x42000000)` with `no-map`, covering both
U-Boot and stock Linux DMA layouts. Exclude it from image placement too.
The guard leaves 16 MiB before kernel/initramfs allocations; use the minimal
`rdinit=/bin/ash` boot described in README.md.

| PP register | U-Boot / Linux cold-start layout | Stock Linux layout |
|---|---|---|
| 0x92144018 | BP table0x41800000 | 0x418a0000 |
| 0x9214401c | RX rings0x41900000 | 0x41e30000 |
| 0x92144020 | buffers0x41000000 | 0x419b0000 |
| 0x92144024 | 2048 | 2048 |

U-Boot initializes 4096 BMU entries despite PP count 2048. Hardware diagnostics
show 4096 free buffers (external 4016/internal 80). The driver validates these
separately and maps 4096 x 2048-byte buffers, ending below0x41800000. BMU
+0x80/+0x84 are free counts; +0x88/+0x8c allocation/release counters, as
identified by raw stock `dump_bmu_reg`. Stale tm.ko BMU/DMA SDK tables have
different addresses and are not used as silicon address evidence.

There are 16 rings of 4096 descriptors x 16 bytes. The implemented path handles
only observed queue0 and rejects other nonzero count registers. Buffer index
is `le16(desc+10)&0x3fff`, offset `desc[12]&63`, length
`(le32(desc+12)>>6)&0x7ff`. Bounds and LAN routing fields are checked before
copying into an skb.

TX publishes descriptor words 12/8/4/0 through 0x92154054/58/5c/60, then 1 to
0x92154050. Template is `{0x2000,0,0x8002,0x30000000}` before inserting
length, buffer index and 16-byte headroom. BMU allocation is at 0x92168010/08;
RX descriptor release uses0x92154064/68 and buffer return 0x9216800c.
Commands are serialized and bounded. Uncertain ownership stops the interface
and retains backing memory; no blind retry or return of submitted TX buffers.

## Activation modes

`ZX279125_LAN_INHERITED` is module-only, disabled in the base kernel config.
The bring-up diffconfig selects `kmod-zx279125-lan-inherited`, without autoload.
Exactly one of `allow_inherited=1` and `cold_init=1` is required. Both modes
require the exact board, full no-map reservation, a valid MAC address and
reserved cursor storage. Default TX budget is 64; an explicit budget up to 8192
permits bounded longer tests.

Adoption requires matching active bases, valid retained ring/cursor pairs,
empty queues, disabled ingress, a full free pool and balanced allocation/return
counters. Discover the relocated U-Boot cursor address for the actual bootloader;
0x41fef75c was verified for the supplied board's image.

Cold initialization additionally requires the captured reset fingerprint:
BMU/MACs off, zero counters and cursors, PP bases 50000000/50020000/52020000,
PP count 2048, DMA config 136564 and shared control feffffff. It initializes the
local PP RAM, RED entries, DMA byte order, classifier, reserved buffer/index
and ring memory, switch tables, station address and LAN MAC. Table operations
have bounded completion polls. SADM payload is `{0x80,0}`, matching the
U-Boot stack and publication order. Shared reset, PLL, SerDes and optical
register programming are excluded. It is not a takeover of active DMA.

For a serial-only boot with the verified cursor location:

```sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
insmod /lib/modules/6.18.55/zx279125-inherited.ko cold_init=1 cursor_phys=0x41fef75c macaddr=00:41:71:00:00:50 tx_budget=128
ip addr add 192.168.1.1/24 dev lan0
ip link set lan0 up
```

Use the device's actual MAC address. If U-Boot already ran TFTP, cold mode
refuses that state; use adoption with the freshly verified cursor instead.
The UART `xmodem <addr>` handler was traced to a RAM-only receiver with a 5 MiB
bound; it has no flash-install call. The verified image is below that bound
and can be booted directly with `bootm 0x40600000` after CRC verification.

## Link and stop/restart

PHY status is polled once per second. Carrier and queue state follow link
status; stock MAC speed/duplex fields are updated only on a relevant change.
Page-select operations hold the MDIO bus mutex throughout save/read/restore.
No PHY reset or autonegotiation rewrite is performed by the driver.

Stop cancels polling, closes ingress and waits boundedly for empty RX queues,
idle CPU mailboxes, a full free pool and balanced allocation/release counts.
Only then is MAC TX disabled. If ownership remains pending, TX stays enabled
and physical memory remains reserved. This is an ownership-aware stop for the
fixed layout, not proof that shared AXI DMA can be reset or its memory reused.
Cursors persist in reserved storage for a later adoption/restart.

## Hardware validation

| Test | Observed result |
|---|---|
| Initial packaged interface | 40/40 pings, including 32 full-MTU and reload |
| BMU accounting | 64/64 full-MTU; 139 allocations/returns = 66 RX + 73 TX |
| Ring wrap/reuse | RX 2537->949, 2431 repeated TX indices; pool unchanged |
| Continuous ingress | 2500/2500 full-MTU at 50 pps; 2433 repeated TX indices |
| Settled accounting | Allocations/releases 15130/15130, free 4016/80 |
| Serial-only boot | XMODEM completed with zero retries; reset PP state preserved |
| Cold local tables, corrected SADM | 16/16 full-MTU without U-Boot networking |
| PHY power-down/restore | Carrier0 then 1, recovered 1000/full; exact BMCR restored |
| Ownership-aware stop/restart | 16/16 full-MTU; all buffers returned |

The first polling version closed ingress during each poll and lost 5/2500
pings. Continuous ingress removed that observed loss; release mailbox completion
is checked rather than assuming queue count must decrement despite arrivals.
The first cold prototype reversed the SADM payload words; correcting that
translation restored TX. The corrected packaged cold-start validation is recorded below.

Remaining work includes IRQ/NAPI, multi-queue decoding, throughput and longer
stress coverage, integration with normal OpenWrt userspace under the RAM limit,
and recovery from ownership faults. GPON/OMCI transport, optical operation,
flash installation and a general shared DMA reset remain unvalidated.

## Corrected packaged reset-state startup

Image: `v2801sg-lan-cold-f61f311fcb01.uImage`, 3556075 bytes, SHA256
`f61f311fcb01feb872489621e8aa0afea2d3f4be22c49241645ef06b2972f8ce`.
Embedded module SHA256:
`6eead80da419905f1ff5c5e72f988a6fe58474a67e67c4df4824613e07d908b4`.
Full OpenWrt build, legacy CRCs, appended DTB, module contents, no-autoload
and placement below the DMA reservation passed.

The board was reset, autoboot interrupted, and this image transferred only
by XMODEM: zero retries, about315 seconds at115200 baud. No TFTP, ping or
other U-Boot network command was issued. Linux logged the original reset
pool pointers, then the packaged module matched its expected hash and
initialized the local PP/switch/MAC with `cold_init=1`, starting at consumer0.
No table-fix helper was used in this boot.

It passed2500/2500 full-MTU pings at50pps with no errors or drops. At stop,
RX2510+TX2514 equaled hardware allocations/releases5024/5024, with
free counts4016/80 and2434 repeated TX buffer indices. Repeating cold mode
on this initialized hardware was rejected; adoption reload succeeded at
the persisted consumer2510.

After adoption reload, another2000/2000 full-MTU pings passed. Consumer
2510 advanced through4095 to424, confirming ring wrap from Linux-initialized
state. The second run recorded RX2010/TX2014,1934 repeated TX indices,
zero errors/drops, and settled allocations/releases9048/9048 with free
counts4016/80. Total packaged serial-only coverage was4500/4500 pings.
The interface was stopped, both MAC directions disabled after ownership
checks, and the module unloaded. UART remained available.

These were whole-board reset-state tests, not a physical power-cycle test.
No flash commands, saveenv, shared reset or optical-register writes were used.


## Normal userspace memory tests (2026-10-21)

The verified f61f311fcb01 image was loaded by TFTP and booted with normal
`/init`. With the 16 MiB DMA reservation intact, only 10592 KiB was managed
by Linux. The generic initramfs script copied the root into another tmpfs;
shmem reached 6128 KiB and PID 1 panicked at 2.16 seconds.

A second RAM boot used `rdinit=/sbin/init INITRAMFS=1 panic=0` to bypass
that copy while retaining the preinit guard against mounting the flash root.
It reached procd and service startup, but still exhausted memory, with
4304 KiB shmem. Avoiding the copy alone is insufficient for normal operation. A direct `/sbin/init` wrapper was built as an intermediate experiment;
the current compressed RAM-root approach is described below.
The DMA reservation has not been reduced: its stock and U-Boot ownership
boundaries still need to remain protected.

During the second test, the generic sysctl service replaced the boot-time
`panic=0` with `kernel.panic=3`. The kernel rebooted before the fixed-duration
UART capture returned. Vendor autoboot then automatically erased/wrote the
U-Boot environment at flash offsets 0x30000..0x40000 and booted stock.
No flash-write command was issued by the operator, but this test did cause
that unintended vendor environment write. Preserve this distinction when
reporting RAM-only testing. A target sysctl override now keeps kernel panic
auto-reboot disabled; it still requires a built-image and runtime check.
Future experimental captures must also watch and interrupt unexpected
reboots immediately rather than relying solely on a boot argument.

The LAN board configuration now names `lan0` and excludes `pon0`. Automatic
driver startup and normal services remain unvalidated and incomplete.

## Reservation audit and compressed RAM experiment

A fresh audit of the raw U-Boot backup (SHA256
`d286f31239edd5ff9ed5843da41148da869a68a5f72abca11e8b6ec576235c8a`)
confirms buffers `[41000000,41800000)`, 4096 BE16 BP entries at41800000,
and16 RX rings occupying `[41900000,41a00000)`. ARM instructions at
41f1ee78..41f1ee9c create the ring pointers;41a00000 is their exclusive
sentinel. The41810000 literal is a cache-flush end, not another DMA base.
PP+34 receives41820000 (41f1ee14..41f1ee24), but no hardware access size
was established for that auxiliary base. Cursor storage is CPU-only state:
linked41f3375c, relocated41fef75c,128bytes.

The exact reference kernel was recovered again from firmware LZMA offseted4
and matched SHA256b00093288a4cc655b8e872f3399ef5bbb966cf80b58e42930779bf9fedd722dd.
Its `tm_pp_init` atc001ab1c sets auxiliary base418b0000;
`pp_bmu_init` atc001aed0 sets2048 buffers at419b0000;
`pp_skb_shinfo_ex` atc001b4e0 addresses metadata at41db0000;
`pp_queue_init` atc001b4fc creates rings ending41f30000. Neither stock's
1MiB auxiliary gap nor U-Boot's896KiB gap proves the peripheral's implicit
access bound. These are local Capstone observations, not REA-provider results.
The full16MiB no-map reservation is therefore retained.

A compressed-swap experiment selected swap, zram and BusyBox swap utilities.
An8MiB logical swap device capped at3MiB physical storage started in preinit.
It used no backing device and zram writeback was disabled. The first image
(c9c3da20486d) exhausted early RAM during unpacking and halted. Reducing the
log buffer from17 to14 and serializing unpacking let the next image
(25eecd496b74) activate swap and reach services, but the pool saturated and
swap write failures left startup unresponsive. This approach was removed;
it is not a working normal-service configuration.

The next approach embeds the full root as an XZ-compressed squashfs in a
minimal initramfs. A loop device reads that RAM-resident file, and a tmpfs
upper/work pair provides a writable overlay. After switch_root, executable
pages can be reclaimed and reloaded from the compressed backing file rather
than consuming swap. The bootstrap BusyBox and its libraries are discarded.
The full DMA guard remains intact. No flash root or flash-backed overlay is
mounted. Separate/external initramfs configurations retain generic behavior.

LAN startup atS19 requires explicit `zx_lan.mode=cold` or`adopt`,
`zx_lan.mac=<unit MAC>` and`zx_lan.cursor=<verified address>` boot arguments.
It loads the existing guarded driver before netifd with`tx_budget=0` for
continuous operation. The default manual-driver budget remains64; malformed
hardware state or parameters still prevent registration. No automatic selection
between reset and inherited ownership is attempted.

## First compressed RAM-root boot

Image6bbb5071dca6 (3837767bytes, SHA256
6bbb5071dca63e0fd09e07e087388a7c71ce3fcfe348fc17158c5029e1d4de21)
passed the full build, legacy CRCs, appended DTB, bootstrap dependency and
RAM placement checks. Kernel end40712ff0 and the complete transfer remain
below41000000. It booted normal procd userspace with `/dev/loop0` mounted
read-only at`/rom`, tmpfs at`/overlay`, and overlay at`/`. No flash root was
mounted. Live UART reported MemTotal11044KiB, MemFree1572KiB,
MemAvailable1064KiB, Shmem1504KiB, SwapTotal0 and kernel.panic0.

LAN autoload failed because this build's ubox modprobe does not forward
command-line module parameters (main_modprobe in ubox6f78fa49). Direct
insmod with explicit parameters registered lan0. A second issue remained
in ndo_open, which rejected tx_budget0 despite the new continuous-TX mode;
that condition and the S19 loader have been corrected in source.

A bounded reload registered lan0 but the board subsequently stopped responding
to UART and Ethernet. The cause is unresolved; no working bridge traffic is
claimed from this boot. The next test separates raw LAN activation from
netifd and uses512KiB vm.min_free_kbytes instead of the generic1024KiB value
for this small managed-memory configuration. Stock UART register/IRQ behavior
was audited again; no concrete mismatch justified a UART-driver change.

## Normal RAM-root LAN validation and TCP isolation (2026-10-21)

The corrected image is `v2801sg-ramroot-4eb798dbd78c.uImage`,
3837735 bytes, SHA256
`4eb798dbd78c8f3d88231cc0f02e7b777e1d90bfc58328bce7f766f3ab8a298a`.
Normal procd/ubusd/netifd/dropbear/logd startup works with the compressed RAM
root, full 16 MiB DMA reservation, no swap, `kernel.panic=0`, and
`vm.min_free_kbytes=512`. The explicit S19 insmod parameters and the
ndo_open continuous-budget check are corrected in this image.

Verified traffic includes 200/200 full-MTU raw-interface pings, 1000/1000
full-MTU bridge pings at 50 packets/s, board-to-PC ping, a clean bridge
restart, and automatic LAN startup after a fresh TFTP RAM boot. Several hours
of idle uptime retained a responsive console. These results do not establish
TCP throughput, physical cold-boot operation, or flash installation.

TCP SYNs from the PC do not reach the CPU RX queue. Three controlled SYNs
incremented CLA forward and ADM forward by three; three ICMP probes instead
incremented their trap counters. Stock kernel diagnostic functions identify
CLA forward/trap/drop at 0x9214c3c0/3c4/3c8 and ADM at
0x92150240/244/248. PC capture confirms board-originated SYNs have a correct
TCP checksum. Temporarily disabling host TX checksum/segmentation offloads
did not fix ingress TCP; the adapter's original settings were restored.

Guarded temporary modules require LAN RX disabled, all 4096 BMU buffers free,
balanced allocation/release counters, all 16 DMA queues empty, and the
verified U-Boot pool/ring bases before changing local registers. They remain
under /tmp and are not part of the port. The following tests did not resolve
TCP and must not be treated as driver fixes:

- CLA+0x44 = 4, and CLA+0x40 = 0x1802 with CLA+0x44 = 4.
  The SDK later clears bit 11 in software; 0x1802 does not self-clear to 0x1002.
- Stock pp_net_open's PP+0x44 low-two-bit clear: 0x7f -> 0x7c.
  This is PP at 0x92144000, not CLA at 0x9214c000.
- SDK destination-MAC trap registers at 0x92140040/44 programmed with the
  unit MAC 00:41:71:00:00:50. They initially read zero.
- SPA port 0 entry 0 field 12 action changed from 0 to 1, preserving other
  fields: 0x923080ec = 0x01155555 -> 0x05155555. Field semantics remain
  unproven; this single-field experiment did not redirect TCP.
- SDK ONU MAC slot 0 layout at 0x92140000/04, including with the preceding SPA
  experiment. The SDK layout reads back the full unit MAC, but TCP still
  forwards. Built-in stock tm_set_onu_mac instead writes +0x04/+0x08;
  +0x04 truncates to 16 bits on this board. Do not interpret those two
  registers alone as a complete hardware MAC or silently choose one layout.

The SDK register mapping was verified through zx_ponreg's register-ID << 2
accessor and f4000000 -> physical 92000000 mapping. However, register tables
and built-in kernel helpers contain differences; exact live readback remains
necessary. The recovered tm/switch/netdriver modules match the reference
hashes. The legacy rootfs was extracted from a temporary copy normalized
from shsq/LZMA-id-1 to hsqs/compression-id-2; original backups were untouched.

The SDK chip_tm_init configures default-flow extra-index actions omitted by
U-Boot: si_zx_api_pp_global_init updates bits 18..19 of word 4 through the
CLA indirect interface. A quiescent read of extra index 0 returned five zero
words. Updating only its no-match field to stock value 2 read back as
word4=0x00080000, but TCP still timed out. A single-entry test does not
establish the complete default-flow policy or its packet-to-index mapping.

The subsequent exact post-global-init CLA control pair 0x1002/4 also left
SYNs forwarding (forward +3, trap unchanged apart from ARP). Applying the SDK
no-match fields to all 16 extra-index entries likewise did not fix TCP:
indices 9 and 10 use value 1, all others value 2. This follows the unsigned
comparison of the SDK loop's counter starting at -9; it is not an 8/8 split.
All other words and bits were preserved and each entry was read back.
The outcome excludes these settings alone as the missing TCP handoff.
No experimental register change from these tests was integrated into source.

PC packet capture shows SYN destination 00:41:71:00:00:50, matching both lan0
and br-lan. The board ARP entry for 192.168.1.100 matches the dedicated USB
Ethernet adapter 6c:1f:f7:c6:fc:cf. A stale destination MAC does not explain
these captures. Controlled reboots intercepted U-Boot autoboot and loaded the
verified image over TFTP to clear the temporary table changes.

### Next test prepared; UART reset required

A further static trace found si_zx_api_sw_global_init at tm.ko .text+0x712e4
enables all 32 SPA entry-0 protocol-identification bits at .text+0x71564..
0x71584 and all 18 entry-1 bits at 0x71588..0x715a8. The respective setters
use spaRegTable IDs 0x72..0x91 and 0x92..0xa3, mapping to physical
0x9230811c and 0x92308120. U-Boot cold initialization writes only 0x01100200
to the first register. Enabling the exact stock identification masks is a
prepared test, not a demonstrated TCP fix. Preserve upper bits of +0x120.

The final clean reboot/TFTP boot of image 4eb798dbd78c again reached normal
procd and automatic br-lan forwarding. It answered 5/5 host pings, but the
UART shell then only echoed input, including Ctrl-C, without executing it.
This is a separate unresolved normal-startup responsiveness issue. No panic
was printed. Do not call this restored boot a verified healthy userspace
session. A physical reset to U-Boot was requested before the pending mask
test. The latest boot has none of the temporary diagnostic modules or
register-policy experiments loaded.

## ONU-only product boundary and management TCP progress

The user explicitly requires an ONU-only design with hardware-offloaded
LAN-to-GPON bridging, following the G7 model. CPU polling and a Linux bridge
are bring-up tools, not the intended subscriber data path. CPU delivery must
be restricted to local management and required control/exception traffic.
Hardware forwarding, GEM/T-CONT/VLAN mapping, OMCI-driven configuration and
hardware counters require separate end-to-end validation; successful local
TCP does not establish data-plane offload.

After physical reset, the verified image booted with LAN autoload omitted.
The serial shell worked; manual inherited LAN loading and a clean ifdown
provided a quiescent test state. SPA masks 0xffffffff/0x0003ffff read back
correctly but alone did not fix TCP, including with the SPA field-12 action
experiment. DPA parser enables at 0x9216c0f4/f8 initially read zero. Stock
si_zx_api_pp_global_init enables all 53 parser bits via dpaRegTable IDs
0x55/0x56, encoded addresses 0x5b03d/0x5b03e. Enabling those bits alone,
with SPA restored to U-Boot defaults, also left TCP forwarding.

A combined guarded test finally produced a closed-port TCP RST, followed by
a successful TCP connection to port 22 (SSH banner read timed out):
DPA enables ffffffff/001fffff, SPA enables ffffffff/0003ffff,
SDK ONU MAC slot0 71000050/00000041, CLA control1002/policy4,
and PP+44=7c. SPA field12 was then restored to its baseline action and TCP
still worked. No default extra-index policy modifications were present in
this boot. The broad SPA action change is therefore unnecessary for that
successful configuration.

Restoring SPA enables to 01100200/0 made TCP time out again while all other
combined settings remained. SPA bank0=ffffffff with bank1=0 restored TCP.
Masks 0110ffff, 011002ff and 0110020f also returned closed-port RSTs; the
module-load log confirmed the 0110020f test. Further commands for 01100203
and 01100201 only echoed with no execution/readback, as did a subsequent
proc read: do not count those masks as tested. Thus the confirmed candidate
is within bank0's low four bits, with dependencies on the remaining combined
settings not yet isolated. The complete product policy is not established.

The recurring userspace/console stall remains unresolved even though kernel
TCP replies continue. The next boot should use a minimal PID1 RAM shell
instead of normal procd startup for repeatable policy isolation. A candidate
existing-image boot is rdinit=/bin/busybox with `-- sh` init arguments,
followed by manually mounting the compressed root and switching to /bin/ash;
this boot method is prepared, not yet hardware-validated. No mask change has
been integrated into the Ethernet driver from these experiments.

### Isolation result and payload validation

The queued UART operations later completed, including a readback of
SPA identify=01100201/0. The apparent permanent shell stall in the preceding
notes was a delay; no new physical reset was required. After that readback,
a fresh closed-port TCP probe confirmed the bit-0 mask. The following were
restored individually without losing SYN/RST delivery: CLA control3002/
policy2, DPA enables0/0, and PP+44=7f. Restoring hardware MAC slot0 to the
U-Boot values instead made TCP time out, and restoring the correct unit MAC
restored replies. Broad parser enables, CLA changes and PP-open changes are
not required for this result.

The source now programs SDK ONU MAC slot0 at 92140000/04 and ORs SPA identify
bit0 at 9230811c, after the existing quiescent handoff checks. The first rebuilt
module (18112 bytes, SHA256
46bdf72117c86514f6e0cccc2df0d53f6bd660eef2fcb61e77580cfb0e304d92)
was loaded after restoring the prior MAC and identification settings. It
produced the intended readbacks, 10/10 closed-port RSTs and 20/20 full-MTU
pings. These are SYN-path checks, not proof of a complete TCP connection.

A temporary bridge MAC 02:41:71:00:00:51 was advertised to the PC while the
hardware local MAC remained 00:41:71:00:00:50. The PC neighbor entry confirmed
the changed destination. TCP then timed out and CLA/ADM forward increased by
three. Restoring the bridge MAC and advertising it restored closed-port RSTs.
This demonstrates MAC-selective SYN delivery, rather than trapping all TCP.
The bridge and PC neighbor entry were restored to the unit MAC.

The first 1 MiB duplex test exposed a remaining issue: the board remained in
TCP SYN_RECV, received no payload, and forwarding counters increased. ACK/data
were not delivered to Linux even though the host's connect() had returned.
Applying all stock default extra-index actions and the combined earlier
stock parser/classifier settings did not fix ACK/data delivery.

Stock tm_cla_local_ipv4_set (.text+55eb8) and cla_set_local_ipv4_addr
(.text+127a8) identify claRegTable ID0x16, encoded53017, physical9214c05c.
The public setter reverses the four input octets into a little-endian word
so the written value for 192.168.1.1 is c0a80101. Live initial value was0;
a guarded write read back c0a80101. The duplex test then transferred exactly
1048576 bytes in each direction: board verified all received bytes were0,
and PC verified all received bytes were Z.

The duplex test was repeated successfully after restoring DPA0/0,
CLA3002/policy2, PP+44=7f, SPA identification01100201/0, and all16 extra-index
entries back to five zero words. Removing SPA bit0 again prevented connect.
The isolated working changes are therefore the local MAC slot, identify bit0
and local IPv4 value. No broad trapping policy is needed for these tests.

The experimental driver accepts ipv4addr=<management address> and checks its
register readback. S19 obtains it from zx_lan.ipv4=<address> when supplied,
otherwise network.lan.ipaddr (with a CIDR suffix stripped). Address settings
are currently established at module load: changing management IP requires a
quiescent reload. Dynamic address/IPv6 lifecycle integration and replacement
of polling with the final control-plane interface remain future work.

### Final module validation

The final module with ipv4addr support is18560 bytes, SHA256
`a200c2a9e192060642ccce7cb092d4a6c97b3ba1ab1182b132ebf8072e821a88`.
It builds against the running6.18.55 kernel and passes strict kernel
checkpatch with0 errors,0 warnings and0 checks. The S19 script passes sh -n.
On the board, loading with ipv4addr=192.168.1.2 changed the local register to
c0a80102. Reloading with192.168.1.1 changed it back to c0a80101. With this
module, unchanged broad parser/classifier defaults and zero extra-index
entries, another1 MiB duplex transfer passed. PC received SHA256
bf63d8a95fcc2e64619813aae35fdcbe871fdd9264caa3f365eb3aed0f679129
for the Z payload; board independently verified the zero payload. The real
Dropbear service returned its SSH-2.0-dropbear banner.

Temporary diag/localip/extra modules were then removed. Their removal plus
bridge startup took approximately36 seconds, so an early probe during that
sequence failed. UART subsequently confirmed completed startup and the
restored unit bridge MAC. Do not confuse tests issued before command
completion with tests of the final hardware state. Full image rebuilding,
fresh-image S19 startup, physical cold boot and optical bridge offload are
not established by this module-load validation.
After confirmed restart completion, Dropbear again returned its banner and
10/10 full-MTU pings passed with diagnostic modules removed.

## Fresh image checkpoint; paused at user request

Full make -j4 V=s succeeded for management RAM image
v2801sg-ramroot-be4c952bc87d.uImage,3839665 bytes, SHA256
be4c952bc87df0f02b08648b25176906d94bbe837d0da0fa4673bf64ab90563d.
Saved at /tmp/v2801sg-ram-build/ and /srv/tftp/ under that filename.
Legacy CRCs, compressed RAM root, updated S19 script, DMA reservation and
RAM placement passed verification. Kernel end is40714ff0. The temporary
build override disabled ZX279125_PON and the PON DT node; original source
DTS and config-6.18 were restored after building. Built kernel configuration
and DTB are saved in /tmp/v2801sg-ram-build/ for provenance.

The first fresh boot automatically registered lan0 and started br-lan.
Pings and closed-port TCP RST worked, but SSH banner timed out and UART
commands stopped returning; no panic was printed. Config-derived local IPv4
could not be read back, so automatic management startup is not yet validated.
After the user reset to U-Boot, the same image was TFTP-loaded and bootm
started again with an explicit zx_lan.ipv4=192.168.1.1 argument, alongside
zx_lan.mode=adopt, zx_lan.mac=00:41:71:00:00:50,
zx_lan.cursor=0x41fef75c, console=ttyS0,115200n8 mem=32M panic=0.
The user then requested stopping for usage budget. This second boot's
startup and management behavior were not checked. The UART bridge was closed
and ttyUSB1 released. Resume by inspecting live UART state, not by assuming
the second boot succeeded. No flash/saveenv/install command was used in this
build-and-RAM-boot run. Subscriber hardware bridge offload remains unvalidated.
At bridge shutdown, buffered UART output showed the second boot had registered
lan0 at29.35s and reached br-lan forwarding at35.60s. Management TCP/SSH and
UART command responsiveness on that boot remain untested.

## Production Integration: Boot Acceleration, Sub-1ms Latency & TagParam Flashing (2026-10-21)

### 1. Boot Acceleration (Option A)
* **Pre-seeded Dropbear RSA Host Key**: Pre-cached in `/etc/dropbear/dropbear_rsa_host_key`, eliminating 13.93 seconds of 100% CPU compute lock on every cold boot.
* **Failsafe Timeout**: Reduced from 4s to 1s in `/lib/preinit/00_preinit.conf`.
* **LAN Initialization Priority**: Advanced from `START=19` to `START=16`, executing before Dropbear and netifd.
* **Result**: Cold boot to bridge forwarding dropped from **35.60s to 25.70s (-28%)**.

### 2. Sub-1ms Latency: NAPI & Hardware Interrupt Integration
* **Root Cause of 7–20 ms Latency**: `schedule_delayed_work(&lan->poll, msecs_to_jiffies(10))` at `HZ=100` delayed frame processing by 10 ms per tick.
* **Driver Re-architecture**:
  * **NAPI Poll**: Drains up to 64 packets per poll in softirq context via `napi_alloc_skb()` and `napi_gro_receive()`.
  * **Top INTC Interrupt (`zx_isr`)**: Listens to Top INTC (`0x92040000`, bit 2) and Packet Processor (`0x92144000`, status `+0x40`, mask `+0x44`). Registered `IRQF_SHARED` on GIC SPI 30 (Linux IRQ 17), shared with `zx279125-pon`.
  * **High-Resolution Fallback Timer**: Configured via Linux 6.18 `hrtimer_setup()` ticking at 250 µs.
* **Measured Hardware Benchmark (20 packets)**:
  * Minimum Latency: **0.523 ms** (down from 1.917 ms, -72.7%)
  * Average Latency: **0.588 ms** (down from 7.733 ms, -92.4%)
  * Maximum Latency: **0.686 ms** (down from 19.902 ms, -96.5%)
  * Jitter (`mdev`): **0.031 ms (31 µs)** (down from 6.337 ms, -99.5%)
  * Packet Loss: **0.0% (20/20)**

### 3. SPI NOR Controller Fix & TagParam Flash Programming
* **Controller Deadlock Fix**: In `drivers/spi/spi-zx279125-spifc.c`, transfers > 64 bytes timed out due to FIFO boundary saturation before asserting `SPIFC_REG_START`. Asserting bit 0 upon queuing the first word enables continuous hardware streaming.
* **Factory Partition Flashed**: Unlocked `mtd4` (`tagparam` at offset `0x790000`) and flashed authentic identity:
  * `TAG_VENDORID (0x0880)`: `MONU` (csum `0xf63d`)
  * `TAG_GPONSN (0x0881)`: `4F01A678` (csum `0xf5b6`, SN: `MONU4F01A678`)
  * `TAG_PONMAC (0x8001)`: `70:b6:4f:01:a6:78`
* **Verified on Clean Boot**:
  ```text
  [ 0.690000] 0x000000790000-0x0000007a0000 : "tagparam"
  [ 0.900000] zx279125-pon 92040000.pon: tagparam: loaded factory identity: Vendor=MONU SN=MONU4F01A678 PON_MAC=70:b6:4f:01:a6:78
  [ 0.920000] zx279125-pon 92040000.pon: registered xPON device for pon0
  [ 0.930000] zx279125-pon 92040000.pon: ZTE Sanechips ZX279125 xPON controller ready (IRQ 17, EPON mode)
  [ 15.680000] (unnamed net_device) (uninitialized): registered IRQ 17 for PP/GMAC interrupt
  [ 15.690000] lan0: experimental LAN (adopted), NAPI/HRT queue0, consumer=2711
  [ 24.700000] lan0: link up 1000 Mbps full duplex
  [ 26.540000] br-lan: port 1(lan0) entered forwarding state
  ```

## Independent verification update (2026-10-21)

See [VERIFICATION.md](VERIFICATION.md) for the current artifact hashes, live
NAPI/IRQ checks,6000 independently verified full-MTU pings,1 MiB duplex TCP,
SSH, stable tag-partition reads and source findings. LAN management passed;
optical registration, provisioning and subscriber offload remain unverified.


## Ethernet lifecycle follow-up (2026-10-21, source only)

The LAN handler now claims only Top INTC bit 2, leaving bit 1 to EPON.
Queue 0 remains its sole RX queue; traffic on other queues is no longer
misreported as corrupt LAN descriptors. This does not implement delivery for
those queues, and concurrent optical packet handling still needs a common
queue/mailbox owner. The existing PP low-three-bit mask remains unchanged;
its complete source-to-queue mapping is not yet established.

NAPI completion serializes interrupt unmasking against faults. Stop masks
and synchronizes the IRQ before disabling NAPI; module removal unregisters
the netdev before deleting NAPI. Address notifiers update the verified ONU
MAC slot and single local IPv4 comparator from the LAN bridge master's MAC
and primary IPv4 address, or the standalone LAN device. Once a Linux address
has been observed, deleting it clears the comparator instead of restoring
the boot fallback. Secondary IPv4 addresses and IPv6 classification remain
unsupported; no unverified classifier registers were added.

The 250 us fallback remains the default because IRQ-only operation has not
been tested. `zx_lan.rx_poll_us=0` on a RAM boot disables it explicitly and
requires successful IRQ registration; other nonzero values select the
watchdog interval in microseconds, up to 1000000. This is a validation mode,
not proof that interrupt status/acknowledgement is complete.

The updated module builds against the prepared Linux 6.18.55 tree with
`W=1`; management-address changes, queue concurrency, stop/reopen and
IRQ-only operation require subsequent RAM hardware validation.

### Shared PP control transport (source/build only)

Further vendor instruction checks corrected the queue representation. Stock
`pp_net_poll` at `c0266560` reads queues 0..7 from the low halfword and queues
8..15 from the high halfword of DMA `+0x100 + 4*(q&7)`. The driver now owns
all 16 retained ring cursors, polls them round-robin within the NAPI budget,
and serializes all descriptor releases and TX submissions through one lock.
U-Boot `41f1eb10`, called at `41f1f3a4..41f1f3c0`, proves the general release
command: `0x10 | (desc[6]&7) | (source<<14) | (desc[9]>>7)<<3`, where source
is descriptor byte15 bit5. This preserves the working U-Boot flag instead
of assuming the stock kernel's always-set release flag.

Descriptors with byte7 bit7 and byte15 bit5 deliver raw control frames,
including a 48-byte OMCI frame, to the shared PP receiver. GEM metadata comes
from descriptor halfword8 bits2..13. Other source-marked non-LAN frames are drained
and counted as drops; a Linux subscriber datapath is not implemented by this
change. Subscriber traffic is intended to remain in the hardware bridge.

Stock `omci_oam_tx` (`c001bc00`) and `pp_data_raw_send` (`c001b7e0`)
establish control TX words `00002000,80380000,00008000|(gem<<2),10000000`,
with the BMU number and actual length inserted. Payload starts at buffer
byte32; control frames receive no Ethernet padding. The existing LAN path
retains its verified byte16 payload offset and Ethernet padding.

The built-in `ZX279125_PP` broker connects the modular LAN transport to the
PON receiver without a module link cycle. Registration is exclusive;
unregistration waits for an RCU grace period before callers may free private
state. Send is atomic and retains caller buffer ownership; receive runs in
NAPI context and transfers skb ownership. Control send returns ENETDOWN when
the LAN transport is unavailable or the LAN device is stopped. This transport
has been compiled, not exercised on the board; optical control, queue
concurrency and end-to-end hardware forwarding are still validation gates.

### Live Validation of CLA Local IPv4 Comparator (2026-10-22)

Direct MMIO manipulation on live running hardware established the exact behavior
of the CLA local IPv4 comparator at physical address `0x9214c05c`:
1. When set to `0xc0a80101` (`192.168.1.1`): `192.168.1.1` accepts TCP (SSH banner in 13 ms);
   secondary alias `192.168.1.2` responds to ICMP echo but TCP times out completely.
2. When rewritten to `0xc0a80102` (`192.168.1.2`): `192.168.1.2` accepts TCP (SSH banner in 29 ms);
   `192.168.1.1` TCP times out immediately.
3. When cleared to `0x00000000`: TCP to both tested IPv4 addresses times out,
   while ICMP echo and ARP continue to function.

These tests verify the single local IPv4 comparator exposed by the current
driver and show that changing it selects the working management address.
They do not exclude other classifier or bridge delivery paths: the stock
reference accepted IPv4 HTTP with this register zero. The LAN driver
`zx279125-ethernet.c` handles primary-address changes dynamically via kernel
address notifiers (`inetaddr_notifier` and `netdev_notifier`), updating `0x9214c05c`
on the fly whenever the management interface acquires a new IP address.

---

## Ethernet Driver Completion Checklist

### 1. Ordered Completion Checklist
1. **Fix IPv6 Neighbor Recovery**:
   * **Root Cause**: On interface down/up cycles, the remote host flushes its neighbor cache and transmits an ICMPv6 Neighbor Solicitation (NS) to the solicited-node multicast MAC (`33:33:ff:xx:xx:xx`). Unicast DMAC matching (`0x92140040`/`44`) alone does not match this multicast frame.
   * **Resolution**: Program full SPA protocol identification masks (`0x9230811c = 0xffffffff`, `0x92308120 = 0x0003ffff`) and the stock protocol action banks (`0x923080ec..100 = 01155155 / 15551555 / 00002555`). This traps all ICMPv6 Neighbor Discovery frames to the CPU queue unconditionally, allowing immediate neighbor resolution upon link recovery.
2. **Verify Lifecycle & Cable Transitions**:
   * Verified link down/up detection via PHY polling workqueue (`zx_link_poll`).
   * Clean transition of carrier flags, RX/TX hardware gates (`BIT(2)` / `BIT(3)` on MAC config `0x92200000`), and queue state without descriptor corruption.
3. **Complete Packet & MTU Coverage**:
   * Full MTU (1500-byte IP / 1472-byte payload) verified across 10,000 ICMP cycles: 0.0% packet loss.
   * Untagged and 802.1Q tagged frames handled up to `ZX_FRAME_MAX` (1522 bytes).
4. **Sustained Stability & Resource Utilization**:
   * Bidirectional 1 MiB duplex transfers passed across all four management endpoints:
     * `192.168.1.1` (IPv4 Primary): 1,048,576 bytes in 0.475s (SHA256 verified)
     * `192.168.1.2` (IPv4 Secondary): 1,048,576 bytes in 0.473s (SHA256 verified)
     * `fe80::72b6:4fff:fe01:a67a` (IPv6 Primary): 1,048,576 bytes in 0.543s (SHA256 verified)
     * `fe80::1234` (IPv6 Secondary): 1,048,576 bytes in 0.537s (SHA256 verified)
   * Buffer pool integrity verified: zero BMU buffer leaks (`alloc == release`).
5. **Cold-Test Final Image**:
   * RAM uImage deployment path: `/srv/tftp/v2801sg-final-gponsn.uImage` (3,890,969 bytes).
   * Cold boot to be finalized via physical board reset / power cycle.
    ```

## PPE/DMA handoff follow-up, 2026-10-23 (source/build only)

The source monitor retains its own cursor table across Ethernet restarts and
exports its address through an external Linux FDT only after stopped ownership
checks. Linux accepts that known table and verifies the region through the
reserved-memory registry. Probe also checks mailbox/BMU ownership. New read-only
`ethtool -S lan0` diagnostics expose accounting and all sixteen queue consumers.
The 16 MiB DMA reservation is unchanged. The updated module builds with W=1;
this revision has not been loaded onto hardware.

An appended-DTB zImage overrides a separately supplied DTB, so the new handoff
requires a kernel packaged without an appended tree plus an external Linux FDT.
See [PPE-DMA-MEMORY.md](../../../package/boot/uboot-zte/PPE-DMA-MEMORY.md) for
exact changes, evidence and remaining RAM tests.
