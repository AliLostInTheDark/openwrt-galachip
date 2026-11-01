# V2802RH U-Boot recovery tests

This is the record of the chainload experiments. The current install keeps the
stock U-Boot and doesn't need any of this; see [README.md](../../README.md).

2026-10-26: host checks passed; the new builds still need the board tests
below. Nothing was flashed by the agent. Keep the existing stock bootloader
and its saved boot command until the flash tests have been approved and run.

## Changes and evidence

U-Boot branch `vsol-v2802rh-2026.10`, build `4447ad1c`:

- `ecbd3222`: bounded HTTP length/MIME parsing, LMB-backed upload RAM and
  stale-metadata cleanup. Retransmitted request bytes cannot overwrite an
  upload already moved to the load address.
- `9b70ec32`: reject short LZMA headers and pass the correct compressed
  span to the decoder. The original short-input overread was reproduced
  under ASan before fixing it.
- `2807ac3f`: `v2802rh_check` validates image size, CRCs, architecture,
  type, load/entry, raw squashfs bounds and the kernel's partition map
  before the recovery scripts can erase. CRC32 is available on the console.
  Failed writes retain failure status instead of ending with a successful
  LED command.
- `4447ad1c`: poll LAN2's PHY every 250 ms, including while HTTP waits,
  and restore its forced switch link when a cable is connected later.

OpenWrt branch `vsol-v2802rh`, source `03ab16b4ac`, image
`r37655+158-6db25ee835`: adds a `vsol,v2802rh-chainload-v1` compatibility
declaration and requires it for future V2802RH sysupgrades. A small image
with the old kernel partition map can otherwise reclaim U-Boot's space.
The current installed +155 firmware does not have this new upgrade guard.

Host results:

- Real firmware/U-Boot multipart uploads remain byte-identical, including
  binary data, split requests and retransmissions. Malformed headers,
  lengths, closing boundaries, empty/unknown fields and invalid RAM ranges
  fail. Also ran 2000 deterministic mutated requests under ASan/UBSan.
- The actual U-Boot LZMA decoder passes real images and rejects short and
  truncated inputs under ASan/UBSan.
- The board checker rejects wrong image slots/types/addresses, bad CRCs,
  truncated payloads/filesystems, sysupgrade files in the raw-firmware path
  and the saved pre-layout firmware. The actual decoder and libfdt were
  used in the native checker tests.
- Eighteen cases execute the actual environment script text with mocked
  flash/LED/network commands. Invalid images and transfer failures never
  reach erase. Probe/erase/write/compare failures stop the chain, and
  recovery only resets after success. This does not test SPI hardware or
  replace a test of the commands under U-Boot's shell.
- OpenWrt's checker accepts declared-layout metadata, rejects old/raw
  images, repeats cleanly and preserves the other-board path.
- Both builds passed. U-Boot's defconfig round-trip, CRC/decompression,
  slot size and RLX scan pass (only the dead secondary-CPU `wait`).

Local harnesses and logs, intentionally uncommitted:
`tmp/v2802rh/uboot-audit-20261026/`. Existing OpenWrt modpost warnings
about the disabled full-SerDes-reinit path remain; no GPON code was changed.

## Blink hang follow-up

Hardware results for `4447ad1c`: two pings, flash read/compare, firmware
TFTP, validation and an HTTP RAM upload with the power LED steady all
passed. The uploaded 3,293,188 bytes matched the TFTP reference and CRC32
`6aab80ca`. With software blinking enabled, HTTP stalled at the start of
GMAC shutdown. A separate `led green:power blink 250` followed by `sleep 3`
also hung after one blink. This reproduces the failure without Ethernet;
the exact failing operation remains unknown.

Temporary diagnostic commit `4dc8c0c5e259` traces the first four scheduler
passes and LED callbacks and enables the cyclic console command. This is
instrumentation, not a confirmed fix; remove it after diagnosis. Its
separately staged image `v2802rh-u-boot-blink-debug.img` is 103,594 bytes,
CRC32 `082bf77d`, SHA-256
`8630bc0ebac049c093f9fbe4d395c3ed3f48e3a75c9575c9cc37366c2982071d`.
The ordinary staged images in the table below remain unchanged.

Chainload this diagnostic from stock U-Boot, interrupt its countdown,
and capture the trace from `led green:power blink 250` then `sleep 3`.
Do not continue installation while this hang remains unresolved.

## Staged files

| TFTP filename | Bytes (hex) | Whole-file CRC32 | SHA-256 |
|---|---:|---|---|
| v2802rh-u-boot.bin | 282552 (44fb8) | 5ab8cdd4 | 03a3fd916a39bcf96eee92f846875845bb6fe71fed7e511ddab1c183f543ee90 |
| v2802rh-u-boot.img | 102927 (1920f) | a0686a48 | 8a85cabda08966d49c7a3d9499a7b066c652e27d0671ebfb70afb7ac4b584a7f |
| v2802rh-kernel.bin | 1743209 (1a9969) | ee841e5d | c43a0368b9f2bfb7ab72096cbab440c3c96238c9dec3e1f7d5439d0daf234e80 |
| v2802rh-rootfs.bin | 1548080 (179f30) | 17d8de41 | 220ce11ecd9b77e84975c2b1efb2c8b51d7da99fc6d190bee415d1ed734d6010 |
| v2802rh-firmware.bin | 3293188 (324004) | 6aab80ca | f2c450feb78ec0b3d19d62407782e55ccb1a1fdc465df83c21cc26572677d8fb |
| v2802rh-sysupgrade.bin | 3293488 (324130) | c9bae1d6 | 382b5ff04a11cd1858ca26d03320539605349b6dce8e7d0b426ecb2dcfb0b852 |

Copies already performed, from the main OpenWrt checkout:

```bash
cp /home/ali/Desktop/V2802RH-u-boot/build/u-boot.bin /srv/tftp/v2802rh-u-boot.bin
cp /home/ali/Desktop/V2802RH-u-boot/build/u-boot-lzma.img /srv/tftp/v2802rh-u-boot.img
cp bin/targets/realtek-luna/taroko/openwrt-realtek-luna-taroko-vsol_v2802rh-squashfs-kernel.bin /srv/tftp/v2802rh-kernel.bin
cp bin/targets/realtek-luna/taroko/openwrt-realtek-luna-taroko-vsol_v2802rh-squashfs-rootfs.bin /srv/tftp/v2802rh-rootfs.bin
cp bin/targets/realtek-luna/taroko/openwrt-realtek-luna-taroko-vsol_v2802rh-squashfs-firmware.bin /srv/tftp/v2802rh-firmware.bin
cp bin/targets/realtek-luna/taroko/openwrt-realtek-luna-taroko-vsol_v2802rh-squashfs-sysupgrade.bin /srv/tftp/v2802rh-sysupgrade.bin
```

## RAM tests: do these first

**For uploads use `httpd` directly. A successful upload through `run recovery`
invokes flash writes.** None of the network/upload/checker commands below
erase or program NOR. `setenv` here changes RAM only; do not use `saveenv`.

### 1. Chainload and check flash reads

Power-cycle, interrupt stock U-Boot, then:

```text
tftp 81000000 v2802rh-u-boot.img
bootm 81000000
```

Expect `U-Boot 2026.10-g4447ad1c418f`; interrupt its countdown. If the
compressed chainload fails, capture the output and try the raw path from
stock after another power-cycle:

```text
tftp 80200000 v2802rh-u-boot.bin
go 80200000
```

In the new U-Boot:

```text
sf probe
sf read 81800000 27000 40
cmp.b 81800000 b4027000 40
```

Expect all 64 bytes to match. Leave the reset button released.

### 2. LAN1 TFTP and reference bytes

Connect the PC through LAN1. Server IP is 192.168.1.100; ONU is 192.168.1.1.

```text
ping 192.168.1.100
ping 192.168.1.100
tftpboot 81000000 v2802rh-firmware.bin
crc32 81000000 ${filesize}
v2802rh_check firmware 81000000 ${filesize}
setenv refsize ${filesize}
cp.b 81000000 81800000 ${refsize}
```

Expect filesize `324004`, CRC32 `6aab80ca` and a valid firmware result.
The checker decompresses into scratch RAM at 0x80800000–0x81000000;
the upload and reference buffers are outside that area.

### 3. HTTP firmware upload, comparison and LED

On the new U-Boot:

```text
led green:power blink 250
httpd 81000000
```

On the PC:

```bash
curl --noproxy '*' -H 'Expect:' --fail-with-body -F 'firmware=@/srv/tftp/v2802rh-firmware.bin' http://192.168.1.1/upload
```

The upload returns U-Boot to its prompt. Check that the power LED blinked,
then run:

```text
led green:power on
printenv filesize refsize httpd_upload
crc32 81000000 ${filesize}
cmp.b 81000000 81800000 ${refsize}
v2802rh_check firmware 81000000 ${filesize}
```

Both sizes must be `324004`, field `firmware`, CRC32 `6aab80ca`, all
3293188 bytes must compare equal, and the checker must accept the image.
After the basic upload passes, repeat with curl's `--limit-rate 256k` and
a brief cable unplug/replug. A completed transfer must still compare
exactly. If the connection fails, Ctrl-C the server and confirm a fresh
upload succeeds; do not use a partial upload.

### 4. Malformed request and invalid image

Start `httpd 81000000` again. On the PC:

```bash
curl --noproxy '*' -i -H 'Content-Type: multipart/form-data; boundary=a' --data-binary x http://192.168.1.1/upload
```

Expect HTTP 400. The server should remain responsive; Ctrl-C at U-Boot
exits. Start `httpd 81000000` once more, then on the PC:

```bash
printf 'not firmware\n' > /tmp/v2802rh-invalid.bin
curl --noproxy '*' -H 'Expect:' --fail-with-body -F 'firmware=@/tmp/v2802rh-invalid.bin' http://192.168.1.1/upload
```

The receive operation itself succeeds. At the returned U-Boot prompt:

```text
v2802rh_check firmware 81000000 ${filesize}
```

Expect rejection, with no reset or flash operation. Keep this invalid RAM
buffer for the next test.

### 5. Invalid-image fallback without corrupting flash

With reset released, temporarily point boot checking at the invalid RAM
buffer from step 4:

```text
setenv fw_flash 0x81000000
boot
```

Expect an invalid-image result and automatic web recovery. View the page
only; **do not upload here**. Ctrl-C exits. Restore the normal boot source:

```text
setenv fw_flash 0xb4027000
```

### 6. HTTP U-Boot upload

First establish a TFTP reference:

```text
tftpboot 81000000 v2802rh-u-boot.img
setenv refsize ${filesize}
cp.b 81000000 81800000 ${refsize}
httpd 81000000
```

On the PC:

```bash
curl --noproxy '*' -H 'Expect:' --fail-with-body -F 'uboot=@/srv/tftp/v2802rh-u-boot.img' http://192.168.1.1/upload
```

At the returned U-Boot prompt:

```text
printenv filesize refsize httpd_upload
crc32 81000000 ${filesize}
cmp.b 81000000 81800000 ${refsize}
v2802rh_check uboot 81000000 ${filesize}
```

Expect both sizes `1920f`, field `uboot`, CRC32 `a0686a48`, all 102927
bytes matching and a valid U-Boot result.

### 7. LAN2, cable changes and fast LED blink

```text
led green:power blink 100
httpd 81000000
```

View the page through LAN1, then move the cable to LAN2 while HTTP remains
active. Wait for physical link and view the page again. This checks that a
cable connected after command startup becomes usable. Confirm the faster
LED blink. Ctrl-C exits; then:

```text
led green:power on
ping 192.168.1.100
ping 192.168.1.100
tftpboot 81000000 v2802rh-firmware.bin
crc32 81000000 ${filesize}
v2802rh_check firmware 81000000 ${filesize}
setenv refsize ${filesize}
cp.b 81000000 81800000 ${refsize}
```

Expect CRC32 `6aab80ca` and a valid firmware result. Capture the transfer
rate and any timeouts. Repeat the HTTP upload and comparison from step 3
on LAN2; the commands above have recreated its firmware reference.

### 8. Reset selection and boot the installed firmware

Run `button reset` released/held/released: expect off/on/off. Hold reset
and enter `boot`: expect recovery. View only, release reset, then Ctrl-C.
With `fw_flash` restored and reset released:

```text
boot
```

Expect the currently installed OpenWrt **+155**, its reserved partition
map and an interactive console. The newly staged +158 image has not been
installed by these tests. Normal Linux startup may write its existing
overlay, as it does on a regular boot.

## Flash tests: pending separate approvals

After the RAM results pass:

1. Install the companion +158 OpenWrt image while retaining settings, then
   use `sysupgrade -T` to confirm that a saved +155 image is rejected and
   the current declared-layout image is accepted. No force upgrade is needed.
2. Approve erasing/programming only the reserved U-Boot area, compare all
   written bytes against RAM, then boot from that flash address manually.
3. Approve a test save of the new U-Boot environment at 0x3df000, check it
   survives a power-cycle/chainload and approve any cleanup save separately.
4. Only then approve updating the stock saved boot command, followed by
   cold boot, reset-button recovery and stock-prompt rollback checks.
5. Interrupted-write/power-loss behavior has not been demonstrated by host
   fault injection. Plan that physical test separately with a verified
   recovery route; do not cut power during a write as part of the RAM tests.

Rollback tags in both repositories:
`codex/v2802rh-before-recovery-audit-20261026`. Prior U-Boot 67fe7286 and
OpenWrt +155 images are saved under
`tmp/v2802rh/uboot-chainload/67fe7286/` and `r37655-155/`.
