# ZX279125 packet engine: hardware L2 bridging and buffer pool

Checkpoint: 2026-10-24. Read-only analysis of the vendor firmware
(`V2801SG_all_V6.1.2-260526_normal_GPON.bin`, extracted kernel and
`/kmodule`). No register was written on hardware for this note.

## Current OpenWrt data path

Subscriber traffic does not yet use hardware forwarding. `zx279125-ethernet`
receives LAN frames through the packet processor's CPU queues (NAPI) and the
Linux bridge `br-lan` joins `lan0` and `pon0`. `pon0` CPU transmit still drops
data (ONU-BRIDGE-PROFILE.md), so PON-to-LAN forwarding is not functional either
way. A software bridge at 600 MHz would also not sustain GPON line rate.

## The hardware has an L2 switch

The vendor `tm.ko` (traffic manager, 1.1 MiB) and `switch.ko` drive a
hardware bridge behind the packet processor. Function names, by block:

| Block | Vendor API | Role |
| --- | --- | --- |
| SBRG (switch bridge) | `sbrg_add_mactable`, `sbrg_set_pt_learn_mode`, `sbrg_set_pt_da_lookup_en`, `sbrg_set_pt_smac_look_en`, `sbrg_set_srcaddr_aging_cycle`, `sbrg_set_macaddr_ln_num_limit` | MAC table, learning, aging, learn limits, per-port DA/SA lookup |
| | `sbrg_set_pon_brdcst_flood_portmask`, `sbrg_pon_unkuni_flood_portmask`, `sbrg_set_unkmul_flood_portmask`, `sbrg_set_unknown_unicst_fwd`, `sbrg_set_brdcst_fwd_en` | Flooding of broadcast, unknown unicast and unknown multicast |
| | `sbrg_add_vltbl`, `sbrg_set_*_vltrans_table`, `sbrg_set_inport_vl_chk_en`, `sbrg_set_isolate_pt_cfg`, `sbrg_set_pt_tls` | VLAN table and translation, port isolation, transparent LAN service |
| CLA (classifier) | `cla_set_up_l2_uni_default_flow_cfg`, `cla_set_dn_l2_default_flow_cfg`, `cla_set_dn_unicast_flow_cfg`, `cla_set_dn_broad_flow_cfg`, `cla_set_dn_multi_flow_cfg`, `cla_set_dn_unknown_da_action_cfg`, `cla_set_*_omci_flow_cfg` | Upstream/downstream default L2 flows, OMCI trap, unknown-DA action; forward/drop/trap counters |
| ADM | `adm_set_policing_enable`, `adm_set_*_bucket_*` | Ingress policing (token buckets) |
| SQMG / SSCH | `greg_set_sqmg_*_init`, `greg_set_ssch_*_init` | Queue manager and scheduler (DWRR, shaping) |
| switch.ko | `l2_hardfastReg`, `sw_acl_l2_setFwdInfo`, `sw_acl_l2_hardfast_vlan_aggregation_add`, `sw_mac_*` | Higher-level L2 fast path, ACL forward info, MAC filtering |

`switch.ko` also has `l3_hardfastReg` and `hf_set_l3_entry` (an L3 fast path);
that is not needed here.

Hardware PON-to-LAN bridging therefore exists in silicon. The vendor driver
programs it through indirect register access (`sbrg_set_indreg_cmd`,
`cla_get_indirect_rw_*`, `tmOnuRegWrite`). The register map behind those calls
is not decoded yet.

### Next steps (read-only first)

1. Recover `tmOnuRegWrite`'s module/register addressing and the SBRG/CLA
   indirect-access protocol from `tm.ko` (relocations give exact call sites).
2. Capture the vendor init order: `tm_initial` → `si_zx_api_*_init` →
   `cla_set_*_default_flow_cfg` → `sbrg_set_*`, with the constant arguments.
3. Map the GEM-port side: how a GEM port configured by OMCI becomes an SBRG
   port (`zte_gpon.ko`, `gpondrv.ko`).
4. Implement a minimal transparent bridge (learning on, PON↔LAN forward,
   OMCI trap to the CPU) in the kernel driver, and test it with fiber
   disconnected first (LAN-side flooding and counters), then on an OLT.

## Buffer pool: the OS can re-initialize it smaller

Stock U-Boot programs 4096 buffers at `0x41000000` (buffer manager `+0x54` =
`0x7f`, 4096-entry pointer table at `0x41800000`). The vendor kernel does not
keep that layout. At `0xc0326ac0` it reserves `0x418a0000` + `0x760000` and
prints `reserve bmu=0x6e0000, oss=0xc000, base=0x418a0000`. Its engine init at
`0xc001aec4`, the counterpart of stock's `0x41f1ea3c`:

| Register | Stock U-Boot | Vendor kernel |
| --- | --- | --- |
| Pointer table entries written | 4096 | 2048 |
| BMU `+0x54` (count / 32 − 1) | `0x7f` | `0x3f` |
| PP `+0x18` pointer table | `0x41800000` | `0x418a0000` |
| PP `+0x20` buffer data | `0x41000000` | `0x419b0000` |
| PP `+0x24` (buffer size) | 2048 | 2048 |
| BMU `+0x48` | `0x10000000` | `0x8000000` |

The vendor's Linux runs with 2048 × 2 KiB = 4 MiB of buffers and keeps
`0x40000000..0x418a0000` (24.6 MiB) for the kernel, after stock U-Boot has
already run the engine for its multicast-upgrade check. This shows that the OS
can re-initialize the buffer manager with half the buffers at another address.

A smaller pool would free about 5 MiB beyond the current 17 MiB. Before trying:

- Decode the rest of the vendor init (rings/descriptors, auxiliary `+0x34`, the
  BMU `+0x48` meaning) so the new layout is complete, not partial.
- Quiesce the engine first, as the vendor does after U-Boot; the existing
  `zx_cold_start` path in the Ethernet driver is the starting point.
- Test from RAM (R9 method) with buffer-manager balance checks, before any
  flash write.
