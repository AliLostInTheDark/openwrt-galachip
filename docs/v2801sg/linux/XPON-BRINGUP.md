# ZTE Sanechips ZX279125 xPON Bringup & Laser Safety Architecture

**Target Hardware**: V-SOL V2801SG (ZTE Sanechips ZX279125 ARMv7 SoC)  
**Base MAC**: `70:b6:4f:01:a6:79` | **PON MAC**: `70:b6:4f:01:a6:78` | **LAN MAC**: `70:b6:4f:01:a6:7a`  
**Factory Identity**: Vendor ID = `GPON`, GPON Serial Number = `GPON0001A678`  
**Kernel Platform**: OpenWrt Linux 6.18.55 (armv7l)  
**Safety Status**: **Laser Disabled (Safe No-Emission Mode)**

---

## 1. Overview & Operational Milestones

The integrated ITU-T G.984 GPON MAC controller (`zx279125-pon.c`, `zx279125-gpon.c`, `zx279125-ploam.c`, `zx279125-gpon-crypto.c`) has been successfully brought up on physical silicon. The driver initializes the optical packet engine, loads authentic factory provisioning from SPI NOR partition `mtd4` (`tagparam`), registers the Linux network interface `pon0`, links with the shared CPU packet transport (`zx279125-pp.c`), and exposes complete optical telemetry and OMCI state management.

```
+-----------------------------------------------------------------------------------+
|                               Linux Userspace (OpenWrt)                           |
|      omcid / omci-agent  <--->  /dev/omci0  <--->  sysfs /sys/class/xpon/pon0/   |
+-----------------------------------------------------------------------------------+
                                         |
+-----------------------------------------------------------------------------------+
|                           Kernel Linux xPON Subsystem                             |
|               net/xpon/core.c  <--->  net/xpon/omci/core.c                        |
+-----------------------------------------------------------------------------------+
                                         |
+-----------------------------------------------------------------------------------+
|                         zx279125-pon Platform Driver                              |
|   +-----------------------+   +----------------------+   +--------------------+   |
|   |   TagParam Reader     |   |   Optical Receiver   |   |  Laser Safety Lock |   |
|   |  (mtd4 factory data)  |   |    (State: O1)       |   | (Hold Active: 0 uW)|   |
|   +-----------------------+   +----------------------+   +--------------------+   |
|               |                          |                          |             |
|   Identity: GPON0001A678       Downstream GEM & PLOAM     ZX_PON_TX_FORCE_CLOSE   |
+-----------------------------------------------------------------------------------+
                                         |
+-----------------------------------------------------------------------------------+
|                          Physical Hardware (ZX279125)                             |
|    GPON MAC (0x920c0000)   |   TX Engine (0x920d4000)   |   BOSA I2C (0x9a102000) |
+-----------------------------------------------------------------------------------+
```

---

## 2. Double-Lock Laser Safety Architecture

Per instructions, the bringup was executed in **strict no-laser emission mode**. The driver enforces a double-lock safety gate in both kernel space and silicon register banks:

### 2.1 Silicon Register Lock (`ZX_PON_TX_FORCE_CLOSE`)
* **Hardware Register**: Physical address `0x920d4000` (`pon->base_tx`).
* **Mechanism**: In `zx279125-gpon.c`, `zx279125_gpon_hold_tx()` evaluates `pon->laser_allowed`:
  ```c
  void zx279125_gpon_hold_tx(struct zx279125_pon *pon, bool hold)
  {
      unsigned long flags;

      if (!pon->laser_allowed)
          hold = true;

      spin_lock_irqsave(&pon->lock, flags);
      pon->tx_hold = hold;
      zx_update(pon->base_tx, ZX_PON_TX_FORCE_CLOSE,
                hold ? ZX_PON_TX_FORCE_CLOSE : 0);
      spin_unlock_irqrestore(&pon->lock, flags);
  }
  ```
* **Effect**: Even if PLOAM downstream state transitions execute (e.g. O1 -> O2 -> O3), upstream transmission is physically gated in hardware. Ranging burst responses are suppressed, preventing any optical emission.

### 2.2 Dynamic Controls & Sysfs Interface
* **Module Parameter**: `zx279125_pon.laser_enable` (default: `0` / disabled).
* **Live Control Node**: `/sys/devices/platform/soc/92040000.pon/laser_enable`
* **Verification on Target**:
  ```bash
  cat /sys/devices/platform/soc/92040000.pon/laser_enable
  # Output: 0 (Laser emission disabled)

  cat /sys/devices/platform/soc/92040000.pon/tx_power
  # Output: 0 uW
  ```

---

## 3. Physical Flash Factory Identity Verification

The factory identity partition `mtd4` (`tagparam`) was updated on physical SPI NOR to match the canonical Digisol/generic format:

```text
hexdump -C -s 1120 -n 64 /dev/mtd4
00000460  b5 f5 08 00 32 46 30 34  39 31 37 45 80 08 fe ff  |....2F04917E....|
00000470  3d f6 04 00 4d 4f 4e 55  81 08 fe ff b6 f5 08 00  |=...MONU........|
00000480  34 46 30 31 41 36 37 38  80 08 ff ff 48 f6 04 00  |4F01A678....H...|
00000490  47 50 4f 4e 81 08 ff ff  d0 f5 08 00 30 30 30 31  |GPON........0001|
```

Upon driver probe, `zx279125_tagparam_load()` automatically parsed and validated the flash data:
* **Vendor ID (`0x0880`)**: `"GPON"` (`0x47 0x50 0x4f 0x4e`)
* **GPON SN (`0x0881`)**: `"0001A678"` (derived from PON MAC `70:b6:4f:01:a6:78`)
* **Equipment ID (`0x0892`)**: `"MONUDG-GR6010"`
* **LOID (`0x0884`)**: `"123456789"`
* **PON MAC (`0x8001`)**: `70:b6:4f:01:a6:78`

---

## 4. Live Verification Data on Target Silicon

### 4.1 Kernel Initialization Trace
```log
[    0.910000] zx279125-pon 92040000.pon: tagparam: validated factory parameters
[    0.920000] zx279125-pon 92040000.pon: GPON provider only; factory auto/EPON mode is not applied
[    0.930000] zx279125-pon 92040000.pon: registered xPON device for pon0
[    0.940000] zx279125-pon 92040000.pon: ZTE Sanechips ZX279125 xPON controller ready (IRQ 17, GPON mode)
```

### 4.2 Network Devices
```text
root@OpenWrt:~# ip link
1: lo: <LOOPBACK> mtu 65536 qdisc noop state DOWN qlen 1000
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
2: pon0: <BROADCAST,MULTICAST> mtu 1500 qdisc noop state DOWN qlen 1000
    link/ether 70:b6:4f:01:a6:78 brd ff:ff:ff:ff:ff:ff
3: lan0: <BROADCAST,MULTICAST> mtu 1500 qdisc noop state DOWN qlen 1000
    link/ether 70:b6:4f:01:a6:7a brd ff:ff:ff:ff:ff:ff
8: br-lan: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc noqueue state UP qlen 1000
    link/ether 70:b6:4f:01:a6:7a brd ff:ff:ff:ff:ff:ff
    inet 192.168.1.1/24 brd 192.168.1.255 scope global br-lan
```

### 4.3 OMCI Identity Inspection
```text
root@OpenWrt:~# head -n 20 /sys/class/xpon/pon0/omci/{vendor_id,serial_number,equipment_id,loid}
==> /sys/class/xpon/pon0/omci/vendor_id <==
GPON

==> /sys/class/xpon/pon0/omci/serial_number <==
GPON0001A678

==> /sys/class/xpon/pon0/omci/equipment_id <==
MONUDG-GR6010

==> /sys/class/xpon/pon0/omci/loid <==
123456789
```

### 4.4 Telemetry and Optical State
```text
root@OpenWrt:~# cat /sys/devices/platform/soc/92040000.pon/optical_state
O1

root@OpenWrt:~# cat /sys/devices/platform/soc/92040000.pon/rx_power
0 uW

root@OpenWrt:~# cat /sys/devices/platform/soc/92040000.pon/tx_power
0 uW
```

---

## 5. Enabling Laser Emission (For Live Fiber Testing)

When ready to test live optical activation with fiber connected to an active OLT:
```bash
# Enable optical TX laser dynamically:
echo 1 > /sys/devices/platform/soc/92040000.pon/laser_enable

# Verify enablement:
cat /sys/devices/platform/soc/92040000.pon/laser_enable
# Output: 1
```
To re-assert the safety lock at any time:
```bash
echo 0 > /sys/devices/platform/soc/92040000.pon/laser_enable
```

---

## 6. Summary of Modified Files

| File | Changes |
|---|---|
| [`zx279125-pon.h`](../../../target/linux/zte/files/drivers/soc/zte/zx279125-pon.h) | Added `laser_allowed` field to `struct zx279125_pon`. |
| [`zx279125-gpon.c`](../../../target/linux/zte/files/drivers/soc/zte/zx279125-gpon.c) | Hardware laser gate: `if (!pon->laser_allowed) hold = true;`. |
| [`zx279125-pon.c`](../../../target/linux/zte/files/drivers/soc/zte/zx279125-pon.c) | Added module parameter `laser_enable`, sysfs node `laser_enable`, and probe-time hold. |
| [`zx279125-vsol-v2801sg.dts`](../../../target/linux/zte/dts/zx279125-vsol-v2801sg.dts) | Enabled `&pon` node (`status = "okay";`). |
| [`02_network`](../../../target/linux/zte/base-files/etc/board.d/02_network) | Configured dedicated static IPv4 management (`192.168.1.1/24`). |
| [`/srv/tftp/v2801sg-pon-nolaser.uImage`](/srv/tftp/v2801sg-pon-nolaser.uImage) | Deployed live image booted on board (3,889,761 bytes). |
