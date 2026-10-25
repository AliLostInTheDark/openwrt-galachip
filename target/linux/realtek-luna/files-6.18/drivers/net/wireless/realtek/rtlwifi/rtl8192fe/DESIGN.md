# Design Document: `rtl8192fe` — Clean-Room mac80211 PCIe WiFi Driver for the Realtek RTL8192F

**Status:** Architecture / implementation plan
**Target:** OpenWrt kernel 6.18, big-endian MIPS SoC
**Device:** Realtek RTL8192F, PCI `0x10ec:0x818c`, 2T2R 2.4 GHz 802.11n
**License:** GPL-2.0-only (own SPDX, original copyright; no third-party headers)
**References:** the published RTL8192F datasheet facts (register addresses, field bit positions, init sequences, numeric constants) and the mainline GPL `rtlwifi`/`rtl8192ee` and `rtl8xxxu`/`8192f` drivers.

---

## 1. Architecture

`rtl8192fe` is an **rtlwifi PCI sub-driver**: it reuses the entire `rtlwifi.ko` + `rtl_pci.ko` core unchanged and contributes only the chip-specific personality. It adds **zero** new bus, DMA, or mac80211 plumbing. From the PCI core (`rtl_pci.ko`) it inherits PCI probe/BAR-map/device-lifetime (`rtl_pci_probe`/`_disconnect`/`_suspend`/`_resume`), the TX/RX DMA-ring engine and ISR, the IRQ/MSI request path, and the generic `rtl_intf_ops` (`rtl_pci_ops` — `adapter_start/stop/tx/flush/reset_trx_ring`, ASPM enable/disable) which it does **not** redefine. From `rtlwifi.ko` it inherits mac80211 hardware registration (`_rtl_init_mac80211`, `rtl_init_core`, `rtl_init_rfkill`, the `ieee80211_register_hw` call inside `rtl_pci_probe`), the async firmware-callback plumbing (`rtl_fw_cb`), the efuse read engine (`read_efuse`, `efuse_shadow_read`, `rtl_efuse_shadow_map_update`), power-save/IPS/LPS/ASPM (`ps.c`), the pwrseq command parser (`rtl_hal_pwrseqcmdparsing`), and the generic CAM/security, rate-control, regulatory, statistics, and debug subsystems. What `rtl8192fe` **writes new** is exactly three things: (a) a `const struct rtl_hal_cfg` (`.ops`, `.mod_params`, `.maps[]`, `.bar_id`, `.write_readback`), (b) a `const struct rtl_hal_ops` populated with the RTL8192F function pointers, and (c) the chip data — PHY/RADIO/MAC/AGC/PG arrays plus the pwrseq flows. This is the same shape every existing `rtl81xx`/`rtl82xx` sub-driver takes; the `pci_device_id` `{0x10ec, 0x818c}` binds the device to `rtl_pci_probe`, which then calls into our `cfg->ops`.

---

## 2. File Manifest

Tree: `drivers/net/wireless/realtek/rtlwifi/rtl8192fe/` (mirrors the `rtl8192ee` layout). Sizes below are order-of-magnitude estimates anchored to the corresponding `rtl8192ee` files.

| File | Responsibility | Est. size (LOC) | Complexity |
|---|---|---|---|
| `sw.c` | `rtl_hal_ops`, `rtl_hal_cfg`, `pci_device_id[]`, `pci_driver`, `rtl_mod_params`, `init/deinit_sw_vars`, FW request | ~380 | **Low** — ~90% structural; change device id, names, FW filename, IMR/RCR/ASPM constants |
| `reg.h` | All register `#define`s + IMR/RCR/efuse/CAM symbol names referenced by `maps[]` | ~2200 | **Med** — ~70% shared MAC reg block; audit 8192F deltas (TRXDMA_CTRL, REG_HIMR/E, pwr regs, type-3 block) |
| `def.h` | `version_8192f` enum, `rtl_desc_qsel`, `DESC_RATE*`, chip constants, ring depths | ~75 | **Low** — version magic + `RX_DESC_NUM`/`TX_DESC_NUM` |
| `sw.c`/`hw.c` split → `hw.c` | `hw_init`, `card_disable`, `read_eeprom_info`, `read_chip_version` (RF_2T2R), `set/get_hw_reg`, `set_network_type`, `set_qos`, beacon, security/`set_key`, GPIO radio on/off, suspend/resume | ~2600 | **Med-High** — ~60% reuse; substitute 8192F MAC-init values, efuse map offsets, LLT/queue-page alloc, init order |
| `phy.c` | `query/set_bb_reg`, RF-serial read/write, BB/RF config-with-table, `set_bw_mode`, `sw_chnl`, IQK, LCK, txpower-by-rate/PG | ~3200 | **High** — largest genuinely-8192F file; IQK/LCK sequences rewritten from 8192F facts (differ materially from 8192E) |
| `rf.c` | `phy_rf6052_config`, CCK/OFDM txpower set | ~130 | **Low** — value substitution |
| `dm.c` | `dm_watchdog`, DIG, dynamic txpower, RA-report, ratr, edca, thermal/TX-power tracking hook | ~1100 | **Med** — ~80% skeleton; substitute 8192F thresholds/IGI/swing tables |
| `trx.c` | `tx_fill_desc`, `tx_fill_cmddesc`, `rx_query_desc`, `set/get_desc`, `is_tx_desc_closed`, `rx_check_dma_ok`, `rx_desc_buff_remained_cnt`, `tx_polling`, `get_available_desc` | ~1050 | **High** — PCIe TX/RX descriptor bit-layout + BD ring entry; the field semantics are facts, the PCIe BD plumbing follows the 8192-series PCIe format |
| `fw.c` | `_rtl_fw_download` page push, `fill_h2c_cmd`, H2C/C2H box, rate-adaptive report, reset-FW | ~870 | **Med** — ~85% shared box mechanics; FW image `rtl8192fefw.bin` |
| `pwrseq.c` | `wlan_pwr_cfg` power-on / radio-off / card-disable / card-enable flows | ~410 | **Med** — structure reused, values 8192F-specific |
| `table.c`/`table.h` | `RTL8192FE_PHY_REG_ARRAY`, `_PHY_REG_ARRAY_PG`, `_RADIOA_ARRAY`, `_RADIOB_ARRAY`, `_MAC_ARRAY`, `_AGC_TAB_ARRAY` | ~860 | **Med** — 100% data, 0% logic, all 8192F values |
| `led.c`/`led.h` | `led_control`, sw-LED via GPIO reg | ~115 | **Low** — only LED GPIO reg/bit may change |
| `Makefile` | obj list | ~15 | trivial — `rtl8192fe-objs := dm fw hw led phy pwrseq rf sw table trx`, `obj-$(CONFIG_RTL8192FE)` |

**Rough total:** ~13–14k LOC. The dominant *new-logic* effort concentrates in **`phy.c` (IQK/LCK)**, **`table.c` (all 8192F data)**, **`trx.c` (PCIe descriptor layout)**, and **`hw.c` (init order + efuse map)**. Everything else is structural reuse with value substitution.

---

## 3. The `rtl_hal_ops` Checklist

All function pointers populated, grouped by porting effort. Tags: **[boilerplate]** = copy structure/rename, ~0 chip logic; **[value-sub]** = same algorithm, 8192F numbers; **[chip-logic]** = must be derived from 8192F facts with highest care.

**Software-vars / lifetime**
- `init_sw_vars` — **[value-sub]** — FW vzalloc + `request_firmware_nowait`; set FW name + IMR/RCR/ASPM constants
- `deinit_sw_vars` — **[boilerplate]**
- `get_btc_status` — **[boilerplate]** — returns `true`

**Hardware init / power**
- `hw_init` — **[chip-logic]** — init *order* preserved from the 8192-series; 8192F MAC/BB/RF values from `table.c` + pwrseq; calls IQK/LCK
- `hw_disable` (`card_disable`) — **[value-sub]**
- `hw_suspend` / `hw_resume` — **[chip-logic]** — 8192F pwrseq + LPS clock handling
- `set_rf_power_state` — **[value-sub]** — RF pwr regs
- `radio_onoff_checking` (`gpio_radio_on_off`) — **[value-sub]** — 8192F GPIO bit
- `read_eeprom_info` — **[value-sub]** — efuse parse; offsets from the 8192F efuse map (§5)
- *(no `read_chip_version` in `rtl_hal_ops`)* — read internally in `hw.c`; 8192F is 2T2R (`rf_paths=2`, `RF_2T2R`)

**Register accessors**
- `set_hw_reg` / `get_hw_reg` — **[value-sub]** — variable→reg map
- `get_bbreg` / `set_bbreg` / `get_rfreg` / `set_rfreg` — **[boilerplate]** — generic BB/RF-serial wrappers; only RF-serial reg addrs differ
- `set_desc` / `get_desc` — **[boilerplate]** — desc field accessors (mechanical once trx layout known)

**MAC config**
- `set_network_type`, `set_qos`, `set_bcn_reg`, `set_bcn_intv`, `set_channel_access`, `set_chk_bssid` — **[boilerplate]**
- `enable_hw_sec`, `set_key` — **[boilerplate]** — CAM via core
- `scan_operation_backup` — **[boilerplate]** — uses core `rtl_phy_scan_operation_backup`

**PHY / RF**
- `switch_channel` (`phy_sw_chnl`) — **[chip-logic]** — 8192F channel BB+RF write sequence + CCK-PSF revise
- `set_bw_mode` (`phy_set_bw_mode`) — **[chip-logic]** — 8192F 20/40 MHz BB+RF sequence
- `phy_iq_calibrate` — **[chip-logic]** — 8192F 3-run IQK (TX-LOK+IQK then RX-IQK, A then B)
- `phy_lc_calibrate` — **[chip-logic]** — 8192F LCK
- `dm_watchdog` — **[value-sub]** — DM thresholds; drives thermal/TX-power/LCK/IQK re-cal
- `update_rate_tbl` / `c2h_ra_report_handler` — **[value-sub]** — RA report format

**TRX / descriptors**
- `fill_tx_desc` — **[chip-logic]** — 8192F PCIe TX descriptor bit layout
- `fill_tx_cmddesc` — **[chip-logic]**
- `query_rx_desc` — **[chip-logic]** — 8192F PCIe RX descriptor + phystatus parse
- `is_tx_desc_closed` — **[chip-logic]** — own-bit / WP semantics of the 8192F PCIe BD
- `tx_polling` — **[boilerplate]**
- `rx_desc_buff_remained_cnt`, `rx_check_dma_ok`, `get_available_desc` — **[value-sub]** — ring-status reg reads

**Interrupt**
- `enable_interrupt` / `disable_interrupt` / `update_interrupt_mask` — **[boilerplate]** — write IMR
- `interrupt_recognized` — **[value-sub]** — read `REG_HISR`/`REG_HISRE`, mask with `irq_mask[]`

**Firmware**
- `fill_h2c_cmd` — **[value-sub]** — H2C element ids (8192F set)

**LED**
- `led_control` — **[boilerplate]** — GPIO toggle

### PCI-id + hal_cfg registration (`sw.c` tail)

- **`pci_device_id`:** `{RTL_PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x818c, rtl8192fe_hal_cfg)}` (`0x10ec` = `PCI_VENDOR_ID_REALTEK`), trailing `{}` sentinel + `MODULE_DEVICE_TABLE(pci, …)`. This single id is the load-bearing change vs. the 8192ee template (`0x818B`).
- **`rtl_hal_cfg`:** `.name = "rtl8192fe_pci"`, `.ops = &rtl8192fe_hal_ops`, `.mod_params = &rtl8192fe_mod_params`, `.bar_id = 2`, `.write_readback = true` (8192-series uses BAR2 — verify against the part), `.maps[…]` keys resolved from the new `reg.h`/`def.h`.
- **`pci_driver`:** `.probe = rtl_pci_probe`, `.remove = rtl_pci_disconnect`, `.driver.pm = &rtlwifi_pm_ops`, `module_pci_driver(...)`.
- **Module metadata:** `MODULE_DESCRIPTION("Realtek 8192FE 802.11n PCI wireless")`, `MODULE_FIRMWARE("rtlwifi/rtl8192fefw.bin")`, own SPDX/`MODULE_AUTHOR`.

---

## 4. Phased Implementation Plan (BE-MIPS bring-up)

Each phase is independently testable, ordered so a failure is localized to the newest layer.

### Phase 0 — PCIe endpoint enumeration (PREREQUISITE, currently blocked)

> **This is a host-controller task, not a driver task.** It must be solved before any later phase can be exercised.

- **Deliverable:** `0x10ec:0x818c` is visible in PCIe config space with valid (non-`0xeeeeeeee`) reads; BARs assignable.
- **Test:** `lspci -nn` / config-space dump shows the endpoint; BAR2 maps and a known register reads back a sane value.
- **Status / risk:** the link trains but the endpoint currently returns the `0xeeeeeeee` abort pattern — config reads do not complete. The endpoint loads its internal ROM only after PCIe link-up, so until config-space reads return valid data the `pci_device_id` can never match and `rtl_pci_probe` is never entered. **This is owned by the host root-complex / link-stability work and is tracked separately.** No amount of driver code substitutes for it.

### Phase 1 — Probe + power-on + firmware download

- **Deliverable:** driver binds, runs the power-on sequence, downloads firmware, observes the FW-ready bit.
- **Build blocks:** `sw.c` (cfg/ops/ids), `pwrseq.c` (power-on flow), `reg.h`/`def.h`, `hw.c` power-on + chip-version + minimal efuse read, `fw.c` (page download + checksum + boot poll).
- **Test:** `modprobe rtl8192fe`; driver probes without error; `REG_MCUFWDL` shows `FWDL_CHKSUM_RPT` then `WINTINI_RDY` set after the 8051 self-reset.
- **Key risks:** power-on poll bits (`REG_APS_FSMCO` bit17 power-ready, MAC-enable poll) must time out cleanly (timeout ≠ success); FW blob name (`rtl8192fefw.bin` is the PCIe-naming gap-fill — confirm the packaged blob); the FW-header detect `(signature & 0xFFF0) == 0x92E0` and 32-byte strip; **BE-MIPS:** the `__le16 ramcodesize`/`__le32 svnindex` header fields must be byte-swapped on access.

### Phase 2 — BB + RF init + calibration

- **Deliverable:** BB/AGC/RadioA/RadioB tables applied; LCK + IQK run; radio register reads sane.
- **Build blocks:** `table.c` (all five arrays), `phy.c` (table apply, BB/RF-serial accessors, `set_channel`/`set_bw_mode`, LCK, IQK), `rf.c`.
- **Test:** read back representative BB regs (e.g. `0xC50`, `0x800`) and RF reg `0x18` after channel set; IQK candidate selection returns a valid (non-failure-pattern) matrix; LCK bit15 self-clears within the poll budget; thermal meter (`RF_A 0x42 = 0x37cf8`) enabled.
- **Key risks:** IQK failure-signature detection (bits[25:16]==0x42 = fail; identity matrix on fail so traffic still flows); the LOK→TX-PA-LUT feedback loop (RF `0x33` writes) is 8192F-specific; the CCK-TX-PSF revise for ch13/ch14 must stay in sync with the PHY init table; **BE-MIPS:** RF-serial read assembles a 32-bit value from BB FSIR/FSPI registers — host-order math, no descriptor swap, but verify the BB MMIO accessor (`rtl_read_dword`) byte-orders correctly.

### Phase 3 — TRX rings + RX path

- **Deliverable:** TX/RX DMA rings allocated and programmed; the chip receives beacons; mac80211 scan shows APs.
- **Build blocks:** `trx.c` (`query_rx_desc`, RX phystatus parse, RX BD ring), `hw.c` DESA ring-base programming + IMR arming, `dm.c` watchdog skeleton.
- **Test:** `iw dev wlanX scan` returns nearby SSIDs; RX RSSI values are plausible (OFDM `rx_pwr = (gain & 0x3f)*2 − 110`; CCK lna/vga table + 16); `REG_HISR` shows `IMR_ROK`.
- **Key risks:** RX descriptor field map (24-byte status, `drv_info_size` in 8-byte units, phystatus block offset) and the DESA/RXBD_NUM register programming; RDU replenish vs ROK service split; **BE-MIPS:** every `__le32` RX descriptor word and the phystatus struct must be read through `le32_to_cpu`/typed `__le` accessors — see §6.

### Phase 4 — TX path + mac80211 connect

- **Deliverable:** TX descriptors + BD ring drive real frames; STA associates to an AP.
- **Build blocks:** `trx.c` (`fill_tx_desc`/`fill_tx_cmddesc`, BD segment fill, `is_tx_desc_closed`, TXBD_IDX), `fw.c` H2C (rate adaptive), `dm.c` rate/EDCA.
- **Test:** `wpa_supplicant` associates and passes DHCP + ping over a WPA2 link; per-queue `*DOK` interrupts fire; TX reclaim works.
- **Key risks:** TX descriptor word/bit layout (pkt_size/offset/QSEL/sec_type/rate/agg/seq/buffer-addr/TXBUFFERSIZE) and the BD own-bit/segment semantics (SEG_NUM=1 → 4 segments; seg0 = txdesc, seg1 = payload); QSEL/hw-queue→IDX-register mapping; **BE-MIPS:** TX descriptor and BD words are `__le32` — fill via `cpu_to_le32`; DMA addresses split low/high words.

### Phase 5 — AP mode (the goal)

- **Deliverable:** `hostapd` beacons; a client associates and passes traffic; bridged into the OpenWrt LAN.
- **Build blocks:** beacon-queue TX (`fill_tx_cmddesc` → BCNQ), `set_bcn_reg`/`set_bcn_intv`, beacon-DMA IMR bits, `RXFLTMAP` (PS-Poll via `REG_RXFLTMAP1=0x0400`), security CAM for client keys.
- **Test:** a phone/laptop sees and joins the SSID; bidirectional ping; multiple clients; PS-Poll honored.
- **Key risks:** beacon DMA timing (`IMR_BCNDOK0`/`IMR_BCNDMAINT0`, `REG_TBDOK`/`TBDER`); `RXFLTMAP1` control-frame filter must admit PS-Poll for AP power-save clients; CAM key install for multiple stations (`max_sec_cam_num = 64`, `max_macid_num = 128`).

---

## 5. Concrete Data for `table.c` / `reg.h`

The implementer fills these arrays/defines with the documented 8192F facts. All tables are `{addr, value}` fact-lists re-emitted as original arrays.

### `table.c` array sizes + anchors

**`RTL8192FE_PHY_REG_ARRAY` (BB PHY_REG)** — flat `{u32 addr, u32 value}`, terminator `{0xFFFF, 0xFFFFFFFF}`; pseudo-addr delays `0xFE`=50ms…`0xF9`=1µs. **~285 entries**, BB regs 0x800–0xF50. Anchors: `0x800=0x80006C00`, `0x804=0x00004001`, `0x808=0x0000FC00`, `0x810=0x20200322`, `0x90C=0x81121313`, `0x910=0x024C0000`, `0xA00=0x00D047C8`, `0xA04=0xC1FF0008`, `0xA20=0xE82C0001`, `0xA24=0x64B80C1C`, `0xA28=0x00158810`, `0xA2C=0x10BB8000`, `0xC00=0x00000080`, `0xC50=0x00E48020`, `0xC78=0x0FE07F1F`, `0xC80/0xC88=0x40000100`, `0xE00=0x25252525`, `0xE30=0x01007C00`, `0xE34=0x01004800`, `0xE60=0x02100000`, `0xF18=0x07D003E8`, `0xF50=0x00000000`. (Single flat list — no rev split for this board.)

**`RTL8192FE_AGC_TAB_ARRAY`** — **192 writes to reg `0xC78`** in three 64-entry banks (idx 0x00–0x3F, 0x40–0x7F, 0x80–0xBF), then **2 writes to `0xC50`** (`0x00E48024`, `0x00E48020`), then terminator. Value encoding: bits[31:14] gain, bits[13:8] LUT index, bits[7:0]=`0x1F` strobe. Bank heads: `0xC78=0x0FA0001F` (idx 0), `0xC78=0x0FA0401F` (idx 0x40), `0xC78=0x0FA0801F` (idx 0x80); bank tails `…3F1F` / `…7F1F` / `…BF1F`.

**`RTL8192FE_RADIOA_ARRAY`** — `{u8 rf_addr, u32 value}` (value masked 0xFFFFF), terminator `{0xFF, 0xFFFFFFFF}`; pseudo-addr `0xFFE`=50ms, `0xFE`=100µs; 1µs inter-write. **165 entries.** Key regs: `0x00=0x30000` (re-written `0x31DD5` at end), `0x18=0x0FC07` (→`0x08C07`), `0x1B=0x746CE`, `0xEF` bank-select family (`0x00800/0x00400/0x00200/0x00100/0x20000/0x80010/0x80000`), `0x33` LUT port, `0xDF` gain/CCA, `0x30/0x31/0x32` per-band power triplets, `0xB0=0xFFBCB`, `0xB1=0x33B8F`, `0xB2=0x33762`, `0xB4=0x141F0`, PA/LNA bias `0xC2..0xC6=0x02C01/0x0000B/0x81E2F/0x5C28F/0x000A0`, synth `0x51..0x5C` block, `0x6E=0x38319`, `0xF5=0x43180`.

**`RTL8192FE_RADIOB_ARRAY`** — **139 entries** (omits the path-A-only IPA/aux tail); path-B mirror of the above.

**`RTL8192FE_MAC_ARRAY`** — `{u16 addr, u8 value}`, terminator `{0xFFFF, 0xFF}`, simple write8 loop. **~139 entries.** Load-bearing values: `0x420=0x00, 0x422=0x78, 0x428=0x0A, 0x429=0x10, 0x430..0x437=00 00 00 01 04 05 07 08, 0x43C..0x43F=04 05 07 08, 0x440=0x5D, 0x441=0x01, 0x444=0x10, 0x445=0xF0, 0x446=0x0E, 0x447=0x1F, 0x480=0x20, 0x49C=0x30, 0x49D=0xF0, 0x49E=0x03, 0x49F=0x3E, 0x4C8=0xFF, 0x4C9=0x08, 0x4CA=0x3C, 0x4CB=0x3C, 0x4CC=0xFF, 0x4CD=0xFF, 0x4CE=0x01, 0x500=0x26, 0x501=0xA2, 0x502=0x2F, 0x550=0x10, 0x551=0x10, 0x559=0x02, 0x55C=0x50, 0x55D=0xFF, 0x605=0x30, 0x608=0x0E, 0x609=0x2A, 0x60C=0x18, 0x620..0x627=0xFF, 0x638=0x50, 0x63C..0x63F=0A 0A 0E 0E, 0x640=0x40, 0x642=0x40, 0x652=0xC8, 0x66E=0x05, 0x700=0x21, 0x701=0x43, 0x702=0x65, 0x703=0x87, 0x708..0x70B=21 43 65 87, 0x718=0x40, 0x7C0=0x38, 0x7C2=0x0F, 0x7C3=0xC0, 0x7C4=0x77, 0x024=0xC7, 0x073=0x04, 0x7EC..0x7EF=0xFF`, plus the **type-3-only quad** `0x2448/0x244A/0x244C/0x244E=0x06`.

**`RTL8192FE_PHY_REG_ARRAY_PG` (TX power by rate)** — 6-word rows `{band, rf_path, tx_num, addr, bitmask, data}`. **~28 rows.** Path-A targets BB `0x0E00,0x0E04,0x0E08,0x0E10,0x0E14,0x0E18,0x0E1C` + CCK `0x086C`; path-B targets `0x0830,0x0834,0x0838,0x083C,0x0848,0x084C,0x0868,0x086C`. Sample rows: `{0,0,0,0x0E00,0xffffffff,0x36384040}`, `{0,0,1,0x0E00,0xffffffff,0x34363838}`, `{0,0,0,0x086C,0xffffff00,0x36363600}`, `{0,1,0,0x0830,0xffffffff,0x36384040}`.

### `reg.h` anchor constants

**Power domain:** `REG_APS_FSMCO=0x0004` (MAC_ENABLE=BIT8, MAC_OFF=BIT9, SW_LPS=BIT10, HW_SUSPEND=BIT11, PCIE=BIT12, HW_POWERDOWN=BIT15, WLON_RESET=BIT16, power-ready=BIT17); `REG_SYS_FUNC=0x0002` (BBRSTB=BIT0, BB_GLB_RSTN=BIT1, CPU_ENABLE=BIT10); `REG_RF_CTRL=0x001F` (RF_ENABLE|RF_RSTB|RF_SDMRSTB=0x07); `REG_LDOA15_CTRL=0x0020`, `REG_LDO_SW_CTRL=0x007C` (BIT31), `REG_SYS_ISO_CTRL=0x0000` (ANALOG_IPS=BIT5), `REG_AFE_MISC=0x0010`, `REG_RSV_CTRL=0x001C`.

**CR / MAC:** `REG_CR=0x0100` (PCIe enable mask `0x063C` cold, +MAC_TX/RX_ENABLE bits6/7 at bus-quirk); `REG_TRXDMA_CTRL=0x010C` (**32-bit, 3-bit fields — 8192F shifts VOQ=4, VIQ=7, BEQ=10, BKQ=13, MGQ=16, HIQ=19; LOW=1/NORMAL=2/HIGH=3**); `REG_AUTO_LLT=0x0224` (BIT16 auto-LLT init); `REG_PBP=0x0104` (=0x22, 256-byte pages); `REG_RQPN=0x0200`, `REG_RQPN_NPQ=0x0214`; `REG_TRXFF_BNDY=0x0114` (+2 @0x0116=0x3F3F).

**Page counts (8192F):** `TX_TOTAL_PAGE_NUM=0xF7`, `HI/LO/NORM_PQ=0x08` each, `PUBQ=0xDE` (all-three-queue case; 2-EP case computes 0xE6). Boundaries: `REG_TXPKTBUF_BCNQ_BDNY(0x0424)=0xF8`, `MGQ_BDNY(0x0425)=0xF8`, `WMAC_LBK_BF_HD(0x045D)=0xF8`, `REG_TDECTRL+1(0x0209)=0xF8`.

**RX/TX engine:** `REG_RCR=0x0608` (init `0x7000E00E` = phys-match|mcast|bcast|mgmt|HTC|append-phystat/ICV/MIC; promisc adds BIT0/BIT11); `REG_RXFLTMAP0(0x06A0)=0xFFFF`, `RXFLTMAP1(0x06A2)=0x0400` (PS-Poll only), `RXFLTMAP2(0x06A4)=0xFFFF`; `REG_RX_DRVINFO_SZ(0x060F)=4`; EDCA `BE=0x005EA42B/BK=0x0000A44F/VI=0x005EA324/VO=0x002FA226`; `REG_ACKTO(0x0640)=0x40`; AMPDU cap `max_aggr_num=0x1F1F`, `ampdu_max_time=0x5E`.

**Interrupt:** `REG_HIMR=0x00B0`, `REG_HISR=0x00B4`, `REG_HIMRE=0x00B8`, `REG_HISRE=0x00BC`. Default arm: `irq_mask[0] = PSTIMEOUT|C2HCMD|HIGHDOK|MGNTDOK|BKDOK|BEDOK|VIDOK|VODOK|RDU|ROK`, `irq_mask[1] = RXFOVW`. (Set-0: ROK=b0, RDU=b1, VODOK=b2…HIGHDOK=b7, C2HCMD=b10, BCNDOK0=b16, BCNDMAINT0=b20, TBDOK=b25; set-1: RXFOVW=b8, TXFOVW=b9, MCUERR=b28.)

**FW / H2C:** `REG_MCUFWDL=0x0080` (EN=BIT0, RDY=BIT1, CHKSUM_RPT=BIT2, WINTINI_RDY=BIT6); page size 4096, ≤8 pages, FW region 0x1000–0x5FFF; H2C boxes `REG_HMEBOX_0..3=0x01D0/D4/D8/DC`, ext `0x01F0/F4/F8/FC`, free-flag `REG_HMETFR=0x01CC`.

**DMA ring base (DESA):** MGQ=0x0310, VOQ=0x0318, VIQ=0x0320, BEQ=0x0328, BKQ=0x0330, RX=0x0338, BCNQ=0x0308, HQ0..7=0x0340…0x0378; `TXBD_NUM` 0x0380–0x039A, `RXBD_NUM=0x0382`, `TXBD_IDX` 0x03A0–0x03D4, `RXBD_IDX=0x03B4`; `REG_PCIE_CTRL_REG=0x0300`. Depths: `TX_DESC_NUM=512`, `RX_DESC_NUM=512`, `SEG_NUM=1`; `TX_DESC_SIZE=64`, `RX_DESC_SIZE=24`, `USB_HWDESC_HEADER_LEN=40`.

### `def.h`

`version_8192f` chip enum (normal-chip id), `RF_2T2R`, `RX_DESC_NUM=512`, `TX_DESC_NUM=512`, rate codes `DESC_RATE1M=0x00 … DESC_RATEMCS15=0x1b`, QSEL values (`BE=0x00, BK=0x02, VI=0x05, VO=0x07, BEACON=0x10, HIGH=0x11, MGNT=0x12, CMD=0x13`).

### EFUSE → EEPROM map (8192F)

Signature `rtl_id == 0x8129` @0x00. Load-bearing fields: TX-power path A @0x10, path B @0x3A; `channel_plan` @0xB8; `xtal_k` @0xB9 (`& 0x3F`); `thermal_meter` @0xBA; `iqk_lck` @0xBB; `rfe_option` @0xCA (`& 0x1F`; types 1/5 validated, 7/8/9/12 ePA branch); `country_code` @0xCB; `mac_addr` @0x107 (6 bytes); per-channel kfree gains at raw bytes **0x1EA / 0x1EC / 0x1EE** (nibble-split per path: 0x0F=A, 0xF0=B). `EFUSE_UNDEFINED = 0xFF`.

> **MAC provisioning override:** the operational MAC must come from the board (`of_get_mac_address()` precedence: DT/nvmem, else SoC-derived). The efuse `mac_addr` @0x107 is a fallback/identity hint only — it must **not** be baked as the operational address.

---

## 6. Big-Endian Risks (BE-MIPS accessor audit)

The chip's DMA descriptors, efuse map, and FW header are **little-endian on the wire**; the host CPU is big-endian. Every multi-byte field crossing that boundary needs an explicit accessor. Audit points:

1. **TX descriptor + TX BD words** (`fill_tx_desc`, `fill_tx_cmddesc`, BD segment fill) — all `__le32`. Build each word in host order, then `cpu_to_le32` on store; never write the struct field directly. The own-bit (word0 bit31) and `psb`/`len_0` packing must be set in the `__le32` value, not the CPU value. DMA addresses split into low/high words — assign each `__le32` separately.

2. **RX descriptor words** (`query_rx_desc`) — read via `le32_to_cpu` before extracting bitfields (`pkt_len[13:0]`, `drv_info_size[19:16]`, `physt` b26, `own` b31, etc.). A raw read on BE yields byte-swapped garbage and silently mis-sizes every frame.

3. **RX phystatus / `phy_status_rpt` struct** — this is a packed on-wire structure (`path_agc[2]`, `cck_agc_rpt`, `path_cfotail`, `stream_rxevm`…). On BE, multi-byte members (CFO tails, EVM, csi) need `le16/le32` accessors; single-byte gain/lna/vga fields are endian-safe but the **bit extraction** within a byte is fine — only multi-byte members are at risk.

4. **`set_desc` / `get_desc`** — these are the single choke point for descriptor field access; implement them with `le32` accessors so every caller is automatically BE-correct, and avoid open-coding descriptor reads/writes elsewhere.

5. **EFUSE map parsing** (`read_eeprom_info`) — the efuse byte map is read byte-at-a-time through the core `read_efuse` engine, so individual bytes are endian-safe. But any field read as a `u16`/`u32` (TX-power sub-structs, channel plan words) must be assembled byte-wise or `le`-converted, **not** cast over a `u8*`.

6. **FW header** (`struct rtlwifi_firmware_header`) — `signature __le16`, `version __le16`, `ramcodesize __le16`, `svnindex __le32`: the header-present test `(le16_to_cpu(signature) & 0xFFF0) == 0x92E0` and the 32-byte strip length must use the converted values.

7. **DESA ring-base / TXBD_IDX programming** — the low/high 32-bit halves of each 64-bit ring base are written to `offset` and `offset+4`; confirm the split matches the chip's expectation independent of host endianness (these are register writes via `rtl_write_dword`, which the core already endian-normalizes — the risk is in the DMA-address *value* split, not the register write).

8. **H2C box fill** — element_id in box byte0 and the 3-byte ext-box payload are byte-oriented; pack explicitly rather than memcpy'ing a host-order word.

General rule: anything DMA'd to/from the chip is `__le*`; anything written through `rtl_write_*`/`rtl_read_*` register accessors is already endian-normalized by the core. The bugs live exclusively in the DMA-descriptor and on-wire-struct paths.

---

## 7. OpenWrt Packaging

**Build reality:** the in-kernel `rtlwifi/` tree is **not compiled** (kernel `.config` carries no `CONFIG_RTLWIFI*`). OpenWrt builds all Realtek WiFi from `package/kernel/mac80211`, which pulls a separate **backports-6.18.26** tarball (`PKG_SOURCE`/`PKG_BUILD_DIR`). The drivers actually built are whatever `package/kernel/mac80211/realtek.mk` lists in `PROVIDES` and defines a `KernelPackage/` for — and neither `rtl8192ee` nor a hypothetical `rtl8192fe` is present today, so a package entry is mandatory either way.

### Recommended: (A) backports patch + `realtek.mk` KernelPackage

Drop the new sub-driver dir into the backports source via a build-time patch, and register it in `realtek.mk`:

1. **Patch** under `package/kernel/mac80211/patches/` (against `backports-6.18.26`) creating `drivers/net/wireless/realtek/rtlwifi/rtl8192fe/` (all files from §2), adding `CONFIG_RTL8192FE` to the backports Kconfig, and `obj-$(CONFIG_RTL8192FE) += rtl8192fe/` to the realtek Makefile.
2. **`realtek.mk`:**
   - append `rtl8192fe` to the `PROVIDES` list,
   - add `config-$(call config_package,rtl8192fe) += RTL8192FE`,
   - add a `KernelPackage/rtl8192fe` block:
     - `DEPENDS += +kmod-rtlwifi-pci +rtl8192fe-firmware`
     - `FILES := $(PKG_BUILD_DIR)/drivers/net/wireless/realtek/rtlwifi/rtl8192fe/rtl8192fe.ko`
     - `AUTOLOAD := $(call AutoProbe,rtl8192fe)`

**Why (A):** it reuses the already-built `rtlwifi.ko`/`rtl_pci.ko` from the same backports build, so symbol/ABI versioning (tied to `PKG_VERSION`) is correct; it inherits btcoexist/efuse/ps; it matches how every existing Realtek sub-driver ships; and it is the upstreamable shape (a backports patch maps 1:1 to a future mainline submission). It yields a clean `kmod-rtl8192fe` with proper `DEPENDS`.

### Rejected: (B) standalone out-of-tree package

A separate package `#include`-ing the rtlwifi core headers and building against exported symbols. **Avoid:** it depends on the core's unstable in-tree headers (`../wifi.h`, `../pci.h`, `../core.h`) and on `EXPORT_SYMBOL`s that are **not a stable KABI**; it would have to track backports header churn, replicate Kconfig selects (`RTLWIFI`, `RTLWIFI_PCI`, `RTLBTCOEXIST`), and risk ABI-version mismatch against the mac80211-built `rtlwifi.ko`. Justified only if the backports source genuinely cannot be touched.

**Decision: take (A).** Own SPDX/copyright throughout; 8192F register addresses, field bits, init order, and numeric values implemented from the documented datasheet facts and the mainline GPL `rtl8xxxu`/`8192f` and `rtl8192ee` references.

---

## 8. Effort Estimate + Critical Path

| Phase | Scope | Est. effort | New-logic weight |
|---|---|---|---|
| **0** | PCIe endpoint enumeration | *external blocker* (host-controller team) | n/a — not driver work |
| **1** | Probe + power-on + FW download | ~1–1.5 wk | `pwrseq.c`, `fw.c`, `hw.c` power-on, `sw.c` skeleton |
| **2** | BB+RF init + LCK/IQK | ~2.5–3.5 wk | **`table.c` (all data) + `phy.c` IQK/LCK — the heaviest single chunk** |
| **3** | TRX rings + RX path | ~1.5–2 wk | `trx.c` RX desc + phystatus, DESA programming |
| **4** | TX path + STA connect | ~1.5–2 wk | `trx.c` TX desc + BD, H2C rate adaptive |
| **5** | AP mode | ~1 wk | beacon queue, RXFLTMAP, multi-client CAM |

**Total driver effort:** ~8–10 engineer-weeks once Phase 0 is unblocked.

**Critical path:** **Phase 0 (PCIe enumeration) → Phase 1 (probe/FW) → Phase 2 (BB/RF + IQK/LCK) → Phase 3 (RX) → Phase 4 (TX/STA) → Phase 5 (AP).** The path is strictly serial because each layer is untestable until the one beneath it works on real silicon. The two highest-risk nodes are:

1. **Phase 0** — a *hard external prerequisite* that is **currently blocked**: the link trains but the endpoint returns the `0xeeeeeeee` abort pattern, so config-space reads never complete and the driver never binds. No driver work substitutes for it; it is owned by the host root-complex/link-stability task and gates everything downstream.
2. **Phase 2 IQK/LCK in `phy.c`** — the largest genuinely-new-logic block, with chip-specific quirks (LOK result fed into the TX-PA LUT via RF `0x33`, the bits[25:16]==0x42 failure signature, the 3-run candidate selection, ePA RFE branch handling) that diverge materially from the 8192E template and must be validated against live radio-register readback.

Everything off the critical path (LED, debug, power-save tuning, thermal/TX-power tracking, optional DPK for ePA RFE types) can land incrementally after Phase 4 without blocking the AP-mode goal.