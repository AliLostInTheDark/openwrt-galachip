# V2801SG PCB photo review

Source: `/home/ali/Desktop/pictures.tar.gz`, containing fifteen JPGs taken
2026-10-23. The archive was inspected without running any attached content.
All photographs were reviewed in an overview, with detailed front/back
inspection using IMG_20261023_190429784.jpg and
IMG_20261023_190736924.jpg. Analysis crops are local under
`/tmp/v2801sg-pcb-photos/`; the original archive is unchanged.

## Visible facts

- Both PCB sides are shown, including the main processor area, small resistor
  networks, accessible headers and power circuitry.
- UART-related TX/RX/GND silkscreen and pads are visible at the upper-left
  edge of the component side. This is consistent with the already working
  console, not evidence of a separate ROM-entry signal.
- XS1 is explicitly labeled GND, SDA, SCL and 3.3V. It is an I2C-labeled
  header, not a BOOT-select jumper or an independently identified UART port.
- An eight-pin IC1 package is visible above the main heatsink. Its marking
  cannot be read reliably from these photographs; its identity and individual
  pin functions are not established by this review.
- Numerous resistor/optional-component footprints are visible around the
  heatsink and on the back, but none has a visible BOOT/BOOTSEL/UART_BOOT
  designation. Zero-ohm links and unpopulated footprints alone do not
  establish boot-strap function.

## What the photos do not establish

The four ROM mode bits read from 94000038 have not been mapped to external
package pins or board resistor references. The observed value 20 hex means
mode 2 (SPI NOR), but a latched register value does not identify its physical
inputs or prove a writable software override. Hidden package connections
and inner-layer routing prevent that association from a visual trace alone.

No resistor, pad or flash pin can be named as a verified UART-boot selector
from this evidence. No shorting, soldering, flash-bus interference or
boot-mode-register write is proposed. More photographs of the same areas
are unlikely to supply the missing logical pin assignment. The useful next
evidence would be a chip pinout/schematic identifying the boot inputs, or
electrical mapping guided by such documentation.

The images therefore document the board and disambiguate headers, but do
not resolve reset-state entry into ROM UART mode. Source work and bounded
RAM/SPI verification can continue independently; cold replacement boot
still requires an established reset-state entry/recovery method.
