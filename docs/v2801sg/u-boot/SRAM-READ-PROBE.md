# Bounded SRAM read probe

`tools/sram-read-probe.S` is a RAM-only ARM application linked at 00200000.
It reads 1..64 aligned words from an address supplied in SRAM. It does not
initialize DDR, touch flash, or change peripheral configuration. It temporarily
installs low vectors and an abort-mode stack, then restores VBAR, CPSR, the
abort bank and caller callee-saved registers before returning.

Input/output at 00200e00:

| Offset | Meaning |
| --- | --- |
| 00 | Input start address |
| 04 | Input word count (1..64) |
| 08 | Status: 0 success, 1 data abort, 2 invalid input |
| 0c | Successfully copied word count |
| 10 | DFSR on data abort |
| 14 | DFAR on data abort |
| 18 | Faulting instruction address |
| 40 | Copied words |

Entry refuses non-SVC mode, enabled MMU/data/instruction caches, high vectors,
Thumb exception entry or big-endian exception state. Return value is the
status, or ffffffff on entry refusal (record is then untouched). Faults are
recoverable only at the one source-load instruction; other exceptions halt.
Fault status registers may change on a real abort. A nonresponding bus can
stall without raising an abort; this helper cannot recover from that condition.
It is not a license to sweep unknown MMIO or read device FIFOs.

Build artifact: `v2801sg-sram-read-0a152f8b.bin`, 408 bytes (198 hex),
CRC32 1cb4b053, SHA256
0a152f8b6df8fe10267d4710c79a9160edbd6266ced8b33471c7c1bce1afe96f.
Cross-assembly/link passed. An instruction model checked the full successful
64-word path, injected synchronous data-abort return, invalid parameters and
CPU guards, including callee-saved registers and banked-state restoration.
This is software validation; the abort path has not been verified on hardware.

First hardware check reads the known executable SRAM region itself. Back up
and restore the first 4 KiB, including the record and destination buffer.
Unknown-address reads remain exploratory hardware tests, even after the
alignment test. A successful known-SRAM read does not prove a ROM mapping
or external bus-abort recovery.

```text
tftp 0x40500000 v2801sg-sram-read-0a152f8b.bin
crc32 0x40500000 0x198
cp.b 0x00200000 0x40510000 0x1000
crc32 0x40510000 0x1000
dcache off
icache off
cp.b 0x40500000 0x00200000 0x198
crc32 0x00200000 0x198
mw.l 0x00200e00 0x00200000 1
mw.l 0x00200e04 0x10 1
go 0x00200000
md.l 0x00200e00 0x7
md.l 0x00200e40 0x10
cmp.b 0x00200000 0x00200e40 0x40
cp.b 0x40510000 0x00200000 0x1000
crc32 0x00200000 0x1000
ping 192.168.1.100
```

Expected header: 00200000 00000010 00000000 00000010 00000000
00000000 00000000. Expected comparison: 64 identical bytes. Restore CRC
must equal the backup CRC printed in this run; do not assume an older value.
These commands require the modern RAM-chainloaded `ZX279125 #` monitor.

## Hardware result: initial revision

The user tested revision 956b298e on the RAM-chainloaded monitor. Its 16-word
SRAM read completed with status 0, count 10 hex and 64 identical bytes.
SRAM was restored to CRC32 d11f7b32 and subsequent Ethernet ping passed.
The application returned 10 hex because r0 retained the completed-word count;
revision 0a152f8b fixes this to return the status field. The instruction model
now also checks that return value. This revision remains hardware-untested;
no unknown ROM address or hardware data-abort path has been tested.

## Alignment-abort hardware test

Compile with `-DZX_READ_ALIGNMENT_TEST` to accept only address 00200001 and
count 1. This variant refuses entry unless SCTLR.A is already set, and never
changes SCTLR. Its deliberately unaligned word load targets known SRAM,
raising an alignment data abort rather than accessing an unknown bus address.
The same handler records the fault and skips the load. Expected status/return
is 1, completed count 0, DFAR 00200001, and DFSR fault status 1 (alignment).
The faulting PC is the variant's `probe_load` symbol. A failed expectation
must be investigated before reading any unknown address.

Artifact `v2801sg-sram-align-352b8d3c.bin`: 412 bytes (19c hex), CRC32
3ec07586, SHA256
352b8d3c151dc736530b1c59702847ac659868a4db6e236b6b1bbee565ae4f72.
Cross-build and instruction-model checks passed for injected abort return,
parameter rejection and the alignment-enable entry guard. Hardware result below.
Use the same backup/restore sequence above, substituting this artifact and
size, and write input address 00200001 / count 1. Dump the seven-word result
header after return; no output payload comparison is expected.

### Hardware result

The user ran artifact 352b8d3c with both image CRCs matching 3ec07586.
It returned rc=1 and produced:

```text
00200e00: 00200001 00000001 00000001 00000000
00200e10: 00000001 00200001 002000a0
```

This verifies a real alignment data abort: status 1, zero copied words,
DFSR=1, DFAR=00200001 and faulting PC=002000a0. The handler returned to
the monitor. SRAM restoration matched the backup CRC d11f7b32 and subsequent
ping to 192.168.1.100 succeeded. This verifies the alignment-abort recovery
path, not external bus-abort recovery, bus-stall recovery or a ROM mapping.

## First mapping candidate: low vector address

No confirmed ZX279125 BootROM address was found in the retained stock
decompilation or the inspected public platform material. The captured stock
VBAR is zero, but that does not establish that memory at zero is mapped or
contains ROM. Address 00000000 is therefore an exploratory low-vector
candidate, not an identified ROM base. Do not infer the ZX279125 mapping
from the different ZX297520 or ZX279128S SoCs.

The next proposed test uses the normal 0a152f8b probe for 16 words at zero,
with the same SRAM backup/restore sequence. Inspect both status and payload:
success alone does not distinguish ROM from an alias, RAM, or default bus
data. A synchronous external abort is not yet verified recoverable, and
a nonresponding bus may stall indefinitely. If the monitor does not return,
stop hardware testing and recover at the desk; do not queue further probes.
No live read of this candidate has been performed by the assistant.

### User hardware capture: 00000000..0000003f

The user ran normal probe 0a152f8b with start 0 and count 10 hex. Status
and return were 0, completed count 10 hex. Captured words:

```text
00000000: e59ff09c eafffffe eafffffe eafffffe
00000010: eafffffe eafffffe eafffffe eafffffe
00000020: e10f0000 e38000c0 e121f000 e3c0001f
00000030: e38010d3 e121f001 e59fd068 e59f0068
```

This is ARM vector/startup code, distinct from the stock flash prefix.
Instruction 0 loads PC from address a4 (PC+8+9c); exception slots 4..1c
loop locally. Startup at 20 masks interrupts and selects SVC mode; 38 loads
SP from a8 and 3c loads r0 from ac. Those literal values are not yet captured.
This is a strong BootROM candidate, but read-only capture alone does not
prove physical ROM versus a memory alias. Raw bytes are saved locally as
`tmp/v2801sg-stock-live-20261023/low-vectors-64B.bin`.
SRAM restoration CRC d11f7b32 matched the backup, and ping succeeded.
Next bounded capture: 64 words at zero (00000000..000000ff), covering these
three literals without sweeping the address space.

### User hardware capture: 00000000..000000ff

The next user capture completed all 64 words with status/return 0. Reset
literal a4 is 00000020, stack literal a8 is 00204000. The startup's data
initialization literals at ac/b0/b4/b8 are 00000ff2, 00202000, 00202000,
00202400. Equal data start/end means the byte-copy loop skips. The zeroing
loop at 68..7c loads its start from b8 and end from a8, clearing
00202400..00203fff. The data load-address literal ff2 is consistent
with a roughly 4 KiB linked image, but does not prove the physical ROM extent.
Code at bc reads boot-mode bits 7:4 of 94000038 and dispatches through a
jump table at fc. It calls 3e0 first. These targets need a larger capture.
Raw bytes and instruction listing are saved in the local capture directory
as low-vectors-256B.bin and low-vectors-256B.asm. SRAM restore CRC and ping
again passed.

Next proposed capture: 00000000..00000fff in sixteen bounded 256-byte
probe calls. Stop on the first nonzero record status. Copy each completed
block into RAM 40520000..40520fff for one consolidated UART dump, then
restore SRAM. This extends the observed mapping; it is not proof that all
4 KiB is readable before the test. Do not execute the captured reset code.
