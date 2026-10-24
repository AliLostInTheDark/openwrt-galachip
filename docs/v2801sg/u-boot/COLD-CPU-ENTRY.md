# Cold CPU entry and SRAM fault recording

`tools/early-cpu.S` provides support code for a future SRAM first stage. It is
not connected to U-Boot startup, packaged for ROM or staged for hardware use.
It must not be invoked as a standalone application: the caller must already
have a valid SRAM stack and a defined return/halt path.

The initializer rejects actual PC or SP in the DDR aperture, a mode other
than SVC, an active MMU/data cache, and a nonzero DDR controller enable word.
The last guard rejects the established warm state; it does not prove that
every other reset-state prerequisite is met. Failure returns -1 before CPU
control writes. Success masks IRQ/FIQ, invalidates TLB/I-cache/predictor,
applies the observed stock SCTLR mask, and installs the aligned SRAM VBAR.
Explicit synchronization follows the control writes. This is a cold-only
operation and does not restore the caller's interrupt/cache/vector state.

Eight vectors lead to SRAM handlers that use the observed 0020a000 stack
top, save handler-visible r0-r12 and raw exception LR, record status/address
registers, publish a completion marker and halt without UART or DDR accesses.
FIQ has banked r8-r12: those slots describe the handler's bank, not the
interrupted non-FIQ registers. The record does not capture the interrupted
stack pointer. Raw LR needs exception-specific interpretation; it is not
presented as an adjusted fault PC.

| Record address | Content |
| --- | --- |
| 00200e00 | Completion marker 46544c31, written last; zero while filling |
| 00200e04 | Vector index 0..7 |
| 00200e08 | Handler CPSR |
| 00200e0c | SPSR |
| 00200e10..1c | DFSR, IFSR, DFAR, IFAR |
| 00200e20..54 | Saved r0-r12 and raw exception LR |

The linker script bounds code/literals below 00200e00 and rejects initialized
data/BSS. The compiled support binary is 440 bytes, with vectors aligned at
002000a0. It is retained only in `/tmp/v2801sg-early-cpu.{o,elf,bin}` for
offline checks. It has no DDR initializer, SPI operation or flash writer.

The focused model `/tmp/v2801sg-check-early-cpu.py` executes the assembled
instructions with modeled CP15/MMIO state. It checks refusal paths before
control writes, stock SCTLR masking, aligned VBAR selection, and all eight
vector paths saving the registers/status/marker and halting. The model does
not simulate bus stalls, cache behavior or real exception delivery. SRAM
records are not known to survive reset, so the record alone is not a way to
recover evidence from a powered-off or inaccessible board.

Before integration, the first-stage caller must set its stack, establish ROM
load compatibility and choose a usable diagnostic retrieval method. A future
warm test must exercise vectors without calling this cold initializer or
DDR setup. The previously failed DDR replay remains retired.

That separate warm vector test has now passed on hardware. The undefined
instruction entered SRAM vector 1, recorded UND/SVC status and the exact
resume LR, returned to U-Boot, and allowed SRAM restoration and ping. See
[VECTOR-AUDIT-TEST.md](VECTOR-AUDIT-TEST.md) for the raw record and limits.
The cold initializer itself remains untested and is not called by this audit.

The helper, fault handlers and optional DDR phase trace are now integrated in
the separate [cold diagnostic](COLD-DIAGNOSTIC.md). Only its DDR-free warm
guard audit is staged; accepted cold entry and DDR execution remain untested.
