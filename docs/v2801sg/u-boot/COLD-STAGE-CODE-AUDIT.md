# Cold-stage code audit and complete-path emulation

This audit executes the captured ROM and actual replacement machine code
offline. No board reset, serial command, MMIO register write on hardware,
flash operation or programmer operation was performed.

## Exact inputs

| Input | SHA256 |
| --- | --- |
| Captured low-address ROM, 4096 bytes | 59169c7e8c2d3ef66b42892a16d171bca043ad7d421e875ae53df9f8e93b9c1d |
| Current cold stage, 3008 bytes | b5ce7e4c83612e0bba65ac572ce8fe14bddad1c4a013a71fee7db7fcd1704619 |
| Previously hardware-tested UART-init monitor, 232544 bytes | 85dbdf9f7b8fe6f0cbf9b0b30010bcaba9ec9422865186eb32f76f7f797f76f4 |
| Combined offline image, 240896 bytes | f082ce289e70124332f6973a1fbcdc40f31fe88edb711cc008720d46c50146c5 |

Current cold-stage CRC32 is 6ad95389. It remains an offline prototype,
not an installation-ready release. The combined image was not staged for
TFTP or flashed. Each emulation result is bound to its combined-image hash.

## Review findings and fixes

The DDR initializer's ordered 35 MMIO writes and three 2000-iteration
delays match the retained stock instruction-derived trace. The 2020 donor
bootloader independently has byte-identical stock CPU/DDR/bootstrap code.
The source uses relaxed DDR accesses, bounded readiness polls and the same
SCTLR update as stock. The loop counts are not calibrated microsecond delays;
the source compiler's loop instruction shape differs from the old compiler.

The SPI setup error previously left the top-level trace at DDR phase 2,
even when DDR phase 8 was already complete. `load_flash_pages()` now sets
phase 3 before configuring SPI, so a setup timeout is correctly attributed
to SPI. This changes diagnostic reporting; the actual MMIO sequence remains
the same. The RAM-copy audit remains byte-identical. The rebuilt SPI-audit
variant is 1084 bytes, SHA256
97aa5e6fb7643637cc6e101cbdae8bcc5707153ca0496ed992a2fab57b5cdf5c,
CRC32 e0667790; this revision has software verification only. The earlier
70b4ed21 SPI hardware result remains tied to its original artifact.

A ROM documentation error was corrected: instructions 0068/006c load zeroing
start from literal 00b8 (00202400) and end from literal 00a8 (00204000).
The zeroing loop covers 00202400..00203fff, not 00202000..002023ff. The
first stage reuses that area after the ROM hands over, so the correction
does not require moving its diagnostics or stacks.

## Layout and binary identity checks

- The first-stage linker accepts no initialized data/BSS and limits all
  linked code/literals to 00200000..00201fff.
- The first stage's 8192-byte slot is ff-padded; the monitor starts at flash
  offset 2000, is padded to a 256-byte boundary, and ends before 40000.
- SRAM trace 00202400, fault record 00202500, fault stack top 00203000 and
  main stack top 00204000 are separate from the ROM load window.
- GCC stack-usage data gives a conservative 112-byte maximum for the main
  stage call chain (48-byte entry save, 24-byte caller, 32-byte DDR function,
  8-byte delay). Lowest main-stack write observed in emulation is 00203f94;
  padding accounts for the distinction from the minimum stack pointer.
- Monitor destination 40400000..40438c5f does not overlap its initial stack
  at 40700000 or the SRAM stage. The padded SPI read ends at 40438cff.
- Every allocated, file-backed ELF section of the monitor matches the
  corresponding payload bytes. Its native binary plus the exact 1800-byte
  DTB equals the complete payload. Ordinary raw objcopy without the build's
  ff gap-fill differs in linker gaps; those padding differences are expected.
- The monitor has SKIP_LOWLEVEL_INIT unset and HAS_VBAR enabled. Its actual
  ARM startup installs its vectors and performs CP15 setup before `_main`.

## Machine-code test method

Unicorn 2.1.2 uses its Cortex-A9 CPU model. It was installed only into a
temporary analysis directory. The program begins at the captured ROM reset
vector at address zero, not at a substitute C implementation. The ROM's
SPI receive loop loads the first stage, which executes CPU setup, the real
compiled DDR function, SPI page reads, CRC and the automatic monitor branch.
The monitor's actual startup then runs until entry to board_init_f at
4040c22c, with its SP and global-data pointer checked in initialized DRAM.
The test stops before executing board_init_f; it does not emulate complete
U-Boot device initialization or Linux boot.

The MMIO backend supplies specified PLL/DDR readiness values, SPI FIFO data
from the exact combined image and UART readiness. No boot-stage function,
DDR delay loop, SPI-copy loop or CRC loop is stubbed. Writes outside the
expected MMIO register set and any DRAM access before DDR completion are
rejected by the harness. Only opcode 03 SPI transfers are allowed.

## Results: 16 cases passed

| Case | Required outcome |
| --- | --- |
| Normal NOR reset path | Reaches board_init_f; correct payload and CRC; no early DRAM access |
| Alternate preserved SCTLR bits, 08c50078 | Same successful path |
| Corrupt second-stage byte | CRC error -74; halts before monitor entry |
| PLL never locks | Bounded timeout -110; no second-stage loading |
| DDR status 104 never ready | Bounded timeout; no DRAM access |
| DDR status 100 never ready | Bounded timeout; no DRAM access |
| SPI setup never ready | Timeout attributed to phase 3; no handoff |
| SPI FIFO stalls on first page | Timeout; no handoff |
| SPI completion never arrives | Timeout; no handoff |
| SPI FIFO stalls after 16 pages | Timeout; correct completed count 1000 hex; no handoff |
| Already-enabled DDR controller | Refuses before peripheral writes; caller CPU state restored |
| I-cache enabled at entry | Refuses before stack/peripheral writes |
| IRQ-mode entry | Refuses before stack/peripheral writes |
| Synthetic data abort during DDR write | SRAM fault record and halt; no DRAM access or monitor entry |
| ROM mode input 0 | ROM prints UART and ROM#; no first-stage/DDR execution |
| ROM mode input f | Same UART-console path |

The successful NOR run performs exactly one 8192-byte ROM read from flash
zero and 909 subsequent 256-byte page reads from 2000 onward. Every transfer
offset, size, FIFO read count and copied monitor byte is checked. Its DDR
MMIO/delay event trace matches the retained stock trace exactly.

For the exception case the harness constructs an ARM data-abort exception
at the DDR store instruction, with DFSR=8 and DFAR=94300080. The actual
vector/handler records magic 46544c31, type 4, correct fault registers and
raw LR, then halts on its separate SRAM stack. This verifies the software
record path, not physical external-abort delivery by the bus controller.

The shared SPI C loader was also rebuilt and tested natively with a simulated
backend for all 909 page parameters, full padded copy, CRC, timeout progress,
corruption rejection and DDR-readiness guard. These complement the prior
physical SPI tests rather than replacing them.

## Practical limit

This is strong verification of digital control flow, instruction compatibility,
copy boundaries, checksums and failure handling. It cannot model actual PLL
settling, DDR signal/training behavior, undocumented clock-domain effects,
cache timing or a bus transaction that never completes. A finite polling loop
cannot bound a hardware bus stall. Physical reset-state UART selection is
also still unresolved: supplying 0/f to the emulator is not selecting those
values on the board.

Cold hardware execution and subsequent complete monitor bring-up remain
unverified. The prototype still needs appropriate UART progress reporting
before a controlled cold test. No first-attempt success guarantee follows
from these tests, and no flash operation is authorized by this audit.

Reproducible local artifacts: tmp/v2801sg-cold-audit/emulate.py,
summary.json and final-cases.log; current build script and image manifest
are in tmp/v2801sg-first-stage/. These are ignored local analysis artifacts.
