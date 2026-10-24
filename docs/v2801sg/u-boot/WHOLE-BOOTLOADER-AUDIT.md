# Whole stock bootloader analysis — 2026-10-23

The complete 256 KiB stock dump has now been imported into REA/Ghidra and
batch-analyzed. The raw whole-image disassembly already existed; previous
work focused on small routines and repeated hardware probes. This pass adds
retained procedure-wide decompilations and cross-reference evidence so further
investigation can proceed offline without requiring a new UART test for each
question. No UART device was visible to this session and no hardware action
was performed.

## Artifacts and coverage

Analysis directory: `/tmp/v2801sg-whole-boot-audit/`.

- `manifest.json`: hashes and sizes of raw image, wrapped views and indexes.
- `stock-uboot.elf`: analysis-only ARM ELF executable at linked base 41f00000.
  Its binary round-trip exactly matches `v2801sg-mtd1.bin`, SHA256
  d286f31239edd5ff9ed5843da41148da869a68a5f72abca11e8b6ec576235c8a.
  Wrapper SHA256 is 0df16c56573ff93b4af6447fe3abdac7ff587a1c281c049f3ecd612d2e045191.
- `full-disassembly.txt`: full payload disassembly at the linked address.
- `procedures.json` and `decompile-ledger.json`: all 583 discovered procedures
  and each request's result. 578 decompilations succeeded. Five exception
  handlers failed Ghidra decompilation with an address-range construction
  error; raw assembly fallbacks are retained at their addresses.
- `functions/`: pseudocode plus assembly for each successful function;
  `.asm` fallbacks for 41f001e0/2a0/300/360/3c0.
- `full-analysis-evidence.json`: 586 retained REA evidence records, about
  82 MB; `full-analysis-snapshot.json`: resumable analysis snapshot, about
  163 MB. These are analysis artifacts, not firmware images.
- `stock-uboot-sram-view.elf` and `sram-view-evidence.json`: the identical
  payload analyzed at base 00200000 to expose the position-dependent early
  path. This is a hypothetical analysis mapping, not a claim that 256 KiB
  physically fits SRAM or that ROM loads that entire range.

The first relocatable ELF attempt made Ghidra add 10000 to procedure addresses.
Those initial results are superseded by the executable wrapper above. Only
the correctly based full-analysis and SRAM-view evidence should be used.

## Findings relevant to the failure

1. At its normal DRAM address the bootstrap explicitly skips DDR setup.
   Ghidra consequently simplifies `stock_bootstrap` to a return. In the SRAM
   analysis view it exposes the DDR call, bus/pinmux changes and 256-byte SPI
   copy loop into 41f00000. Decompiling only the normal linked view can hide
   the exact path being reconstructed. SRAM-view evidence:
   ev_5a17094aa39f84444f2c6154d7bbfbf2d47148b1f24b6e6c8d0db38be76984cd.
2. The CP15 setup explicitly computes SCTLR `(old & ffffdff8) | 1802` before
   low-level startup. That corroborates the instruction-cache/prediction
   mismatch in our failed test. REA evidence:
   ev_c85877088f5b0ff7b1967509d8100a6a3769ab853699fefab850f59f5d3b63bb.
3. Ghidra's DDR pseudocode moves the apparent 94000008 write relative to raw
   instructions. Its representation is not a timing/order specification.
   Preserve the existing raw-instruction trace comparison as the ordering
   authority. DDR evidence:
   ev_59fa0bc2a1a7bd7f17f83a444737a302f35ce972f169694f01f46a50dacf9f22.
4. Later pinmux/GPIO setup at 41f1d5a4 copies the table at 41f21bec to
   94200000..08 and configures GPIO9 as input, consistent with button
   recovery. It is distinct from the early DDR/clock path. Evidence:
   ev_fff80c13568b5aa708479ab5791a73f078b1d282935b6619dfaa853198a3ffba.
5. The normal serial setup at raw 41f119dc writes UART control/divider/line
   state. The submitted UART flags snapshot alone does not establish TX-empty
   semantics; exact putc paths test only bit 5 for TX-full. No new bit mapping
   is justified by the broader pass yet.

These findings improve coverage but do not identify the exact failed bus
transaction. Code-byte coverage and Ghidra's discovered procedure count are
different: inline data, indirect calls, exception state and runtime-selected
paths require manual inspection. Synthetic symbols and section layout were
added for analysis; recovered types, inferred constants, function boundaries
and pseudocode optimizations must be checked against raw assembly. In
particular, an apparent empty routine does not prove it does nothing when
relocated or entered with a different machine state.

## Instruction-level comparison of the failed SRAM loader

The previously reported 35 matching MMIO writes and three delays compare
register events, not the instructions between them. In the compiled failed
loader (`/tmp/v2801sg-ddr-engine-recovery/disassembly.txt`),
`zx279125_ddr_init` at 0020059c..0020082b contains 42 `f57ff05f` instructions
(`DMB SY`). The raw stock DDR routine at 41f004a8..41f00610 contains no such
barriers. U-Boot's ARM `readl`/`writel` wrappers insert these barriers around
the relaxed MMIO accesses. This is a real timing/ordering difference, though
it does not establish that any barrier caused the observed stall. The host
register-trace test cannot detect it. Do not remove barriers solely to make
the instruction counts match: the replay also runs against initialized DDR,
whereas stock calls this routine during cold startup.

The cold-start source was subsequently changed to use U-Boot's
`readl_relaxed`/`writel_relaxed` in `ddr.c`, matching the vendor's direct MMIO
accesses at this instruction boundary. An independent compile of that source
produced `/tmp/v2801sg-ddr-relaxed.o`; its `zx279125_ddr_init` disassembly has
no inserted DMB instructions. The 38-event vendor trace still passes. This is
an offline source correction only: the retired failed binary still contains
the barriers, and no new SRAM image was built or run from this change.

Stock reset code at 41f00058 selects SVC mode unless already in Hyp mode and
masks IRQ/FIQ. The CP15 routine at 41f0017c invalidates TLB, instruction cache
and branch predictor
and writes SCTLR `(old & ~0x2007) | 0x1802`. The stock low-level path installs
an SRAM stack at 0020a000 before calling the position-dependent bootstrap.
The failed loader masks interrupts and uses that stack, but enters from a
running U-Boot with inherited CPU control state; its guard requires instruction
cache off. It also does not install SRAM exception vectors. Thus a fault during
DDR disruption would not have a reliable SRAM exception log. The successful
pre-DDR banner proves the entry and UART output worked up to that point, not
that the later CPU state matched cold startup.

The stock bootstrap at 41f00774 prints `ddr init`, invokes DDR setup only when
the running PC is outside the 4xxxxxxx region, configures bus/SPI, copies the
first 256 KiB of NOR to 41f00000 in 256-byte chunks, and jumps there. At the
DRAM address it skips DDR setup. The failed loader deliberately forces that
early path from SRAM after a warm chainload; there is no evidence yet that
reinitializing the already-running DDR controller is safe even with packet
engine DMA stopped. The stock register snapshot shows 94000004 currently `b`
and 94000014 `077faabf`, while early DDR code writes `8` and `ffffffff`;
those changes may alter active clock/reset state. Their exact bit meanings on
ZX279125 remain unverified.

Raw code at 41f119dc shows stock's later UART initialization: it disables
control, writes integer/fractional divisors 13/36, line control `70`, masks
interrupts and enables control `301`. That divisor pair is consistent with a
25 MHz UART reference at 115200 baud. The early `putc` at 41f0063c polls only
flags bit 5 (TX-full); it does not validate our bit-7 transmitter-drain test.
The OpenWrt chainload serial driver's `setbrg` currently preserves the
inherited UART settings. The cold-build driver now optionally programs the
seven observed stock UART register writes when its device probes. The
`ZX279125_SERIAL_INIT` Kconfig option is enabled in the base defconfig and
disabled by the RAM-chainload package variant. Both driver forms compiled:
the chainload probe remains a two-instruction return with no register writes;
the cold probe contains seven writes and no inserted DMB instructions. The
complete package patch applies to a clean U-Boot 2026.10 tree. Early debug
UART still uses `DEBUG_UART_SKIP_INIT`, so cold entry remains dependent on the
earlier SRAM stage or ROM for messages printed before the driver probes.

The separate RAM UART-init test was subsequently hardware-verified: image
`85dbdf9f` loaded with CRC `aae282a5`, printed its test banner and retained a
responsive console. Divisor and line/control readbacks were `0d/24` and
`70/301`, and ping succeeded. See [UART-INIT-TEST.md](UART-INIT-TEST.md).
This confirms warm driver initialization, not cold clock/reset startup.

Both U-Boot package variants then built successfully. The RAM-chainload
binary remained byte-identical to the hardware-tested `7f1be111` image
(232480 bytes), with `ZX279125_SERIAL_INIT` disabled. The base cold build has
the option enabled and produced a 282496-byte binary, SHA256
`51b04460313b8adc5a8a649e7187033b46309259541d2937d302fb673a979375`.
That is 20352 bytes over the 256 KiB boot partition even before adding an
SRAM first stage. It is a build result, not a flashable image; cold DDR, ROM
load format and size reduction remain open.

Current conclusion: the whole image is available for offline analysis, but
the failed warm DDR replay has at least three material differences from the
vendor's cold path: active-controller state, CPU/cache state, and inserted
MMIO barriers. The output of repeated line feeds does not locate the failing
transaction. The next implementation work should establish a cold-entry CPU
state and a bounded phase trace that survives long enough to identify the
first failing operation. A fresh full DDR replay is not justified by the
current evidence. RAM chainloading can continue to test U-Boot functions
without calling DDR initialization.

## Consolidated live snapshot, only if needed

`STOCK-SNAPSHOT.txt` is one pasteable read-only command batch for the recovered
stock U-Boot prompt. It collects environment and the known clock, DDR, packet
engine and MAC registers together. It performs no test jump, networking,
reset, flash operation or configuration change. Its output would be a single
serial log to analyze offline. It is a scoped machine-state snapshot, not
every address in the MMIO map. Blindly reading all MMIO can pop FIFOs or clear
status; the UART receive-data and mailbox command/data windows are excluded.

No new live snapshot is required now: the latest clock/DDR snapshot and
previous engine snapshots already cover most of this batch. The full flash
backup also exists, so repeating a multi-megabyte UART memory dump would add
little. Continue the offline audit first; request this batch only for specific
state gaps. The combined DDR image remains retired, and the separate UART
sampler is optional rather than a prerequisite for this analysis.
