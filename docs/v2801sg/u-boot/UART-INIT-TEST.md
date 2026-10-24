# RAM UART initialization test

This separate `vsol_v2801sg-uart-test` package variant enables the seven-write
stock UART initialization in the serial driver's probe. It retains the RAM
monitor's disabled autoboot, empty boot command, environment in RAM and no
saveenv. It does not invoke the DDR initializer. Early debug output still
inherits the existing UART before the driver probes; this tests the later
driver initialization against running hardware, not reset-state UART clocks.

Build passed with:

```sh
make package/boot/uboot-zte/compile CONFIG_PACKAGE_u-boot-vsol_v2801sg-uart-test=y -j4 V=s
```

Artifact: `bin/targets/zte/zx279125/v2801sg-uboot-uart-85dbdf9f.bin`.
232544 bytes (`38c60` hex); CRC32 `aae282a5`; SHA256
`85dbdf9f7b8fe6f0cbf9b0b30010bcaba9ec9422865186eb32f76f7f797f76f4`.
Staged in `/srv/tftp/` under the same name. Build log:
`/tmp/v2801sg-uart-test-build.log`.

The linked probe at 40417ab4 contains the seven expected writes in order:
UART offsets 08=0, 34=0, 24=13, 28=36, 30=70, 40=0, 34=301 (hex except
the decimal divisors 13/36). Config checks confirm UART init enabled,
ENV_IS_NOWHERE, no ENV_IS_IN_SPI_FLASH/saveenv, BOOTDELAY=-1 and empty BOOTCOMMAND.

From the working `ZX279125 #` RAM monitor, with PC 192.168.1.100:

```text
tftp 0x40400000 v2801sg-uboot-uart-85dbdf9f.bin
crc32 0x40400000 0x38c60
```

Require 232544 transferred bytes and CRC `aae282a5`, then:

```text
go 0x40400000
```

Expect `RAM-UART-init-test` in the banner and a working prompt. Then:

```text
version
printenv bootdelay bootcmd
md.l 0x94405024 0x2
md.l 0x94405030 0x2
setenv ethaddr 70:b6:4f:01:a6:7a
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.100
ping 192.168.1.100
```

Expected divider words: `0000000d 00000024`; line/control: `00000070
00000301`. Require a responsive console and ping. This test reprograms UART
control, divisors and FIFO/line state; some characters queued before the
probe may be lost. If the console fails to recover, stop and power-cycle at
the desk.

## Hardware result

The user completed this test successfully. TFTP transferred 232544 bytes and
the loaded image CRC was `aae282a5`. The new monitor printed the
`RAM-UART-init-test` banner and accepted all console commands. Its environment
reported bootdelay -1 and no bootcmd. Divisors read back as `0d/24` and
line/control as `70/301`. Ping to 192.168.1.100 succeeded after setting the
factory MAC and IP addresses. No console failure or flash operation occurred.

This validates the driver's stock-derived UART initialization during a warm
RAM chainload. Initial clock/reset state, early pre-driver UART output and
cold DDR startup remain separate requirements.
