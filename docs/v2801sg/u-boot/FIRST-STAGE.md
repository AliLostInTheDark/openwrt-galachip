# Integrated first-stage prototype

The low-address boot-code capture establishes an 8 KiB NOR load to SRAM
00200000. `tools/first-stage.S`, `first-stage.c` and `first-stage.lds` implement
a prototype using that layout. It is not connected to an installer or normal
package image recipe, and no flash write is part of its test.

The cold variant checks its entry CPU state, switches to the ROM-observed
SRAM stack at 00204000 and calls the existing cold CPU helper. An already
enabled DDR controller is refused before CPU reconfiguration or DDR writes.
After acceptance it runs DDR initialization, bus/SPI setup, then reads the
second stage from flash offset 2000 into 40400000 in bounded 256-byte pages.
It validates the exact unpadded payload CRC before branching to 40400000.
Failures after CPU setup halt in SRAM; there is no DDR rollback.

The linker enforces the 8 KiB code/literal budget and rejects data/BSS.
Diagnostic words are at 00202400, exception records at 00202500, exception
stack top at 00203000 and entry stack top at 00204000, outside the ROM load
window. Optional DDR tracing now accepts a base address without changing the
existing default diagnostic layout.

## Current build

The payload is the previously tested UART-init monitor with empty bootcmd,
bootdelay -1 and link address 40400000:

- 232544 bytes (38c60 hex), CRC32 aae282a5.
- SHA256 85dbdf9f7b8fe6f0cbf9b0b30010bcaba9ec9422865186eb32f76f7f797f76f4.

The cold prototype is 3008 bytes, CRC32 6ad95389, SHA256
b5ce7e4c83612e0bba65ac572ce8fe14bddad1c4a013a71fee7db7fcd1704619.
The offline combined image has an ff-padded 8192-byte first-stage slot,
followed by the monitor padded to a 256-byte boundary: total 240896 bytes.
Its SHA256 is
f082ce289e70124332f6973a1fbcdc40f31fe88edb711cc008720d46c50146c5.
It intentionally remains offline and is not staged for TFTP or flashing.

Build script, generated payload header, binaries, maps and manifest are saved
in the ignored local directory `tmp/v2801sg-first-stage/`. The script extracts
the existing U-Boot compile flags and checks the monitor's link address,
disabled autoboot, payload bounds, 8 KiB slot and 256 KiB combined size.

## RAM-only verification

The `ZX_STAGE_RAM_AUDIT` variant does not link DDR, SPI or CPU-init helpers.
It requires a running DDR controller, validates the monitor preloaded at
40800000, copies it to 40400000 and validates the destination. It returns
to the calling monitor. It only reads the controller-enable register; it
does not reconfigure clocks, DDR, peripherals or vectors. The caller's CPSR,
SP, return address and callee-saved registers are restored by the wrapper.

Artifact `v2801sg-stage-audit-5c110518.bin`: 444 bytes (1bc hex), CRC32
a83bae78, SHA256
5c110518e2c7861385534a4f0e739844d3d26b3c78f06b5f6d3a33343423c3c2.
This is a different binary from the offline cold stage; do not interchange
them. Only this RAM audit and the known monitor payload are staged for tests.

Trace at 00202400 contains twelve words:

| Word | Meaning |
| --- | --- |
| 0 | Magic 53544731 |
| 1 | Phase: 1 source check, 2 DDR, 3 SPI setup/copy, 4 destination check, 5 complete |
| 2 | Signed error code, zero on success |
| 3 | Last computed CRC |
| 4 | Payload byte length |
| 5 | Expected CRC |
| 6 | Initial DDR-controller enable register |
| 7 | Completed SPI byte count, including final page padding |
| 8..11 | Cold-only DDR phase / last MMIO address / value / write state |

Cross-build/link passed for both variants. Native execution of the actual
audit C source with fixed-address RAM mappings passed a full monitor copy,
CRC verification, corrupted-source rejection before destination writes and
DDR-not-ready rejection. The ARM wrapper's instruction listing was inspected;
these checks do not establish cold hardware operation. Back up and restore
SRAM 00200000..00203fff around the test, including
the scratch record and stack. No further warm DDR replay follows from it.

## User hardware result

The user verified monitor CRC aae282a5 and audit CRC a83bae78, then ran the
audit from SRAM. It returned rc=0 and recorded:

```text
00202400: 53544731 00000005 00000000 aae282a5
00202410: 00038c60 aae282a5 00000001 00000000
00202420: 00000000 00000000 00000000 00000000
```

Destination 40400000..40438c5f had CRC aae282a5 and all 232544 bytes matched
source 40800000. SRAM 00200000..00203fff was restored to its backup CRC
d9ca14f8; ping then succeeded. The user manually executed `go 40400000` and
the copied UART-init monitor reached its ZX279125 prompt with working
serial output. Its banner remained RAM-UART-init-test, as expected for the
unchanged payload, and reported the configured 16 MiB managed RAM bank.

This validates the RAM audit's CRC/copy path and a manual monitor entry.
It does not validate the cold-stage SPI transport, automatic branch after
CPU/DDR setup, cold DDR initialization or a replacement flash boot. The
offline cold image has not been executed. A real cold test still needs a
reset-state entry method; do not substitute a warm DDR replay.

## Shared SPI-copy audit

The cold image and `ZX_STAGE_SPI_AUDIT` now use the same `load_flash_pages()`
implementation. The latter requires initialized DDR, skips CPU/DDR and bus
clock setup, resets/configures only the SPI controller, reads flash into
40400000 and verifies the result before returning. It does not link the
DDR or cold CPU initializer and has no jump to the copied data. No flash
program/erase command is present in its page reader (opcode 03 only).

For this test the expected data is the existing stock flash slice, rather
than the replacement monitor: flash 2000..3ac5f, 232544 bytes, expected CRC
30bdfede from the saved current bootloader. Reads are page rounded through
3acff (232704 bytes, 38d00 hex, 909 pages). A reference `sf read` at 40800000
must confirm the baseline CRC before execution. If that baseline differs,
stop and investigate the installed image rather than treating it as a SPI
failure. Compare the complete padded copy against the reference afterward.
The copied data is an interior slice of stock flash; never execute it.

Artifact `v2801sg-stage-spi-70b4ed21.bin`: 1084 bytes (43c hex), CRC32
 a4ce4030, SHA256
70b4ed2102ee514b3d9bd6d381b345a32743cd7180e8acdbd5492eb6ad9d8cb1.
The prior RAM audit remains byte-identical. The current cold prototype size
and hashes above reflect the shared copy helper and progress reporting;
it remains offline and hardware-untested.

Cross-build/link passed. Native execution of the actual shared C copy loop
with a simulated SPI backend checked all 909 offsets, destinations and page
sizes, the full padded copy, expected CRC, injected page timeout with partial
progress, CRC corruption rejection, and DDR-not-ready rejection. The RAM
copy checks were rebuilt and passed too. These are source/build checks;
the physical SPI-audit result is recorded below. Staging this read-only audit
does not authorize or perform a bootloader flash installation.

### SPI hardware result

The user chainloaded monitor 7f1be111 from stock U-Boot and ran SPI audit
70b4ed21. U-Boot's reference read at flash offset 2000 matched the expected
unpadded CRC 30bdfede, and the audit binary matched CRC a4ce4030. The audit
returned rc=0 and recorded:

```text
00202400: 53544731 00000005 00000000 30bdfede
00202410: 00038c60 30bdfede 00000001 00038d00
00202420: 00000000 00000000 00000000 00000000
```

The complete 232704-byte padded destination matched the independent U-Boot
reference byte for byte; its unpadded CRC was 30bdfede. All 909 pages were
copied. SRAM restoration CRC 89242f8e matched the backup from this run and
Ethernet ping succeeded. No copied stock-data slice was executed and no
flash was written.

This verifies the shared SPI page loader on initialized hardware. Combined
with the earlier RAM copy and manual monitor entry, it validates those
components separately; it does not prove cold DDR startup, SPI operation
immediately after that startup, or the complete automatic reset-to-monitor
handoff. The current combined image remains an offline prototype.

## Complete-path code audit

See COLD-STAGE-CODE-AUDIT.md for sixteen machine-code emulation cases,
including captured ROM reset through automatic monitor entry and board_init_f,
error paths, warm-entry guards and a synthetic data-abort record. The cold
image hashes above now identify the diagnostic phase-reporting correction
verified by that audit. No new image was flashed or executed on the board.
