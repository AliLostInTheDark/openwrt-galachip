# Integrated cold-entry diagnostic and warm guard audit

The diagnostic now links the source CPU-entry helper, shared SRAM exception
handlers and source DDR initializer. It records CPU entry state and DDR phase
progress in SRAM. It does not load a second stage, use UART/SPI, write NOR or
have a proven ROM wrapper. It is not a replacement boot image.

The complete cold diagnostic is 2164 bytes and remains offline at
`/tmp/v2801sg-cold-diagnostic/cold.elf` and `cold.bin`. Code/literals must end
below 00200c00; the exception stack ends at 00200d00, the exception record is
at 00200e00 and the phase record at 00200e80. The caller stack is at 0020a000.
The same fault handlers passed the preceding hardware undefined-instruction
audit. In the cold diagnostic they record and halt for every vector.

The CPU helper rejects initialized DDR before CPU control writes. Once it
accepts cold entry, it configures CPU state and installs the SRAM vectors;
the diagnostic then calls the traced DDR initializer and halts with its
result. Exceptions during CPU setup before VBAR installation are not covered
by the new handlers. A stalled bus transaction may not deliver an exception.
SRAM survival across reset and retrieval from an unresponsive CPU remain
unproven; phase records alone do not resolve those limitations.

`ZX_DDR_TRACE` is an optional diagnostic-only C compile define. It records
logical DDR phases plus the last write address/value and before/after state.
The normal DDR object has byte-identical init/delay text compared with the
previous relaxed-MMIO object. The trace variant changes timing by writing
SRAM around MMIO; it must not be described as instruction-identical to stock.

## Phase record

| Offset from 00200e80 | Meaning |
| --- | --- |
| 00 | Magic 43535431 |
| 04 | Stage: 1 CPU entry, 2 guard refusal, 3 CPU ready, 4 DDR entered, 5 DDR returned successfully, 15 error |
| 08 / 0c | CPU / DDR return value; DDR result is meaningful only if call count is 1 |
| 10 / 14 | SCTLR before / after CPU helper |
| 18 / 1c | VBAR before / after CPU helper |
| 20 | Initial DDR controller enable word |
| 24 | DDR call count |
| 28 | DDR phase: 1 PLL programming, 2 PLL lock wait, 3 clocks, 4 timing registers, 5 readiness 104, 6 readiness 100, 7 final clock write, 8 complete |
| 2c / 30 / 34 | Last DDR MMIO write address / value / state (1 before, 2 after) |

## Verified offline

Build and instruction-model evidence is in `/tmp/v2801sg-cold-diagnostic/`.
The compiled warm paths return with no CPU-control/MMIO writes and restore
the caller's SP, CPSR and callee-saved registers. The guard-only image refuses
zero controller-enable state before calling the CPU helper. Modeled cold
execution installs SRAM vectors, produces all 38 vendor MMIO write/delay
events in order, records completion and halts. The delay body is abstracted
by the model, and MMIO/CP15 results are modeled rather than hardware proof.

## Hardware test: guard-only image

Only the guard audit is staged. It has no DDR initializer linked into it.
It verifies the actual CPU helper refuses the current warm hardware state,
while the wrapper compares SCTLR/VBAR before and after. It does not exercise
the accepted cold CPU/cache transition or DDR programming.

File: `v2801sg-sram-coldguard-f410546d.bin`; 728 bytes (`2d8` hex), CRC32
`73ab1c2e`, SHA256
`f410546db2a82b5271d80b1188af886eb49d17f6d719d33afd62e5c95c95a7ad`.
The source is `tools/sram-cold-diagnostic.S` built with
`ZX_COLD_GUARD_AUDIT`; the cold helper and fault handlers are linked from
`tools/early-cpu.S` with fault stack 00200d00. No fault-return audit define
is enabled. Hardware result: passed, as recorded below.

From the working RAM monitor:

```text
tftp 0x40500000 v2801sg-sram-coldguard-f410546d.bin
crc32 0x40500000 0x2d8
cp.b 0x00200000 0x40510000 0x1000
crc32 0x40510000 0x1000
```

Require payload CRC `73ab1c2e` and backup CRC `d11f7b32`, then:

```text
dcache off
icache off
cp.b 0x40500000 0x00200000 0x2d8
crc32 0x00200000 0x2d8
```

Require SRAM CRC `73ab1c2e`, then:

```text
go 0x00200000
```

Require application return code 0 and a responsive prompt. Then:

```text
md.l 0x00200e80 0xe
cp.b 0x40510000 0x00200000 0x1000
crc32 0x00200000 0x1000
ping 192.168.1.100
```

The first words must be `43535431 00000002 ffffffff 00000000`.
Both SCTLR values and both VBAR values must match; the initial DDR enable
should be 1, DDR call count and phase 0. Restored prefix CRC must be
`d11f7b32`, followed by working ping. Stop on unexpected output; if the
application does not return, stop hardware work and recover physically.

## Hardware guard result

The user transferred 728 bytes and confirmed DRAM/SRAM image CRC `73ab1c2e`.
The original prefix backup CRC was `d11f7b32`. The application returned 0
and the phase record was:

```text
00200e80: 43535431 00000002 ffffffff 00000000
00200e90: 08c5087a 08c5087a 40fb4000 40fb4000
00200ea0: 00000001 00000000 00000000 00000000
00200eb0: 00000000 00000000
```

Stage 2 and CPU return -1 confirm refusal on the initialized controller
(enable word 1). SCTLR remained `08c5087a`; VBAR remained `40fb4000`.
DDR call count, phase and write trace stayed zero. The prefix was restored
with CRC `d11f7b32`, and ping to 192.168.1.100 succeeded afterward.

This verifies the integrated helper's warm-state refusal and observable
SCTLR/VBAR preservation. It does not exercise its accepted cold CPU/cache
transition, install the diagnostic vectors on that accepted path, or execute
DDR initialization. The cold diagnostic remains offline.
