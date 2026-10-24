# ZX279125 low-address boot code capture

The user captured addresses 00000000..00000fff through the bounded SRAM read
probe. All sixteen calls returned success. Reconstructed bytes match the
on-board CRC32 f1f4dafd; SHA256 is
59169c7e8c2d3ef66b42892a16d171bca043ad7d421e875ae53df9f8e93b9c1d.
The complete binary, raw UART transcript and instruction listing are retained
under `tmp/v2801sg-stock-live-20261023/` as bootrom-low-4K.*.
The SRAM restore CRC matched d11f7b32 and Ethernet ping succeeded afterward.

This is reset/boot code mapped at zero, strongly identifying the BootROM
mapping. The capture itself does not independently prove physical mask-ROM
immutability or exclude an alias. No captured code was executed by the test.

## SPI NOR loading contract

| Address | Instruction-level observation |
| --- | --- |
| 0000 / 00a4 | Reset vector loads entry address 0020 |
| 0038 / 00a8 | Initial SP is 00204000 |
| 00bc..00c4 | Boot mode is bits 7:4 of register 94000038 |
| 00f4..0138 | Mode dispatch table: 0 and 15 UART; 2 SPI NOR; 1 invalid; remaining entries SPI NAND variants |
| 014c..0154 | Prints `SPI NOR`, then calls 0d14 |
| 0d14..0d20 | SPI controller setup helpers 0d00 and 0ee0 |
| 0d24..0d34 | Calls SPI transfer 0ba8 with r0=0, r1=00200000, r2=0, r3=2000 |
| 0d3c..0d40 | Unconditionally branches to 00200000 after transfer returns |

Transfer 0ba8 selects a nine-byte command record using r0 as command ID.
Record zero at 0fbc begins `00 03 01 02 00 01 00 00 00`, including SPI opcode
03. It writes r2 (zero) to controller address register +1c and r3-1 (1fff)
to transfer-length register +18. The receive loop copies ceil(r3/4) words
from the receive FIFO at +38 to r1. Controller base is 94407000.

Therefore the decoded NOR path loads flash bytes 0000..1fff into SRAM
00200000..00201fff and enters 00200000 in ARM state. It does not use the
stock U-Boot header markers, linked address or relocation lengths to decide
this extent, and has no visible header, checksum or signature validation on
this path. This is an instruction-derived loading contract, not a cold-boot
test of a replacement image.

The ROM startup's zeroing loop covers 00202400..00203fff and initially uses
stack 00204000. Its linked data-load address is 00000ff2; the data-copy loop skips
because data start and end both equal 00202000. Those facts explain the
approximately 4 KiB captured image; they do not establish a larger ROM size.

## UART monitor

Mode 0/15 dispatches to 0918, which prints `ROM# ` and parses commands via
07d0. The retained help strings and dispatch implement:

- `x [addr]`: receive data via the packet receiver at 0480; default 00200000.
- `g [addr]`: print address then BLX to it; default 00200000.
- `b <o|u>` / `b s ...`: invoke boot routines with supplied options.
- `r [addr] [size]`: aligned memory display.
- `w [addr] [data]`: aligned word write.

The receiver implements packet checks including CRC-table processing. These
checks must not be confused with NOR-image authentication: the NOR loader
does not call this receiver. Pin/strap access and practical entry into the
ROM monitor have not been verified. Do not change straps or execute the ROM
entry on warm hardware from this finding. Stock reset-button upgrade.bin
recovery is still a separate later U-Boot feature.

### Reset-state entry investigation

The stock reset routine at linked address 41f00cd0 writes 1 to 9400003c.
The current source monitor uses the same reset register. Neither routine
sets a boot-mode override. The decoded ROM unconditionally reads bits 7:4
of 94000038 and dispatches; the SPI NOR path contains no UART-key check
before loading/executing the first stage. Sending UART characters during
ordinary NOR boot therefore has no demonstrated way to select ROM UART mode.

Searches of the retained stock code and available platform sources found no
consumer that establishes a writable override for 94000038 or identifies
board-level boot-mode pins. Modes 0/15 enter UART; mode 2 selects NOR.
Whether the field is latched from pins, writable, or reset-retained has not
been established. Do not write it based only on the ROM's read instruction.
The next read-only hardware check is `md.l 94000038 1`; derive current mode
as `(value >> 4) & f`. A register read alone will not identify the pins or
prove that a software override exists.

A warm branch into ROM startup is not a reset-state test. It would clear
ROM SRAM work areas and reconfigure bus/SPI state while DDR is already live.
No warm ROM-startup command or boot-mode register write has been issued.

## Consequences for the replacement

The first-stage load budget is 8192 bytes. The existing offline cold
diagnostic is 2164 bytes, so it fits that budget, but lacks the complete
second-stage loading/handoff path. The packaged first stage must include all
code and literals needed before DDR, with the initial branch at flash zero.
The next implementation is a combined layout with an 8 KiB first-stage slot
and the compact U-Boot monitor after it, plus bounded SPI reads, a defined
handoff, linker size checks and diagnostic records.

An 8192-byte slot plus the current 232584-byte compact monitor is 240776
bytes, leaving 21368 bytes in the 256 KiB boot partition. This replaces the
stock environment region rather than preserving it; persistent environment
is already disabled in the compact monitor. These are layout calculations,
not an installation-ready image. Cold DDR initialization and the integrated
handoff remain hardware-unverified. No flash write is authorized or performed.
