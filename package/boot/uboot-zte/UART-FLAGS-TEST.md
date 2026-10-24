# Quiet SRAM UART-flags sampler

2026-10-23; hardware execution pending. This reads only UART flags 94405014
and writes SRAM scratch results. No UART output during sampling; no engine,
clock, DDR, reset or flash writes. Uses the tested SRAM entry wrapper, which
preserves caller state and rejects enabled CPU caches/MMU before sampling.

Artifact `v2801sg-sram-uart-flags-af13e298.bin`: 292 bytes (`124` hex), CRC32
65dff0b9, SHA256
af13e2983e0173b739125980cddbffa103b5bc6435216434b480c2bd0b6830aa.
Staged in `/srv/tftp/`. Sources: `tools/sram-engine-probe.S` linked with
`tools/sram-uart-flags.c` only; the wrapper's engine_probe symbol names the
sampler here and does not link any packet-engine code. Empty data/BSS and no
undefined symbols. Link bound 00200e00; results occupy e00..e27 and trace
e40..f3f, all inside the saved first SRAM page. Host tests cover constant
states, one transition, bit counters and trace indices. Evidence:
`/tmp/v2801sg-uart-flags/`. Sample iterations are not calibrated duration.

Use the existing RAM monitor, not stock `go` with unknown cache state. From
stock first TFTP `v2801sg-uboot-stop-7f1be111.bin` to 40400000, require
232480 bytes, then `go 0x40400000`. Set the board's factory ethaddr and
192.168.1.1 / serverip 192.168.1.100 as in earlier tests. Then:

```text
tftp 0x40500000 v2801sg-sram-uart-flags-af13e298.bin
crc32 0x40500000 0x124
cp.b 0x00200000 0x40510000 0x1000
crc32 0x40510000 0x1000
cp.b 0x40500000 0x00200000 0x124
crc32 0x00200000 0x124
dcache off
icache off
go 0x00200000
md.l 0x00200e00 0xa
md.l 0x00200e40 0x40
cp.b 0x40510000 0x00200000 0x1000
crc32 0x00200000 0x1000
```

Require size 292 and CRC 65dff0b9 in both locations before `go`. Require
restored SRAM CRC to equal the saved CRC (previously d11f7b32). Capture output;
if it hangs, stop until physical recovery. No DDR replay follows this test.

The ten result words are magic 55534631, sample count 40000 hex, first flags,
last flags, bitwise OR, bitwise AND, bit-7 set count, bit-8 set count, number
of adjacent changes, completion marker 1. The following 64 words are raw
samples taken every 4096 iterations. This can expose stable/transient flag
states without command-echo writes during the loop. It does not by itself
name a busy bit or prove FIFO versus shift-register semantics. Return/prompt
printing occurs after the record is complete and cannot alter the record.
