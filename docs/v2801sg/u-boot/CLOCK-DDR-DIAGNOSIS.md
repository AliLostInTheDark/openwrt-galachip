# Failed warm DDR replay: diagnostic findings

2026-10-23. Hardware testing remains paused after the failed combined probe.
Do not run another DDR image from these findings alone.

## Established evidence

The combined image's two CRCs matched. The complete banner was visible before
60669 line-feed bytes and four other bytes in the supplied capture. Its SHA256
is 506a60e93ae8484694e95aec1d9605b432d1d5dd1e67695b57c39790aecb2917.
The assembled entry prints that banner only after engine-reset success and
DMA/BMU/MAC/reset-request guards. No handoff or DDR-completion message appears.
There is no intentional repeated-newline loop in the diagnostic source.

The DDR routine's 35 writes and three delay events match the raw stock routine
at 41f004a8..41f00610. This does not establish identical execution timing,
CPU state or safe replay against a running DDR controller. In particular a
bounded poll cannot help if an MMIO load/store itself stops completing.

## Material CPU-state difference

Raw stock instructions at 41f0017c..41f001ac invalidate TLB, instruction cache
and branch predictor, clear SCTLR M/C bits, and set SCTLR I (bit 12), Z
(bit 11) and A (bit 1), with high vectors cleared. Stock then branches through
41f001b4 to its SRAM-stack low-level entry. Thus stock DDR startup uses
instruction cache and prediction enabled, whereas our failed probe explicitly
rejects enabled instruction cache and leaves prediction state inherited.

That is a real mismatch, not proof that enabling cache fixes DDR. Any experiment
must first establish its cache-maintenance, SRAM code visibility and entry
state, and should not conflate a cache change with UART/clock changes. The
current default guard must not simply be removed to force a retry.

## UART limitation and clock-changing writes

Our drain polls flags 94405014 bit 7, named TX-empty by the current driver.
The UART-only audit physically validates draining/return under unchanged
clocks. It does not independently prove the transmitter shift register is
idle during a clock change. Do not infer a BUSY-bit definition from PL011 or
ZX279128S without exact ZX279125 evidence.

The DDR routine also rewrites global clock/reset words 94000004/08/0c/10/14,
PLL controls 94000018/20/28, and peripheral controls 94400010/14/18. The
upstream ZX279128S clock table identifies the latter offsets as UART0/UART1/SPI,
but exact ZX279125 field semantics remain unproven. Repeated newline output
could reflect UART behavior after those writes rather than a software loop.
It cannot tell whether DDR polling, SPI copy or CPU execution failed.

## Next non-invasive evidence after physical recovery

Power-cycle with button released, interrupt normal stock autoboot and confirm
the prompt. These are reads only; do not run them in the failed session:

```text
md.l 0x94000000 0xc
md.l 0x94400000 0x7
md.l 0x94405014 0x1
md.l 0x94300000 0x4
md.l 0x94300100 0x2
md.l 0x94302018 0x1
md.l 0x94302024 0x1
```

Compare live PLL/control values against the vendor write sequence before
designing a narrower experiment. A future diagnostic should mark before/after
each clock/DDR phase in SRAM and preserve exception context with SRAM vectors;
UART output alone is insufficient localization. SRAM trace survival across
reset is not established, so do not assume a power-cycle preserves such a
record. No instrumented replay is staged or proposed for execution yet.

## Stock snapshot supplied after recovery

The user read PLL controls 94000018/20/28 as 00040c19/00040a0d/00040d62;
all three lock/status words have bit 28 set (14000000, 14000000, 1447ae14).
Clock selector 9400000c is 01a0060e, 94000010 is 10, and 94000008 is ffffffff.
These agree with the replay targets. Stock's later state differs at 94000004
(0b versus the early write 08) and 94000014 (077faabf versus ffffffff).
Replaying the early sequence therefore also changes established runtime
selector/gate state; equal PLL target values do not make it a no-op.

Peripheral controls 94400010/14/18 are already 303. DDR control/timing values
66/31, controller enable 1, both readiness words 1, and PHY timing words 4/4
also match the expected initialized state. This establishes a correct baseline,
not safe reinitialization of an already-active controller.

UART flags sampled by `md.l` were 110. This has bit 7 clear and bit 8 set,
but it is not an idle measurement: the command itself echoes characters
before reading the register. Consequently this sample does not invalidate
the bit-7 definition, prove bit 8 is TX-empty, or justify changing either
driver. Exact stock bootloader putc tests only TX-full bit 5; its getc tests
RX-empty bit 4. A quiet SRAM sampler or exact vendor-kernel tx_empty routine
would be needed to resolve transmitter-empty/busy semantics independently.

No clock, UART or DDR constants were changed based on this snapshot. The
failed combined image stays retired. The next localization must distinguish
UART transmission effects from CPU/clock failure, without another blind full
DDR replay or assumed PL011 status definitions.
