# SRAM undefined-exception audit

Image `v2801sg-sram-vectors-006bb201.bin`: 556 bytes (`22c` hex), CRC32
`225a6c70`, SHA256
`006bb201c9444f2294b9dd9d27fda380cc4de621e3ed1059bc4cef193182c844`.
Staged at `/srv/tftp/`. Hardware result: passed, as recorded below.

The warm wrapper in `tools/sram-vector-audit.S` requires SVC mode and MMU,
I-cache/data cache, high vectors, exception Thumb state and exception big-endian
state disabled. It preserves the caller's SRAM stack frame, VBAR, CPSR and
undefined-mode SP/LR/SPSR. It installs the shared SRAM vector table, executes
one ARM undefined instruction at 00200078, then restores state on returning
to 0020007c. Only the audit build's undefined handler returns; unexpected
exceptions still record and halt. No UART, SPI, clock/reset or DDR MMIO is
used. The cold CPU initializer is discarded by the test linker script.

The exception stack at 00200d00 is separate from the wrapper stack at
0020a000. Code/literals end at 0020022c, below the linker limit 00200c00.
The exception record at 00200e00 occupies 88 bytes. The existing first-4-KiB
backup covers code, exception stack and record; wrapper stack scratch outside
that prefix is used but not restored, as with previous SRAM probes.

Offline build/model evidence: `/tmp/v2801sg-vector-audit/`. The assembled
instruction model checks guard refusal before state writes, one exception,
return through the SRAM handler, register/banked-state restoration and VBAR
restoration. Writes are restricted to the SRAM record and two stack areas.
It does not establish real exception routing or recovery on this SoC.

From the current working RAM monitor:

```text
tftp 0x40500000 v2801sg-sram-vectors-006bb201.bin
crc32 0x40500000 0x22c
cp.b 0x00200000 0x40510000 0x1000
crc32 0x40510000 0x1000
```

Require 556 transferred bytes, payload CRC `225a6c70` and backup CRC
`d11f7b32`. Stop on mismatch. Then:

```text
dcache off
icache off
cp.b 0x40500000 0x00200000 0x22c
crc32 0x00200000 0x22c
```

Require SRAM CRC `225a6c70`, then:

```text
go 0x00200000
```

Expect application return code 0 and a responsive U-Boot prompt. No output
comes from the probe itself. After it returns:

```text
md.l 0x00200e00 0x16
cp.b 0x40510000 0x00200000 0x1000
crc32 0x00200000 0x1000
ping 192.168.1.100
```

The record starts with `46544c31 00000001`; raw exception LR at 00200e54
must be `0020007c`. Restored prefix CRC must be `d11f7b32`, followed by a
working ping. Retain the entire record. A nonzero return or unexpected record
requires stopping after restoring the prefix; an unresponsive board requires
physical recovery and no further tests.

## Hardware result

The user transferred 556 bytes and confirmed image/SRAM CRC `225a6c70`.
The original prefix backup CRC was `d11f7b32`. The application returned 0
and the record was:

```text
00200e00: 46544c31 00000001 400001db 400001d3
00200e10: 000008a3 00001006 1c2154b0 300040d0
00200e20: 00000000 00200e00 00200100 600001d3
00200e30: 40fb4000 00200000 00000000 00000000
00200e40: 39044b50 40ba3ee0 00000002 40fc2d54
00200e50: 40ba3660 0020007c
```

The completion marker and vector index match. Handler CPSR has undefined
mode 1b; SPSR has interrupted SVC mode 13. Raw exception LR `0020007c`
matches the compiled resume label. The undefined instruction does not itself
establish new data/prefetch fault status, so the captured DFSR/IFSR/DFAR/IFAR
values must not be interpreted as a memory failure from this test.

The user restored the first 4 KiB with CRC `d11f7b32`, then successfully
pinged 192.168.1.100. This verifies actual SRAM exception routing, recording
and return for the controlled undefined instruction, with a functioning
monitor afterward. It does not verify data/prefetch abort paths, cold CPU
setup, instruction-cache transitions, DDR initialization or trace retrieval
after a bus stall/reset.
