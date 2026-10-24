# VSOL V2801SG: U-Boot replacement journey and cold-boot handoff

**Checkpoint: 23 October 2026. Branch: `v2801sg-zte-pon`.**

This document consolidates the source reconstruction, failed experiments, successful hardware tests, ROM discovery, and current replacement-image audit. It describes the state before any replacement bootloader installation. Historical test images are identified for evidence, not offered as instructions to rerun them.

## 1. Current conclusion

A modern U-Boot monitor works when RAM-chainloaded after stock U-Boot has initialized the board. UART, repeated ping, TFTP and SPI NOR reads have passed on hardware. The actual ROM console also works when entered through a controlled warm wrapper: memory reads, jumps, XMODEM-CRC upload and execution of a harmless SRAM payload passed.

A separate source-built first stage now implements the decoded ROM loading contract, CPU setup, reconstructed DDR initialization, SPI page loading, payload CRC verification and automatic monitor handoff. The captured ROM and exact replacement machine code passed sixteen offline emulation cases. **The replacement has not booted from physical cold reset.** DDR electrical behavior, reset-time clocks and complete cold monitor initialization remain unverified.

The installed bootloader is still vendor U-Boot 2013.04. The user replaced the system firmware earlier; that did not replace this bootloader. No replacement bootloader installation was performed. An earlier RAM-kernel panic did trigger an unintended vendor environment write on automatic reboot, as recorded in the Ethernet bring-up log; that is distinct from an operator-issued flash command. No programmer or optical operation was performed.

### Evidence levels used here

- **Hardware:** observed on this board, bound to the particular tested artifact.
- **Instruction/source:** established from retained bytes, disassembly or source inspection.
- **Model:** native test backend or machine-code emulation with simulated peripherals.
- **Unknown:** not established by the above. A passing component test is not a passing cold boot.

The current first-stage manifest and [latest code audit](COLD-STAGE-CODE-AUDIT.md) take precedence over earlier prototype sizes, outstanding-test descriptions and historical commands in older notes.

## 2. Hardware, installed software and working setup

| Item | Recorded state |
| --- | --- |
| Board | VSOL V2801SG; related Indian branding Digisol DG-GR6010 |
| SoC | ZTE/Sanechips ZX279125, ARMv7 Cortex-A9; stock reports 600 MHz |
| Physical DRAM | Stock reports 32 MiB |
| SPI NOR | ZB25VQ64, 8 MiB, JEDEC `5e 40 17`; modern SPI driver reports 256-byte pages, 4 KiB erase sectors |
| Installed loader | U-Boot 2013.04, Mar 07 2025, 16:14:51 |
| Stock toolchain | GCC 4.1.2, ZTE Embsys-TSP V2.08.20_P2, binutils 2.18 |
| Replacement monitor | U-Boot 2026.10, OpenWrt GCC 14.4.0, binutils 2.46.1 |
| UART | `/dev/ttyUSB1`, FTDI A5069RR4, 115200 8N1 |
| Dedicated PC Ethernet | `enx6c1ff7c6fccf`, `192.168.1.100/24` |
| Board management address | `192.168.1.1` |
| TFTP root | `/srv/tftp` |
| Test MAC | `70:b6:4f:01:a6:7a`, explicitly assigned in RAM tests |
| Optical state | Fiber disconnected during this work |

The current monitor deliberately manages the low **16 MiB** RAM bank. Commit `2a526f7e24` advertised a second 6 MiB bank at `41a00000` and gave Linux that range; it exposed the live stock cursor table at `41fef75c` and unverified DMA extents, and is reverted. Linux now describes 32 MiB with the whole `41000000..42000000` region no-map. Neither configuration measures physical capacity or proves that the packet engine intrinsically needs a fixed 16 MiB reservation. Packet pools and software cursors must remain protected until their ownership and handoff are established.

The last recorded hardware state was the modern `ZX279125 #` prompt, UART released. This is a recorded checkpoint, not a fresh hardware query made while writing this document.

## 3. Sources: vendor, upstream framework and local reconstruction

| Component | Provenance and implementation | What may be claimed |
| --- | --- | --- |
| Boot code mapped at address zero | 4096 bytes captured from this board | Actual decoded reset/NOR/UART control flow; not a vendor SDK source release |
| Stock U-Boot | Exact original backup and fresh live flash capture | Primary evidence for ZX279125 register sequences and bootstrap behavior |
| Modern U-Boot core | Version 2026.10 through OpenWrt's U-Boot package framework | Uses upstream ARM startup, relocation, driver model, command/network/SPI infrastructure |
| ZX279125 board, UART, timer, Ethernet, SPI controller and DT | Local package patch, using board-specific evidence | Local port built around upstream interfaces; **not established as upstream accepted or fully upstream compliant** |
| DDR and early SPI code | Source reconstruction of stock instructions | Vendor-derived programming sequences expressed in C; not an opaque copied runtime blob |
| First-stage wrapper, guards, trace, CRC and audit tools | Local source integration around those reconstructed primitives | New source-controlled development; hardware knowledge remains vendor-derived |
| DFP-34X-2C2 internet dump | User-supplied file; external origin not independently authenticated | Independent byte-level corroboration of matching early routines, not board-equivalence proof |
| ZX279128S Linux patch series | Related but different SoC | Reference for architecture/driver conventions, not a drop-in ZX279125 DDR or ROM implementation |

**“Vendorless” has two separate meanings:** the replacement can execute without the old vendor U-Boot binary, while still depending on register values and sequences learned from it. Removing that binary dependency does not make the hardware support upstream reviewed. Likewise, SPDX headers record declared licensing; this work is not a legal provenance audit.

### Package and upstream-facing implementation

- [Makefile](Makefile): pins U-Boot `2026.10`; source archive SHA256 `3f856da3cbbc93af51ff95fde18588a348b4a26723b01d9841e999d988481015`.
- [Platform patch](patches/001-arm-zte-add-support-for-ZTE-Sanechips-ZX279125-and-V.patch): board/DT/Kconfig, UART, timer, Ethernet, SPI controller and NOR-ID changes. It also changes common CLI files; this is broader than adding isolated board drivers and needs separate upstream review.
- [DDR source](files/board/zte/zx279125/ddr.c), [header](files/board/zte/zx279125/ddr.h).
- [Early SPI source](files/board/zte/zx279125/early_spi.c), [header](files/board/zte/zx279125/early_spi.h).
- [Earlier early-boot integration](files/board/zte/zx279125/early_boot.c), [header](files/board/zte/zx279125/early_boot.h): retained development primitive; distinguish it from the current standalone first-stage caller.

The patch contains paths for `board/zte/zx279125/zx279125.c`, `configs/zx279125_vsol_v2801sg_defconfig`, `include/configs/zx279125.h`, `drivers/net/zx279125_eth.c`, `drivers/serial/serial_zx279125.c`, `drivers/spi/zx279125_spifc.c` and `drivers/timer/zx279125_timer.c`. Generated build-tree copies are not the package's canonical editable source.

The patch introduction still mentions an environment in SPI flash at `0x7a0000`. **The effective package customization overrides that:** `ENV_IS_NOWHERE` is enabled and `CMD_SAVEENV` disabled. Read the Makefile plus generated configuration, not the introductory prose alone.

### Monitor variants

| Variant | Purpose |
| --- | --- |
| `vsol_v2801sg` | Base build; bootdelay -1 and empty bootcmd since the PPE/DMA pass, because the earlier fixed appended-DTB boot command cannot carry the external-FDT handoff |
| `vsol_v2801sg-chainload` | Skips local UART reinitialization, bootdelay -1, empty bootcmd |
| `vsol_v2801sg-uart-test` | Enables reconstructed UART initialization, bootdelay -1, empty bootcmd; current combined prototype's payload |
| `vsol_v2801sg-dump` | Chainload-style snapshot exporter with `CMD_TFTPPUT` |

Common compact configuration disables persistent environment, automatic flash unlock, several unused loading/network commands and other unnecessary features. `u-boot.img` is a mkimage-wrapped RAM artifact at load/entry `0x40400000`; it is **not** the ROM-first-stage combined image. The current combined prototype is built separately by the retained analysis script, not the normal package image recipe.

## 4. Who initializes DDR: resolved stock call path

The captured ROM NOR path does not initialize external DDR. It loads a small first stage into internal SRAM. The stock first stage then initializes clocks/DDR and loads the rest of U-Boot into DRAM.

Raw stock instruction offsets establish:

```text
flash offset 0: reset branch -> 0x58
  -> CPU setup 0x17c
  -> stack setup: SRAM SP = 0x0020a000
  -> bootstrap 0x774
       executing outside 0x4xxxxxxx:
         print "ddr init"
         call DDR initializer 0x4a8
         bus/SPI setup 0x7d0..0x838
         read flash 0..0x3ffff into 0x41f00000
         branch to 0x41f00000
       executing in DRAM 0x4xxxxxxx:
         skip that destructive initialization path
```

An early decompilation at the linked DRAM address simplified away the PC-dependent SRAM branch. That made the bootstrap look empty. Comparing raw ARM instructions in both SRAM and DRAM address views resolved the apparent contradiction. **Linked-address pseudocode alone was insufficient.**

The normal modern monitor's `s_init` is empty and `dram_init` reports configured RAM; those functions do not replace the stock cold DDR path. The standalone first stage supplies that missing work.

Stock header words include `12345678`, linked base `41f00000`, relocation/copy lengths, and `0badc0de`. The latter is a mutable exception-stack slot, not a discovered image checksum. The decoded ROM NOR path does not consult these words to choose its initial transfer length.

See [DDR path review](DDR-PATH-REVIEW.md), [whole-loader audit](WHOLE-BOOTLOADER-AUDIT.md), [early boot](EARLY-BOOT.md) and [clock/DDR diagnosis](CLOCK-DDR-DIAGNOSIS.md).

## 5. Actual ROM loading contract

The captured low-address image is strongly identified as the BootROM mapping by its reset vectors and functioning console. Capturing bytes does not independently prove physical mask-ROM immutability or exclude an alias.

| ROM observation | Meaning |
| --- | --- |
| Reset vector / literal `0x00a4` | Entry `0x20` |
| Stack literal `0x00a8` | Initial SP `0x00204000` |
| Startup zeroing | `0x00202400..0x00203fff`; earlier notes naming `0x00202000..0x002023ff` were corrected |
| Read `0x94000038`, bits 7:4 | Boot-mode dispatch; recorded register `0x20`, mode 2 |
| Modes 0 / f | UART console at ROM `0x918` |
| Mode 2 | SPI NOR loader at `0xd14` |
| NOR transfer | Read opcode `03`, flash offset zero, length `0x2000`, destination `0x00200000` |
| NOR handoff | Restore `lr = 0x158`, then `bx` to `0x00200000` |
| Stage return | `0x158` → `0x258`: print `FAIL`, spin at `0x260`; no fallback |

No header/checksum/signature validation is visible in this decoded NOR path. That is a bounded claim about these captured instructions, not a universal statement about every boot mode or ZTE SoC.

The first-stage code and literals must fit in **8192 bytes**. The monitor must be loaded separately after DDR is usable. See [BootROM analysis](BOOTROM-ANALYSIS.md); its older “next implementation” and console-test paragraphs are historical and superseded by [FIRST-STAGE.md](FIRST-STAGE.md) and [ROM-CONSOLE-TEST.md](ROM-CONSOLE-TEST.md).

## 6. Journey: hardware milestones and failed approaches

Sizes below are decimal; transfer lengths and CRCs are hexadecimal. A filename suffix identifies one artifact, not all later builds of that source.

| Step / artifact | Result and limitation |
| --- | --- |
| Modern stopped monitor `uboot-stop-7f1be111` | 232480 bytes; RAM chainload, prompt, repeated ping, TFTP and SPI reads passed. Stock DDR initialization was inherited. |
| SRAM execution `sram-probe-e253c8ef` | 302 bytes, CRC `88c5d620`; PC `00200000`, SP `00209fd8`, clean return. Established executable SRAM. |
| SPI read `sram-spi-e1a92936` | 874 bytes, CRC `c210be94`; 256 bytes matched independent `sf read`, data CRC `1209ed5c`. |
| Boot copy `sram-copy-c46c925c` | 947 bytes, CRC `af2bd70b`; full 256 KiB matched stock reference, CRC `34d4cc5a`. |
| SPI setup `sram-setup-c71e7d15` | 1190 bytes, CRC `ae548881`; setup plus 256 KiB copy passed. |
| Bus/SPI replay `sram-bus-cc5094cf` | 1309 bytes, CRC `6fa47230`; full copy passed; bus registers restored to `403f1f00` / `000003c0`. |
| Warm DDR replay `sram-ddr-recovery-97208ab2` | 2262 bytes, CRC `8bf777a7`; endless `g` output, no normal return. **Retired; not a cold-start test.** |
| Later engine/DDR attempt `92e55c1c` | 3305 bytes, CRC `424ebc83`; whitespace/newline flood after banner, no return. Retained `uboot-test.txt`; **retired**. |
| UART drain audit `96f6be2b` | 2461 bytes, CRC `1f03cb0c`; UART-only test returned; DDR deliberately not called. |
| Engine gating `sram-engine-d0dd09a3` | 648 bytes, CRC `253cd2d3`; DMA mask/BMU disable and restoration recorded; ping worked afterward. |
| PP reset probe `sram-pp-reset-4647d6a5` | 932 bytes, CRC `6d94aaba`; isolated controls tested. Did not establish a complete DDR-safe global bus drain. |
| UART-init monitor `uboot-uart-85dbdf9f` | 232544 bytes, CRC `aae282a5`; source UART initialization reached prompt and ping passed. |
| SRAM vectors `sram-vectors-006bb201` | 556 bytes, CRC `225a6c70`; intentional undefined instruction caught, caller/VBAR restored, ping passed. |
| Cold-state guard `sram-coldguard-f410546d` | 728 bytes, CRC `73ab1c2e`; already-enabled DDR rejected; SCTLR/VBAR unchanged; return and ping passed. |
| Read probe `sram-read-956b298e` | Correct reads but returned copied count instead of status; corrected source in subsequent build. |
| Alignment probe `sram-align-352b8d3c` | 412 bytes, CRC `3ec07586`; intentional unaligned read caught with DFSR 1, DFAR `00200001`, completed zero. |
| Corrected read probe `sram-read-0a152f8b` | 408 bytes, CRC `1cb4b053`; low vectors and complete 4 KiB ROM captured successfully. |
| Integrated RAM audit `stage-audit-5c110518` | 444 bytes, CRC `a83bae78`; 232544-byte monitor validated/copied/revalidated, full compare passed; manual `go 40400000` reached monitor. |
| Shared SPI audit `stage-spi-70b4ed21` | 1084 bytes, CRC `a4ce4030`; all 909 pages matched independent read; no execution of copied stock interior bytes. |
| ROM console wrapper `rom-console-f91a8de0` | 284 bytes, CRC `21584b93`; real `ROM#`, reads, jump back, SRAM restoration and ping passed. |
| Automated ROM upload | Bad-CRC NAK, corrected-packet ACK, EOT ACK, 128-byte and 3072-byte readback, harmless ARM execution and return all passed. |
| Current cold-stage audit | Sixteen machine-code cases passed offline; no new board execution or flash write. |

The failed warm experiments do not demonstrate that the stock cold sequence is wrong. They reconfigured clocks/DDR in a running, already-initialized system with possible DMA activity and inherited CPU/peripheral state. UART corruption alone cannot identify the failing instruction. Later work therefore added SRAM exception records, entry guards and isolated component tests instead of repeatedly replaying DDR warm.

### Packet-engine shutdown findings

Stock MAC cleanup clears RX/TX bits 2 and 3 at MAC bases `921c0000` and `92200000`. It does not poll global DMA/bus idle. Stock helpers separately control BMU `92168000` and DMA enable mask `000f0000` at `92164000`.

The isolated engine record showed `001f6564` / BMU 1 becoming `00106564` / BMU 0 and returning to the originals, followed by successful ping. A balanced ownership snapshot, empty mailboxes and zero sampled queue counts strengthen the observation but do not constitute an AXI drain acknowledgement. Pool-address registers remaining programmed are not proof that transfers are active or stopped.

**Warm DDR reset remains outside the validated path.** The cold-stage guard rejects an already-enabled DDR controller; successful engine probes do not override it. Detailed records: [RECOVERY.md](RECOVERY.md), [ENGINE-TEST.md](ENGINE-TEST.md), [PP-RESET-TEST.md](PP-RESET-TEST.md), [DDR-ENGINE-TEST.md](DDR-ENGINE-TEST.md).

## 7. Full stock capture and independent donor comparison

A fresh live snapshot was collected instead of relying only on scattered register reads. It includes the complete current 8 MiB NOR, separate bootloader, CPU context, register/command transcripts and chunk manifests.

| Input | SHA256 |
| --- | --- |
| Original stock bootloader backup, 256 KiB | `d286f31239edd5ff9ed5843da41148da869a68a5f72abca11e8b6ec576235c8a` |
| Fresh current bootloader, 256 KiB | `857b170b3737fd9678eb68a8d4e8f0f50f298dccff9f39e286fbd2aedf8be34e` |
| Fresh complete NOR, 8 MiB | `446cd10108602c87108d3b1db94e008e43666304d2b08e0879f3e74a588778e1` |
| Original complete backup | `45c519313ef980c68e25268d045ec85a2cdc824f443dbac05006a202b2796ab4` |
| Common first 192 KiB stock code | `5d3dc762f04968dc31e5725e48a1bd27852c760156d9142176064c4fb5a5142f` |
| Captured ROM, 4096 bytes | `59169c7e8c2d3ef66b42892a16d171bca043ad7d421e875ae53df9f8e93b9c1d` |
| DFP-34X-2C2 internet dump, 8 MiB | `358a3873ca706b50ef5fd85eb9f8a41f80f76b930321f71c5e33305bacc15239` |

Fresh and original bootloader code match for the first 192 KiB; seven differing bytes lie in the environment region at `0x30000..0x3024c`. The complete bootloader blocks therefore differ: CRC `072cb726` for the original backup, `34d4cc5a` for the current partition. Hardware CRCs `34d4cc5a` recorded in this journey refer to the current partition. **Use SHA256 and byte comparisons for identity.**

The fresh complete NOR contains the current system firmware, not an untouched original vendor system. Preserve both original and current backups. The capture included a stock soft reset; it is not a trace of physical power-on ROM execution. CPU context was captured after network/helper activity and must not be described as pristine reset state.

The donor contains U-Boot 2013.04 from March 2020. Its CPU setup `17c..1b4`, DDR `4a8..614`, early SPI/helpers `614..774`, and bootstrap `774..880` are byte-identical to VSOL. Its reset routine likewise writes 1 to `9400003c`. This corroborates the early sequence, but neither proves electrical compatibility nor supplies a missing ROM boot selector.

Sources: [live capture](LIVE-STOCK-CAPTURE.md), [snapshot](STOCK-SNAPSHOT.txt), [donor comparison](DFP34X-DUMP-COMPARISON.md).

## 8. Current first-stage layout and artifact identity

```text
SPI NOR offset                     Runtime
000000..001fff  8 KiB stage slot -> SRAM 00200000..00201fff
002000..03ac5f  monitor bytes    -> DRAM 40400000..40438c5f
03ac60..03acff  final-page padding -> read through 40438cff
03ad00..03ffff  remaining boot-partition space
```

Stage code is 3008 bytes; unused bytes in its slot are `ff`. The total combined image is `0x3ad00` / 240896 bytes, leaving `0x5300` / 21248 bytes in a 256 KiB boot partition.

**This layout overlaps the old environment at flash `0x30000..0x3ffff`.** It does not preserve that environment. The selected monitor uses RAM-only defaults and has no `saveenv`. The combined file must not be mistaken for an environment-preserving drop-in update.

| Artifact | Size | CRC32 | SHA256 |
| --- | ---: | --- | --- |
| Current cold stage | 3008 | `6ad95389` | `b5ce7e4c83612e0bba65ac572ce8fe14bddad1c4a013a71fee7db7fcd1704619` |
| Current monitor payload | 232544 | `aae282a5` | `85dbdf9f7b8fe6f0cbf9b0b30010bcaba9ec9422865186eb32f76f7f797f76f4` |
| Combined offline prototype | 240896 | `bc2f9d2c` (not in manifest) | `f082ce289e70124332f6973a1fbcdc40f31fe88edb711cc008720d46c50146c5` |
| UART-progress cold stage | 3472 | `cab400c9` | `8b0ad3ff914f0f3981e91fdaea8a595bbb6d5462d0f10d4a038601d72d756d02` |
| UART-progress combined, same payload | 240896 | `9e016337` | `7bf73793a5a1edd8e67445d9f4499c2ee21b19865ae398bb47ce73d6e7b5daaa` |
| RAM audit | 444 | `a83bae78` | `5c110518e2c7861385534a4f0e739844d3d26b3c78f06b5f6d3a33343423c3c2` |
| Rebuilt SPI audit, software-tested revision | 1084 | `e0667790` | `97aa5e6fb7643637cc6e101cbdae8bcc5707153ca0496ed992a2fab57b5cdf5c` |

**Reproducible build (24 October):** the original `build.py` took the monitor from the `uart-test` build directory, which has since been rebuilt (234792 bytes, CRC `e1a6d58b`). [build-first-stage.py](tools/build-first-stage.py) replaces it: it takes an explicit payload, refuses a SHA256 mismatch, and uses the U-Boot tree only for compiler flags. The frozen payload is retained as `tmp/v2801sg-cold-candidate/inputs/monitor-85dbdf9f.bin`. Rebuilding the unchanged stage sources from it reproduced `cold.bin`, the combined image, `audit.bin` and `spi-audit.bin` byte-for-byte.

### UART-progress candidate

The successor candidate keeps the same payload and adds UART output, writing directly to the UART the ROM has already configured (`94405000`, 25 MHz divisor 13 + 36/64; the monitor programs the same divisor). Each character waits at most 100000 polls for FIFO space, then is dropped rather than blocking boot.

```text
success:  ZXS ddr spi ok
failure:  ZXS ddr [spi] FAIL <phase> <result> <ddr-phase> <copied-bytes>
fault:    ZXS FAULT <type> <DFAR> <raw exception LR>
```

Fields are the trace words 1, 2, 8 and 7 in hexadecimal; ` spi` appears once DDR has completed. ROM-level refusals still print only the ROM's `FAIL`. The fault print is enabled by `ZX_EARLY_FAULT_UART`, so other users of `early-cpu.S` are unchanged. That UART output survives the DDR PLL writes is inferred from the fixed 25 MHz divisor, not measured.

The cold stage and rebuilt SPI audit remain offline, not staged as new hardware test images. The SPI hardware result belongs to the earlier `70b4ed21` artifact. The subsequent change moves top-level trace phase 3 before SPI setup so a setup timeout is correctly attributed; it does not change the intended MMIO sequence.

### SRAM and monitor boundaries

| Region / setting | Address or rule |
| --- | --- |
| Stage link/entry | `00200000`; linker budget ends before `00202000`; initialized data/BSS rejected |
| Trace | `00202400`, twelve words |
| Fault record | `00202500` |
| Fault stack top | `00203000` |
| Main stage stack top | `00204000` |
| Monitor link/entry | `40400000` |
| Monitor initial stack | `40700000` |
| Early monitor malloc | `0x4000` bytes |
| Main stage conservative stack usage | 112 bytes including entry save; lowest observed emulated stack write `00203f94` |

Trace words: magic `53544731`; phase; signed result; computed CRC; payload length; expected CRC; original DDR enable; completed SPI bytes; DDR phase; last DDR MMIO address; value; write-state marker. Phase 5 means loader completion, not proof of a subsequently functioning monitor. Fault magic is `46544c31`.

The monitor ELF's allocated file-backed sections match the payload. `u-boot-nodtb.bin` plus the exact 1800-byte DTB equals that payload. Build gap-fill is `ff`; raw objcopy with zero-filled gaps differs only in those expected padding locations.

## 9. DDR source equivalence and its limits

Current [ddr.c](files/board/zte/zx279125/ddr.c) reconstructs stock `41f004a8..41f00610`. Its 35 ordered writes and three 2000-iteration delays match the retained instruction-derived trace. It uses relaxed MMIO and bounded polling, rather than adding an ordering barrier around every access.

Ordered sequence, with values in hexadecimal:

```text
94000018=00040c19; 94000020=00040a0d; 94000028=80040d62
DELAY 2000 iterations; 94000028=00040d62
POLL: AND of 9400001c / 94000024 / 9400002c must contain 10000000
9400000c=01a0060e; 94000010=00000010
94000014=ffffffff; 94000008=ffffffff; 94000004=00000008
9a100010=00027303
94400010=00000303; 94400014=00000303; 94400018=00000303
94300000=66; 94300004=31; 9430000c=0
94300080=14; 94300084=3; 94300088=a; 9430008c=7
94300090=3; 94300094=a; 94300098=3; 9430009c=2
943000a0=2; 943000a4=10; 943000a8=c; 943000ac=32; 943000b4=33
94302018=4; 94302024=4
94000008=read_value OR 180; DELAY 2000 iterations
94300008=0; POLL 94300104 != 0
9430000c=1; POLL 94300100 != 0
94000008=read_value OR c00; DELAY 2000 iterations
```

Poll ceiling is 1,000,000 iterations, not a calibrated wall-clock timeout. Delay loop instruction timing differs between the old vendor compiler and current GCC. Matching write order and loop count does not establish identical settling time. A bus transaction that never returns can defeat an otherwise finite polling loop.

The CPU helper follows the stock SCTLR update `(old & ~0x2007) | 0x1802` and associated invalidation/barrier sequence, with additional entry guards and SRAM vectors. It refuses incompatible execution state and warm initialized DDR. This does not turn warm replay into a cold test.

## 10. Offline verification: exactly what ran

[Complete audit](COLD-STAGE-CODE-AUDIT.md) and retained harness use Unicorn 2.1.2, Cortex-A9, starting at the captured ROM reset vector at zero. The ROM loads the actual stage; actual compiled CPU/DDR/SPI/CRC code executes; the actual monitor startup executes until **entry to `board_init_f` at `4040c22c`**. The harness stops there, before full U-Boot device initialization.

Only peripheral behavior is supplied by the model: PLL/DDR ready values, SPI FIFO contents and UART readiness. No DDR routine, delay loop, page-copy loop or CRC function is substituted. The harness rejects unexpected MMIO writes and DRAM accesses before DDR completion. Success checks the ROM's one 8192-byte transfer and every one of the stage's 909 256-byte reads.

Sixteen passing cases:

1. Normal NOR reset reaches monitor `board_init_f` with exact payload.
2. Alternate preserved SCTLR input `08c50078` reaches the same handoff.
3. Corrupted payload halts on CRC error `-74` before monitor execution.
4. PLL never locks: bounded timeout `-110`.
5. DDR status `104` never ready: timeout, no premature DRAM access.
6. DDR status `100` never ready: same.
7. SPI setup never ready: timeout correctly reported as phase 3.
8. First-page FIFO stall: timeout, no handoff.
9. SPI completion never arrives: timeout.
10. FIFO failure after sixteen pages: timeout with completed count `0x1000`.
11. Already-enabled DDR: refusal before peripheral reconfiguration, caller state restored.
12. I-cache enabled on entry: refusal before stack/peripheral writes.
13. IRQ-mode entry: refusal before stack/peripheral writes.
14. Synthetic data abort during DDR store: actual SRAM vector/record/halt path.
15. ROM mode input 0: ROM UART console, no DDR/first-stage execution.
16. ROM mode input f: same.

On a physical cold boot, "caller state restored" (cases 11–13) or a CPU-setup refusal means returning to the ROM, which prints `FAIL` and hangs. In the frozen `6ad95389` stage, CRC, timeout and fault cases spin silently. The UART-progress candidate passed the same sixteen cases with its output asserted: the exact success line, a `FAIL` line matching the SRAM trace for every halt case, and `FAULT 00000004 94300080 <LR>` for the synthetic abort. Its lowest stack write is unchanged at `00203f94`. Every cold failure still ends in a hang requiring external recovery; the output only identifies where.

Rerun: `tools/emulate-first-stage.py <stage-dir> <out-dir> success alternate-sctlr crc pll ddr104 ddr100 spi_setup spi_fifo spi_done spi_fifo_late warm icache irq fault mode0 modef`. Unicorn 2.1.2 is retained in `tmp/v2801sg-emulation-deps` (override with `ZX_EMULATION_DEPS`); the DDR reference trace is now tracked as [ddr-vendor.trace](tools/ddr-vendor.trace).

The synthetic abort supplies DFSR 8 / DFAR `94300080` at the DDR store. It verifies the handler, not physical bus fault delivery. Native tests of actual C additionally cover full copy, CRC, rejection before writes, all page arguments, partial progress and readiness guards.

**None of these simulate actual DDR signals, training, PLL settling, undocumented clock domains or physical reset-mode selection.**

## 11. Recovery: three distinct mechanisms

### A. Stock reset-button upgrade

The user's held-button startup reached stock U-Boot and requested `upgrade.bin` from `192.168.1.100` into `40600000`. Aborting printed update failure and `downver` usage, then reset. Stock instructions identify GPIO input `94404014` bit 9 and dispatch `downver version`.

This installer can write flash; it is not merely a convenient download command. It depends on stock U-Boot still executing, so replacing its code may remove this path. Keep unintended `upgrade.bin` downloads out of RAM experiments. It is not BootROM recovery.

### B. ROM console and XMODEM: warm access verified

The [wrapper](tools/rom-console-audit.S) enters only ROM routine `0x918`, skipping ROM reset, boot dispatch and DDR initialization. It fingerprints ROM instructions and checks CPU state. `g 200100` returns through a fixed SRAM trampoline.

Hardware verified:

- `r 0 10` displays the expected low-address boot code.
- Bare hexadecimal parsing: addresses are entered without `0x`.
- `x 201000` receives XMODEM-CRC, 128-byte SOH packets and big-endian CRC16.
- Deliberately bad CRC gets NAK `15`; corrected packet gets ACK `06`; EOT `04` gets ACK.
- One-block and 24-block / 3072-byte uploads match complete readback.
- Harmless 28-byte ARM payload writes `55504c31` at SRAM `00201200`, then returns to `ROM#`.
- Return to U-Boot, full SRAM restoration CRC `89242f8e`, and ping passed in both automated runs.

Payload SHA256: `da4238e6e886a96835806f2dafec6e117c81fe0053a31c886e6577af2b3c7e6e`.

This proves the receiver and execution mechanism once inside the console. **It does not prove how to enter it from reset after corrupting NOR.**

### C. Physical reset-state entry / external programmer

ROM reads mode bits from `94000038`; modes 0/f select UART, observed board mode is 2. No validated writable override or board pin assignment has been found. Stock and donor reset routines only write 1 to `9400003c`. The NOR path has no demonstrated UART-key escape or bad-image fallback before executing flash contents.

It is not yet established whether the mode field comes from straps, another latch, or some software-controllable configuration. Do not equate a decoded mode table with a verified boot-selector circuit.

Fifteen PCB photos did not reveal a mapped selector. XS1 labels GND/SDA/SCL/3.3V identify I²C, not a ROM boot switch. Public product/catalogue material did not supply the missing pin mapping. See [photo review](PCB-PHOTO-REVIEW.md) and [ROM UART investigation](ROM-UART-ENTRY.md).

The user has a CH341A and a full backup, but reported a finicky clip. Possession of those is not a demonstrated restore path. Before relying on it for a one-time cold test, independently establish repeatable chip identification/readback and a practical restoration procedure. No programmer write has been tested in this journey.

## 12. Source and evidence map

Paths below are relative to `/home/ali/openwrt-galachip` unless explicitly absolute. Ignored `tmp/` files are local retained evidence, **not guaranteed to survive cleanup or appear in Git**. Preserve them outside build scratch before installation work.

| Location | Contents |
| --- | --- |
| `package/boot/uboot-zte/tools/first-stage.S`, `.c`, `.lds` | Current entry/guards, CRC/page loader and linker constraints |
| `package/boot/uboot-zte/tools/early-cpu.S`, `.lds` | CPU setup and SRAM exception support |
| `package/boot/uboot-zte/tools/stock-context.S`, `.lds` | Passive stock CPU snapshot helper |
| `package/boot/uboot-zte/tools/sram-read-probe.S`, `.lds` | Bounded read and fault recovery |
| `package/boot/uboot-zte/tools/sram-vector-audit.S`, `.lds` | Intentional exception/return test |
| `package/boot/uboot-zte/tools/sram-cold-diagnostic.S`, `.lds` | Cold-entry diagnostic development |
| `package/boot/uboot-zte/tools/rom-console-audit.S`, `.lds` | Warm ROM console wrapper and return trampoline |
| Other `tools/sram-*` | Historical SRAM/SPI/engine/UART/DDR experiments; consult their dated test notes |
| `package/boot/uboot-zte/tools/build-first-stage.py`, `emulate-first-stage.py`, `ddr-vendor.trace` | Pinned-payload stage build, ROM-to-monitor emulation harness and DDR reference trace |
| `tmp/v2801sg-cold-candidate/` | `inputs/monitor-85dbdf9f.bin`; `repro-6ad95389/` (byte-identical rebuild); `uart/` (UART-progress candidate, manifest, emulation log) |
| `tmp/v2801sg-emulation-deps/` | Unicorn 2.1.2 used by the harness |
| `tmp/v2801sg-first-stage/` | Historical build.py, generated header, manifest, binaries and maps of the `6ad95389` candidate; combined file named `combined-OFFLINE-NOT-FOR-FLASH.bin` |
| `tmp/v2801sg-cold-audit/` | emulate.py, summary.json, final-cases.log, build.log |
| `tmp/v2801sg-stock-live-20261023/` | Complete NOR, bootloader, ROM bin/asm/UART, CPU/register logs, chunk manifests |
| `tmp/v2801sg-rom-upload/` | Sender scripts, harmless payload, raw UART logs, one-/multi-block structured results |
| `tmp/dfp34x-analysis/` | Donor bootloader, byte comparison and early instruction listings |
| `/tmp/v2801sg-whole-boot-audit/` | Full stock REA exports, raw listings, evidence, manifests; temporary location |
| `/tmp/v2801sg-ddr-audit/vendor.trace` | Ordered instruction-derived DDR reference trace; temporary location |
| `/tmp/zte-upstream-review-20261023/` | Related ZX279128S Linux patch-series review; temporary location |
| `/home/ali/Desktop/MISC-3/V2801SG_Backups/v2801sg-mtd1.bin` | Original vendor bootloader backup |
| `/home/ali/Downloads/DFP-34X-2C2-mtd0.dd` | User-supplied donor dump |
| `/home/ali/Desktop/uboot-test.txt` | Failed warm engine/DDR output capture |
| `/home/ali/Desktop/pictures.tar.gz` | PCB photo source archive |

The whole-loader REA pass enumerated 583 functions; 578 decompiled, five retained raw-instruction fallbacks. An SRAM-mapped analysis view was used to understand PC-dependent code; it is not evidence of 256 KiB physical SRAM. Full disassembly remains authoritative where decompiler control flow is misleading.

Useful retained REA evidence identifiers:

| Subject | Evidence ID |
| --- | --- |
| CPU setup | `ev_76d18272a72dea2b2080d818ba2ff5eba90b2fd1cd0629ef5034e378382a69ab` |
| DDR routine | `ev_78fd71c2cdd374f19205e5ac246a33b0c82b7349f165d3d90ce76a14247413f0` |
| Delay | `ev_e30c123e45925d86251f9d73237befee57f21c00dfa51496c49d87a39b28a0f5` |
| SRAM bootstrap | `ev_5a17094aa39f84444f2c6154d7bbfbf2d47148b1f24b6e6c8d0db38be76984cd` |
| Reset-register write | `ev_69cd8d119c45ecd6e23fbf2735c145b5a4691f5990d0ae8e53e29cc31e47d276` |

### Detailed development notes

- CPU/state: [COLD-CPU-ENTRY.md](COLD-CPU-ENTRY.md), [COLD-DIAGNOSTIC.md](COLD-DIAGNOSTIC.md), [VECTOR-AUDIT-TEST.md](VECTOR-AUDIT-TEST.md).
- UART: [UART-FLAGS-TEST.md](UART-FLAGS-TEST.md), [UART-INIT-TEST.md](UART-INIT-TEST.md).
- ROM: [SRAM-READ-PROBE.md](SRAM-READ-PROBE.md), [BOOTROM-ANALYSIS.md](BOOTROM-ANALYSIS.md), [ROM-CONSOLE-TEST.md](ROM-CONSOLE-TEST.md).
- Layout/history: [ROM-LOAD-CONTRACT.md](ROM-LOAD-CONTRACT.md), [FIRST-STAGE.md](FIRST-STAGE.md), [CHAINLOAD.md](CHAINLOAD.md).
- Current audit: [COLD-STAGE-CODE-AUDIT.md](COLD-STAGE-CODE-AUDIT.md).
- Wider port context: [Ethernet bring-up](../../../target/linux/zte/ETHERNET-BRINGUP.md), [xPON integration](../../../target/linux/zte/XPON-INTEGRATION.md). Linux bridge/optical readiness is separate from loader readiness.

### External reference trail

These identify the sources consulted earlier; no new external review was performed when consolidating this handoff.

- [Related Linux ARM patch thread supplied by the user](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186634.html): ZX279128S-related upstream work; distinguish SoC and subsystem before adopting anything.
- [Sanechips product page](https://www.sanechips.com.cn/en/ProductEn/WiredEn/fixedNetworkTerminalEn/1552570424078929921.html) and [Chinese product page](https://www.sanechips.com.cn/chinese/product/wiredProducts/terminalChip/1556481690484883457.html): feature information, not a recovered boot-pin specification.
- [JLC part listing](https://jlcpcb.com/partdetail/JLCPCBAssembly-ZX279125/C9900040717): catalogue lead; no usable verified pin library recovered from it.
- [Other ZX279125 device research](https://github.com/0xXA/hacking-sharp-vision): related platform evidence, not this board's boot selector.
- [ZX279128S reference project](https://github.com/Zlion-Y/e2631-mainline-zx279128s): different SoC, not a substitute for ZX279125 cold-start evidence.

## 13. What remains before a controlled replacement cold test

| Requirement | Current status / next action |
| --- | --- |
| Exact ROM transfer contract | Decoded from captured instructions; model exercised |
| First stage fits ROM window | Linker checked; 3008 / 8192 bytes |
| Combined image fits partition | Checked; 240896 / 262144 bytes; old environment overlap explicit |
| CPU/DDR source fidelity | Raw stock and donor comparison, ordered trace and model checks passed |
| SPI transport and copied monitor | Separately hardware tested on initialized board |
| Complete cold DDR + monitor | **Not hardware tested** |
| Early failure visibility | UART progress/FAIL/FAULT lines implemented and model-checked (`cab400c9`); not yet seen on hardware |
| Reproducible candidate | Pinned-payload build reproduces the frozen stage byte-for-byte; scripts not yet committed |
| Reset-state ROM UART selection | **Unresolved**; warm console success does not supply it |
| Independent recovery | Programmer/backup available in principle; repeatable connection/restore capability not demonstrated |
| Package/install integration | Combined image still a separate offline prototype, no validated installer |
| Environment and board identity | Preserve original data separately; selected monitor uses RAM defaults, test MAC currently set manually |
| Linux handoff after new monitor | Validate retained queue consumers, reserved pools and cursor compatibility explicitly |
| Upstream acceptance | No acceptance established; local drivers, bindings and common CLI changes still require review |

Early progress visibility and a reproducible candidate build are done in source and model (section 8). Without flashing, hardware can only confirm the new stage's warm refusal (the guard must reject initialized DDR before any output or peripheral write, as model case 11 shows); its UART lines first appear on a real cold boot. The next hardware milestone is a recoverable physical reset-to-monitor boot, then repeated power cycles, UART, SPI identity/readback, network and Linux handoff tests. Do not use the retired warm DDR images to stand in for that milestone.

A first-attempt cold success cannot be guaranteed from the present evidence. This handoff records the work needed to make the decision concrete; it does not authorize installation or supply flash-write commands.

## Subsequent PPE/DMA source checkpoint

[PPE-DMA-MEMORY.md](PPE-DMA-MEMORY.md) records the later source changes to cursor ownership, fault handling, external-FDT handoff and Linux diagnostics. The image identities in this journey remain the frozen earlier cold prototype; the newly rebuilt monitors are separate artifacts, not silently substituted payloads. Hardware verification of the later changes remains pending.
