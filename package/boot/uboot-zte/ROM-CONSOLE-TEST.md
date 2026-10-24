# ROM console routine audit (warm hardware)

This test enters only the captured ROM command loop at 00000918, bypassing
ROM reset startup, boot-mode dispatch, bus/SPI setup and DDR initialization.
It tests the real ROM console on an already initialized board. It does not
select UART boot mode, reset the board, or validate cold ROM UART entry.

The wrapper checks SVC mode, MMU/caches/exception-state compatibility and
the first four instruction words at 0918 against the captured ROM before
changing the stack. It saves the caller on SRAM stack 00204000 and masks
interrupts before entering the console. Return command `g 200100` reaches
the fixed SRAM trampoline, restoring the original caller state and returning
rc=0 to U-Boot. The linker fixes this trampoline at 00200100. A mismatching
ROM or incompatible entry state returns ffffffff without entering the loop.

Only commands `r 0 10` and `g 200100` are in scope for the initial test.
The ROM parser expects bare hexadecimal, without an 0x prefix. `r 0 10`
displays the reset-vector words; it does not execute them. Do not issue its
boot, write or receive commands in this test. If input/output stops working,
stop hardware testing and reset at the desk; a return cannot be guaranteed
before the ROM loop is hardware-tested.

Artifact v2801sg-rom-console-f91a8de0.bin: 284 bytes (11c hex), CRC32
21584b93, SHA256
f91a8de0db88f2afc3dd1bc952b9a640e31f5c1b4e92e08415f8dd795a64488c.
Cross-assembly/link passed. An instruction model checked fingerprint
rejection and caller restoration through the trampoline after simulated
ROM register/stack clobbering; it does not model UART hardware behavior.
Physical execution passed as recorded below.

Back up SRAM 00200000..00203fff before the wrapper, restore it after returning
and compare CRCs. The wrapper neither changes VBAR nor writes CPU-control
registers. The read/display test does use the ROM's UART transmit/receive
register accesses. Boot mode remains the observed NOR mode 2 throughout.

## User hardware result

The user loaded the wrapper with matching CRC 21584b93 and entered ROM#.
Command `r 0 10` displayed 64 bytes matching the captured reset/startup code
at zero. Command `g 200100` reached the SRAM return trampoline and returned
to the calling monitor with rc=0. SRAM restoration CRC 89242f8e matched the
backup from this run and the subsequent Ethernet ping succeeded.

This verifies live ROM console input/output, its memory-display command,
its jump command and the wrapper return path. It does not verify the upload
receiver, physical boot-mode selection or cold UART boot. No reset, DDR
initialization or flash write was performed.

## Automated ROM upload verification

After the user released /dev/ttyUSB1, the real ROM console receiver was
tested at 115200 baud using `x 201000`. The sender used XMODEM-CRC with
128-byte SOH packets, block numbers/complements and big-endian CRC16/XMODEM.
No flash or reset operation was performed.

Two runs passed:

- One 128-byte block containing a 28-byte ARM payload with 1a padding.
- Twenty-four successive blocks containing 3072 bytes, comparable to the
  current first stage's size. The extra bytes were a deterministic pattern.

In each run a deliberately invalid CRC received NAK (15), followed by an
accepted corrected packet (ACK 06). EOT (04) received ACK and the console
reported OK. A ROM memory dump matched the complete transmitted buffer,
including all 3072 bytes in the multi-block run. The payload was executed
with `g 201000`, wrote 55504c31 to SRAM 00201200 and returned to ROM#.
The marker was read back as bytes 31 4c 50 55. Payload SHA256:
da4238e6e886a96835806f2dafec6e117c81fe0053a31c886e6577af2b3c7e6e.

Both runs then used `g 200100` to return to U-Boot with rc=0, restored SRAM
to the saved CRC 89242f8e and passed Ethernet ping. UART was closed/released
afterward; the board was left at the ZX279125 prompt. Raw logs, structured
results, sender scripts and the harmless payload are retained locally under
`tmp/v2801sg-rom-upload/` (ignored build artifacts).

The XMODEM-CRC receiver, multi-block SRAM loading and execution are now
hardware-verified. Reset-state entry into UART boot mode remains unverified;
these warm console tests do not establish that entry or cold DDR operation.
