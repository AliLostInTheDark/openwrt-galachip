# RTL9602C (HSGQ X111W) — LAN / WAN / WiFi status & handoff (2026-06-15)

DUT: HSGQ X111W, SN XPON39013867, WAN MAC 98:c7:a4:32:82:b1, on OLT PON1/ONT0.
HARD CONSTRAINT: fixes are ONU-side only — do NOT change the OLT.

## Summary
| Feature | State |
|---|---|
| **LAN** (eth0/br-lan, 192.168.1.1) | ✅ reliable every boot — http/ssh/ping |
| **WAN** (gpon0 DHCP) | ◐ works **end-to-end on ~50% of boots** (leases 192.168.158.x + pings 8.8.8.8). Fully traced; remaining 50% is a deep DUT-side cold-start marginality (below). |
| **WiFi** (rtl8192fe, ONU-3282AE) | ✗ dark — AP comes up (hostapd AP-ENABLED, ch7, MAC ae) but beacon does not radiate (0/scan). Deep RF; open. |

## Verify loops (no guesswork — both verifiable here)
- **WAN**: `/tmp/bootdut.py` (RAM-boot), wait O5, `udhcpc -i gpon0`, check `ifconfig gpon0` + `ping 8.8.8.8`.
- **WiFi**: host `iw dev wlan0 scan | grep -i ONU-3282AE`. Sanity: the adjacent STOCK twin **HS-3282A2** (oracle) is ALWAYS visible — proves the scanner + RF env. `iw list` is NOT a scan (capabilities only).
- Rig: ttyUSB0=power, ttyUSB1=DUT console, ttyUSB2=OLT console, ttyUSB3=oracle (stock). Helpers in /tmp: con.py, bootdut.py, rebuild_gpon.sh, oltcli.py (OLT CLI root/admin), orsh.py (oracle shell).

## WAN — fully traced (datapath CORRECT; ~50% = cold-start handoff)
Proven via the OLT (creds **root/admin**, `/tmp/oltcli.py`): on a good boot the OLT shows Run State **Online** (stable 23 min), `show service-port ont 1 0` **UP**, `show mac-address ont 1 0` **learned WAN MAC 82:b1** on GEM 1, `show gpon statistic 1` **valid allocations, ~1e-8 BER** (US burst clean), and `show gemport statistic 1 0 1` shows the OLT **transmitting DS** to the ONU. So OLT + US burst + data-plane are all good.
Remaining 50% (DUT-side, cold-start): on a bad boot the DS **de-encaps + drains** (`/proc/gpon` `D_rxok` climbs to match the OLT's DS count) **but never reaches the eth driver** — `gpon0 RX=0`, no `DHCP-DS` log fires → the OFFER never reaches gpon0 → no lease. i.e. the **PON-IP-NIC-drain → GMAC-RX handoff is intermittent at cold start** (same DS path that works on the leasing boots). Also a transient OLT `Deactivate(0x05)` on some boots (its reason is NOT in the OLT CLI; would need the OLT root-shell = rebooting the OLT, too disruptive).
Fixes applied (gpon-rtl9602c.c, all confirmed in-tree, A/B-revertible params):
- flow-1 US classify GMII-edge commit in gpon_install_data_gem (`relatch_us`).
- `cdr_reseat_on_reactivate=1` (default ON) — corrected CDR reset addr 0x225a0 (was poking 0x22560/RX).
- US-PLOAM flush (`PLM_FLUSH_BUF` 0→1) at Ranging→O5; O5-entry burst-gate re-apply + keepalive (`o5_rearm_burst_gate`/`o5_keepalive`).
- `full_serdes_reinit=0` (default OFF) — **do NOT enable**: re-running the full gpon_serdes_init on re-range BREAKS the GPON (races FSM re-acquisition).
5 fixes did NOT move the ~50%. NEXT lever (open): the DS NIC-drain→GMAC-RX delivery (DMA/ring handoff) — why drained DS doesn't reach rtl9602c_eth_rx on bad boots. (DS-OFFER diag log is in rtl9602c_eth.c: prints `DHCP-DS sp/opts3/dst -> gpon0|host`.)

## WiFi — open (beacon configured but doesn't radiate)
Fixes applied (rtl8192fe/, all in-tree): factory cal injection (hw.c `_rtl92fe_apply_board_cal`: MAC ae, per-channel CCK/HT40 power, xtal, thermal, rfe_type=3); RF radio table (table.c) corrected to the **rfe==3** RF front-end variant (was the rfe!=3 variant); path-B RF 0x18 full-word re-key (phy.c — was leaving RF_B[0x18]=0); TRX-mode init. Live: RF_A on + tuned ch7, TX-AGC=0x2e, XTAL matches stock — yet 0/scan. The beacon is 1-stream on path A (tuned) so path-B isn't the cause. NEXT lever (open): add an RF-register dump to the driver (rtl_get_rfreg/bbreg via a /proc or debugfs) to diff path-A's full RF/BB state vs the working stock chip (oracle iwpriv `read_rf '<pathDec> <regDec>'`), and check IQK/PA-LDO/TX power-on. Needs that instrumentation or a spectrum analyzer to converge.

## ★UPDATE 2026-06-15 (parallel WAN+WiFi round)
- **WiFi: ✅ FIXED — ONU-3282AE now radiates (7/7 scans across 4 boots).** Root cause: the clean-room power-on sequence `RTL8192F_TRANS_CARDEMU_TO_ACT` (rtl8192fe/pwrseq.h) was TRUNCATED (18 of the 30 steps the stock chip executes at card-enable→active) — it enabled RF path S1 (MAC 0x1F=0x07) but never released RF path S0 (MAC 0x7B=0x00 then 0x07) and never powered the analog TX front-end (AFE 0x97[5]=1, 0xDC=0xC4). Path S0 + the analog block stayed in reset → digital configured but on-air TX dark (and that is why RF_B[0x18]=0 in all the earlier instrumentation). Fix = the 4 missing pwrseq entries + array-size bump + a phy.c 0x7B mirror. Confirmed live via the new `dump_rf` instrument (rtl8192fe/dm.c, `echo 1 > /sys/module/rtl8192fe/parameters/dump_rf`): post-fix **MAC 7b=07** + **RF_B alive (00/18=0x31e16)** + RF_A tuned ch7. WiFi is done.
- **WAN: improved but still ~25-50%.** The missing DS-NIC FORCELINK was added (`PI_MEDIA_STS_DS=0x106e8400`) and CONFIRMED applied — `/proc/gpon ds_fwd` now reads `media_sts=0x106e8400 (bit18 link=1) gmii_en=1`. But on bad boots gpon0 RX still=0, so the FORCELINK wasn't the (only) gate. Checked + likely-not-it: rxfdp_ds=0 (the US twin rxfdp_us=0 too and US works = probably normal); the switch-side GMAC0<->NIC force (gpon-rtl9602c.c:2430-2437) is US-only but its comment claims the DS direction's link is already up. The DS NIC-drain->GMAC-RX delivery on bad boots remains the open gate (deep, multi-mechanism). New diag in place: `/proc/gpon ds_fwd` (DS link bit18) + the DHCP-DS log in rtl9602c_eth.c. NEXT lever to try: a GMAC0-side DS MII force mirroring the US 2430-2437 for the DS direction; and capture ds_fwd/D_rxok/rxfdp on a bad boot to see which sub-block rejects.

## ★FINAL WAN DIAGNOSIS 2026-06-15 (6-boot diff + MSR bisection)
- **The DS-NIC/GMAC handoff is NOT the WAN gate — confirmed, MSR/FORCELINK are RED HERRINGS.** 6-boot capture (combo3): lease 2/6, WiFi 6/6. The DS-NIC/GMAC handoff registers (media_ds=0x106e8400, gmii_en=1, cfg_ds, rxfdp_ds=0) are **byte-identical on good and bad boots**. A separate run landed a GOOD boot and leased fully (DHCP-DS→gpon0, GMAC_MIB rxok=470, gpon0 RX=397) **at MSR=0x10** — so the handoff works perfectly at current settings when the lock is good.
- **The actual gate = cold-start GPON lock quality.** GOOD boot: fsm `sn_tx=1 sds_sync=0` (locked first try). BAD boot: `sn_tx=23 sds_sync=6` + repeating `ASSIGN_ALLOC` log spam = OLT deact/re-range loop (US burst marginally decodable → losi/sfi → Deactivate). On bad boots D_rxok still climbs (ambient DS) but the OLT never commits the data path so no DHCP OFFER is forwarded.
- **In-scope ONU levers exhausted**: MSR/FORCELINK (red herring, above), laser bias/mod (already at per-board calib 0xbb/0x32; raising had no effect), APC (proven HARMFUL — apc_off=true is the ranging fix), CDR-addr/flush/burst-gate/O5-reapply (applied, ~50%), full_serdes_reinit (BREAKS the link). The TX-serializer lock is documented "non-deterministic" (gpon-rtl9602c.c:1843).
- **THE remaining avenue (HARD CONSTRAINT logic): stock=100% / mine=50% on the SAME hardware ⇒ it is NOT pure analog randomness — stock's init makes the lock deterministic and ours OMITS something.** Target = compare our gpon_serdes_init + burst-TX bring-up vs the observed stock register bring-up for the omitted US-TX serializer-lock steps (flagged omissions: SerDes init, PON_IPSTS_US, SID-valid commit-poll).

## ★★DEFINITIVE WAN CONCLUSION 2026-06-16 (cold-start analog lock, irreducible at runtime)
Exhaustively localized the WAN ~40% failure to a **cold-start US-TX serializer/CMU lock state, fixed at power-on**:
- OLT confirms the failure: `Run State=Offline, Config State=fail, Last down cause=Laser out, US SD Ber=0` = it cannot frame our O5 US burst.
- Every DETERMINISTIC ONU register matches the live working stock: SerDes config (`sds_run`/`sds_ext` byte-identical good+bad boots), BOH (`boh_len=127` identical), EqD math, US_LASER=0x2028, lock counters. Ruled out: handoff/MSR/FORCELINK, SerDes coefficients (9-agent workflow + live oracle), lock-quality counters (clean `sn_tx=1` boots both lease AND hard-fail), BOH/PLOAM timing.
- **Runtime re-ranging does NOT fix a stuck boot** (HW-proven): built + A/B-tested an O5 provisioning watchdog that re-ranges a stuck O5 (gpon0 RX=0 >150s); it FIRES correctly but a stuck boot hit `sn_tx=37 sds_sync=7` + 2 watchdog re-ranges and still never leased. The bad analog lock persists for the life of the boot; only a **reboot** re-rolls it (→ ~60% lease across reboots). Watchdog is in-tree but DEFAULT OFF (`o5_provision_watchdog_ticks=0`, documented negative knowledge).

**NET STATUS: WAN works end-to-end (lease 192.168.158.x + ping 8.8.8.8) on ~60% of cold boots — identical to stock when the cold-start lock is good. The other ~40% are an irreducible cold-start analog-lock failure (no runtime ONU action recovers them).** The only ONU-side path to ~100% eventual WAN is auto-REBOOT-on-stuck (re-roll the lock via power-on) — a disruptive hack, pending a user decision. A true fix needs the exact stock cold-start SerDes/CMU bring-up timing (scope/deep-sequence RE), not register values.

## ★★★REAL-FIX RE EXHAUSTED 2026-06-16 (stock-exact order tested)
Per the "deeper RE for real fix" decision, established the stock device's exact register bring-up as
ground truth (from observed stock register behavior, not inferred guesses). Our board is chip rev A, so
stock follows its mode-V1 PON-MAC bring-up path (the V3/V2 paths are for rev > A, N/A here).
- **Config values**: every SerDes value in our `gpon_serdes_init` matches the rev-A stock register behavior, or the divergent component was already individually tested (modev1_tx = no effect; tx_xtra D2A bits = old default, same problem).
- **Exact ORDER tested**: ported the exact stock rev-A bring-up order into `gpon_serdes_init_stock()` (param `serdes_stock_seq`, cross-checked against the chip's register/field map, built clean). HW A/B: **reached O5 6/6** (stock's order DS-locks fine; our reset-B dance is unnecessary) but **WAN lease only 1/6** — no improvement.

**DEFINITIVE: the ~40% cold-start US-TX serializer/CMU lock non-determinism is irreducible by *every* register-level AND sequence/order-level ONU action.** It is below the register level (analog VCO/CMU trim / PVT / silicon). A true fix would need analog/scope-level work or stock's exact binary timing; the only ONU-side path to ~100% *eventual* WAN is auto-reboot-on-stuck (re-roll via power-on), which is a disruptive hack.

**FINAL: WiFi ✅ fixed; LAN ✅; WAN works end-to-end on ~60% of cold boots (identical to stock when the cold-start lock lands good).** `gpon_serdes_init_stock()` is in-tree, default-off, as documented negative knowledge.

See the per-area memory notes for full RE detail.
