# Internet DFP-34X-2C2 dump: boot-path comparison

Input supplied by the user: /home/ali/Downloads/DFP-34X-2C2-mtd0.dd.
Size 8388608 bytes; SHA256
358a3873ca706b50ef5fd85eb9f8a41f80f76b930321f71c5e33305bacc15239.
Internet provenance/device identity is not independently authenticated. The
file was inspected offline, not executed or installed.

It contains a U-Boot 2013.04 banner dated Mar 13 2020, 14:18:18. Its stored
environment identifies U-Boot SFUV1.0.0B06 20200313142443. The environment
CRC at flash 30000..3ffff validates. Two uImage headers at 40140 and 3e0140
validate, including their complete payload CRCs. Both name Linux Kernel
Image and use load/entry 40008000; payload sizes are 1235288 and 1237867.
This supports the interpretation as an 8 MiB dual-image flash dump, not
an isolated BootROM dump or a SoC pinout document.

## Comparison with freshly captured V2801SG stock code

Ranges are file offsets, with exclusive ends:

| Component | DFP range | V2801SG range | Result |
| --- | --- | --- | --- |
| Early CP15 setup | 17c..1b4 | 17c..1b4 | Identical bytes |
| DDR initializer | 4a8..614 | 4a8..614 | Identical bytes |
| Early SPI and helper code | 614..774 | 614..774 | Identical bytes |
| SRAM bootstrap and literals | 774..880 | 774..880 | Identical bytes |
| Reset-register write | cdc..cec | cd0..ce0 | Identical bytes |

The reset routine still only writes 1 to 9400003c. No extra mode-selection
write exists in that routine. The byte-identical SRAM bootstrap takes the
same PC-dependent path, calls the same DDR initializer, then configures SPI
and loads stock U-Boot at 41f00000. The header/relocation lengths and some
later call targets differ; matching link addresses alone was not used to
claim function equivalence.

This is useful independent corroboration that the V2801SG DDR/startup
sequence is not peculiar to its 2025 firmware. It does not prove the source
replacement executes correctly from cold reset or independently establish
identical electrical design across the two boards.

No new reset-state ROM UART entry method was found in these compared boot
paths. There is no literal 94000038 in the first 256 KiB, but that bounded
literal search cannot exclude accesses synthesized from a register base,
indirect accesses or other unexamined paths. No whole-image absence claim
is made. Stored firmware data cannot by itself identify external boot pins.

## Evidence and limits

Byte comparisons, SHA256 digests, raw ARM instruction listings and validated
headers are retained under tmp/dfp34x-analysis/ in comparison.json and
dfp-early.asm / vsol-early.asm. The first 256 KiB was retained separately.
The existing V2801SG REA interpretation supplies the reference call-path
analysis; its CPU/DDR/bootstrap body bytes are identical in this input.

The attempted REA firmware signature scan was unavailable because its
caller-supplied REA_BINWALK_COMMAND was not configured. No scanner was
installed or configured to work around that limitation. Header inspection
and byte comparisons above were performed with ordinary local tools, not
reported as new REA Evidence or a complete filesystem analysis. An hsqs
byte match at 60e0 inside the bootloader is only an unvalidated signature
candidate, not an identified filesystem.
