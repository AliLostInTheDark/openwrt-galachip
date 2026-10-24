# ROM UART entry: current evidence and unresolved selection

The console and XMODEM receiver work on initialized hardware; see
ROM-CONSOLE-TEST.md. Entering that console routine from U-Boot is not evidence
that reset-state UART boot has been selected.

## Established

- Captured ROM reads `(read32(94000038) >> 4) & f` and dispatches on it.
- The board reports 00000020, selecting NOR mode 2.
- Modes 0 and f dispatch to the ROM console; the NOR path has no UART-key
  selection check before loading/executing its SRAM first stage.
- Stock U-Boot reset writes 1 to 9400003c; no boot-mode override is set by
  that routine. Available source monitor reset uses the same mechanism.
- Neither the inspected stock decompilation nor the available board code
  establishes a writeable override for the mode field.

The mode field's physical source is still unknown. It may involve sampled
pins or other chip configuration; calling it a proven external strap input
would go beyond the captured instruction evidence. No pin number, resistor
reference or register-write recipe has been established for selecting mode
0/f on this board.

## Public-document investigation

The manufacturer's English and Chinese ZX279125 product pages describe
networking features but supply no boot configuration or pin assignment:

- https://www.sanechips.com.cn/en/ProductEn/WiredEn/fixedNetworkTerminalEn/1552570424078929921.html
- https://www.sanechips.com.cn/chinese/product/wiredProducts/terminalChip/1556481690484883457.html

A JLCPCB listing identifies part C9900040717 as ZX279125, QFN88-10*10-0.4:
https://jlcpcb.com/partdetail/JLCPCBAssembly-ZX279125/C9900040717
Its generic page text advertises an EasyEDA symbol/footprint. The actual
embedded library viewer was inspected in the browser and instead reports
that no library is available, offering a CAD-model request. No request or
message was submitted. This listing is a package lead, not a signal pinout.

Searches for the exact chip with English/Chinese boot-mode, boot-select,
pinout, schematic and SDK terms did not produce a verifiable boot-pin
assignment. Commercial ONU specification sheets and other-chip platform
sources do not fill this gap. No inference from ZX297520/ZX279128S pinouts
was used to propose an electrical modification.

The photographs identify headers but do not map the four mode bits to
package pins; see PCB-PHOTO-REVIEW.md. Repeating the same ROM dump or reading
the selector again cannot establish those electrical connections.

## Required next evidence

A ZX279125 pin-function/reference-schematic document, or equivalent proven
board-level UART-boot procedure, is needed before prescribing a strap change.
A documented software override would be an alternative; none has been found.
Without one, reset-state entry remains unresolved rather than verified.

No mode-register writes, resets, pin shorts, resistor changes or flash writes
were performed during this investigation. The current working monitor and
the installed stock bootloader were left untouched.
