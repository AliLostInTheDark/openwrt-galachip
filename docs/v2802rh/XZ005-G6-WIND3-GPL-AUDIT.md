# XZ005-G6 WIND3 GPL tree inspection

Date: 2026-10-30. Input: `/home/ali/Desktop/TP/XZ005-G6_WIND3_GPL/`.
Purpose: inspect the complete release layout and its major platform subsystems,
identify usable V2802RH references, and separate source, binaries, missing
components and unrelated third-party code.

## Result

This release is a valuable **RTL9601D SFU reference**, especially its original
named kernel and GPL Ethernet driver. It is not a complete rebuildable vendor
SDK, nor a drop-in V2802RH firmware. Its selected optical frontend is UX3320S,
not the V2802RH GN25L95. The most useful immediate follow-up remains the
[hardware QoS implementation plan](HARDWARE-QOS-AUDIT-2026-10-30.md).

No target binaries or build/install scripts were executed. No original GPL
files, ONU settings, production code or firmware were changed. No build,
flash or hardware test was attempted.

## Coverage and retained evidence

Every regular file was inventoried by path, length and header; symlinks were
recorded without following directory links. All detected ELF files were
classified with `file`, and ELF/uImage artifacts were hashed. Selected source
and board configuration files have a separate SHA-256 manifest. The major
build, board, bootloader, Ethernet, PON/QoS, flash, startup, userspace and image
paths were inspected. This is a whole-release inventory and subsystem audit,
not a claim to have reviewed every statement of every bundled package or
decompiled every kernel function.

Local evidence directory: `tmp/xz005-g6-gpl-audit/` in this checkout.

| Evidence | Contents |
|---|---|
| `FILES.tsv` | Complete regular-file and symlink inventory |
| `inventory.json` | Counts, sizes, extensions, symlinks and license-file locations |
| `ARTIFACTS.json`, `ELF-INVENTORY.tsv` | Header-identified artifacts, hashes and ELF architectures |
| `SOURCE-SHA256SUMS` | Thirty selected source/configuration/boot artifact hashes |
| `BUILD-PATHS.json` | Explicit presence/absence checks for build dependencies |
| `TOOLCHAIN-ARCHIVES.json`, `*.members.txt` | Outer toolchain archive membership lists |
| `kernel-symbols.txt`, `symbols-*.txt`, `SYMBOL-GROUPS.json` | Original ELF symbol inventory, grouped by subsystem |
| `UIMAGE-VALIDATION.json` | Header/payload CRC and decompression results |
| `TRUNK-MAPPER.json` | Original kernel's RTL9602C trunk callback slots |
| `source-*.txt`, `*-directories.txt` | Navigable source and package directory lists |

Counts from the filesystem walk:

- 107,713 regular files; 2,081,325,858 logical bytes (disk usage about 2.3 GB).
- 34,297 `.c` and 20,228 `.h` files, mostly bundled upstream packages.
- 92 ELF files, three uImage-header files, 63 gzip, ten bzip2 and 62 xz headers.
  Header counts include test fixtures and do not imply installed components.
- 25 symlinks; ten do not resolve on this host. Four are normal target-runtime
  `/var/...` links. Six are unresolved bootloader source/header links.
- 251 conventional license/copyright/notice filenames. This is a location
  inventory, not a legal conclusion that every file has the same license.

## Release map

| Area | Inspected material | Assessment |
|---|---|---|
| `platform/build` | Model config, environment, SDK/public/private app/module rules, rootfs/image recipes | Useful product assembly contract; private dependencies missing |
| `platform/targets/XZ005-G6V1/ITWIND3` | Boot binary, kernel ELF/uImages; empty image/dataModel directories | Strong kernel reference; no complete assembled production rootfs/image |
| `platform/targets/fs.dir` | Generic filesystem skeleton, init/hotplug and network scripts | Mixed-product template, not a dump of the XZ005 running rootfs |
| `platform/modules/public` | Parental-control netfilter module, PPTP and USB/IP | Present source; not the private board/GPIO/PON implementation |
| `platform/apps/public` | BusyBox 1.23.2, curl 7.62.0, dnsmasq 2.85, MAP and support libraries | Public platform userspace; selected inclusion must be checked against model |
| `platform/host_tools` | Image builders and source variants, filesystem tools, enc/mkcfg and host executables | Image format reference; multiple unrelated silicon/vendor lanes |
| `platform/toolchain` | MIPS toolchain plus nested ARM/AArch64/source archives | Mixed build infrastructure; not evidence the ONU is ARM |
| `sdk/rtl9601/bootloader/uboot` | U-Boot 2011.12, Otto board/CPU/cache/timer/NOR source | Useful low-level reference; preloader tree absent |
| `sdk/rtl9601/linux-2.6.x` | Linux 2.6.30.9, RLX BSP, Ethernet and flash drivers | Useful GPL platform source; proprietary switch/PON SDK absent |
| `sdk/rtl9601/user` | 258 package directories, BOA/RG glue and ONU communication | Broad old distribution; presence does not mean installed/enabled |
| `sdk/rtl9601/user2` | gdbserver, proc_var, rsh and rtk_nv | Utilities; not a hidden full PON SDK |
| `sdk/rtl9601/lib` | 23 library directories, including several TLS/crypto generations | Historical distribution infrastructure, not a modern dependency set |
| `sdk/rtl9601/vendors` | Several reference board profiles including selected 9601D SFU | Configuration evidence; other boards must remain distinguished |

## Selected board identity and compatibility

`platform/build/model/XZ005-G6V1/ITWIND3/config.bba` selects
`SDK_CONFIG="9601D_nor_demo_SFU"`, Realtek and MIPS. `config.kernel` selects:

- `CONFIG_RTL9601D=y`, `CONFIG_9601D_SFU=y`, RTL9602C-series support.
- RLX5281, big-endian CPU; the original ELF independently confirms MIPS MSB.
- `CONFIG_GPON_FEATURE=y`, reduced OMCI V1, and EPON/EPON LOS recovery.
- RTL8221B support, device 0 PHY address 7; external MDIO polling uses
  GPIO 26/27 in this profile.
- The RTL9601D SFU/HGU PON LED profile.

The platform config also contains `INCLUDE_LITTLE_ENDIAN=y`/little-endian
rootfs flags despite the CPU/ELF being big-endian. Treat these as platform
packaging/configuration fields, not proof of a little-endian CPU.

Optical selection is `INCLUDE_GPON_BOBUX3320S=y`; BOB7572 is disabled. No
GN25L95-specific calibration source was located by unignored file/source
search. Do not transplant bias/APC/laser or GPIO settings to V2802RH.

The public LED/button model files describe power GPIO 41, LAN GPIO 40 and
reset GPIO 19. Those are XZ005 model inputs, not V2802RH wiring. The supplied
profile is not the V2802RH two-port board, even though the SoC family overlaps.
EPON configuration and named kernel functions establish a vendor EPON code
path; they do not establish an implemented/tested EPON backend in our firmware.

## Kernel artifact integrity and symbol value

Original ELF:
`platform/targets/XZ005-G6V1/ITWIND3/kernel/vmlinux`

SHA-256: `7ad7b661efd9213eaf43c2100f2e8ea5ab5453f997b45aefa4035717a0c2a5e8`.

`kernel/linux.7z` is a misleading filename: it is a legacy LZMA uImage,
identical to `kernel/uImage`, not a 7z archive. Both header and payload CRCs
pass. Their SHA-256 is
`ba41b9453c0358b0bbb2fdc2d2d12d449f86d5ca99056f883063064b6816cd9d`.

The decompressed payload is 2,979,964 bytes and **exactly matches** cross-
`objcopy -O binary` output from the original ELF. Raw SHA-256:
`7b4894271b94e44e96353ae8e4d087b6a375d07a0e02cfdcb918f0cfb3a2ae92`.
Load/entry are both `0x80000000`. This ties symbols to the shipped kernel
payload; it does not prove every supplied source file matches that build.
`temp_tag_kernel.bin` is a distinct larger packaging artifact, not another
identical uImage.

The ELF retains local/global function names and a single inspected DWARF
compilation unit for `drivers/net/re8686_rtl9602c.c`. It does not provide full
PON SDK source/types. Overlapping symbol groups contain 77 PON-MAC functions,
624 GPON, 106 EPON, 610 L2/VLAN/classifier/ACL and 206 meter/QoS functions.
These are indexed symbols, not a count of function bodies audited in full.

REA's native lane rejects this ELF's MIPS ISA; previous REA identification
succeeded but decompilation did not. The focused static observations here use
normal cross-binutils. No ELF flags were changed to bypass that restriction.

## Ethernet, CPU learning and offload

Actual GPL source is present:
`sdk/rtl9601/linux-2.6.x/drivers/net/re8686_rtl9602c.c` and `.h`.
Related 9601B/general/DSP drivers and verification code are also supplied.

`_tx_additional_setting()` at about lines 1167–1268:

- Enables the CPU tag and resolves a direct egress port mask.
- Sets `dislrn=1` for relayed frames from LAN/WAN domain devices (lines
  1228–1231), explicitly labelled “disable cpu port learning”. This supports
  the learning-suppression principle behind the completed management repair.
- Distinguishes direct-mask TX from hardware lookup and couples L34 keep
  with the GPON keep bit.
- Includes a short-IPv4-frame length workaround and excludes OMCI stream 0x40.

These are strong implementation references, not permission to copy legacy
skb/descriptor code into Linux 6.18. The GPL driver depends on absent SDK
headers/hooks, and the modern driver's tags/descriptor path must be compared
field by field before making any change. Existing management remains closed.

## QoS, classifier, meters and aggregation

Useful source glue exists in
`sdk/rtl9601/user/boa/src/LINUX/rtusr_rg_api.c`:

- Queue setup around 4947–4983 passes strict/weighted type and queue weights
  through `RTK_TR142_IOCTL_SET_QOS_QUEUES`.
- `RTK_RG_QoS_Queue_Set()` around 5004–5090 maps user queues in reversed
  GPON hardware order and calls RG scheduling operations.
- ACL/QoS setup references share meters; other paths cover storm controls,
  downstream broadcast VLAN filtering and PCP treatment.

The underlying TR142/PON/RG headers and implementations are not supplied
under the referenced `drivers/net/rtl86900` tree. The kernel symbols and
focused assembly therefore remain the implementation evidence for those parts.
The previous QoS audit addendum records exact addresses for queue add,
scheduling type, meter bucket and classifier entry/field functions.

Fresh original-ELF inspection of `rtk_trunk_port_set` at `0x801553a4` shows a
callback lookup at mapper offset `0xc5c`, returning 0x12 if null. The named
`dal_rtl9602c_mapper` at `0x802b2650` has zero words throughout trunk callback
slots `0xc4c..0xc80`. Thus exported trunk names are wrapper APIs without a
working implementation through this mapper. This release does **not** supply
a hardware-bonding solution. It is not proof about every undocumented chip.

The richer reference does not resolve our reproduced partial-remap ownership
bug. That software transaction repair remains first priority, followed by
PCP-aware GEM classification, verified arbitration and meter/burst units.

## GPON, EPON, optics and recovery

The kernel contains named GPON/EPON driver, LOS, laser/optics, CDR and dying-
gasp functions. `user/onucomm/onucomm.c` exposes PON LOS through
`rtk_ponmac_losState_get()` around lines 226–234 and packages telemetry for
its userspace protocol. BOA's `checkomci.c`/OMCI page are control/display glue,
not the missing OMCI protocol engine.

The source alone does not provide complete OMCI managed-entity application,
PLOAM recovery, or per-unit optical calibration. A full raw rootfs containing
vendor OMCI/OAM executables/modules is not present in the target output.
Named ISR/CDR/laser functions are useful future entry points; they have not
all been traced to prove blackout or power-failure behavior. Existing V2802RH
physical-blackout validation is not replaced by this reference.

## Bootloader, timer, flash and image construction

The board file `bootloader/uboot/board/Realtek/rtl9602C/rtl9602C.c` recognizes
RTL9601C/9601D chip IDs, reads DRAM geometry and prints the matching board.
The common `arch/otto/lib/otto_time.c` uses the LX hardware timer, masks
28-bit wrap, and overrides `sleep` with repeated microsecond waits. The
SoC-local `timer.c` marks its alternative implementation `#if 0` in favour
of this common file. A stopped counter can stall a polling delay, but this
inspection does not diagnose the previously deferred live bootloader hangs.

NOR SPI core and probe source exists under
`linux-2.6.x/drivers/mtd/chips/luna_nor_spi/`; it includes controller polling
and failure loops. U-Boot's generic RTL9602C header includes environment
locations/default boot delay. None is automatically the exact XZ005 compiled
layout or V2802RH's layout: configuration and missing preloader inputs matter.

The platform config contains an 8M flash selection label yet explicit image
sizes of `0x400000`, boot `0x78000`, kernel `0x100000`, misc `0x20000`.
This inconsistency means labels alone cannot establish actual flash capacity
or safe partition boundaries. No values were applied to V2802RH.

Image recipes and host-tool sources document TP-Link tagging, boot/kernel/
misc assembly, SquashFS LZMA packing and model metadata. `fs_build` creates
normal/beta/trans/datecode rootfs variants. These are useful format references,
not compatible flashing commands for our board.

## Build completeness and runtime templates

Confirmed absent paths include:

- `linux-2.6.x/drivers/net/rtl86900` (switch/PON SDK, RG and OMCI dependencies).
- `linux-2.6.x/drivers/net/rtl8221B` despite selected RTL8221B configuration.
- `bootloader/src` and the top-level `bootloader/Makefile` referenced by recipes.
- `platform/apps/private`, `platform/modules/private`, `platform/dataModel`.
- SDK `romfs`; target output `image`/`dataModel` are empty.

`Makefile.sdk.realtek.pon` also optionally includes absent `sdk/make.common`;
its environment helper can install a toolchain into `/` and rewrite configs.
It was read, never executed. Existing vendor toolchains were listed without
installation. The RSDK MIPS archive has 10,300 outer members; another MIPS
archive has 5,317; the ARM/AArch64/source bundle has four outer members.
Nested toolchains were not unpacked or run.

Unresolved source symlinks point to missing preloader `src/...` and generated
SoC headers. In contrast, `/etc/passwd`, TZ/resolver/localtime links target
runtime `/var` locations and are not evidence of missing source.

Startup scripts expect private `tp_board.ko` and `tp_gpio.ko` and run SDK
`rc0..rc63`; the model hook itself is a placeholder. The filesystem skeleton
also contains a QCA `ssdk_sh` VLAN script and unrelated voice/hotplug assets.
Their presence is not RTL9601D functionality. The SFU VLAN UI provides OLT,
tag and transparent modes but no evidence of full per-port VLAN/LACP control.

Model `config.user` enables BOA, telnetd, diag shell and debugging selections;
platform config disables SSH access and product-level QoS UI. Rootfs absence
prevents asserting which services are exposed by the released firmware.
Legacy TLS/HTTP components and developer utilities should not be carried into
our firmware merely because they are included. Individual license headers
also differ; preserve provenance when evaluating reuse.

## Recommended use in V2802RH

1. Keep this tree immutable and use the exact-matched ELF as the named
   reference for queue, meter and classifier traces.
2. Repair QoS ownership/rollback and add requested/applied diagnostics in our
   driver, following the current audit rather than transplanting vendor code.
3. Trace PCP field encoding and meter/bucket units against the original ELF;
   validate each feature on controlled hardware before enabling it.
4. Retain Ethernet learning/descriptor, timer and NOR source as comparative
   references; do not reopen working cases without a specific new defect.
5. Treat EPON, dying gasp, additional UNI QoS and vendor upgrade format as
   separate investigations. No hardware bonding implementation was found.

Open items are semantic reconstruction and hardware validation, not missing
inventory: WRR/WFQ arbitration, independent downstream meter placement,
burst/CIR semantics, detailed dying-gasp wiring/sequence and absent vendor
userspace components cannot be established from a filename or symbol alone.
