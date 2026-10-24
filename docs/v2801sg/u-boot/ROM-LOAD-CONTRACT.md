# ZX279125 ROM loading contract: observed and unresolved

Update: the later low-address boot-code capture resolves the SPI NOR initial
loading extent and entry. See [BOOTROM-ANALYSIS.md](BOOTROM-ANALYSIS.md):
flash 0..1fff is loaded to SRAM 00200000..00201fff, then entered at 00200000.
The earlier unknowns below describe the stock-image-only analysis and are
retained as provenance; they are superseded for this decoded NOR path.

The stock flash dump is SHA256
`d286f31239edd5ff9ed5843da41148da869a68a5f72abca11e8b6ec576235c8a`.
The whole-image raw disassembly is retained under
`/tmp/v2801sg-whole-boot-audit/`. Addresses below are the stock link addresses;
the initial image was also observed at SRAM address 00200000 on hardware.

## Header fields traced through stock instructions

| Flash offset | Value | Established stock-code use |
| --- | --- | --- |
| 00 | ea000014 | ARM branch to reset code at offset 58 |
| 04..1c | e59ff014 | Exception-vector loads through the table at offsets 20..38 |
| 3c | 12345678 | Observed marker; ROM validation semantics not established |
| 40 | 41f00000 | Reset code at 41f00084/ac/ec reads the linked destination/base |
| 44 | 0002f000 | Meaning unresolved; no direct PC-relative consumer found in the retained disassembly |
| 48 | 000296f8 | Reset code at 41f000d4 reads this as the later relocation copy length |
| 4c | 000337e8 | Startup at 41f01100..10 reads it into the runtime sizing field used in relocation placement |
| 50 | 0002e2d8 | Reset code at 41f00090 reads this as a copy extent; later startup also refers to this word |
| 54 | 0badc0de | Mutable exception-stack slot, not an established ROM checksum |

The last field is especially relevant: code at 41f01220..34 stores a runtime
stack address (`[r8 + 40] + 8`) into 41f00054. Exception handlers at
41f001c0 and following load their stack from that slot. Its initial marker
therefore cannot be assumed to describe a ROM authentication/checksum rule.
This does not exclude ROM inspecting the initial value; no ROM code has been
analyzed to establish such a rule.

The non-DRAM bootstrap calls DDR initialization, configures SPI and copies
the first 256 KiB of flash into 41f00000 before jumping there. Consequently,
the later reset copy extents do not prove that ROM initially loaded that
many bytes into SRAM. In particular, neither the 2f000 header value nor the
stock 0020a000 stack literal establishes the ROM's copy length or SRAM size.

## Size checkpoint

The compact base monitor is 232584 bytes, SHA256
`a0ea43a51fbb7176e238cb317ab595c1044551c66f25ce26f456fa95dd195c58`.
It leaves 29560 bytes within a 256 KiB combined boot partition. Its UART
initialization is enabled; TFTP, ping, SPI-NOR access and Linux boot commands
remain. Persistent environment storage/saveenv and unused command families
are disabled through the package's shared compact configuration.

The chainload remains 232480 bytes, SHA256
`7f1be11117e6e742cc7f258cd5b9be865f869d75504668088ec2340a2ebcafac`.
Both package builds passed; log: `/tmp/v2801sg-compact-build.log`.

The stock image has its environment at flash 30000..3ffff. A 232584-byte
monitor cannot also preserve that 64 KiB region within the same 256 KiB
partition. The size checkpoint is for a future combined layout with no stock
environment in that partition; it is not a claim that such a layout is ready
to install. No combined image or flash writer was produced.

## Remaining prerequisites

Before packaging the first stage, establish ROM's actual initial load extent,
entry convention and any required header validation. The existing SRAM
prefix/stack tests establish execution of their tested footprints, not this
ROM contract. A first stage must fit the proven load/execution region and
carry its complete code, literals and stack outside DDR.

Cold DDR execution still requires the observed stock CPU/cache entry setup
and a way to identify the first failing phase. A warm `go` into an SRAM
routine does not recreate reset-state DDR. No new warm DDR replay follows
from this analysis.
