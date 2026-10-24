# ZX279128S submissions compared with the V2801SG port

Reviewed 2026-10-23. Baseline: dirty `v2801sg-zte-pon` checkout at
`2a526f7e24fb0e6adcbfa53f160ed565c322700f`; comparisons use the current files,
including untracked drivers, rather than just HEAD. This review changes no
hardware or functional driver code.

## Result

The strongest reuse candidates are the common flash-controller design,
MDIO transaction structure, clock/reset framework architecture, generic GPIO
support, and bridge lifecycle handling. The five-port Ethernet driver is a
reference for concepts, not a replacement for our different packet engine.
Its IPv4/NAT backend is outside the ONU bridge profile. None of these Linux
patches supplies BootROM packaging or DDR initialization.

Submission is not acceptance. The archive contains outstanding technical
objections, one binding acknowledgement, and a build warning report. The cover
letter's hardware results describe the author's H3600, not our V2801SG.

## Scope and reproducible comparison

Read both complete series: v1 RFC patches 01–24 and v2 patches 01–24, both
cover letters, and all 48 replies attached to those two threads in the
retrieved October linux-arm-kernel index: **98 messages total**. This is the
review trail visible in this archive, not a claim to have searched every
maintainer mailbox or proved merge status elsewhere.

- [v1 cover](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185412.html)
- [v2 cover](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186635.html)
- Evidence directory: `/tmp/zte-upstream-review-20261023/`.
- `inventory.json`, `originals.json`, `comments.json`: thread coverage.
- `mail-manifest.json`: URL and SHA256 for each archived HTML message.
- `v1/`, `v2/`: extracted submitted diffs and newly added source files.
- `v1-v2-common-files.diff`: revisions to files present in both versions.
- `comparison-manifest.json`: local source hashes and comparison mappings.
- `diffs/`: eight real unified comparisons: clock, MDIO, SPI, Ethernet main,
  SoC DTS, board DTS, and both networking bindings.

The HTML archive obfuscates some `@` characters as ` at `, including DTS node
names. The DTS comparisons normalize those node-name spellings only. Archived
patches are review material, not ready-to-apply mailbox patches. Existing
copyright/licence notices remain in the extracted source.

Useful comparisons:
[clock](/tmp/zte-upstream-review-20261023/diffs/clock.diff),
[MDIO](/tmp/zte-upstream-review-20261023/diffs/mdio.diff),
[SPI](/tmp/zte-upstream-review-20261023/diffs/spi.diff),
[Ethernet](/tmp/zte-upstream-review-20261023/diffs/ethernet-main.diff),
[SoC DTS](/tmp/zte-upstream-review-20261023/diffs/soc-dts.diff).

## Hardware differences that prevent direct replacement

| Item | Current ZX279125 port | Submitted ZX279128S/H3600 |
|---|---|---|
| CPU / RAM | One Cortex-A9; 32 MiB | Two Cortex-A9; board DTS declares 512 MiB |
| Flash | 8 MiB SPI NOR | 128 MiB SPI NAND; UBI layout |
| UART | Console at 94405000; custom data +04 / flags +14 | UARTs at 94404000/94405000; described as PL011 |
| SPI | Physically verified at 94407000 | DTS puts SPI at 94406000 |
| GPIO | Disabled node at 94404000, no implemented local driver | Six 16-line banks beginning at 94407000 |
| MDIO | 9a101000, one combined clock handle | Same base and core command fields; separate pclk/wclk |
| MAC / ports | One Ethernet UNI, MAC control at 92200000 | Five MACs, 92200000 plus 40000 stride |
| CPU packet block | PP 92144000, BMU 92168000, DMA 92164000 | TM 92340000, BMU 92348000, DMA 92350000 |
| Bridge / classifier | Bridge 92310000, classifier 9214c000 | Bridge 92388000, classifier 9238c000 |
| RX layout | 16 queues; 4096 slots each; two queue counts packed per word | 8 serviced queues; 1024 slots each; one count per word |
| Descriptor BP / length | BP at bytes 10–11; length word 3 bits 16:6 | BP split across bytes 7–8; length word 3 bits 15:2 |
| TX publication | Mailbox descriptor writes | Coherent TX descriptor ring plus kick register |
| Reset request | Local kernel and U-Boot use 9400003c | Submitted DTS reboot uses 94000044 |
| Factory identity | TAGH0201 parser and tag identifiers | Fixed NVMEM MAC bytes at tag-partition offset 1e |
| PHY | Bring-up record: PHY 1, ID 03815c11 | ZX5201 driver matches 84b95011; H3600 uses addresses 8 and 10–13 |

These are source/record comparisons. Similar names or identical offsets inside
one peripheral do not establish compatibility of the entire SoC.

## What can be used

### 1. Clocks, reset and the failed SRAM DDR test

Our `clk-zx279125.c` declares nominal CPU/DDR/AXI/APB rates and combines the
peripheral register/work gates into one handle. The submitted driver exposes
parent/mux/divider/gate relationships and separate work/register clocks. Its
architecture is a better direction for independent peripheral ownership, but
its particular rates and tables are not established for ZX279125.

The most relevant review is [v2 clock feedback](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186927.html):
reuse the ZTE clock infrastructure instead of adding another independent
implementation. That `drivers/clk/zte/` infrastructure is absent from the
inspected local 6.18 tree, so using it would require an explicit backport.
The [initial review](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185533.html)
also questions reference dividers, LSP PLL frequency, 32 kHz parentage, missing
peripheral clocks and unidentified reset bits. V2 still retains disputed
assumptions. Do not derive our DDR or UART rate from those values.

The related LSP table assigns UART0/UART1/SPI control registers at
94400010/14/18. Our exact vendor DDR routine writes 303 to those addresses.
This is a useful clock-interaction hypothesis for the repeating-g failure,
not proof that peripheral assignments are identical. Our own Linux table
assigns timers to 10/14/18 and UARTs to 20/24; this disagreement needs exact
ZX279125 vendor gate-function evidence and readback before any correction.

For restart, retain the ZX279125 evidence until its 3c register is reconciled
with the other chip's 44 register. Do not import the H3600 reboot node blindly.
Use logical reset IDs and shared regmap access when a verified reset driver
is introduced; reviewers prefer appropriate clock/reset/MFD separation.

### 2. SPI: highest-value common-driver candidate, with blockers

Our 362-line driver and their common driver share the 04–38 register layout,
16-word FIFO count fields, opcode/address/data phase model, and command-done
bit. Candidate improvements: batching available FIFO words, explicit
controller enable/ack, command-format error propagation, valid operation
capabilities, optional DMA later, and separate pclk/wclk ownership.

The submitted naming suggests register 08 is controller enable/ack, whereas
our helper calls it reset/FIFO. Actual FIFO-clear fields are in control 0c.
This is useful semantic evidence; the validated early-reader instruction
sequence should remain intact until those meanings are checked on ZX279125.

Do not replace our driver with v2 unmodified:

- [Author review on Oct 19](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1188687.html)
  questions its speed-setting function: flash SCLK and controller work clock
  are different, and timing/divider control belongs in that calculation.
  Minimum/maximum speeds and sampling bits are also questioned.
- The submitted PIO functions cast data buffers to u32 pointers and round
  lengths up to words. A host canary check of the actual submitted RX function
  confirms writes past the requested length: 1/2/3-byte reads alter 3/2/1 guard
  bytes; 5/6/7-byte reads do the same. Our actual RX function preserves all
  guard bytes for lengths 1–8. Keep our tail and unaligned-access handling.
  Harness/results: `checks/spi-tail.c`, `checks/spi-tail.txt` in the evidence
  directory. This tests memory access behavior with mocked MMIO, not flash.
- Static DMA audit: a read with RX DMA absent but TX DMA present can select
  the TX path; timeout unmaps before terminating the active channel. These
  paths need correction and testing before DMA reuse.
- [Build robot](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1187243.html)
  reports pointer-to-integer and register-write conversion warnings. Removal
  writes the complement of the enable bit, setting other bits: audit it.
- Bindings omit DMA/reset properties that the driver consumes, as noted by
  [DT review](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1187077.html).

Our driver also needs work: it advertises CPOL/CPHA without programming them,
leaves effective SPI frequency inherited, and merely warns on a format-error
status before returning success. Those are concrete local follow-ups. Begin
with PIO, single-line NOR reads at a validated rate. NOR programming/erase and
DMA are separate later validation tasks.

### 3. MDIO and PHY

The MDIO register offsets and Clause 22 command fields agree closely. Both
clear command/status, construct PHY/register/opcode fields and poll completion.
This is a credible shared-driver candidate once clocks are reconciled. Keep
our timeout cleanup: the submitted driver returns early on timeout before
its normal command/status cleanup, whereas ours runs cleanup in both cases.
Its shorter 2 ms timeout is not an established improvement over our 10 ms.

[Networking review](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186785.html)
questions the start/end ordering and the work-clock/MDC interpretation. Those
are unresolved hardware questions, not fixes to copy. Move toward separate
pclk/wclk handles only with the correct ZX279125 clock sources/dividers.

The ZX5201 PHY initialization is unsuitable for the recorded V2801SG PHY ID.
Its address+1 SerDes accesses and LED writes must not be applied to our PHY.
The review also requests address-31 rejection and clarification of GMII versus
RGMII/SerDes timing. Retain our PHY behavior; consider standard phylib/phylink
integration after identifying the exact local PHY and link mode.

### 4. Ethernet and the required ONU bridge

Reusable design ideas: standard phylink callbacks; bridge membership, STP,
learning/isolation notifications; FDB flushes on topology changes; DMA memory
barriers; BMU allocation/return diagnostics; shutdown quiescence; ethtool MAC
counter accumulation across 32-bit wrap; safe debug output that skips
clear-on-read completion registers.

Their register values, descriptors, release fields, queue counts and RX
resynchronization cannot be transplanted. Our driver preserves inherited
cursor ownership and stops on uncertain descriptor/release state. Their
scan-forward recovery depends on a different cold-initialized ring format.
Likewise their DMA-enable/reset masks are not a safe fix for quiescing our
hardware before a DDR experiment.

Their `switch.c` is useful guidance for the L2 backend, but implements only
known-unicast switching among Ethernet switch ports, for an unfiltered
bridge. Broadcast, multicast and unknown unicast remain on the CPU; VLAN
filtering and incompatible bridge states fall back to software. Port zero
on the PON side is not included as a managed subscriber port. It provides
neither GEM/T-CONT/OMCI provisioning nor a complete PON-to-UNI datapath.
Our backend still needs those pieces, VLAN rules, subscriber isolation,
control/management traps and a defined BUM forwarding policy.

The submitted `ppe.c` is IPv4 TCP/UDP routing/NAT with address/port/MAC/TTL
rewrites. Exclude it from our bridge-only ONU profile. Its indirect-table
helpers are reference material only after ZX279125 table formats, command
ordering and error rollback are independently verified.

Memory: H3600 declares a 38 MiB stock reservation and separately allocates a
16 MiB DMA arena. Our 32 MiB board cannot adopt that arrangement. Both use
4096 × 2048-byte buffers (8 MiB); a smaller ring does not itself shrink that
pool. The H3600 source reports trouble with a 2048-buffer pool after cold
boot; treat that as a warning to test cold pool sizing, not as proof of a
ZX279125 hardware limit. Retain our reservation until every DMA extent and
bootloader handoff is accounted for.

### 5. GPIO, pinctrl, CPU/cache and board description

The small GPIO driver or `gpio-mmio` binding could avoid a large custom
implementation. Verify bank bases, direction semantics, 32-bit access width,
clock and every optical/board control pin first. V2 changes register access
width from 2 to 4 bytes while keeping 16 lines; this is a useful review detail.
It has no GPIO IRQ implementation. Do not activate our currently disabled
GPIO node using H3600 addresses or pin numbers.

The pinctrl-single binding received an acknowledgement. The related DTS
labels 94200000 as pinmux, supporting review of our generic BUS naming. Its
bit-per-mux settings remain a partial hardware description, not a complete
pin controller. Our saved/restored warm bit operations passed, but that does
not establish a full pinmux map.

SMP code manipulates a boot vector in SRAM at 00200800 on the other chip.
Our hardware-verified entry is 00200000 and the V2801SG is configured for one
CPU. Do not copy its SRAM reservation or secondary-core trampoline. The
PL310 settings and global/private timers are useful discovery leads, but
our DTS lacks that L2 node and uses a custom timer. Validate ID/clock/cache
state first; do not replace working timer/cache setup from this other DTS.

The three v1 cache/mt76 workaround patches were removed from v2 following
review. They address reported PCIe/L2 contention on a dual-core Wi-Fi router;
they do not diagnose the current standalone UART/DDR-stage failure. No
blanket outer-cache lock, cache disable, Wi-Fi, PCIe or USB addition is
justified for this ONU profile.

H3600 fixed NVMEM MAC cells cannot replace our active TAGH0201 record parser.
Its NAND slot/UBI partition layout and boot-header assumptions also do not
apply to our NOR wrapper and environment.

## Implementation order supported by this review

1. Finish the UART-only SRAM diagnostic after stock recovery is confirmed;
   obtain exact ZX279125 clock/reset and DMA-quiescence evidence before another
   DDR test. The Linux series supplies no replacement DDR algorithm or ROM ABI.
2. Correct local SPI operation/error semantics, then adapt common PIO helpers
   with safe tails and proper speed control once input clocks are verified.
3. Model verified clocks/resets with shared ZTE infrastructure, keeping core
   PLLs unchanged during ordinary peripheral probe. Update bindings with it.
4. Consolidate MDIO and integrate the identified local PHY through phylib.
5. Implement a ZX279125-specific L2 switchdev backend and PON mapping, using
   the submitted bridge lifecycle design while retaining our descriptor and
   buffer ownership rules. Validate VLANs, BUM, isolation and optical behavior
   on suitable hardware before claiming complete ONU offload.

The following matrix accounts for every submitted patch and its revision.

## Every-patch matrix

| V2 | V1 counterpart | Topic | Decision for this port |
|---|---|---|---|
| [01](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186636.html) | part of 05 | MAINTAINERS: Consolidate ARM/ZTE ZX SOC SUPPORT with Stefan Doesinger | Common maintainer/team structure; do not copy another board’s ownership claims. |
| [02](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186637.html) | 04 | dt-bindings: arm: Add ZTE ZX279128S platform descriptions | Add our own SoC/board compatibles and bindings; SRAM/SMP compatibility differs. |
| [03](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186638.html) | 05 | ARM: zte: Add ZX279128S platform and CPU hotplug support | Reuse minimal DT machine structure; keep single CPU and our IRQ routing; no SMP trampoline. |
| [04](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186639.html) | 06, 11 | dt-bindings: clock: Add ZTE ZX279128S CRM clock bindings | Good separation of clock domains; fix outstanding schema and provider-resource comments first. |
| [05](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186640.html) | part of 06 | dt-bindings: reset: Add ZTE ZX279128S reset controller bindings | Logical reset IDs are useful ABI practice; verify our actual lines and polarity. |
| [06](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186641.html) | 07 | clk: zte: Add ZX279128S CRM clock and reset driver | Architecture reference only; reuse shared ZTE infrastructure and verify rates/gates on ZX279125. |
| [07](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186642.html) | 08 | dt-bindings: gpio: Add ZTE ZX279128S GPIO controller | GPIO schema model only; our bank layout is not established. |
| [08](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186643.html) | 09 | gpio: Add ZTE ZX279128S GPIO driver | Generic GPIO implementation candidate after bank/clock/pin verification; no IRQ support. |
| [09](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186644.html) | 10 | dt-bindings: pinctrl: pinctrl-single: Add ZTE ZX279128S pin mux | Acknowledged pinctrl-single addition; create ZX279125-specific mux description. |
| [10](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186645.html) | part of 13 | dt-bindings: phy: Add ZTE ZX279128S PCIe PHY binding | PCIe PHY schema under review; no demonstrated need on this ONU. |
| [11](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186647.html) | part of 13 | phy: zte: Add ZTE ZX279128S PCIe PHY driver | Separate PHY subsystem is sound design; do not apply its analog/PLL register writes. |
| [12](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186646.html) | 12 | dt-bindings: PCI: Add ZTE ZX279128S host controller | No V2801SG PCIe device in current port; keep out of the ONU image. |
| [13](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186648.html) | 13 | PCI: dwc: Add ZTE ZX279128S host controller driver | Framework organization only; shared reset, ASPM and lane settings are H3600-specific. |
| [14](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186649.html) | 14 | dt-bindings: net: Add ZTE ZX279128S MDIO controller | Strong shared MDIO binding candidate after clock-interface reconciliation. |
| [15](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186650.html) | 15 | net: mdio: Add ZTE ZX279128S MDIO controller | Closest driver match; preserve our timeout cleanup and verify clock/order questions. |
| [16](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186651.html) | 16 | net: phy: Add Sanechips ZX5201 PHY support | Reject for our recorded PHY ID; address+1 and LED writes are not portable. |
| [17](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186652.html) | 17 | dt-bindings: net: Add ZTE ZX279128S Ethernet switch | Useful port/PHY schema concepts; our PON + UNI and resources require their own schema. |
| [18](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186654.html) | 18 | net: ethernet: zte: Add ZX279128S Ethernet switch driver | Use lifecycle/bridge ideas; register map, DMA format, queues and memory are incompatible. |
| [19](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186653.html) | 19 | dt-bindings: spi: Add ZTE ZX family SPI flash controller binding | Common SPI binding direction useful; DMA/reset properties and compatibility still reviewed. |
| [20](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186658.html) | 20, rewritten | spi: zx: Add ZTE ZX family SPI flash controller driver | Best common-driver candidate after tail safety, frequency, DMA and cleanup issues are fixed. |
| [21](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186656.html) | 21 | dt-bindings: usb: Add ZTE ZX279128S DWC3 controller | USB binding not applicable to the current hardware/profile. |
| [22](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186655.html) | 22 | usb: dwc3: generic-plat: Add ZTE ZX279128S | Generic DWC3 glue reuse only if USB hardware is independently established; no current import. |
| [23](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186659.html) | 23 | ARM: dts: zte: Add ZX279128S SoC description | Source for comparisons, not a replacement DTS: UART/SPI/GPIO/SRAM/timers/IRQs differ. |
| [24](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186657.html) | 24 | ARM: dts: zte: Add ZTE ZXHN H3600 board | Board wiring, 512 MiB RAM, NAND partitions and external PHY are not transferable. |

V1-only patches [01](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185413.html), [02](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185414.html), [03](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185415.html) are the withdrawn cache-binding/global-cache-lock/mt76 changes. Do not import them. V1 patch 11’s syscon binding change was reorganized into v2 CRM bindings; it is not a missing driver.

## Review comment ledger

Each of the 48 retrieved replies is accounted for below. Quoted patch context remains in the cached full messages.

| Version / message | Review consequence |
|---|---|
| [v1 / 1185740](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185740.html) | RFC dropped from DT patchwork; organize submissions and CC subsystem maintainers. |
| [v1 / 1186617](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186617.html) | Author removes cache-workaround patches from v2; validation claims remain author reports. |
| [v1 / 1185442](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185442.html) | Global cache/read lock questioned as invasive; investigate existing outer-sync behavior. |
| [v1 / 1185476](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185476.html) | Author describes trial-and-error origin and plans outer-sync comparison. |
| [v1 / 1185503](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185503.html) | Author reports outer-sync alone fails and serialization passes H3600 stress; not ZX279125 evidence. |
| [v1 / 1185581](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185581.html) | Defer workaround; possible architectural MMIO approach; no settled general solution. |
| [v1 / 1186616](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186616.html) | Author retains workaround downstream and focuses v2 on platform support. |
| [v1 / 1185439](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185439.html) | Use a common ZTE platform/maintainer team rather than separate overlapping platforms. |
| [v1 / 1185470](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185470.html) | Existing maintainer open to co-maintenance and review. |
| [v1 / 1185475](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185475.html) | Author agrees to consolidate MAINTAINERS. |
| [v1 / 1185529](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185529.html) | Split CRM bindings, logical reset IDs, resource-based MFD design; discover missing reset controls. |
| [v1 / 1185533](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185533.html) | Reuse shared clocks/PLL infrastructure; validate parent rates, peripheral slots and reset bits. |
| [v1 / 1185555](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185555.html) | GPIO layout resembles existing ZTE hardware; assess generic support and IRQ availability. |
| [v1 / 1187540](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1187540.html) | GPIO reviewer suggests generic base and questions inverted-direction interpretation. |
| [v1 / 1187573](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1187573.html) | Other-board GPIO/PSHOLD behavior discussed; not evidence for our optical pins. |
| [v1 / 1185790](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185790.html) | Avoid unnecessary mod_devicetable.h dependency in platform drivers. |
| [v1 / 1186618](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186618.html) | Author folds header cleanup into v2. |
| [v1 / 1185565](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185565.html) | PCIe should use clock and Generic PHY providers instead of raw syscon sequences. |
| [v1 / 1185586](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185586.html) | Existing spi-zx driver offered; PIO batching and optional DMA discussed. |
| [v1 / 1186615](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186615.html) | Author reports common SPI works on H3600 NAND; proposes clock-rate adaptation. |
| [v1 / 1185995](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185995.html) | Reviewer requests independent hardware understanding, improved clocks and pinmux coverage. |
| [v1 / 1186634](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186634.html) | Author describes v2 restructuring; acknowledges remaining clock/pinmux gaps. |
| [v1 / 1186798](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186798.html) | Networking patches must be routed to netdev. |
| [v1 / 1186929](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186929.html) | Reviewer reiterates learning/hardware understanding and ongoing collaboration. |
| [v2 / 1186935](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186935.html) | MAINTAINERS status and patch/file placement questioned. |
| [v2 / 1186963](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186963.html) | Reduce exported clock IDs and syscon usage; avoid implementation-only PHY child nodes. |
| [v2 / 1187070](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1187070.html) | Fix binding compatible alternatives, PHY resources, child schema and incomplete examples. |
| [v2 / 1187071](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1187071.html) | Binding subject naming cleanup. |
| [v2 / 1187072](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1187072.html) | Reset constants belong with their schema; remove non-binding count definitions. |
| [v2 / 1186824](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186824.html) | Reset driver duplication, lock placement and unnecessary early reset registration questioned. |
| [v2 / 1186927](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186927.html) | V1 clock objections persist; use existing shared ZTE regmap infrastructure. |
| [v2 / 1186947](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186947.html) | gpio-mmio binding may avoid a device-specific driver shim. |
| [v2 / 1187176](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1187176.html) | Use structure initialization at the right point; small generic GPIO driver otherwise viewed positively. |
| [v2 / 1188035](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1188035.html) | Pinctrl-single binding addition receives Acked-by. |
| [v2 / 1187073](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1187073.html) | PCIe PHY child port index/resources questioned; may belong to parent provider. |
| [v2 / 1186822](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186822.html) | Use consistent family-level PCIe naming and driver identity. |
| [v2 / 1186946](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186946.html) | ZTE family naming/capitalization discussion; no hardware compatibility conclusion. |
| [v2 / 1186785](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186785.html) | MDIO command/status order and work-clock/MDC interpretation need explanation. |
| [v2 / 1186790](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186790.html) | Identify SerDes ID, reject PHY address 31, resolve RGMII/GMII/SerDes and delay handling. |
| [v2 / 1186938](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186938.html) | Consider one flash-controller compatible, require interrupt description, optional DMA resources. |
| [v2 / 1187077](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1187077.html) | SPI binding omits DMA/reset resources consumed by implementation. |
| [v2 / 1187079](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1187079.html) | SPI binding subject naming cleanup. |
| [v2 / 1186924](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186924.html) | Common SPI author welcomes testing; code still has TODOs and is not declared ready for upstream. |
| [v2 / 1187076](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1187076.html) | Remove probe chatter, use sleepable polling where possible, simplify redundant compatibility. |
| [v2 / 1187243](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1187243.html) | Build robot reports pointer cast and register value conversion warnings. |
| [v2 / 1188687](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1188687.html) | SPI speed function targets wrong clock level; min/max speed and sampling bits questioned. |
| [v2 / 1186801](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186801.html) | Board PHY mode conflicts with SerDes explanation; interface needs clarification. |
| [v2 / 1186930](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186930.html) | Focus first on base board/clocks; later drivers can follow after prerequisites. |

## Verification boundary

The mail/source inventory, actual unified diffs and SPI tail canary checks are complete. No whole submitted series was compiled/applied, no new firmware was booted, and no hardware registers or flash were changed in this review. All recommended reuse still needs its own implementation and validation. Hardware testing remains paused after the DDR recovery-stage failure.
