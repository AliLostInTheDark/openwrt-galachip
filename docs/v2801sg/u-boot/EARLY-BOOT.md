# ZX279125 source-based early startup — 2026-10-23

## Implemented, offline verified

The source overlay under `files/board/zte/zx279125/` now contains:

- `ddr.c`: clock/DDR sequence reconstructed from stock linked addresses
  41f004a8..41f00610, including the 41f00474 delay helper.
- `early_spi.c`: the polled 256-byte SPI read at 41f00688..41f0076c.
- `early_boot.c`: the post-DDR controller setup and page-copy loop from
  41f007d0..41f00864, with caller-selected destination, flash offset and size.
  It returns to its caller; it does not assume a ROM header or jump into a
  partially constructed replacement image.

No vendor binary fragment is embedded. Constants and operation ordering are
reconstructed from the existing board's flash bootloader, SHA256
`d286f31239edd5ff9ed5843da41148da869a68a5f72abca11e8b6ec576235c8a`.
These are board-specific sequences, not a generic DDR controller driver.

The stock reset path uses the temporary stack 0020a000 and calls its early
loader before main U-Boot. When the PC is outside 4xxxxxxx, it sets up DDR,
configures SPI and reads flash offsets 0..3ffff in 256-byte pages into
41f00000..41f3ffff, then jumps to 41f00000. From the DRAM address range it
skips the DDR/load path. The initial ROM copy address/length, header handling
and executable SRAM range remain unresolved. The stack literal alone does not
prove that all of 00200000..0020a000 is writable executable SRAM.

### Verification

- DDR host trace versus a focused raw-ARM instruction interpreter: all 35
  ordered MMIO writes and three delay events match.
- SPI trace: all 216 MMIO read/write events and 64 destination words match.
- Readiness stalls exercise all bounded timeout paths; actual-PC/SP checks
  reject DDR execution/stack before MMIO. The guard does not prove an entire
  caller chain or linker placement safe.
- Dispatcher checks cover the six setup writes, page offsets/destinations,
  boot-partition/low-DRAM bounds and propagation of DDR/reset/SPI errors.
- Full U-Boot package compilation passed with the actual platform headers.
  DDR/SPI objects have no external runtime dependencies. The dispatcher only
  calls DDR init, SPI setup and SPI read. No allocator, initialized globals or
  timer is used.

The vendor delay is 2000 volatile increments, not a calibrated microsecond
delay. Vendor polls are infinite; the source intentionally adds iteration
ceilings. Register naming beyond the observed operations and actual electrical
timing margins are not established by these tests.

Local evidence/harnesses: `/tmp/v2801sg-ddr-audit/`,
`/tmp/v2801sg-early-spi-audit/`, `/tmp/v2801sg-bootstrap-audit/`.
Build log: `/tmp/v2801sg-early-stage-build.log`.

## Current monitor remains a warm-chainload build

The early objects are compiled for checking but have no startup caller and
are discarded from the monitor by the linker. The weak upstream s_init
remains; dram_init still reports already initialized RAM. No DDR command is
exposed. Do not call the early loader from a RAM monitor: reinitializing DDR
would destroy its instructions, stack and data. Cold execution also requires
the complete call path/literals/stack outside DDR, caches/MMU disabled and
interrupts masked. A timeout must halt there; there is no rollback.

The monitor now advertises only 40000000..40ffffff. The unsafe upper 6 MiB
bank has been removed from both its DRAM bank list and DT. This does not
reclaim or overwrite the hardware packet pool or stock cursor table.

Unused ELF/VxWorks, serial loadb/loads, raw SPI command, MTD command, MD5,
DHCP/BOOTP and image-extraction commands are omitted from the chainload variant.
TFTP, ping, SPI-NOR reads, memory inspection, CRC32 and go remain. The resulting
monitor is 232096 bytes, leaving 30048 bytes within a 256 KiB partition, but
this excludes an actual ROM-compatible first stage and is not a flash image.

New monitor:
`bin/targets/zte/zx279125/v2801sg-uboot-chainload-3723d551.bin`, SHA256
`3723d55199803a4c39fd628b20236ef4037cec015d19a1756e9b6df558e8d607`.
Its RAM entry remains 40400000; CRC32 is ba8fd0ec. It has not been hardware
tested; the earlier successful c2d60c34 monitor remains a separate artifact.

## SRAM probe hardware verified

The user read 00200000..0020007f from the running RAM monitor. All 32 words
match the stock flash reset/header prefix, including branch ea000014,
12345678 marker, link base 41f00000 and the first startup instructions. This
confirms that a copy of at least that prefix is readable there; it does not
establish the ROM's complete copy length.

`tools/sram-probe.S` is a 302-byte probe linked at the candidate 00200000.
It temporarily uses the vendor-observed 0020a000 stack, prints actual PC/SP
through the existing UART, restores the caller's callee-saved registers and
stack, and returns. UART waits are bounded. It never initializes DDR, changes
clocks, accesses SPI or writes flash. Its only writes are the candidate SRAM
stack and UART TX FIFO.

Artifact: `bin/targets/zte/zx279125/v2801sg-sram-probe-e253c8ef.bin`, SHA256
`e253c8ef2dd6a9ad0ad9fdba3c5895fb42b928ee64ef79d128534bc575f918ca`,
CRC32 88c5d620. It is not a bootloader replacement or DDR test.

The user completed the following test from the running RAM monitor, with DDR
left initialized. The commands are retained as the reproduction sequence;
they do not need repeating for this checkpoint:

```text
tftp 0x40500000 v2801sg-sram-probe-e253c8ef.bin
crc32 0x40500000 0x12e
crc32 0x00200000 0x1000
cp.b 0x00200000 0x40510000 0x1000
crc32 0x40510000 0x1000
cp.b 0x40500000 0x00200000 0x12e
crc32 0x00200000 0x12e
icache off
go 0x00200000
cp.b 0x40510000 0x00200000 0x1000
crc32 0x00200000 0x1000
```

The transfer must be 302 bytes. Probe CRCs must be 88c5d620; the original,
backup and restored 4 KiB CRCs must be d11f7b32 (computed from the stock flash
prefix). Stop on any mismatch. After go, expect PC=0x00200000,
SP=0x00209fd8, the no-DDR-init message and a return to the monitor prompt.
The original monitor reported caches disabled; icache off prevents stale
instruction fetches. Restore the saved prefix only after a successful return.
If execution faults or the monitor stops responding, stop testing and recover
at the desk. The SRAM stack scratch is not restored by this prefix backup.

### Observed result — 2026-10-23

- TFTP transferred exactly 302 bytes; DRAM and SRAM probe CRCs were 88c5d620.
- Original and backup first-4-KiB CRCs were both d11f7b32.
- The probe printed PC=0x00200000 and SP=0x00209fd8, followed by the
  no-DDR-initialization message. U-Boot reported return code 0 and resumed.
- The restored SRAM prefix CRC was d11f7b32.

This verifies writes and execution for the probe footprint, enough writable
stack space for this call, UART output and a successful return to the DRAM
monitor. It does not establish the entire SRAM extent, ROM loading contract,
reset-state startup or DDR initialization. The stack scratch outside the
prefix backup was used but not restored. No flash was written.

Next: establish the ROM loading/header contract, create a distinct first-stage
image with a verified linker layout and stack budget, and exercise its SPI
reader from SRAM without touching DDR initialization. True reset-state DDR
startup then needs a recoverable loading method; this warm probe alone does
not justify installing a replacement bootloader.

## SRAM SPI probe hardware verified

`tools/sram-spi-probe.S` calls the actual U-Boot-built early_spi.o from the
same SRAM entry and stack as the verified probe. It reads exactly 256 bytes
from flash offset zero into initialized DRAM at 40520000. It does not call
the DDR initializer or bootstrap loader and leaves clock/reset/divider setup
to the already running monitor. This tests a warm controller, not cold setup.
Only SPI opcode 03 (read) is issued; controller registers are changed but
are not restored. The monitor's next SPI operation may need sf probe again.

The linked reader's 448 bytes match the U-Boot object exactly. It has no
unresolved symbols, writable globals or BSS. The linker bounds the complete
image below 00201000. Stack use is 40 bytes for saved caller registers plus
at most 16 bytes in UART helpers; the compiled SPI reader uses no stack.
The existing offline comparison still passes 216 ordered MMIO events and
64 destination words. The hardware result below supplements these offline checks.

Artifact: `bin/targets/zte/zx279125/v2801sg-sram-spi-e1a92936.bin`.
Size 874 bytes (36a hex), SHA256
`e1a92936fe567b98d1350e02bc444c6a294660b5cdfc8137df1cbb8c1917b9ff`,
CRC32 c210be94. ELF, map and disassembly are in
`/tmp/v2801sg-sram-spi-probe/`. Built by assembling the new ARM wrapper,
linking it with the chainload build's board/zte/zx279125/early_spi.o using
tools/sram-spi-probe.lds, then objcopy -O binary. The built early_spi.c was
compared with the source overlay before linking.

Stage the artifact in /srv/tftp on the PC. In the current RAM monitor:

```text
tftp 0x40500000 v2801sg-sram-spi-e1a92936.bin
crc32 0x40500000 0x36a
sf probe
sf read 0x40530000 0x0 0x100
crc32 0x40530000 0x100
```

Require 874 transferred bytes, probe CRC c210be94 and reference-page CRC
1209ed5c (computed from the unchanged stock bootloader backup). Stop on any
command failure or CRC mismatch. Preserve the restored SRAM prefix again:

```text
cp.b 0x00200000 0x40510000 0x1000
crc32 0x40510000 0x1000
mw.b 0x40520000 0xa5 0x100
cp.b 0x40500000 0x00200000 0x36a
crc32 0x00200000 0x36a
```

Require backup CRC d11f7b32 and SRAM probe CRC c210be94, then:

```text
dcache off
icache off
go 0x00200000
```

Require the probe's SPI read rc=0x00000000 and U-Boot return code 0. After a
successful return, restore the prefix before comparing the flash output:

```text
cp.b 0x40510000 0x00200000 0x1000
crc32 0x00200000 0x1000
crc32 0x40520000 0x100
cmp.b 0x40520000 0x40530000 0x100
```

Require restored prefix CRC d11f7b32, output CRC 1209ed5c and 256 equal bytes.
On a nonzero probe return, restore the prefix but stop further tests and retain
the output for diagnosis. On a hang, stop and recover at the desk. Do not run
DDR initialization as part of this test.

### Observed result — 2026-10-23

The user ran the sequence above from the RAM-chainloaded monitor:

- Transfer: 874 bytes; staged and SRAM probe CRCs both c210be94.
- Monitor sf read: first 256 flash bytes CRC 1209ed5c.
- Probe: PC=0x00200000, SP=0x00209fd8, SPI return code 0; U-Boot
  reported application return code 0 and resumed its prompt.
- Source-reader output: CRC 1209ed5c, with all 256 bytes equal to the
  monitor's independent read.
- SRAM prefix restored: first-4-KiB CRC d11f7b32.

This verifies the source reader executing from SRAM against the physical NOR
with the controller already initialized by the RAM monitor. It does not
verify other page offsets, a full-image copy, cold controller setup, the ROM
loading/header contract or DDR initialization. No flash was written.

Next validation should extend the same read-only SRAM path to multiple pages
and compare a complete boot-partition copy against the normal SPI driver,
using bounded low-DRAM buffers. Keep this separate from cold clock/reset and
DDR tests so any failure has a narrow cause.

## Full boot-partition SRAM copy hardware verified

`tools/sram-spi-copy-probe.S` adds a bounded 1024-page loop around the same
unchanged 448-byte U-Boot early SPI reader, using tools/sram-spi-probe.lds.
It copies flash offsets 0..3ffff into 40600000..4063ffff, reports the first
error or completed byte count, then returns. The monitor reference buffer is
40700000..4073ffff; neither buffer overlaps the staged probe (40500000),
saved SRAM prefix (40510000), observed monitor stack/FDT or relocation.
This replaces any old Linux image data at 40600000; it does not boot it.
No DDR, clock/reset or SPI divider initialization is called.

Artifact: `bin/targets/zte/zx279125/v2801sg-sram-copy-c46c925c.bin`,
947 bytes (3b3 hex), SHA256
`c46c925c2ffe4c2a77dcdad83ca997ae2d734084ba38383b7dbd8d9419e78d7f`,
CRC32 af2bd70b. ELF/map/disassembly: /tmp/v2801sg-sram-spi-copy-probe/.
Link assertions passed, no symbols remain unresolved, and linked reader bytes
match the previously tested object. The assembled loop was inspected for its
0x100-byte increments, 0x40000 limit, early-error branch and register restore.
Hardware execution is recorded below.

The current stock boot-partition backup's first 0x40000 bytes have CRC32
072cb726. Stage the artifact in /srv/tftp, then run individually:

```text
tftp 0x40500000 v2801sg-sram-copy-c46c925c.bin
crc32 0x40500000 0x3b3
sf probe
sf read 0x40700000 0x0 0x40000
crc32 0x40700000 0x40000
```

Require 947 bytes, probe CRC af2bd70b and reference CRC 072cb726. Stop on any
failure or mismatch. Then preserve SRAM and prepare a distinct output buffer:

```text
cp.b 0x00200000 0x40510000 0x1000
crc32 0x40510000 0x1000
mw.b 0x40600000 0xa5 0x40000
cp.b 0x40500000 0x00200000 0x3b3
crc32 0x00200000 0x3b3
```

Require backup CRC d11f7b32 and SRAM probe CRC af2bd70b before execution:

```text
dcache off
icache off
go 0x00200000
```

Require SPI read rc=0x00000000, completed bytes=0x00040000 and application
return code 0. After return, restore SRAM and compare all 262144 bytes:

```text
cp.b 0x40510000 0x00200000 0x1000
crc32 0x00200000 0x1000
crc32 0x40600000 0x40000
cmp.b 0x40600000 0x40700000 0x40000
```

Expected CRCs d11f7b32 and 072cb726, with all 262144 bytes equal. Restore the
SRAM prefix after a nonzero return too, then stop for diagnosis. If the board
hangs, stop testing and recover at the desk. Nothing in this test writes flash.

### Observed result — 2026-10-23

- Transfer: 947 bytes, probe CRC af2bd70b in both DRAM and SRAM.
- Normal monitor SPI read of flash 0..3ffff: CRC 34d4cc5a.
- SRAM source-reader loop: return code 0, completed bytes 00040000;
  application returned to the monitor with code 0.
- Source-reader output CRC: 34d4cc5a. All 262144 bytes matched the monitor
  reference buffer, proving all 1024 page reads agree for this warm test.
- SRAM prefix restored with CRC d11f7b32.

The user continued despite the reference differing from the precomputed backup
CRC 072cb726. Both local stock backups still produce 072cb726 for this region;
the current physical read produces 34d4cc5a by both implementations. The copy
test passed against the live reference, but backup-to-live identity did not.
Do not attribute that difference to an environment update, flash corruption
or reader failure without locating it. The first 4 KiB still match the backup.
No flash write command was included in this test.

Next read-only check: CRC each 64 KiB quarter of the retained live buffer at
40700000, 40710000, 40720000 and 40730000. Backup CRCs in order are 3f436f1b,
f007510f, e0e9bdd2 and dd56133e. Locate any discrepancy before proceeding to
cold controller/reset or DDR initialization tests. Warm full-image reading
does not establish ROM loading/header behavior or cold startup.

### Quarter comparison and vendor environment identification

The user's live 64 KiB CRCs are 3f436f1b, f007510f, e0e9bdd2 and eeae6842.
Thus the first 192 KiB match the backup; the difference is confined to flash
30000..3ffff. Inspection of the backup identifies that entire final sector
as the stock U-Boot environment: a little-endian CRC word f1073313 followed
by NUL-separated variables at 30004. CRC32 of its remaining 65532 bytes is
also f1073313, confirming the environment format and extent. Variables include
bootcmd, bootdelay, ethaddr, serverip and versioninfo, followed by zero padding.

This makes an environment change a plausible explanation for the differing
whole-partition CRC, but the live environment contents/checksum still need
inspection. Read md.b 40730000 300 and crc32 40730004 fffc from the retained
reference buffer; compare the latter with the first little-endian word.
No environment write or save is needed. The replacement base defconfig's
ENV_OFFSET=7a0000 is not the vendor environment location; chainload builds
continue to use ENV_IS_NOWHERE with saveenv disabled. Any persistent layout
must be reviewed separately before a replacement flash image is produced.

### Live environment CRC difference resolved

The user's live environment begins with bytes 1b 33 d6 14; CRC32 of its
65532-byte payload is 14d6331b, so the stored CRC is valid. The displayed
versioninfo ends in `0x40000 0x0 0x9e 0x00`, versus the backup's
`0x40000 0x0 0x8f 0x82`. Replacing only those versioninfo fields in an
in-memory copy of the backup and recalculating its environment CRC reproduces
all three observed live CRCs exactly: payload 14d6331b, last sector eeae6842,
whole boot partition 34d4cc5a. No file or flash backup was modified.

The changed versioninfo and corresponding environment checksum therefore
account for the measured CRC discrepancy; no SPI reader defect is indicated.
This is a matching reconstruction of the supplied dump/checksums, not a
byte-for-byte comparison of a newly acquired complete live flash file.
The semantics of the two changed vendor fields are not yet established.

## SRAM controller-setup probe hardware verified

The existing controller-local sequence was extracted from early_boot.c into
zx279125_early_spi_setup() in early_spi.c. The cold dispatcher calls it after
its unchanged bus setup. The helper checks actual PC/SP before MMIO, clears
bit 2 of 94407008, sets bit 0, waits for bit 1 with a bounded poll, then clears
94407024 bits 17:16 and sets bit 16, preserving other bits with the original
readback/RMW order. This is the observed vendor sequence; it is not proof of
a complete peripheral reset or power-on state initialization.

tools/sram-spi-setup-probe.S invokes only that helper and the 1024-page copy
loop. It does not invoke DDR initialization or write the system bus/PLL
registers. It uses the already verified SRAM entry/stack and low-DRAM buffers.
Setup failure skips every page read; read failure stops the copy immediately.
The returned status covers both stages and the completed count excludes any
partly read failing page. Controller state is changed and is not rolled back.

Offline checks: four ordered setup writes, preserved bits, delayed readiness,
bounded timeout and PC/SP guards pass. The cold-dispatcher regression checks
still pass all six writes, page addresses, bounds and failure propagation.
The U-Boot package build passes (/tmp/v2801sg-spi-setup-build.log). The compiler
now outlines zx_spi_wait, changing reader machine code; the rebuilt reader
still matches all 216 vendor MMIO trace events and passes all six prior
failure cases. Stack-use reports show 8 bytes for setup, 16 for the reader,
and zero for the shared wait, in addition to the probe's 40-byte saved frame.
Link assertions pass and no symbols remain unresolved. Test files are local
under /tmp/v2801sg-spi-setup-audit/.

Artifact: bin/targets/zte/zx279125/v2801sg-sram-setup-c71e7d15.bin,
1190 bytes (4a6 hex), SHA256
`c71e7d158126145155eaeeccf140f46a6d6c5f6ed9e07d88c65bf6465dca6c0d`,
CRC32 ae548881. ELF, linker map and disassembly are under
/tmp/v2801sg-sram-spi-setup-probe/. Hardware execution is recorded below.

After staging the artifact in /srv/tftp, run individually:

```text
tftp 0x40500000 v2801sg-sram-setup-c71e7d15.bin
crc32 0x40500000 0x4a6
sf probe
sf read 0x40700000 0x0 0x40000
crc32 0x40700000 0x40000
```

Require 1190 bytes, probe CRC ae548881, current reference CRC 34d4cc5a.
Stop on a mismatch or command error, then preserve SRAM and prepare output:

```text
cp.b 0x00200000 0x40510000 0x1000
crc32 0x40510000 0x1000
mw.b 0x40600000 0xa5 0x40000
cp.b 0x40500000 0x00200000 0x4a6
crc32 0x00200000 0x4a6
```

Require backup CRC d11f7b32 and SRAM probe CRC ae548881, then:

```text
dcache off
icache off
go 0x00200000
```

Require SPI setup/copy rc=0x00000000, completed bytes=0x00040000 and a return
to U-Boot with code 0. After return, restore and compare:

```text
cp.b 0x40510000 0x00200000 0x1000
crc32 0x00200000 0x1000
crc32 0x40600000 0x40000
cmp.b 0x40600000 0x40700000 0x40000
```

Require d11f7b32, 34d4cc5a and all 262144 bytes equal. On a nonzero return,
restore SRAM but stop further tests; on a hang, stop and recover at the desk.
Nothing in this test writes flash. A pass would validate this sequence on an
already powered/clocked controller, not the untested system bus/PLL setup,
ROM contract or reset-state DDR initialization.

### Observed result — 2026-10-23

The user transferred all 1190 bytes and verified ae548881 in both DRAM and
SRAM. The reference flash read CRC was 34d4cc5a. Executing the probe printed
PC=00200000, SP=00209fd8, SPI setup/copy rc=0 and completed bytes=00040000;
U-Boot resumed with application return code 0. The output CRC was 34d4cc5a
and all 262144 bytes matched the independent monitor read. The SRAM prefix
was restored with CRC d11f7b32. No flash write command was used.

The controller-local setup plus full boot-partition copy therefore passes
on this powered/clocked board. System bus setup, reset-state behavior, DDR
initialization and ROM loading remain untested. Before any bus-register
write, obtain the current values with md.l 94200000 2; the vendor sequence
clears mask 3c030000 at the first register and sets mask 3c0 at the second.
Read those values first to determine whether replay would change anything.

## Temporary bus-setup replay hardware verified

The user read 94200000=403f1f00 and 94200004=000003c0. These match the stock
table at linked 41f21bec, which the later initialization routine at 41f1d5a4
copies to 94200000..94200008. The earlier loader's mask would change the first
register to 403c1f00, clearing only bits 17:16 from the observed live value;
the second remains 000003c0. Exact register/field semantics remain unresolved;
the existing source's BUS label does not establish that these are clock gates.

tools/sram-spi-bus-probe.S saves both words and rejects any initial values
other than the user's observed pair before writing. It applies the two vendor
RMW operations, calls the tested SPI setup and 1024-page reader, then restores
both words before printing the result, including on setup/read failure. It
never calls DDR setup or touches PLL registers. A hardware hang can still
prevent restoration; this is a controlled warm-state test, not cold startup.

Artifact: bin/targets/zte/zx279125/v2801sg-sram-bus-cc5094cf.bin,
1309 bytes (51d hex), SHA256
`cc5094cfdf1c27714324283555a9a54dadbd2b46f283b8e2edd138e273ca1f67`,
CRC32 6fa47230. ELF/map/disassembly and an assembled-wrapper instruction
interpreter are under /tmp/v2801sg-sram-spi-bus-probe/. Link bounds passed.
The interpreter stubs UART/SPI calls and verifies the actual wrapper's 1024
page addresses, temporary writes, restoration of both bus words, caller
registers and stack on success, setup error and page error. Both mismatched
fingerprints reject without writes. It does not emulate peripheral behavior.

After staging the artifact in /srv/tftp, run individually:

```text
tftp 0x40500000 v2801sg-sram-bus-cc5094cf.bin
crc32 0x40500000 0x51d
sf probe
sf read 0x40700000 0x0 0x40000
crc32 0x40700000 0x40000
cp.b 0x00200000 0x40510000 0x1000
crc32 0x40510000 0x1000
mw.b 0x40600000 0xa5 0x40000
cp.b 0x40500000 0x00200000 0x51d
crc32 0x00200000 0x51d
```

Require 1309 transferred bytes, both probe CRCs 6fa47230, reference CRC
34d4cc5a and SRAM backup CRC d11f7b32; stop on any mismatch. Then:

```text
dcache off
icache off
go 0x00200000
```

Require Bus/SPI copy rc=0, completed bytes=00040000 and return to the monitor.
After return (including nonzero return), restore the SRAM prefix:

```text
cp.b 0x40510000 0x00200000 0x1000
crc32 0x00200000 0x1000
md.l 0x94200000 0x2
crc32 0x40600000 0x40000
cmp.b 0x40600000 0x40700000 0x40000
```

Require restored prefix d11f7b32, original register pair 403f1f00/000003c0,
output CRC 34d4cc5a and all 262144 bytes equal. Stop on failure or hang; no
flash write, saveenv or DDR initialization belongs in this test.

### Observed result — 2026-10-23

The user transferred all 1309 bytes and verified probe CRC 6fa47230 in DRAM
and SRAM. The reference flash read CRC was 34d4cc5a. The probe executed at
00200000 with stack 00209fd8, returned Bus/SPI copy rc=0 and completed
00040000 bytes. U-Boot resumed with application return code 0. Both bus
registers read back as their original values, 403f1f00 and 000003c0. The
output CRC was 34d4cc5a and all 262144 bytes matched the independent monitor
read. The restored SRAM prefix CRC was d11f7b32.

This validates replay of the vendor-derived bus operations, SPI controller
setup and full boot-partition read from SRAM on this running board, including
return and restoration. It does not validate power-on defaults, reset-state
DDR initialization, the ROM loading contract or a flash-installable image.
No flash write command was used.

The next milestone is a separately linked first-stage image and a proven
recoverable entry/loading method for DDR initialization. Warm replay tests
must not be treated as proof that the image can replace the installed stock
bootloader. Reinitializing DDR cannot safely return to the current monitor;
all executing code, literals, stack and failure handling must remain in SRAM.

## SRAM DDR recovery stage failed — original artifact retired

`tools/sram-ddr-recovery.S` is a separate SRAM-linked diagnostic. It masks
interrupts, switches to 0020a000, rejects an active MMU, I/D cache or MAC RX,
then calls zx279125_bootstrap_load(41f00000, 0, 40000). This runs the source
DDR sequence, the now hardware-verified bus/SPI sequence, and copies the
installed stock bootloader into its observed high-DRAM link range. It verifies
the stock reset branch ea000014 and header marker 12345678 before jumping to
41f00000. Success is expected to enter installed stock U-Boot. Errors halt in
SRAM; the physical reset/power cycle reloads the unchanged installed loader.
The exact stock restoration tuple was added to the dispatcher's destination
validation; other high-DDR targets remain rejected.

Artifact: `bin/targets/zte/zx279125/v2801sg-sram-ddr-recovery-97208ab2.bin`,
2262 bytes (8d6 hex), SHA256
`97208ab27047e552db23584e32e96c820f3545bc7c591b40110d64fbc0bd24f2`,
CRC32 8bf777a7. ELF/linker map/disassembly and entry-path audit are under
`/tmp/v2801sg-sram-ddr-recovery/`. The linker places every loadable byte
within 00200000..002008d5, below the backed-up 4 KiB SRAM prefix; BSS and
initialized writable globals are empty. There are no unresolved symbols.
The U-Boot package build passed. Host traces still match 38 vendor DDR
write/delay events and 216 vendor SPI MMIO events. Dispatcher checks cover
the strict stock tuple and reject adjacent high-DDR requests. An instruction
interpreter verifies the compiled wrapper refuses active MMU/cache/MAC RX,
halts on loader/image failure, and branches to the stock entry only on success.
These are offline checks; actual DDR reinitialization has not been attempted.

The installed stock loader remains the recovery route because this is loaded
only into SRAM through a running RAM monitor; it is not a ROM-packaged or
flash-installable bootloader. A DDR failure can also disrupt UART and require
a physical power cycle. Active DMA outside the MAC RX enable bit is not fully
characterized. Keep the fiber disconnected and unplug the dedicated LAN cable
after staging over TFTP and before executing the stage. The MAC guard only
checks RX bit 2; it does not prove every packet-engine master is idle. The
power-on ROM copy format/extent remains unresolved.

The original 97208ab2 artifact below failed on hardware with continuous `g`
output. Its commands are historical reproduction instructions, not a retry
sequence. Hardware tests are paused pending confirmation of stock recovery.

### Desk-only RAM test sequence

Before changing SRAM, verify the current NOR and staged artifact:

```text
tftp 0x40500000 v2801sg-sram-ddr-recovery-97208ab2.bin
crc32 0x40500000 0x8d6
sf probe
sf read 0x40700000 0x0 0x40000
crc32 0x40700000 0x40000
cp.b 0x40500000 0x00200000 0x8d6
crc32 0x00200000 0x8d6
dcache off
icache off
md.l 0x92200000 0x1
```

Require 2262 transferred bytes, both stage CRCs 8bf777a7, NOR CRC 34d4cc5a
and MAC RX bit 2 clear. If any differ, do not issue go. After unplugging LAN,
run go 0x00200000 at the desk. This does not return to the RAM monitor. Success
prints the SRAM stage banner, then the 2013.04 stock U-Boot banner/prompt.
If the stage prints an error or the device stops responding, power-cycle once
and stop further tests until the failure is diagnosed. No flash write, erase,
upgrade or environment save is part of this test.

### Failure diagnosis — 2026-10-23

The user verified 2262 bytes and CRC 8bf777a7 in both DRAM and SRAM, flash
CRC 34d4cc5a, and MAC register 92200000=020e0888 (RX bit 2 clear). After go,
the displayed banner stopped at `then loading` and repeated `g` indefinitely.
No return, stock handoff or successful DDR initialization was observed.

The original UART putc waited only for FIFO space. It never waited for queued
text to leave the transmitter before calling the DDR routine, which changes
PLL/clock/reset registers. Exactly 16 bytes of the 67-byte banner remain after
the last visible `g`. The current UART driver specifies a 16-byte FIFO and
uses FLAGS bit 7 for TX empty. A focused interpreter running the original
assembled entry, puts and putc against a 16-byte FIFO model reaches the loader
call with precisely the observed prefix transmitted and 16 bytes pending.
It does not produce an infinite software `g` loop. Thus the truncated visible
banner cannot establish that execution stopped before DDR was called. A
clock/reset interaction with queued UART output is a supported hypothesis,
not a proven cause of the complete hardware failure.

Source now waits for TX-empty with a bounded poll before the loader call and
before the stock handoff. A timeout halts before DDR. The corrected default
stage is 2334 bytes, SHA256
`6fe7ee11dd07530fa90473750dbfdedfdaadaa139b89d2588e9546e25195eb93`,
CRC32 2c1eb1cc, at bin/targets/zte/zx279125/
v2801sg-sram-ddr-drain-6fe7ee11.bin. It is not hardware tested or staged for
retry. The FIFO model verifies its entire banner transmits before the loader
call. This corrects a diagnostic defect; it does not establish working DDR.

A separate build with ZX_DDR_UART_AUDIT uses the same assembly and linked
footprint but returns after the banner, drain and explicit audit message,
without calling the loader or changing clocks/DDR. It preserves the caller's
callee-saved registers, stack and CPSR. A full instruction-path model verifies
both successful output/return and a stuck-drain timeout return, without ever
entering the loader; the timeout test shortens the modeled poll budget only.

UART-only audit artifact: bin/targets/zte/zx279125/
v2801sg-sram-uart-audit-96f6be2b.bin, 2461 bytes (99d hex), SHA256
`96f6be2ba8b8c51732507f0425f2b6ed19d8a8b13bd107a404f079447e17bccb`,
CRC32 1f03cb0c. Both corrected variants remain within the linked 4 KiB bound
and have no unresolved symbols. Original/fixed FIFO models and artifacts are
under /tmp/v2801sg-sram-ddr-recovery/, /tmp/v2801sg-sram-ddr-drain/ and
/tmp/v2801sg-sram-ddr-uart-audit/. After stock recovery is confirmed, the
UART-only audit is the next hardware check; do not retry DDR initialization
based solely on the missing drain having been fixed.

Further shutdown review: stock cleanup at 41f1f7b4 clears bits 2 and 3 in
both 921c0000 and 92200000 without polling DMA idle. The new monitor stop
path now drains receive work and checks ownership before clearing its TX
enable, but this is not a global AXI-idle proof. The default DDR diagnostic
additionally refuses nonzero BMU control at 92168000 or enabled RX/TX on
either MAC. Thus the previously built DDR-drain image is superseded and must
not be used. The UART-only audit remains byte-identical. See
`NEXT-BOOT-TEST.md` for the updated monitor and manual checks, and
`RECOVERY.md` for the observed stock button/TFTP installer path.

### Related ZX279128S RFC — useful comparison, not a ZX279125 specification

The user supplied the author's follow-up at:
https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1186634.html
It discusses a ZX279128S/H3600 Linux platform series and v2 changes. The
associated initial clock patch and review are:
https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185419.html
https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185533.html

That proposed clock driver places LSP CRPM at 94400000 and assigns UART0,
UART1 and SPI control registers at offsets 10, 14 and 18. Our exact vendor
DDR routine writes 303 to those same three physical addresses. This supplies
a concrete cross-SoC lead for UART clock changes during the failed test;
it does not establish identical fields or prove the repeating-g mechanism.
The review explicitly questions unresolved PLL parents/rates and recommends
reuse of the existing ZTE PLL/regmap infrastructure. Do not treat its initial
rate assumptions as measured ZX279125 frequencies.

The SoC DTS patch is:
https://lists.infradead.org/pipermail/linux-arm-kernel/2026-October/1185434.html
It labels 94200000 as pinmux, supporting a pin-control interpretation of our
previously generic BUS label. It also exposes important differences: its SPI
controller is at 94406000 and GPIO at 94407000, whereas our physically tested
ZX279125 SPI controller is at 94407000. Its UART is described as PrimeCell
PL011; our recovered UART layout has different data/flags offsets. Borrow
architecture and test hypotheses, not unverified addresses or compatibles.
This is an RFC and review trail, not proof of a merged or fully validated port.
