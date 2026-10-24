# DDR path review: vendor startup versus our current images

## Conclusion

The vendor flash bootloader contains a real clock/DDR-controller setup path
and calls it from its non-DRAM bootstrap. DDR initialization is not merely
assumed from a function name or a boot message. The raw instructions program
the controller/PHY and wait for readiness before copying NOR into DRAM and
executing there. This does not establish whether ROM performs any preliminary
setup before entering the flash code.

The detour was trying to validate that cold sequence by replaying it after
DDR and the rest of the SoC were already running. A warm SRAM `go` changes
where the CPU executes; it does not restore reset-state clocks, controllers
or outstanding bus transactions. The failed replay does not show that the
vendor DDR constants are wrong.

## Independent reset-path check

Stock image SHA256:
`d286f31239edd5ff9ed5843da41148da869a68a5f72abca11e8b6ec576235c8a`.
The focused model `/tmp/v2801sg-ddr-path-review/reset-path.py` interprets
the raw ARM reset instructions under two mappings. UART output is modeled
at the exact early-putc entry; CP15 effects are modeled, not real cache timing.

| Mapping | Result |
| --- | --- |
| 00200000, SRAM | Reset branch -> CPU setup -> SRAM stack -> bootstrap -> `ddr init` -> DDR entry 002004a8; SP 00209fe8 |
| 41f00000, linked DRAM | Same CPU setup/stack entry, bootstrap skips the DDR path and returns to 41f00080; no `ddr init` output |

Raw call chain, expressed as flash offsets:

```text
0000 -> 0058 reset
     -> 017c CPU control setup
     -> 01b4 -> 0460 SRAM stack setup
                    -> 0774 bootstrap
                       PC outside 4xxxxxxx:
                         078c..07c8 emits "ddr init"
                         07cc calls 04a8 DDR initializer
                         07d0..0838 bus/SPI setup
                         083c..085c copies NOR pages into DRAM
                         0864 jumps to 41f00000
                       PC in 4xxxxxxx: return at 0868
```

The user-observed power-on `ddr init` message is consistent with that exact
path. Ghidra's normal linked-address pseudocode simplifies the bootstrap to
an empty return because the PC comparison is statically true at 41f00000.
That optimization must not be interpreted as absence of the SRAM path.

## What the DDR routine actually does

| Stock linked instructions | Operation |
| --- | --- |
| 41f004b4..04d4 | PLL controls at 94000018/20/28, including an assertion/deassertion and delay |
| 41f004dc..04f4 | Require bit 28 in all three PLL status words |
| 41f004fc..053c | Global clock/control writes, then 9a100010 and peripheral controls 94400010/14/18 |
| 41f00544..05b8 | Controller parameters at 94300000 and timing words at 94300080..b4; PHY values at 94302018/24 |
| 41f005bc..05dc | Clock/control RMW, delay, controller write and readiness poll at 94300104 |
| 41f005e4..0610 | Controller enable at 9430000c, readiness poll at 94300100, final clock/control RMW and delay |

The reconstructed source and the latest traced compiled diagnostic match
35 ordered MMIO writes and three delay events. Differences remain: bounded
polls, different compiler instruction scheduling/delay-loop shape, deliberate
diagnostic SRAM writes, and the entry state. Event parity is not electrical
timing validation. The normal relaxed-MMIO routine has no per-access DMBs;
the earlier failed image did have them.

Neither this routine nor readiness bits constitute a full DDR memory test.
The vendor's subsequent flash copy and execution from DRAM provide additional
operational evidence for its own cold boot. Our accepted cold path has not
yet been run on hardware.

## What blocks our path now

1. **The actual monitor has no cold DDR startup caller.** In the newly compact
   base ELF, `s_init` at 4040044c is just `bx lr`. `dram_init` at 4040124c
   writes 01000000 to the RAM-size field and returns; it does not program
   DDR. The DDR initializer and bootstrap loader are discarded from that ELF.
   Calling this a cold build describes its UART configuration, not a complete
   standalone reset path. The new cold diagnostic is a separate SRAM image.
2. **ROM-to-SRAM loading/packaging remains unverified.** The first-stage code
   must be entered before DDR use, with its complete code/literals/stack
   available. The stock early code and literal pool extend through offset
   087f. Our current cold diagnostic is 0874 bytes, but that useful size
   comparison does not prove ROM copy length, entry rules or header checks.
3. **There is no established recovery method for a broken first stage.** The
   reset-button `upgrade.bin` path belongs to stock U-Boot after its DDR path;
   it is not proof of ROM-level recovery if that loader is replaced.
4. **The warm replay failure is still unlocalized.** Existing runtime words
   94000004=b and 94000014=077faabf differ from early writes 8/ffffffff. The
   sequence also pulses a PLL control and disables/re-enables controller
   state. Stopping the packet engine does not establish that these changes
   are safe on a running SoC. Repeating UART bytes cannot identify the first
   failing operation, and UART reinitialization passing under stable clocks
   does not settle behavior while those clocks change.

The monitor's 16 MiB report is therefore a software limit, not a DDR training
measurement or evidence that half the physical memory failed. The stock
bootloader reports 32 MiB; the port restricts monitor-managed RAM to avoid
overlapping inherited packet-engine memory.

## Direction

Continue resolving the ROM loading contract and recoverable cold entry while
using the separate diagnostic to validate entry guards and SRAM exception
machinery. Do not use another warm full-DDR replay as the acceptance test.
The current guard audit is intentionally DDR-free and can establish only
that the integrated path rejects already-initialized DDR without CPU-control
or DDR writes. It cannot validate accepted cold entry.

No hardware action or firmware change was made during this review. The
source remains available for a genuine cold-start test once loading and
recovery are established; there is no evidence here requiring a different
set of board DDR timing constants.
