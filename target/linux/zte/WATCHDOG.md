# ZX279125 watchdog — 2026-10-24

The system watchdog is the Cortex-A9 private (TWD) watchdog at physical
`0x00800620`, driven by the new `arm_twd_wdt` driver through the mainline
`arm,cortex-a9-twd-wdt` binding. All tests below were RAM boots; nothing was
written to flash.

## Evidence from the vendor kernel

Source: the original vendor uImage at flash `0x40140` in
`v2801sg-full-flash-8M.bin` (LZMA, 3480032 bytes decompressed, linked at
`0xc0008000`). Every string below has exactly one literal reference.

- `zxwdt` init (`c0325f14`): stores the register pointer `0xf0100620`, takes
  the rate of `clk_get(NULL, "cpu")`, registers `{10, feed, enable, disable}`
  with the vendor's Bsp watchdog layer and prints
  `zxwdt: heartbeat %d msec, clock %d` with heartbeat 2.
- Static I/O map (`c033a198`): virtual `0xf0100000` → physical `0x00800000`,
  which is PERIPHBASE: the GIC distributor and CPU interface in our DT sit at
  `0x801000` / `0x800100`.
- Ops, with `R` = `0x00800620`:

| Op | Address | Sequence |
| --- | --- | --- |
| enable | `c0013a90` | IRQs off; `R+14 = 12345678, 87654321`; `R+8 = 0`; `R+c = 1`; `R+10 = 1`; `R+0 = rate & ~bit31`; `R+8 = 9` |
| disable | `c0013b0c` | `R+14` magic pair; `R+8 = 0`; `R+c = 1`; `R+10 = 1` |
| feed | `c0013c30` | when enabled, repeat the enable sequence |

This is the standard A9 layout: load, counter, control (bit 0 enable, bit 3
watchdog mode, bits 15:8 prescaler), interrupt status, reset status and the
disable register whose two-word sequence is the only way out of watchdog mode.

## Clock

Measured in timer mode (control `1`, no watchdog mode): the counter fell
`0x23d0f24f` in four U-Boot seconds. U-Boot's `sleep` runs about 2× fast
(`sleep 11` minus `sleep 1` took 5.02 s of host time), so the real rate is
about 300 MHz, CPU/2. The DT describes it as `periphclk`, a fixed factor of
`ZX279125_CLK_CPU` (600 MHz). The vendor loaded the CPU rate, which gives its
2 s heartbeat.

The fast U-Boot timer is a separate bug in the monitor's timer driver; it
also halves U-Boot's network timeouts and `bootdelay`.

## Driver

`files/drivers/watchdog/arm_twd_wdt.c`, enabled by patch 910 and
`CONFIG_ARM_TWD_WATCHDOG=y`:

- Prescaler 255: 1171875 Hz, timeouts 1..3665 s, default 30 s.
- Start: disable sequence, clear interrupt status, load, control `FF09`.
- Ping: write load; in watchdog mode that reloads the counter.
- Stop: disable sequence and control `0`.
- Stopped on reboot, because the stock loader never feeds it.
- The registers are banked per CPU; the kernel is uniprocessor.

## Hardware results

Kernel `e8b1129a…8f29`, DTB `38551139…2d78`, booted with `bootz` from the
chainload monitor `7ef408d9`:

1. Probe: `arm-twd-wdt 800620.watchdog: 1171875 Hz, timeout 30 s`; procd
   opened `/dev/watchdog` (running, timeout 30, frequency 5). Control read
   `FF09`, load `0218711A` (30 s).
2. Feeding: 12 counter samples over 48 s never fell below about 25 s left,
   and the board stayed up past the timeout.
3. Clean stop: with `magicclose` then `stop`, control read `0` and the board
   stayed up for 40 s more.
4. Real reset: re-armed (procd raised the timeout to 10 s, load `00B2D05E`),
   then `stop` without magic close. The kernel logged `watchdog did not
   stop!` and the board restarted about 10 s later into stock U-Boot, whose
   autoboot was stopped.
5. After the reset all watchdog registers read zero, including reset status,
   so the watchdog reset is a full chip reset and Linux cannot report it as
   the boot reason. The packet engine was at its reset fingerprint, and no
   further reset occurred during 25 s at the stock prompt.
6. With the local cursor table left stale at `0x990`, the chainloaded monitor
   adopted stock's `0xa2`, passed two pings and consumed the stock table.

UART log: `tmp/v2801sg-wdt/uart.log`.
