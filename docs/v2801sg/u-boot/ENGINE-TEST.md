# Reversible SRAM packet-engine control test

No DDR, clock, reset or flash writes. Hardware execution passed below.
This tests reversible control changes, not global AXI-idle status.

Artifact: `v2801sg-sram-engine-d0dd09a3.bin`, 648 bytes (288 hex), CRC32
`253cd2d3`, SHA256
`d0dd09a3763c21422148298dd25955d03793b6c4b98150022ccae768098fef7a`.
Staged in `/srv/tftp/`. Source: `tools/sram-engine-probe.S` and `.c`.
Linked at 00200000 using `sram-ddr-recovery.lds`; actual image ends before
00200e00, where an eight-word result record lives within the backed-up prefix.
No unresolved symbols; empty data/BSS. Code and stack execute in SRAM.

Entry preserves callee-saved registers, caller SP and CPSR, masks interrupts,
and refuses enabled MMU/data/instruction caches. The C routine requires BMU=1,
all four DMA-enable mask bits set, both MAC RX/TX disabled and balanced
ownership for 32 samples. It clears the exact vendor DMA mask, disables BMU,
checks readback and empty ownership for 1024 iterations, then restores BMU
and DMA in reverse order on success or failure. Iterations are not calibrated
time. A failed restore returns an error; hardware faults may still prevent
return, requiring a physical power cycle.

Offline tests exercise successful restoration, busy MAC/high-half queue refusal
with zero engine writes, rejected BMU-disable readback, and new queued work
during the disabled interval. Tests mock MMIO; they do not prove hardware
transactions drain or that DDR reset is safe. Evidence: `/tmp/v2801sg-engine-probe/`.

From the current RAM U-Boot prompt:

```text
tftp 0x40500000 v2801sg-sram-engine-d0dd09a3.bin
crc32 0x40500000 0x288
cp.b 0x00200000 0x40510000 0x1000
crc32 0x40510000 0x1000
cp.b 0x40500000 0x00200000 0x288
crc32 0x00200000 0x288
dcache off
icache off
go 0x00200000
md.l 0x00200e00 0x8
```

Require transfer 648 bytes and CRC 253cd2d3 in both places before `go`.
Record the backup CRC; previously d11f7b32. Expected return rc=0 and record:

```text
454e4731 00000000 001f6564 00000001
00106564 00000000 001f6564 00000001
```

Words are magic, result, saved DMA/BMU, disabled DMA/BMU, restored DMA/BMU.
No UART text is emitted by the probe itself. If return is nonzero or restored
values differ, retain output and stop before network testing. If it hangs,
stop until physical recovery. After a successful result, restore SRAM first:

```text
cp.b 0x40510000 0x00200000 0x1000
crc32 0x00200000 0x1000
ping 192.168.1.100
ping 192.168.1.100
ping 192.168.1.100
```

Require restored CRC to equal the backup. Capture all output including shutdown
warnings. Do not run any DDR stage after this test, even if all checks pass.

## Hardware result — 2026-10-23

The user transferred 648 bytes and verified CRC 253cd2d3 in both DRAM and
SRAM. The application returned rc=0. Its eight-word record exactly matched
the expected values above: DMA 001f6564 -> 00106564 -> 001f6564 and BMU
1 -> 0 -> 1. Restored SRAM CRC d11f7b32 matched the saved prefix, and three
subsequent pings to 192.168.1.100 succeeded without ownership warnings.

This physically validates reversible control writes, readback, sampled empty
ownership and continued warm management operation. It does not validate PP
reset, outstanding AXI transaction completion, DDR reinitialization or cold
boot. The next distinct experiment is an SRAM-isolated PP reset/readback
test with an explicit engine/table reinitialization and recovery plan; do not
extend this passing probe into a DDR test without that separate review.
