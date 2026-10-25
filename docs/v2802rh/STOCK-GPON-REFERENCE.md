# V2802RH: stock GPON state on the live OLT

This is the stock RTOS (`JCO4032_R2.57`), ranged at O5 on the user's OLT
with the internet service on VLAN 100, captured on 2026-10-25. It is the
reference for the PON stage: the RTL9601D `omcid` backend must support this MIB, and the
LAN↔PON hardware offload must reproduce the switch rules in section 4.

Source logs (kept off the repo, they hold session keys):
- `~/Desktop/register-stock-online.log`: `register get all` plus memory
  dumps, online.
- `~/Desktop/omcicli.log`: `omcicli mib get all` plus the classifier
  entries.

## 1. Identity the OLT ranged

| Item | Value |
|---|---|
| GPON serial | `ZYOT 0x20095605` (vendor ZYOT; ONT-G and circuit packs report `ZYOT20095680142f0f`) |
| PLOAM password | `00000001` (`30 30 30 30 30 30 30 31 00 00`) |
| ONT-G | VID `ZYOT`, version `0001`, traffic management option 2 (rate-controlled) |
| ONT2-G | OMCC version 0xa0, 128 priority queues, 16 schedulers, 64 GEM ports |
| Software image | `JCO4032_R2.57`, image 0 active and committed |
| Equipment ID | `MONUV802` (stock vendor ME), ONT-G equipment `0001` |
| OLT | vendor `ALCL` (Nokia/Alcatel-Lucent) |
| ONU ID | 5 (GTC register; `gpon show gtc` prints 0) |

The serial and MACs live in the `config` block (0x25000), which OpenWrt
keeps read-only.

## 2. What the OLT provisions

```
            VEIP 0x0601 (ENET_VEIP)                    ── bridge port 0x0601
                │ ExtVLAN 0x601, assoc. type 10 (VEIP)
   MAC bridge 0x0601 (no learning limit, no STP)
        ├── bridge port 0x0001 ── 802.1p mapper 1 ── GEM IW TP 0x100 ── GEM 256 ── T-CONT 0x8000 (Alloc 256)
        │      VLAN filter: VID 100, forward op 0x10
        └── bridge port 0x0002 ── 802.1p mapper 2 ── GEM IW TP 0x101 ── GEM 257 ── T-CONT 0x8001 (Alloc 257)
               VLAN filter: VID 660, forward op 0x10
OMCI: flow 64, T-CONT 16
```

- **The OLT sees an HGU (VEIP) ONU,** not a bridge with UNIs. The two
  physical UNIs (PPTP 0x0101 = LAN1, type 47; 0x0102 = LAN2, type 50) are
  not bridge ports. Stock connects them to the VEIP itself.
- **Both mappers send all eight p-bits to one GEM** (256 or 257).
- **Upstream queues:** GEM 256 uses priority queue 0x8007 on T-CONT 0x8000,
  GEM 257 uses 0x800f on T-CONT 0x8001. Each queue is 3276 deep, with
  weight 1; scheduler policy 2.
- **Extended VLAN on the VEIP:**

  | Row | Matches | Treatment |
  |---|---|---|
  | 0, 1 | VID 660, single- or inner-tagged | keep VID 660 |
  | 2, 3 | VID 100, single- or inner-tagged | keep VID 100 |
  | 4 | untagged | transparent |
  | 5, 6 | default rows | transparent |

  The LAN side therefore sees VID 100 and VID 660 tagged.
- **Multicast:** a subscriber config on 0x601 with one package (VID
  65535); no multicast GEM is provisioned.
- **Other settings:** MAC aging 300 s. No MAC limits, no port isolation,
  no loop detect. GAL payload 48.
- **Stock vendor WAN (VsWan* MEs):** one WAN, `INTERNET_R_VID_1`, bound to
  both LAN ports (`FeBind 0x03`), NAT off. Its VID 1015 (0x3f7) is left
  over from the previous ISP; the OMCI path uses VID 100.

The OLT's OMCI message-type table lists 77 ME classes in use (`MsgTypeTbl`
in the log). That is the set `omcid` has to answer.

## 3. GPON MAC state

| Item | Value |
|---|---|
| T-CONTs | 0 = Alloc 256, 1 = Alloc 257, 16 = OMCI |
| DS flows | 0 = GEM 256 ETH, 1 = GEM 257 ETH, 64 = OMCI; AES on all three |
| US flows | 0 = GEM 256 → T-CONT 0, 1 = GEM 257 → T-CONT 1, 64 = OMCI → T-CONT 16 |
| Laser | on offset 0x20, off offset 0x28; optical LOS enabled, polarity high |
| FEC | DS 1, US 1 |
| Burst overhead | guard 32 bits, delimiter `ab6073`; T3 preamble 121 before ranging, 55 after |
| Optics (ANI-G) | Rx level 0xce24, Tx level 0x04a7; SF/SD thresholds 5/9 |
| Alarms | none, apart from SD_LONG / SD_MISM latched |

## 4. How stock maps it onto the switch

| Item | Value |
|---|---|
| Classifier 0 (DS) | ACL-latch match 0x1a/0x7f; action C-tag, VID 100 assigned, PRI 0 |
| Classifier 1 (US) | WAN IF 2 (mask 0xf); action C-tag, VID 100, PRI 0, **assign SID 0** (= flow 0 = GEM 256) |
| Classifier 2–10 | invalid |
| Unmatched US / DS | "Permit without PON" / "Permit" |
| CF select port | PON enabled |
| VLAN 100 | members 0–3, tagged on 2 (PON) and 3 (CPU) |
| VLAN 1 | members 0–3, all untagged |
| VLAN 4005 | members 0–3, tagged on 2 |
| PVID | ports 0/1 = 4005, port 2 = 1015 |
| Tag mode | ports 0–2 "original" |
| ACL | entry 0 invalid; templates set but no rule installed |
| Frame length | LAN and CPU 1600 bytes, PON 2031 (in our driver since 4785d9b0a7) |
| Learning | 2 entries learned system-wide while online |

**Reading:** stock forwards only the internet service (VLAN 100) through
the switch, and only through the CPU's WAN interface (classifier match on
WAN IF 2). Its own LAN clients reach the internet through its software
WAN; no LAN port is bridged straight to GEM 256 in hardware. VLAN 660 is
provisioned in OMCI but has no switch rule in stock.

## 5. What OpenWrt needs from this

- **`omcid` (RTL9601D):**
  - answer the ME classes above with the identity in section 1;
  - build the VEIP bridge with mappers 1/2, GEMs 256/257 and T-CONTs
    256/257;
  - apply the ExtVLAN rows.
- **Pure L2 bridge (our target, unlike stock's WAN):**
  - **Downstream:** GEM 256 frames tagged VID 100 egress LAN1 and LAN2
    untagged (VEIP treatment row 2/3 inverted).
  - **Upstream:** untagged LAN frames get C-tag VID 100 PRI 0 and SID 0.
    That is classifier 1 with the match moved from WAN IF to the LAN UNI
    ports.
  - **VLAN 100:** LAN1, LAN2 and PON as members, untagged on the LAN
    ports. The CPU joins only if the board keeps a management IP on the
    service VLAN.
  - **VLAN 660 / GEM 257:** the OLT's second service, probably management
    or VoIP. Map it the same way only if a LAN device needs it.
