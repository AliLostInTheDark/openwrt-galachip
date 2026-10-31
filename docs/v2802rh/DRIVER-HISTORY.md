# Realtek Luna driver history

Bring-up notes moved out of the comments in
`target/linux/realtek-luna/files-6.18/drivers/net/ethernet/realtek/`, copied
verbatim. The dates, file names and parameters are as they stood when each note
was written: `luna_eth.c`, `gpon-rtl960x.c` and most module parameters named
here have since been removed. Headings name the function or register concerned.

## luna_eth_mib.c

### luna_mib_check_names (fragment; rest of the sentence is lost)

```c
/* THE NAME-LENGTH CHECK IS HERE AND NOT IN A */
```

## luna_eth_regs.h

### File header: why family constants and per-chip table are separate; LUT block offsets

```c
/*
* Register facts for the Ethernet MAC and switch core on the Realtek Luna
* family (RTL9602C, RTL9603CVD, and the siblings that follow).
*
* ★ WHY THIS FILE EXISTS, AND WHAT IT MEASURED.  The family had TWO Ethernet
* drivers -- rtl9602c_eth.c for the X111W and luna_eth.c for the G24W --
* each carrying its own copy of the register map.  Compared symbol by symbol on
* 2026-08-28, with the comments stripped so that a re-worded comment could not
* masquerade as a different value:
*
*     34 symbols  IDENTICAL on both chips   -> family facts, and they live here
*      4 symbols  genuinely per-chip        -> struct luna_sw_map, below
*
* That ratio is the argument for the whole port strategy: a new Luna board owes
* a TABLE, not a driver.  Two copies of 34 agreeing constants is not redundancy,
* it is two chances to edit one of them.
*
* ★★ THE PER-CHIP FOUR ARE NOT A MISTAKE, AND THEY ARE THE DANGEROUS KIND.
* The switch LUT block MOVED between the two silicon revisions: the RTL9603CVD
* inserted registers, so everything from LUT_UNKN_UC_DA_CTRL onward shifted.
* Confirmed on 2026-08-28 from each chip's OWN chipdef in the vendor SDK, which
* agrees with what both drivers already had -- two independent tiers:
*
*     register              rtl9602c   rtl9603cvd
*     LUT_UNKN_SA_CTRL       0x1C004     0x1C004    (unmoved)
*     LUT_UNKN_UC_DA_CTRL    0x1C008     0x1C00C    (+4)
*     UNKN_L2_MC             0x1C010     0x1C018    (+8)
*     UNKN_IP4_MC            0x1C014     0x1C01C    (+8)
*     UNKN_IP6_MC            0x1C018     0x1C020    (+8)
*     LUT_BC_FLOOD           0x1C020     0x1C028    (+8)
*     LUT_UNKN_MC_FLOOD      0x1C024     0x1C02C    (+8)
*     LUT_UNKN_UC_FLOOD      0x1C028     0x1C030    (+8)
*
* ⚠ READ THE FIRST AND LAST ROWS TOGETHER: the 9602C's LUT_UNKN_UC_FLOOD and
* the 9603CVD's LUT_BC_FLOOD are BOTH 0x1C028.  Using one chip's constant on
* the other's silicon does not fault and does not read back wrong -- it
* configures unicast flooding when broadcast flooding was meant.  That is the
* same shape as the CFG_PHY_CTRL defect that cost this project weeks, and it is
* exactly why these four are a table and not a #define.
*/
```

### R_MAR0, R_MAR4, R_CMD naming

```c
/* ★ NAMED FROM THIS DRIVER'S OWN REPEATED COMMENTS (2026-09-04).  Each of
* the three was written as a literal at two or three sites and commented
* with the same name every time; the tree established them, the compiler
* just could not see it.  The re8686 NIC window has no chipdef here, so
* these are the file's own record and nothing more is claimed. */
```

### RXD_RCDF

```c
/* RX: DMA error.  RCDF is the SILICON's own
* name (the vendor NIC driver's rx_info
* opts1 bitfield, `rcdf:1;//24`, same in
* the 4.4 and 5.10 SDK drops).  This one
* bit carried TWO shell names, RXD_DMAERR
* (luna_eth.c, invented -- in no vendor
* source) and RXD_RCDF (rtl9602c_eth.c),
* until 2026-09-03. */
```

### RX ring size

```c
/* Ring geometry.  RX is STOCK's: the RTL9603CVD's running vendor driver prints
* RX_RING_SIZE[0]=1024 and the SDK's GMAC0_RX1_SIZE is 1024. With 64 the ring
* filled, the switch sent PAUSE (p3_tx_pause 42774) and G24W forwarded
* CPU-path v6 at ~40 Mb/s with the CPU 99% idle (measured 2026-09-22). */
```

### Switch-core family constants

```c
/* Switch-core registers the two chipdefs place at the SAME address, so they are
* family constants rather than table fields.  Cross-read 2026-08-28 from each
* chip's own reg_list.c; the silicon's own names are kept, because a name that
* follows the silicon is one a reader can look up. */
```

### SW_GPHY_IND_WD, SW_GPHY_IND_CMD, SW_GPHY_IND_RD

```c
/* ★ THE GPHY INDIRECT PAIR, named from the RTL9602C's own chipdef
* (2026-09-04).  rtl9602c_uboot_swcore_bringup() wrote eight values into
* `ep->sw + 0x0` and `+ 0x4` with the comment `switch regs 0x10004/0x0/0x4`
* -- the author knew they were a pair and had no name for it.  It is the
* same shape as every other indirect block here: a DATA word, then a
* COMMAND word that says where the data goes, and a RESULT word that also
* carries BUSY.
*
* ★ THE COMMAND LAYOUT IS ESTABLISHED, and this note used to say it was not.
*   luna_eth.c drives the same window field by field, and the literals the
*   other shell writes decode exactly: 0x0060A400 is WREN|EN, phy 0, OCP
*   0xA400, and 0x0061A400 is the same on phy 1 -- which is the BMCR
*   power-down/power-up pair that file's own comments describe.  Leaving the
*   note standing would keep two shells writing one register two ways.
*
* ⚠ WHAT IS *NOT* SETTLED IS THE OCP MAP PER PORT, which is a per-chip
*   question and lives with the chip, not here. */
```

### 0x198/0x1b8/0x1d8/0x1f8 per-port status array

```c
/* ⚠ 0x198 / 0x1b8 / 0x1d8 / 0x1f8 ARE NOT NAMED HERE, and the attempt is worth
* recording (2026-09-04).  The chipdef names three of them P_ABLTY,
* BYPS_ABLTY_LOCK and MISCELLANEOUS_BONDING -- three unrelated registers -- but
* this driver's own /proc dump labels the four `p0_sts p1_sts p2_sts cpu_sts`,
* a PER-PORT array at stride 0x20, and luna_eth.c already reaches the ability
* word as SW_P_ABLTY(ep, p) built from the per-chip table.  So the chipdef is
* naming ELEMENTS of an array, exactly as it does for the PISO block, and a
* flat name for one element would be the same defect that made the G24W write
* 0x2700c twice.  The compiler caught this one: SW_P_ABLTY was already taken. */
```

### SW_STAT_PORT_RX_MIB_RTL9602C

```c
/* ★ THE RX BASE, ONE PER DIE, AND THE DIE IS IN THE NAME ON PURPOSE.  These
* exist so the number lives in exactly ONE place -- the table rows below are
* initialised FROM them and never repeat the literal -- and so the pin at the
* foot of this file can be a static_assert: a member of a `static const struct`
* is not a constant expression in C, a macro is.
* ⚠ THE SUFFIX IS A WARNING LABEL, NOT DECORATION.  A bare SW_STAT_PORT_RX_MIB
* would look portable and a shared file would reach for it, which is exactly
* how SW_MACPP_STRIDE_9602C got used on the RTL9603CVD.  Nothing outside that
* die's own table row may name one of these. */
```

### STAT_PORT_RX_MIB per-die base

```c
/* ★★ THE RX MIB BASE IS SETTLED, AND IT IS PER-DIE -- WHICH IS WHY IT IS IN
* THE TABLE AND NOT HERE (2026-09-11).  The 2026-09-04 note that used to sit
* at this spot said there was "deliberately no SW_STAT_PORT_RX_MIB" because
* the chipdef named 0x32400 STAT_PORT_OAM_MIB and 0x32600 STAT_ACL_CNT --
* both true, and both the wrong register.  The chipdef DOES declare a
* STAT_PORT_RX_MIB; nobody had looked it up by its own name:
*
*     die          STAT_PORT_TX_MIB   STAT_PORT_RX_MIB   ports   stride
*     RTL9602C          0x32000           0x32200         0..3    0x80
*     RTL9603CVD        0x32000           0x32600         0..6    0x80
*     RTL9607C          0x32000           0x32600         0..11   0x80
*
* ⚠ READ THE TX AND RX COLUMNS TOGETHER.  TX does not move and RX does, so a
* family #define for RX would give the RTL9602C the SIBLINGS' 0x32600 -- and
* on that die 0x32600 is STAT_ACL_CNT, a real register that answers a real
* read.  That is not a hypothetical: the /proc dump in rtl9602c_eth.c was
* reading exactly that address under the label "p3 rx".  Same shape as the
* LUT rows above, same shape as IO_LED_EN vs IO_MODE_EN.
*
* The block LAYOUT is family-invariant (the three chipdefs list the same 23
* RX and 27 TX fields at the same byte offsets inside the 0x80 window), which
* is why luna_eth_mib.c carries ONE field table and the bases are data.
* Independently cross-checked: luna_eth.c's own RX unicast/multicast/broadcast
* addresses on the RTL9603CVD (0x32620 / 0x32628 / 0x3262C) are exactly
* base + IFINUCASTPKTS / IFINMULTICASTPKTS / IFINBROADCASTPKTS. */
```

### SWCORE names used by rtl9602c_datapath_tables_init

```c
/* ★ NAMES BOTH CHIPDEFS AGREE ON, for the SWCORE registers
* rtl9602c_datapath_tables_init writes (2026-09-04).  It spelled these as
* bare hex; `bare_offset_chip_audit.py` resolves each address in the
* rtl9602c AND rtl9603cvd chipdefs and only a SAME verdict is written here.
* The SW_ prefix is this header's own convention for the SWCORE window.
* ⚠ SW_VLAN_PORT_ACCEPT_FRAME_TYPE below is an ABBREVIATION the tree established earlier
* (the chipdefs call 0x13000 VLAN_PORT_ACCEPT_FRAME_TYPE); it is left as it
* is because renaming a name the tree already uses is a separate question
* from naming a number that had none. */
```

### SW_PISO_PORT array

```c
/* ⚠ AN ARRAY, NOT FOUR REGISTERS.  Only element 0 carries a chipdef name;
* the two maps name DIFFERENT later elements (PISO_EXT at 0x27008 on the
* RTL9602C, at 0x2700c on the RTL9603CVD) because the array's extent
* differs, not because anything moved.  Treating one element as a
* standalone register made the G24W write 0x2700c twice and never write
* 0x27008 -- so the BASE is named and the elements are arithmetic. */
```

### SW_VLAN_PB_VID (PVID) packing

```c
/* ★★★ PVID IS PACKED TWO PER WORD -- "stride 4" WAS FALSE (measured 2026-09-12).
* The array is 12-BIT elements in 32-bit words, so port p lives at
* word (p / 2) in bits [(p % 2) * 12 + 11 : (p % 2) * 12], NOT at its own word.
*
* Proven three ways from our OWN stock kernel (cross-compiler/stock_nor/k0_kernel),
* not from the SDK:
*   descriptor @file 0xca15d0 : offset 0x1300c · element stride 0x000c = 12 BITS
*                               · max port index 3
*   _reg_addr_find @0x801b69dc: li 32 / div by stride / div port / sll 2 / addu
*                               => addr = base + 4 * (port / (32 / 12))
*   reg_array_field_write @0x801b72e8: mfhi after div / mult by stride
*                               => shift = (port % (32 / 12)) * 12
*
* ⚠ WHAT THE FALSE STRIDE DID: a writer stepping by 4 set port 0 and port 2,
*   left ports 1 and 3 INHERITED, and wrote its value into 0x13014/0x13018 --
*   which are EXT_VID, a different register entirely. Three defects from one
*   wrong word in a comment, and the code read correctly at every call site.
*/
```

### Packed per-port array arithmetic

```c
/*
* ★★★ THE PACKED PER-PORT ARRAY, ONCE FOR THE WHOLE FAMILY (2026-09-12).
*
* This switch core states most per-port registers as an ARRAY OF BIT-FIELDS,
* not as one word per port: element `port` of a `bits`-wide array lives in word
* (port / (32/bits)) at lsb (port % (32/bits)) * bits. Stock implements exactly
* that arithmetic ONCE, generically, for every array it owns -- own k0_kernel
* _reg_addr_find @0x801b69dc (`addr = base + 4 * (port / (32/bits))`) and
* reg_array_field_write @0x801b72e8 (`shift = (port % (32/bits)) * bits`).
*
* ⚠ WE IMPLEMENTED IT PER CALL SITE AND GOT IT WRONG TWICE IN ONE FILE, each
*   time by assuming one word per port:
*     PVID  (12 bits): stepping by 4 set ports 0 and 2, left 1 and 3 inherited,
*                      and wrote two words of EXT_VID.
*     DLF   ( 2 bits): "16 bits per port" set port 0 only, and the second write
*                      landed on LUT_LEARN_OVER_CTRL, a different register.
*   Meanwhile luna_eth.c already knew the DLF field was 2 bits and said so in a
*   comment -- the knowledge existed in one sibling and not the other, which is
*   the shape this family header exists to end.
*
* ⇒ ONE statement of the arithmetic, here, for both Luna Ethernet shells. It is
*   PURE -- no MMIO, no device pointer -- so each shell keeps its own accessor
*   (ioread32/iowrite32 here, sw_rd/sw_wr there) and neither grows a copy of
*   the layout. Per chip the TABLE supplies the base and the width; a new part
*   costs a row, never a second implementation.
*/
```

### struct luna_sw_map: src_permit polarity, lut_unkn_uc_da width, omci_cpu_reason

```c
/**
* struct luna_sw_map - the switch-core facts that differ between Luna chips
* @src_permit:       L2_SRC_PORT_PERMIT -- one bit per port, and each die
*                    states its OWN width on read-back: 0xFFFFFFFF comes back
*                    0xF on the RTL9602C and 0x3F on the RTL9603CVD.
*                    ★★ WHAT THE FIELD GATES IS NOT ESTABLISHED, AND THIS TREE
*                    CLAIMED IT THREE WAYS, ALL INCOMPATIBLE: luna_eth.c said
*                    "EN=1 DROPS the forwarded frame, 0 is the permissive
*                    value"; rtl9602c_eth.c said "INVERTED polarity: EN=1
*                    PERMITS a frame to egress its OWN ingress port"; the
*                    RTL9607C bring-up said "all-zero means NO port may
*                    forward" and wrote 0xFFFFFFFF to open it.
*                    MEASURED 2026-09-11 on BOTH Luna boards by forcing every
*                    bit and reading the EFFECT, never the read-back
*                    (ONU-test-case/src_permit_polarity.py): the register KEEPS
*                    the write and all three predictions FAIL.  Host<->board
*                    forwarding 3/3 at the factory value, 3/3 forced, 3/3
*                    restored on both dies; 0 of 12 of the host's own
*                    broadcasts came back inbound either way; and on the
*                    RTL9603CVD the CPU port's own flood return
*                    (lan_flood_return) was `returns-internally` at 0x0 and at
*                    0x3F alike.  ⇒ 0 is written because it IS the reset
*                    value, not because 0 was proven permissive, and no
*                    behaviour of this driver may be explained by this field
*                    until a measurement finds one.  The claims are refuted for
*                    the shipped configuration of these two dies; the RTL9607C
*                    one is UNTESTED -- no such board is on this bench.
*                    ⚠ ON THE RTL9602C THIS ADDRESS WAS WRONG ONCE, and the
*                    note travels with the value: it was 0x1C114, which is
*                    QOS_PB_PRI on that chip, so the CPU port's ingress permit
*                    was never set and the fabric dropped every CPU-injected
*                    frame after DMA -- TX counter climbing, nothing egressing.
* @piso_base:        per-port egress-forward (isolation) matrix
* @cpu_port:         the switch port this GMAC is
* @pon_port:         the fibre port (no copper PHY behind it)
* @port_mask:        flood/member mask covering every port on this chip
* @swcore_size:      ioremap length; must cover the highest block the driver
*                    touches (MIB, PISO).  Too small and those reads land
*                    outside the mapping instead of failing loudly.
* @lut_unkn_sa:      unknown-SA action, 2 bits per port
* @lut_unkn_uc_da:   per-port unknown-UC DLF action. ★ 2 BITS PER PORT, every
*                    port in ONE word -- "16 bits per port" was false and the
*                    writer obeyed it (own k0_kernel descriptor @0xca1360:
*                    stride 2 bits, max port 3). The next word, +4, is a
*                    SEPARATE register (LUT_LEARN_OVER_CTRL, descriptor
*                    @0xca1378) and must never be written as this one's port 2/3.
* @unkn_l2_mc:       unknown L2 multicast action
* @unkn_ip4_mc:      unknown IPv4 multicast action
* @unkn_ip6_mc:      unknown IPv6 multicast action
* @bc_flood:         broadcast flood, one bit per port
* @unkn_mc_flood:    unknown-multicast flood, one bit per port
* @unkn_uc_flood:    unknown-unicast flood, one bit per port
* @omci_cpu_reason:  the CPU-tag RX `reason` code (rx opts2[28:21]) the switch
*                    stamps on a DOWNSTREAM OMCI frame it traps to the CPU
*                    port.  PER CHIP AND NOT GUESSABLE: the vendor's own NIC
*                    RX hook switches on the chip id and picks 246 for the
*                    RTL9602C but 229 for the RTL9607C and the RTL9603CVD
*                    (rtl86900/sdk/src/module/gpon/gponapi.c,
*                    rtk_gponapp_omci_rx_wrapper -- tier 3).  Reading the
*                    9602C's 246 on this die matches a reason nothing ever
*                    carries, so the trap stays silent and the log reads as
*                    `the OLT sent no OMCI` -- the same shape as the eighteen
*                    PON-IP offsets that were written with 9602C literals.
*
* A new chip adds ONE instance here and nothing else.  Every field is an
* absolute offset within the switch core, never a delta from a sibling: a
* "+8 from the 9602C" table is a table that silently follows the wrong chip the
* day a third revision moves only half the block.
*/
```

### Port numbers per chip

```c
/* ★ PORT NUMBERS ARE PER-CHIP AND WERE HALF-TABULATED.  luna_eth.c
* already carried pon_port/cpu_port per chip while rtl9602c_eth.c
* hardcoded RTL9602C_PON_PORT=2 and SW_CPU_PORT=3 -- the same shape as
* the MSR value one sibling made tunable and the other did not.  The
* numbers differ genuinely: PON is port 2, 4 and 5 on the three chips. */
```

### macpp_stride / SW_MACPP_STRIDE_9602C

```c
/* ★ THE PER-PORT REGISTER INTERVAL, AND IT IS NOT THE FAMILY'S.  The
* vendor states it as data -- macPpInfo.interval, 0x400 on the RTL9602C
* and 0x100 on the RTL9603CVD and RTL9607C (sdk chipdef chip.c).  It sat
* in luna_gpon_regs.h only as a NUMBER named for one chip
* (SW_MACPP_STRIDE_9602C), so a shared file could reach for it and did:
* luna_gpon.c -- which BOTH Luna boards build -- wrote
* SW_P_MISC_PORT_9602C(2) and (3) at five sites, which on the RTL9603CVD
* address P_MISC[8] and P_MISC[12].  That chip has SIX ports, so both
* writes land outside the declared per-port block (macPpInfo upper bound
* 0x203FF) and the CPU port's RX_SPC -- what makes the switch accept the
* 48-byte OMCI PDU instead of runt-filtering it -- was never set. */
```

### cfg_unhiol

```c
/* ★★ CFG_UNHIOL, and it is IN THE TABLE for a measured reason: on the
* RTL9602C this address is CFG_UNHIOL (bit0 = IPG_COMPENSATION), and on
* the RTL9603CVD the SAME address is CPU_TAG_AWARE -- two different
* blocks, one number.  A family #define here would configure CPU tagging
* where IPG compensation was meant, without faulting and without reading
* back wrong: the CFG_PHY_CTRL defect, re-created.  0 = this chip's
* bring-up does not touch it. */
```

### Per-port ability trio

```c
/* ★ PER-PORT ABILITY TRIO -- per chip, and DEMONSTRABLY so: the RTL9603CVD
* puts force_ablty at 0x198 and p_ablty at 0x1B8, while the RTL9602C's own
* driver puts its force-ability array at 0x180 and its force-MODE array at
* 0x1B4.  The numbers overlap between chips without meaning the same
* thing, which is the third register block today found to do that.
* 0 = this chip's driver does not use that member. */
```

### gmac_mib16

```c
/* ★ THE MAC-LEVEL MIB ARRAY, and it is ESTABLISHED ON ONE DIE ONLY.
* Offset (from the MAC base, not the switch base) of the fourteen 16-bit
* GMAC counters -- TXOKCNT RXOKCNT TXERR RXERR MISSPKT FAE ... -- named
* and 16-bit-confirmed by the RTL9602C's OWN stock firmware, which prints
* them four hex digits wide from /proc/rtl8686gmac/hw_reg (tier 2,
* dev/STOCK_GMAC_REGS.md).  Nothing has established the array on the
* RTL9603CVD or the RTL9607C -- neither shell reads it there and neither
* chipdef covers the MAC window -- so those tables leave it 0 and those
* boards publish no mac_* row at all.  Borrowing the sibling's offset
* would answer with whatever that window holds. */
```

### omci_cpu_reason placement

```c
/* ★ THE DS-OMCI CPU-TAG REASON, and it lives beside the port numbers for
* the same reason they do: BOTH Ethernet shells classify a trapped OMCI
* frame by it, so a second home is how the two come to disagree.  It was
* a bare 246 in flowcore (rtl9602c_l34_logic.h) named for one chip while
* being read as a family fact -- the exact spelling that makes a sibling's
* literal look portable. */
```

### LUNA_MAC_BRINGUP_DEFAULT

```c
/* The address the silicon/bootloader leaves in IDR0/IDR4 when nothing has
* programmed a real one.  It is a VALID unicast address, which is exactly why
* it has to be NAMED: `is_valid_ether_addr()` accepts it, so a plain validity
* check ships it and the random fallback never fires.
*
* ★ A FAMILY FACT, MEASURED ON TWO BOARDS OF DIFFERENT SILICON: the LANLY G24W
* (RTL9603CVD, 2026-08-20) and the HSGQ X111W (RTL9602C, 2026-08-28 -- read
* from the lab host's own ARP table while the board was answering pings, so
* the two readings are independent of each other and of any one driver).
* Two different chips holding ONE address is the whole problem: this lab runs
* three ONUs on ONE L2 segment, and two of them sharing a MAC raises no error
* anywhere -- it produces a switch that learns the address on whichever port
* spoke last, and measurements on somebody else's bench that fail for no
* visible reason.
*
* ⚠ IT LIVES IN THE FAMILY HEADER BECAUSE THE TWO-COPY VERSION ALREADY COST A
* LIVE DEFECT.  The refusal was written into luna_eth.c alone, while the
* RTL9602C's own driver carried a byte-identical IDR reader and a plain
* `is_valid_ether_addr()` test -- so the 9602C shipped the shared default for
* as long as the two copies existed, and nothing anywhere said so.  A family
* fact kept in two chip shells is a repair with a delay fuse on it. */
```

### luna_idr_get/luna_idr_set shared via mapped base

```c
/* ★ SHARED BY TAKING THE **MAPPED BASE**, NOT THE DRIVER STRUCT.  Both Luna
* ethernet drivers had a byte-identical pair of these, differing only in the
* struct they dereferenced to reach `->base`.  That is the whole obstacle to
* sharing driver code in this tree, and passing the io handle removes it: the
* register layout is a FAMILY fact (R_IDR0/R_IDR4 above are already shared),
* only the container was per-chip.
*
* ⚠ AND THE DUPLICATE PAIR IS WHY THE BRING-UP-DEFAULT REFUSAL BELOW REACHED
* ONLY ONE OF THEM for eight days.  Two copies of one fact do not merely cost
* lines; they cost the NEXT repair, which lands in whichever copy the author
* happened to be reading. */
```

### MAC bootarg parser location

```c
/* The bootarg parser and the whole precedence moved to
* drivers/net/gpon/gpon_hwaddr.h on 2026-09-10; luna_mac_is_bringup_default()
* stays here because WHICH constant this silicon powers up holding is a family
* fact. Never spell an address in a header or a DTS: it is per-unit.
*/
```

### DMA_BUS_WINDOW

```c
/* The DMA descriptor address bus window.  ZERO on this SoC, and it is a named
* knob rather than a bare 0 for a measured reason:
*
* ⚠ OR-ING A NON-ZERO WINDOW INTO A DESCRIPTOR ADDRESS IS HARMFUL HERE.  It was
* observed to be a NO-OP for TX egress and to DEGRADE RX -- `dma_alloc_coherent`
* already yields correct bus addresses on this SoC (an artifact of its 1:1 map),
* so or-ing the window CORRUPTS them.
*
* ★ THAT PARAGRAPH EXISTED IN ONE OF THE TWO COPIES ONLY.  Both drivers defined
* this constant, one as 0x00000000u with the warning above and one as 0u with a
* single line that says none of it.  The value survived being copied; the
* measurement behind it did not. */
```

### luna_rx_pending race (DS OMCI wait, 2026-10-08)

```c
/* A frame waits at @head: read from the ring, never from R_ISR.  A NAPI poll acks
* the status it masked, so a descriptor handed back after its drain and before that
* ack loses its bit and would wait for the NEXT frame's interrupt: on 2026-10-08 a
* DS OMCI request waited 7 s for the OLT's retry, and the OLT deactivated the ONU. */
```

### RX refill: EOR

```c
/* ---- RX refill --------------------------------------------------------------
* Arm ONE RX slot with a fresh skb.  Shared because both drivers had it
* character for character apart from the struct they reached `->rx_ring`
* through; the pieces are passed explicitly rather than behind a new container,
* so neither driver has to change who owns its rings.
*
* ⚠ EOR IS SET ON THE LAST SLOT AND NOWHERE ELSE.  Getting that wrong does not
* fault: the engine simply walks off the end of the ring into whatever follows
* it, which is the quietest possible corruption.  `nr` is the ring's own entry
* count, passed in, never a #define read from whichever driver compiled last. */
```

### luna_rx_alloc ownership contract

```c
/* Allocate and map ONE replacement buffer.  -> the skb, or NULL.
*
* ★★ THE OWNERSHIP CONTRACT, and it is the whole point of the split.  A poll
* loop hands its skb away on EVERY arm (napi_gro_receive / netif_rx take it,
* dev_kfree_skb_any frees it), so once the loop body has run there is nothing
* left to put back.  A refill that fails AFTER that point therefore cannot
* "retry next poll": it leaves skbs[idx] pointing at memory the driver no
* longer owns, behind a descriptor whose D_OWN is clear -- so the next poll
* walks straight past the ownership guard and re-delivers a freed skb (and
* unmaps the same address a second time, which fires first).
*
* ⇒ CALL THIS BEFORE CONSUMING THE FRAME, and on NULL drop the frame with
* luna_rx_rearm() instead.  Never NULL out skbs[idx] as a shortcut: the loops
* have no !skb guard, so that converts the use-after-free into a NULL deref
* and still leaves the stale dmas[idx] to be unmapped twice. */
```

### luna_ipsel_cycle_gmac0 (0x1805 hang)

```c
/* Power-cycle GMAC0's clock domain.
*
* ⚠ RESTORE ONLY THE GMAC BIT.  A trial that also OR'd the stock NIC bring-up
* mask 0x1805 here -- taken from a chip-id-0x6266-conditional stock path --
* coincided with a HARD SoC HANG on the RTL9602C: the sibling IP_SEL bits gate
* other clock domains and are not ours to touch.
*
* ★ THAT WARNING EXISTED IN ONLY ONE OF THE TWO COPIES, which is the expensive
* half of duplication -- the code survives being copied, the reason it is
* shaped that way does not, and the next author of the poorer copy has nothing
* to stop them re-running the experiment that hung the board. */
```

### SOC_SW_ENABLE bit 5

```c
/* ---- SoC peripheral clock/enable word -------------------------------------
* ⚠ THIS IS **NOT** `SOC_IP_SEL`.  They are 0x3c apart in the same SoC window,
* and one of the three places that used this address called its pointer
* `ipsel` -- which is the other register's name.  Two SoC control words a
* stone's throw apart, one of them wearing the other's name, is precisely the
* confusion this project renames on sight.
*
* ★ IT HAD THREE SPELLINGS: a #define here in one driver, a bare literal in the
* second, and a locally-named pointer in the third.  One home now.
*
* ⚠⚠ AND THE TWO DRIVERS DISAGREE ABOUT WHAT BIT 5 IS, which is recorded
* rather than resolved: luna_eth.c calls it "switch-core enable" and
* gpon-rtl960x.c calls the SAME bit at the SAME address "PONPBO IP enable".
* Both set it.  Either one name is wrong, or the bit gates a block both need
* and neither name says so.  OWED: settle it from the chip's own SDK
* (tier 3) or a live read, and rename on the spot -- do NOT pick one by
* majority, which is how two wrong names cancel and survive. */
```

## luna_gpon.c

### /proc/gpon_log ring (2026-10-07)

```c
/* The GPON/protocol log: a ring read at /proc/gpon_log, never printk (operator,
* 2026-10-07: dmesg is for the device itself). Protocol events run in the FSM
* timer, where a console line cost the production OLT's Password deadline.
* Lines keep the dmesg spelling, so a reader only changes its source.
* luna_diag() below is the exception: its dmesg lines wait for a quiet line. */
```

### PI_A_* enum (fragment; rest of the sentence is lost)

```c
/* THE 9602C ADDRESS IS WRITTEN ONCE, IN THE */
```

### cdr_reseat_on_reactivate

```c
/* default ON (A/B 2026-06-15): on a deactivate->O1 re-range, re-pulse the
* softirq-safe US-TX SerDes interface reset-B (WSDS_DIG_1D[16], the same primitive the
* boot path + the O3 re-sync use) so a marginal serializer lock from the prior activation
* is re-attempted immediately, cutting the cold-start O5<->O1 activation flapping. Opt-in
* A/B: TX-interface only (the locked DS RX framer is undisturbed). */
```

### ploam_tx_dbg

```c
/* TEST: log per-send US-PLOAM TX (ENQ self-clear = HW transmitted) to
* prove whether the urgent-queue ACK/Password actually leaves the ONU
* (OLT raises LOAi = never gets our acks). Logs first 40 sends. */
```

### o5_rearm_burst_gate

```c
/* o5_rearm_burst_gate: re-apply the US burst-gate cluster (0x5188/0x526c/0x6024/0x6260)
* and re-arm the HW auto-No_message keepalive template on every O5 entry (not just __init),
* so a re-ranged O5 after a GMAC/SDS reset does not run on US-side reset defaults. Default on;
* A/B with luna_gpon.o5_rearm_burst_gate=0. */
```

### o5_ploam_keepalive_ticks

```c
/* o5_ploam_keepalive_ticks: emit a No_message US PLOAM (HW auto queue 0x7) every N FSM ticks
* (10ms each) while at O5 with an assigned ONU-ID, so a valid PLOAM is present in the OLT's
* granted slots regardless of how the shared US-PLOAM buffer was last written, defeating the
* OLT's PLOAM/ack-liveness timeout. 0 = disabled. Default 100 (~1s). */
```

### o5_provision_watchdog_ticks

```c
/* default 0 = off (proven ineffective, see above) */
```

### o5_ungranted_ticks (X111W, 2026-10-12)

```c
/* o5_ungranted_ticks: leave O5 for O1 after this many ~10 ms ticks with no grant to our ONU-ID
* (the OLT dropped us: "Laser out", Offline) -- the X111W sat at O5 13 min so (2026-10-12). A healthy
* O5 sees grants every second; misses in a row double the wait, up to 32x. */
```

### gpon_data_alloc teardown

```c
/* the OLT's DATA Alloc-ID bound to the DATA T-CONT.
* SESSION STATE: cleared by EVERY teardown, like its three
* siblings above. It used to have no teardown clear at all --
* only the OLT's Assign_Alloc-ID DEALLOCATE, which additionally
* demands the Alloc-ID of the session that just ended. A
* re-config handing out a DIFFERENT one was then refused by the
* install guard, and the only recovery was waiting for an
* Alloc-ID the OLT will never send again: the WAN data T-CONT
* dark until a reboot. Pinned by gpon_data_bind{,_policy}_test. */
```

### gpon_data_gem

```c
/* The WIRE GEM Port-ID the OLT assigned in its OMCI ME 268 (GEM Port Network CTP)
* Create, attribute 1 — set from the eth OMCI RX snoop, and what the data-GEM
* install actually programs. It is the OLT's to choose (measured on this lab OLT:
* 223 to one board, 193 to another), exactly as the OMCC's GEM Port-ID comes from
* Configure_Port-ID. GPON_DATA_GEM_DEFAULT is only the value held before the OLT
* has spoken; the install is gated on gpon_data_gem_solicited, so it is never the
* value that reaches the wire on a provisioned session. */
```

### data_tcont (fragment; rest of the sentence is lost)

```c
/* data_tcont: route the WAN data GEM's upstream to the */
```

### Panel LED map (V2801RGW, 2026-10-07)

```c
/* The panel map is the BOARD's, from its DT gpon node: realtek,pon-led,
* realtek,los-led and realtek,port-leds = <index type>... (hardware link/
* activity). A board that declares nothing gets no lamp driven: the X111W's
* indices on another board drove pins the panel does not use (V2801RGW,
* 2026-10-07). What the lamps SAY is the same on every ONU (below). */
```

### /sys/class/leds registration

```c
/* ---- the STANDARD interface: /sys/class/leds ----------------------------
*
* The panel was already driven (gpon_led_force/_claim/_port above) and the
* lamps were reachable from NOWHERE outside this file, so every led_* suite
* case blocked on "this image exposes no front-panel LED instrument" -- true
* about /sys/class/leds and misleading about the port. This registers the two
* lamps the CPU actually FORCES, so their brightness is the truth and not a
* guess; the driver keeps driving them from GPON state and a userspace write
* is an override the next state change takes back.
*
* ⚠ THE LINK LAMPS (FE/GE) ARE DELIBERATELY NOT REGISTERED. They are
* hardware-auto: the switch lights them from its own port link+activity and
* CPU_FORCE_MOD stays 0, so their force field reads 0 while the lamp is LIT.
* Publishing that as brightness=0 would be a counter that reads healthy while
* describing something else -- the exact defect this tree keeps paying for.
* Reporting them honestly means reading the PORT's link state, which lives in
* the Ethernet driver; that is owed work, not a thing to fake here. */
```

### relatch_us (fragment; "finding 3" is not identified)

```c
/* finding 3 (2026-06-13) */
```

### serdes_recommit

```c
/* 0 = OFF: a full SDS re-commit (CMD_SDS_RST_PS) at O5 DROPS the locked DS framer (omcirx->0, DS dead) — DISPROVEN destructive. */
```

### METER_TB_CTRL vs PON_TB_CTRL

```c
/* METER_TB_CTRL and PON_TB_CTRL have MIRRORED layouts, and this site had
* copied the line above onto a register where the two fields swap places:
* PON_TB_CTRL puts TICK_PERIOD at [7:0] and TKN at [15:8], METER_TB_CTRL
* puts TKN at [7:0] and TICK_PERIOD at [15:8] (the chipdef's own layout,
* structurally identical on 9607C/9601B/9603CVD).  So the bucket refilled
* 43 tokens every 189 ticks where stock refills 189 every 43 -- 19x
* slower, on a meter both firmwares leave ENABLED (METER_OP=1).
* MEASURED on the X111W: live stock reads 0x00012bbd and this board read
* 0x0001bd2b, the same nibbles transposed.  rtl9602c_eth.c already wrote
* stock's word; this site ran later and overwrote it. */
```

### LUT_CFG BCAM_DIS

```c
/* ★★★ THE BINARY CAM, WHICH WE WERE LEAVING DISABLED BY INHERITANCE.
* sw_field() is a read-modify-write that names ONE bit, so every bit this
* init never mentions keeps whatever reset or U-Boot left -- and BCAM_DIS
* arrives SET. Stock clears it: a same-day differential on this board reads
* LUT_CFG 14800bb8 on stock against 00600bb8 on ours, and bit 21 is
* BCAM_DIS in this die's own chipdef.
* ⚠ IT IS A LOOKUP-STAGE BIT, which is why it is tried now and not sooner:
* with the die's log armed at LOG_FIRST_DROP a full transit latched NOTHING,
* so the engine is not looking these frames up and discarding them -- it is
* not looking them up at all, and a disabled search CAM is a mechanism for
* exactly that. */
```

### US and DS pool reservation (X100DG, 2026-09-30)

```c
/* The US and DS packet pools may be RESERVED by the board (DT memory-region,
* US then DS) instead of allocated: the 32 MB X100DG's stock keeps both at the
* two holes of its memory map (MSTBASE_US 0x01eff000, MSTBASE_DS 0x016ff000,
* read on stock 2026-09-30), and taking 1-2 MB from its 19 MB starved it. */
```

### gpon_pbo_init (X100DG Oops, 2026-09-30)

```c
/* Exported like datapath_tables_init, and reached from the NIC's open on a
* board whose DT disables the GPON: X100DG 2026-09-30, NULL ponip_base,
* Oops at pi_hwio_rd inside rtl9602c_eth_open. */
```

### DPRU_RPT_PRD

```c
/* DPRU_RPT_PRD: DBA_BLKSIZE=48 plus report period 2,
* the live-stock 0x3002. The HW DBA/DBRu engine uses
* this byte->block divisor to ENCODE per-SID queued
* occupancy into the report the OLT reads to size
* grants; unset, the OLT sees 0 queued while the
* queue holds pages, grants once and STOPS. Stock's
* only writer is its init; ours had regressed it. */
```

### PON_IPSTS_US

```c
/* 0x20f4 = PON_IPSTS_US, a READ-ONLY init-ready status reg (bit0=PONIC_INITRDY);
* stock never writes it. The old pi_wr(0x20f4,1) was a no-op write to a reserved
* bit -> removed. It is POLLED before the GMII latch edge below (usnic_initrdy_poll). */
```

### WAN-PCS digital enables (G24W, 2026-09-11)

```c
/* The two WAN-PCS digital-enable writes below DIVERGE from stock on
* purpose: guarded off, the G24W measured optic_los=1 sdet=0 and stayed
* at O1 (2026-09-11). A stock-parity sweep must NOT close this gap. */
```

### swcore dump span (NAPT page vs FLOWBASED page)

```c
/* ⚠⚠ THIS SPAN STOPPED ONE PAGE SHORT OF THE ENGINE, and the page
* it did cover is the EMPTY one.  MEASURED 2026-09-16 on two
* independent stock captures of the RTL9603CVD: the NAPT control
* page 0x800000 answers 1024 words and every one is ZERO on that
* die, while the FLOWBASED engine at 0x801000 carries 43 non-zero
* words with the vendor forwarding.  So every capture of OURS held
* the empty page and not one word of the engine this port is
* missing -- swcore_blocks_from_capture said exactly that
* ("NO DECLARED ACCESSOR REACHES 0x00801008.."), and no
* stock-vs-ours diff could ever have shown it.
* ⚠ Both pages are kept: which model a die carries is a DIE fact,
* and a span that covered only the live one here would go blind on
* the NAPT dies where the other page is the live one. */
```

### q32_idle removal (fragment; rest of the sentence is lost)

```c
/* `q32_idle` was REMOVED here (2026-09-10) and the removal is */
```

### pi_packed_get readback

```c
/* Read the `bits`-wide entry back through the SAME locate, so a /proc readback
* shows the TRUE value -- the old contiguous read showed the wrong SID's, which
* is why the SID2QID mis-addressing stayed invisible across many boots. */
```

### WAN data-GEM classify re-assert

```c
/* (2b) WAN data-GEM classify, re-asserted every modeset -- the all-SID loop
* above would otherwise wipe it, the same trap that hid the OMCC SID for ~30
* rounds. SID2QID is the data qid so the data US is reported to the OLT's DBA
* on the data Alloc; the legacy path rides the OMCC's T-CONT 16. */
```

### GMII re-latch location

```c
/* The GMII re-latch used to be here, at Assign_ONU-ID */
```

### luna_vlan_rule_install and ME 171

```c
/* ★★★ THE OLT'S VLAN SERVICE MODEL REACHES THIS FAMILY -- and what it can do
* with it is REPORTED rather than claimed.
*
* The core models the whole of ME 171: it decodes each row, DECIDES it into a
* filter+treatment, and hands it to a family through `struct gpon_vlan_ops`,
* counting four outcomes. Measured 2026-09-16: NO family filled that table and
* NOBODY called the walker, so the entire model -- decode, decide, emit -- was
* dead code, and the service the OLT provisions reached nothing at all. That is
* the same shape as `rtk_tr142.ko`, which is precisely this translation on the
* vendor side, and the same shape as `pf_rg`'s missing half.
*
* ⚠⚠ `rule_install` REFUSES, ON PURPOSE, AND THAT IS NOT A STUB. No Luna VLAN
* rule encoding has been MEASURED for these shapes: writing one from the switch
* VLAN tables because they look close would put an unmeasured rule on a WAN that
* works today, which is the defect this tree calls a value the author supplied
* where a human had to register one. A refusal is a THIRD answer and the ledger
* keeps it apart from "nobody was listening": `failed` means the family was
* asked and said no, `no_installer` means nothing was there to ask.
*
* ⇒ what this buys NOW is the measurement that was missing: the OLT's rows, per
* instance, with the shapes it actually asks for. The next increment implements
* those shapes and not a generic VLAN engine.
*/
```

### ONU_STATE write order (X111W, 2026-10-02)

```c
/* The HW ONU_STATE field uses our 1-based numbers (O1..O5 = 1..5) and
* gates the GTC's auto-SN/ranging transmitter.  Written BEFORE any
* print: the console is synchronous, and a GTC left at O4 behind the
* ranged flush answered the OLT's O5 grants as ranging requests (field
* X111W, production OLT, 2026-10-02). */
```

### gpon_o5_rearm_burst vs core No_message

```c
/* The CLUSTER ONLY: gpon_ploam.h's contract ends with "the core emits the
* No_message that follows it", and it does. Wiring this op to
* gpon_o5_rearm_burst(), which sends one too, put TWO No_messages on the
* wire per O5 entry. The native FSM keeps the pair. */
```

### omcc_alloc_override and gpon_omcc_alloc

```c
/* The STATIC default only: 0 means "use the LIVE ONU-ID", which is the rule.
* The live copy below carries gpon_omcc_alloc, a module_param the native FSM
* honours -- nothing copied it here, so with core_fsm=1 the SAME knob was
* silently ignored while core_fsm=0 obeyed it (ploam_fsm_diff_test W3). */
```

### o5_window_ticks

```c
/* DEBUG (CONFIG_GPON_PLOAM_DIAG): the first N ticks of every O5, one line per
* tick -- what the upstream did with the grants the GTC accepted. Built for an
* OLT we cannot see: "grants accepted, nothing sent" (2026-09-30, production
* OLT). gpon_rd only: pi_rd hangs this context. */
```

### luna_lan_keep_open_poll (X100DG, 2026-10-01)

```c
/* Ahead of the activation gate: a board whose activation never becomes ready kept
* VLAN_FILTER on and its LAN dark (X100DG, 2026-10-01). */
```

### feed_rekick

```c
/* feed_rekick: per-tick self-terminating US-feed FIFO re-arm. The one-shot
* O5-entry re-arm is re-parked by later SerDes resets before the OLT's first
* grant, so pages stage in PON-IP SRAM but never build a DRAM descriptor and
* the framer emits nothing. Auto-stops once gemus_omcc advances. */
```

### us_intr_svc

```c
/* us_intr_svc: ack the upstream GPON interrupt deltas the known-good unit
* services on every GTC_US event. The reads clear the sticky latch; if the
* fetch FSM was back-pressuring on it, the payload framer unstalls. */
```

### pi_rd in the fast O5 print

```c
/* rxsid/ustx/dirty were REMOVED from this fast O5 print: pi_rd in this
* context reproducibly HANGS the poll right after OMCC install (two boots
* identical). It is an indirect polled PON-IP access, unsafe here unlike
* sw_rd/gpon_rd; read those counters through /proc instead. */
```

### Board pad routing (X100DG, 2026-10-01)

```c
/* Board pad routing: IO_GPIO_EN w0 w1, GPIO dir/data ABCD, dir/data EFGH. A board
* that declares none keeps its bootloader's pads (the X100DG's I2C bus 1, 2026-10-01). */
```

### luna_c2_* settings before luna_ponmac_mode_set

```c
/* A/B: skip the post-GPON-mode US-TX perturbations (default = skip, stock rev-A) */
```

```c
/* A/B: SDS reset = stock bit0-only (default) vs legacy bit7+bit0 */
```

```c
/* A/B: match live-stock SDS REG01/REG11 post-reset (default) */
```

```c
/* A/B: program full analog CMU/CDR table AFTER the SDS reset (stock rev-A, default) = cold-start determinism */
```

```c
/* A/B: TX-CMU-lock settle before reset-B (cold-start metastability candidate) */
```

```c
/* A/B: clock-gated reset-B release (rank-1 cold-start fix candidate) */
```

```c
/* A/B: skip the gratuitous DIG_1D reset-B pulse (already released) */
```

```c
/* A/B: skip the over-configure golden-table writes (match stock's minimal set) */
```

### DS_CFG FEC_DET_THRSH (2026-09-08)

```c
/* DS_CFG's FEC detection threshold, asserted deliberately (2026-09-08): the
* bootloader left this board at FEC_DET_THRSH=1 while its own stock rests at
* 0, and the vendor sets it from configuration in its init path. Not a BWmap
* acceptance gate -- all four of those already matched. */
```

### DS_CFG BWM_NO_FLT

```c
/* DS_CFG BWM_NO_FLT = 0 (stock). The
* bring-up =1 "accept all grants" left bwm_acpt=0
* against stock's 130k+, i.e. it broke the BWMAP
* parser rather than relaxing it. */
```

### DS PLOAM ONUID_FILTER

```c
/* DS PLOAM ONUID_FILTER ON:
* every Assign_ONU-ID this OLT sends is BROADCAST, so BC_ACC_EN already
* delivers it. What the filter buys is real -- this bench puts two ONUs on
* one splitter, and with it off we also accepted the other unit's PLOAMs.
* Stock reads GPON_GTC_DS_PLOAM_CFG = 0x70b with the bit SET. */
```

### omcc_alloc_override and gpon_omcc_alloc

```c
/* The OMCC Alloc-ID override: the native FSM reads gpon_omcc_alloc directly,
* so the core must get the same value or the two FSMs bind the OMCC T-CONT
* to different allocs under one module parameter. */
```

### early_dwell_report_ticks wiring

```c
/* The fourth, the same omission again: the core read early_dwell_report_ticks
* and nothing gave it a value, so the O1 diagnostic was compiled in and MUTE.
* ONU-test-case/dead_knob_guard.py catches this shape. */
```

## luna_gpon_nic.h

### Control-frame queue (X100DG, V2801RGW, 2026-10-04)

```c
/* The CPU's WAN control frames ride their own flow on the data T-CONT's next queue
* (luna_gpon.c ctrl_queue): an offloaded flood fills the data queue, and the PPP LCP
* echo replies queued behind it were lost (X100DG, V2801RGW, 2026-10-04). */
```

## luna_gpon_regs.h

### File header: per-SoC split

```c
/*
* RTL9602C GPON/SWCORE register offsets -- the PER-SoC half of the driver.
*
* ★★★ WHY THIS FILE EXISTS (operator, 2026-08-27: *"la majoria de la logica
* devria estar en la familia, y solo workaround/offset especifica al SoC"*).
* The driver it was extracted from is 8028 lines, and a measurement of its own
* shape says the split is almost already there:
*
*     105 functions, of which only 6 carry a chip conditional
*     180 register/offset #defines  <- THIS FILE
*      70 module_param knobs        <- workarounds, some genuinely per-chip
*
* 99 of 105 functions never ask which chip they are on. What tied them to one
* SoC was not their logic, it was these constants -- so moving the constants
* out is what lets the logic be family code.
*
* ★ COMPILE-TIME, NOT A RUNTIME TABLE, and that is deliberate. This project
* ships ONE LEAN KERNEL PER MODEL (a GPON ONU has 16 MB of flash and <=64 MB
* of RAM), so a second chip is a second header selected by Kconfig -- not a
* pointer indirection on every register access in the datapath hot path.
* `luna_ponmac.c` uses a runtime table because it must serve four chips
* from one object; this header is the other, cheaper case.
*
* ★ ADDING A CHIP = COPYING THIS FILE AND EDITING THE VALUES. Nothing else.
* That is the whole point of the split: the next Luna part costs a table, not
* a fork of the logic.
*
* ⚠ THIS IS PURE CODE MOTION. It was verified by building before and after and
* comparing the object file byte for byte -- see the commit message. No value
* was changed, no define was renamed, and the file order is preserved so the
* one define whose value references another still resolves.
*/
```

### SC_CMD, SC_DATA vs GPON_INTR_MASK/STATUS offsets

```c
/* ★★ THE SAME TWO OFFSETS IN THE **SWCORE** WINDOW ARE A SERIAL-COMMAND
* CHANNEL, NOT INTERRUPTS.  sc_ldo_init() drives 0x40/0x44 through sw_wr/
* sw_rd to issue an SC read of reg 0xfdca, clear the DRAM-LDO bits and
* commit -- nothing to do with the GPON block's interrupt mask/status,
* which is what those two names mean in the GPON window.
*
* Writing sw_wr(GPON_INTR_MASK, ...) was a NAME THAT LIES: a reader sees
* an interrupt register and the silicon sees a command port.  This
* project renames such a name the day it is proven wrong, and
* luna_reg_alias_test proved it -- it derives a register's window from the
* ACCESSOR it is used with, so one register reached through both gpon_*
* and sw_* is a contradiction it reports by name and line.
*
* The GPON names keep their meaning in their own window; these are for the
* SWCORE window only, and they belong to the SC_* family SC_IND_WD is in. */
```

### GPON_EQD_FRAME_LEN removal

```c
/* ⚠ GPON_EQD_FRAME_LEN LIVED HERE AND IS GONE: it is not a register, it is the
* G.984 UPSTREAM FRAME LENGTH IN BITS, and the core already declares it as
* GPON_PLOAM_EQD_FRAME_LEN with the same value.  Two names for one spec fact,
* in two headers, is how the equalization-delay arithmetic in this driver and
* the one in the core come to disagree -- and the day they disagree the ONU
* ranges to the wrong offset, which does not look like a constant problem at
* all.  The driver takes the core's name; this header keeps only registers. */
```

### GPON_GTC_US_PLOAM_CFG PLM_FLUSH_BUF resting level

```c
/* ★ THE RESTING LEVEL IS 1, NOT 0.
* The chipdef calls it PLM_FLUSH_BUF
* (lsp 4, len 1).  The vendor's
* usPloamBuf_flush() writes 0 THEN 1
* and stops, and this board's own
* stock reads US_PLOAM_CFG = 0x13
* live at O5 -- so 1 is the operating
* level and the 1->0->1 dip is the
* flush.  Resting at 0 leaves the
* transmit buffer permanently
* flushing: MEASURED 2026-09-08,
* every CPU PLOAM cleared ENQ in 0us
* and PLOAM_CPU_TX stayed 0. */
```

### I2C CONFIG/IND register stride

```c
/* The CONFIG and IND registers are two-entry arrays with a 32-BIT array offset
* in the 9602C, 9603CVD and 9607C chipdefs, so bus 1 is the NEXT WORD. Stock
* X100DG (BOSA on bus 1) shows it live: 0xb4/0xbc/0xc4/0xcc carry its last
* transfer and 0x23008 = 0x0235001e (slave 0x54) -- 2026-09-30. It was 0x20. */
```

### I2C clock divider (0x270 = 50 kHz)

```c
/* ★★★ THE NAME AND THE COMMENT WERE BOTH WRONG BY A FACTOR OF TWO, AND THE
* VALUE IS UNCHANGED (renamed 2026-09-12). This read I2C_CLKDIV_100K with
* "(62500/100)-1 -> ~100 kHz". The parent clock is 31250, not 62500 --
* verified in BOTH dies' own stock: dal_rtl9602c_i2c_clock_set @0x801f42c4
* and the RTL9603CVD clock_set @0x803125a8 each load 31250, divide by the
* wanted kHz and subtract 1. So 0x270 = 624 gives 31250/625 = 50 kHz, half
* of what the name promised, on BOTH VERIFIED DIES. The RTL9607C's formula is
* NOT verified, so this says nothing about that part.
*
* ⚠ THE VALUE IS DELIBERATELY NOT TOUCHED, AND THE REASON IS NARROWER THAN THE
*   FIRST DRAFT OF THIS COMMENT CLAIMED. The X111W is healthy at this divisor,
*   which refutes exactly one thing: "50 kHz is universally unusable". It does
*   NOT clear the divisor for the G24W, because the two boards carry DIFFERENT
*   BOSA PARTS -- an RTL8290B here, a Semtech GN25L95 there -- and what one
*   I2C slave tolerates is not evidence about the other.
*   ⇒ G24W clock causality is UNPROVEN: neither established nor refuted. The
*     demonstrated gap there is the MISSING GN BACKEND, not this number.
*
* ⚠ AND EACH DIE'S STOCK AIMS ELSEWHERE AGAIN, so one shared divisor matches
*   NEITHER: the X111W's own stock sets ~1000 kHz (europa_drv.ko
*   rtl8290b_i2c_init .text:944 calls rtk_i2c_clock_set with a1=1000, matching
*   its stock CONFIG 0x0235001e = div 30), and the G24W's stock uses div 314,
*   ~99.2 kHz. A declared gap owed a per-die table, not a number to guess --
*   and a comparison alone never justifies a hardware experiment.
*/
```

### PI_ names for /proc/gpon registers

```c
/* ★ NAMES THE TWO CHIPDEFS AGREE ON, for the registers /proc/gpon reads
* (2026-09-04).  gpon_proc_show() spelled 76 bare offsets; these 34 are the
* ones an ORACLE names -- `bare_offset_chip_audit.py` resolves each address
* in the rtl9602c AND rtl9603cvd chipdefs, in the block its accessor implies,
* and only a SAME verdict is written here.  The PI_ prefix follows this
* header's own convention for the PON-IP window (PI_MEDIA_STS_US is
* MEDIA_STS_US in both chipdefs); nothing else about the name is ours.
*
* ⚠ THE OTHER 41 STAY BARE HEX, ON PURPOSE.  No source in reach names them:
* absent from both chipdefs, or named by only one, which is a difference we
* cannot adjudicate.  Inventing a plausible name for those would put a
* fabrication in the one place a reader would trust it most. */
```

### GPON_GTC_DS_PPS_CTRL

```c
/* The next GTC DS word, read by gpon_proc_show() and previously spelled as the
* bare literal 0x1050 under the label "r1050".  The RTL9603CVD chipdef names it
* GPON_GTC_DS_PPS_CTRL at 0x701050 (tier 4, the sibling die); the RTL9602C
* chipdef does not name it, and its two neighbours 0x1048/0x104c carry the same
* names on BOTH dies, so the block is the same one.  A bare literal here also
* bypasses nothing (gpon_rd is not relocated), but an unnamed address in a
* /proc label is a reader sent to a register nobody can look up. */
```

### GPON_GTC_DS_ACTIVE counter split

```c
/* ⚠ NOT an "active" count: [31:16] CNTR_RANGING_REQ, [15:0] CNTR_SN_REQ
* (rtl9602c chipdef field names, tier 3) -- the ranging requests and the
* Serial_Number requests the GTC RECEIVED.  The chipdef's register name is
* kept; every reader splits it (GPON_GTC_DS_ACTIVE_*).  Clear-on-read like
* its neighbours (measured 2026-09-07). */
```

### RX_SID_GOOD_CNT_US groups

```c
/* ★ THE GOOD-COUNTER GROUPS 1..4 WERE BARE LITERALS AT THEIR TWO READERS
* (0x2040 0x2044 0x2048 0x204c in gpon_proc_show_locked() and in the FSM poll's
* USDIAG line).  Both chipdefs declare RX_SID_GOOD_CNT_US@stride = 4 and
* @entries = 5, at the SAME address 0x0203c on the RTL9602C and the RTL9603CVD,
* and the next NAMED register above it is RX_ERR_CNT_US at 0x02050 -- a 0x14
* gap, which is 5 x 4.  Spelling the groups as base + n * stride is what makes a
* relocation of the base carry its own array, and it is the same shape
* PI_TX_SID_CNT_US / PI_RX_SID_CNT_US already use.  A bare PI literal also
* bypasses the per-chip pi_x() relocation, a defect class this file records. */
```

### CNT_MASK_US word layout

```c
/* ★ ONE SPELLING OF THE COUNTER-MASK WORD, for the writer AND the reader.
* CNT_MASK_US gates RX_SID_GOOD/BAD_CNT_US: one bit per SID, FOUR groups of a
* 128-bit mask [tier 3, the RTL9603CVD chipdef], so group g's word for SID s is
* base + g*16 + (s/32)*4, bit s%32.  Four 16-byte groups end exactly where the
* chipdef puts RX_DROP_CNT_US, which is the arithmetic cross-checking itself.
* The enrolment site spelled this by hand and gpon_proc_show_locked() read the
* BARE literal 0x20ac -- group 0's word for SIDs 32..63, which is enrolled on
* NEITHER declared board (the RTL9602C's OMCC SID is 64 -> word 2, the
* RTL9603CVD's is 127 -> word 3), so that read is zero BY DESIGN -- and it was
* printed under the label "ctl_us", which is PI_PONIP_CTL_US, a different
* register at 0x020d8 / 0x020ec.  A witness that cannot fire is worse than no
* witness: it answers. */
```

### GPON_BWMAP_DATA layout

```c
/* ★ GPON_BWMAP_DATA IS AN ARRAY OF 256 32-BIT WORDS, and gpon_proc_show() reads
* it as 128 captured ALLOCATIONS of two words each (named 2026-09-05).  The
* stride is the chipdef's: "array offset 32" (32 BITS = 4 bytes), index 0..255,
* identical on the RTL9602C and the RTL9603CVD.  The pairing is the consumer's
* own loop -- it always read "GPON_BWMAP_DATA + i * 8" and "0x02404 + i * 8",
* i.e. words 2i and 2i+1 -- which is a G.984.3 BWmap allocation structure
* (Alloc-ID, flags, SStart, SStop, CRC = 8 bytes).  Words 1..5 were spelled
* as 0x02404..0x02414; they are BWMAP_DATA(1)..BWMAP_DATA(5). */
```

### Bandwidth-map capture engine

```c
/* The capture engine's controls -- the rtl9602c and rtl9603cvd chipdefs name
* these three fields identically at these offsets (tier 3), and the vendor's
* own reader arms it as: write 0, write CAP_CLR|frames, write CAP_EN|frames.
* ★ MEASURED on the RTL9602C 2026-09-07: CAP_FRAME_NUM=32 captures 32 frames
*   and CAP_EN then self-CLEARS (a done flag the tree used to say did not
*   exist); CAP_FRAME_NUM=0 keeps CAP_EN set and captures until read.  And
*   the buffer holds only allocations the GTC ACCEPTED: 20 s of capture with
*   a second ONU Online on the same PON held ONE entry -- ours -- and none of
*   the other ONU's grants.  It is a post-filter witness, not a wire tap. */
```

### TCONT_IDLE_BYTE_STAT, GEM_US_BYTE_STAT stride

```c
/* ★ THE TWO US EMISSION COUNTER ARRAYS, NAMED 2026-09-05. Both were reached by
* bare hex from luna_gpon.c (0x6a00, 0x6c80, 0x6c40, 0x6810...) while their
* BASE and STRIDE were spelled only in a driver comment. The names and the
* arithmetic are the tree's own (luna_gpon.c:6785-6218): "TCONT_IDLE_BYTE_STAT
* array base 0x6c00, stride 8 BYTES (register-map 'array offset 64' = 64 BITS),
* 64-bit/entry" and "GEM_US_BYTE_STAT base 0x6800, stride 8". Nothing invented.
* ⚠ THE STRIDE IS 8 BYTES AND THE REGISTER MAP SAYS 64: the map counts BITS.
* A comment three lines above the consumer read that 64 as bytes and computed
* TCONT_IDLE_BYTE_STAT[16] = 0x6c00 + 16*64 = 0x7000; the code reads 0x6c80,
* which is 0x6c00 + 16*8. Naming the stride is what stops that arithmetic being
* re-derived by hand a third time. */
```

### TCONT_IDLE_BYTE_STAT rename

```c
/* ⚠ RENAMED 2026-09-05: this was GPON_TCONT_IDLE_BYTE_STAT for one day.  The
* chipdef's name -- on the RTL9602C, the RTL9603CVD and the RTL9607C alike --
* is TCONT_IDLE_BYTE_STAT with no GPON_ prefix, and the driver comment this
* name was taken from spells it that way too; `bare_offset_chip_audit.py
* --names` reported the prefixed spelling as WRONG.  Same address, same
* stride, same consumers (TCONT_IDLE_STAT(n) below). */
```

### PKT_OK_CNT_US

```c
/* ★ THE UPSTREAM TWINS, AND THEY WERE NEVER READ.  Ten comments in
* luna_gpon.c reason about "PKT_OK/ERR/MISS all 0" on the US-NIC as the
* discriminator between "the frame never reached the US-NIC" and "it reached
* it and was dropped later", and until 2026-09-08 no line of code took the
* reading: only the DS three above existed.  The witness that WAS taken,
* RX_SID_GOOD_CNT_US, is MASK-GATED -- it counts only SIDs enrolled in a
* CNT_MASK_US group -- so a mis-enrolled mask makes it read zero whatever the
* hardware did, which is precisely the trap gpon_install_omcc() records having
* fallen into on the G24W.  PKT_OK_CNT_US is UNGATED: it counts every frame the
* US-NIC MAC accepts, no classification involved.  Same offsets on both
* chipdefs (0xF04010/14/18), so no per-chip move.
* PKT_OK_CNT_US packs TWO counters: TX_OK_CNT[31:16] and RX_OK_CNT[15:0]. */
```

### US PBO DRAM pool

```c
/* US PBO DRAM pool. ROOT CAUSE of "US-NIC RX never receives" (2026-06-12, found
* via the stock memory-usage init behavior + live devmem): the US-NIC RX path
* is a PBO that DMAs received US packets into a DRAM pool at IP_MSTBASE_US. Stock
* sets IP_MSTBASE_US=0x07eff000, RAM_NO=0x1fff, DRAM_RUNOUT=0x1f58 (a 1MB / 8192x
* 128B-page DRAM pool).  Our driver did US SRAM-only too (copying DS) -> the
* US-NIC RX had no DRAM pool to land frames in,
* so EVERY US OMCI frame was dropped at descriptor-fetch BEFORE the MAC, leaving
* PKT_OK/ERR/MISS all 0 (the exact symptom). Allocate the pool and point the HW
* at it. Stock memory-usage init math (usPageSize=128, us_mem_size=1MB): SRAM_NO=0x7f,
* RAM_NO=min(size/128,0x1fff)=0x1fff, DRAM_RUNOUT=size/128-40-128=0x1f58. */
```

### DS PBO SRAM-only vs DRAM per chip

```c
/* ★★★ AND "DS IS SRAM-ONLY" IS AN RTL9602C FACT, NOT A FAMILY ONE (2026-09-08).
* The note above said so flatly; the G24W's OWN stock, read live at O5 while the
* OLT listed it Online, disagrees on the RTL9603CVD:
*     IP_MSTBASE_DS  (0x0a0d0) = 0x06c00000   ours 0
*     PON_DSC_CFG_DS (0x0a0e4) = 0x1fff8007   ours 0x001f001f
*     DSCRUNOUT_DS   (0x0a0cc) = 0x1fcb0006   ours 0x0000001e
*     PON_FC_CONFIG_DS(0x0a110)= 0x1f950036   ours 0x00020016
* i.e. stock gives the DOWNSTREAM PBO the same 8192-page pool it gives the
* upstream one, and we gave it 31 SRAM pages.  MEASURED consequence on this
* board: the OLT sent 13 DS OMCI frames on the OMCC flow right after
* Configure_Port-ID, RX_DROP_CNT_DS (0x0a060) went to 12 where stock's is 0, and
* PON_DSC_USAGE_DS / PON_DS_PBO_PAGE_Q0 never left 0 -- so the OMCI reached the
* GEM layer, found no descriptor page, and was dropped before the trap.  The
* geometry is therefore a PER-CHIP TABLE entry (gpon_swc_map.ds_dram_*): a chip
* that declares order 0 keeps the SRAM-only path byte for byte. */
```

### PON_SID_RPV_TH stride and entry count

```c
/*
* ★ CLOSED 2026-09-08, BY ARITHMETIC ON TWO CHIPDEFS AND NO BOARD.  The open
* question this block used to carry -- whether PON_SID_RPV_TH's "array offset
* 32" means 32 BITS (4-byte stride) or 32 BYTES -- is settled by where each
* chip's array ENDS, because on both dies the next declared register sits
* exactly one entry past the last one:
*
*     RTL9602C    0x02458 + 65 * 4 == 0x0255c == that chip's PONIP_DBG_CTRL_US
*     RTL9603CVD  0x026c8 +  8 * 4 == 0x026e8 == that chip's PONIP_DBG_CTRL_US
*
* Two independent chipdefs agreeing on a 4-byte stride is the second source
* this project's own rule asks for, so the stride below is CONFIRMED and the
* live stock read it used to ask for is no longer owed.
*
* ★★ AND THE ENTRY COUNT IS NOW A PER-CHIP TABLE FIELD, gpon_swc_map's
* .sid_rpv_entries -- 65 and 8, not one number.  PI_SID_NUM (65) used to bound
* this loop for every chip and is RETIRED rather than left to be reached for:
* the same arithmetic that closes the stride proves the overrun it caused,
* 57 registers of the RTL9603CVD's upstream scheduler written by a loop that
* had no business leaving the array.  A count that is right for one die is a
* defect the day a second die exists.
*/
```

### SW_SCH_WFQ_TKN_CTRL, SW_LINE_RATE_2500M

```c
/* ★ THE SWCORE REGISTERS rtl9602c_datapath_tables_init() AND
* rtl9602c_ponmac_modeset_gpon() WROTE AS BARE HEX (named 2026-09-05).  The names
* are the RTL9602C chipdef's own and were already spelled in the driver's
* trailing comments beside each write.  luna_eth_regs.h is the natural home for
* SWCORE names (its SW_ prefix is the one `bare_offset_chip_audit.py --names`
* verifies) and moving these there is OWED; this header is the one this pass
* could edit.
* ⚠ SCH_WFQ_TKN_CTRL and LINE_RATE_2500M exist in the RTL9602C chipdef ONLY --
*   the RTL9603CVD names nothing at either address -- so like the function that
*   writes them they are RTL9602C facts, not family ones. */
```

### SW_P_MISC

```c
/* P_MISC is the per-port MAC misc register: ONE WORD PER SWITCH PORT, at the
* chip's per-port register interval.  Base 0x20004 is P_MISC in all three
* chipdefs (RTL9602C: "port index 0..3").  The INTERVAL is per chip and this
* tree already carried it twice: rtl9602c_eth.c -- "P_MISC = 0x20004 +
* port*0x400 (the chip's per-port register interval is 0x400 -- the old
* 0x20-stride wrote unrelated registers and RX_SPC never landed)" -- and
* luna_ponmac.c, MACPP_INTERVAL 0x100 on the RTL9603CVD/RTL9607C.  The vendor
* SDK states it as data: rtl9602c_macPpInfo = { 0x20000, 0x203FF, interval
* 0x400 } (sdk/src/hal/chipdef/chip.c), 0x100 for the 9603CVD and 9607C.  So
* P_MISC[2] (PON) = 0x20804 and P_MISC[3] (CPU) = 0x20c04, which is what the
* RTL9602C code has always written.  A LITERAL 0x20804 in a shared file would
* be P_MISC[8] on the other two chips -- the reason a name matters here. */
```

### SW_MACPP_STRIDE_9602C

```c
/* ★★★ THE STRIDE IS NOT FAMILY, AND ITS NAME NOW SAYS SO (2026-09-05).
* This was `SW_MACPP_STRIDE 0x400u` in THIS header -- which the RTL9603CVD
* also includes -- while the comment four lines up says that chip's macPpInfo
* interval is 0x100. An unsuffixed name in a shared header invites exactly the
* write it warns against: on the 9603CVD, base + 2 * 0x400 is P_MISC[8].
* The tree already has the convention for this (SW_IO_MODE_EN_9607C, :453).
* ⚠ AND IT WAS DEFINED TWICE. rtl9602c_eth.c carried its own SW_P_MISC
* 0x20004 + SW_P_MISC_STRIDE 0x400u AND includes this header, so SW_P_MISC was
* two macros in one translation unit -- identical token sequences, therefore
* silent under -Wall -Wextra today and a redefinition error the day either
* side is edited. Two agents landed them the same hour in files that were
* disjoint by filename and not by content. One home, one spelling, and the
* arithmetic is asserted once instead of at nine call sites. */
```

### GPON_GEM_US_PORT_MAP stride

```c
/* array: base 0x1400, STRIDE 4 bytes, idx
* 0..127, [4:0] traffic-type. The register map's
* "array offset"=32 is in BITS (32b = 4 bytes), NOT
* bytes — confirmed on a live online stock ONU whose
* tcfg[64] sits at 0x1500 (=0x1400+64*4)=0x4, with
* 0x1c00 empty. (A 0x20 stride wrote outside the
* 0x1400..0x1600 array, into the void.) */
```

### GEM_US_PORT_MAP_IDX_MAX

```c
/* The LAST valid index of that array.  It was carried only in the sentence
* above until 2026-09-01, which meant the host suite hand-typed 127 and could
* not tell a real bound from a stale one -- `gpon_gem_us_test` said so in its
* own output.  Named here so the number is EXTRACTED, and consumed by the
* compile-time GPON_GEM_US_RANGE_OK() assertions on the two flows this driver
* actually stamps, exactly as cortina-gpon.c does with CG_US_PORT_IDX_MAX.
*
* ⚠ THE OBVIOUS GREP IS A TRAP AND COST A WRONG ANSWER ONCE: PI_US_SRAM_NO is
* also 127 and is a different block, and the retired PI_SID_NUM was 65 -- a
* count, not a maximum, of yet another block, which read as 64 while still
* passing every range this driver used.  A write past the end lands in an
* unrelated register and is accepted without complaint -- which is exactly
* what it did, on the RTL9603CVD, until 2026-09-08 (see PI_RPV_TH_STRIDE). */
```

### GPON_GTC_DS_TDM_PTI, GPON_GTC_DS_ETH_PTI

```c
/* ★ THE TWO PTI REGISTERS BESIDE THE OMCI ONE, NAMED 2026-09-05.  gpon_install_omcc()
* wrote them as 0x1200 / 0x1208 under a guessed description ("general DS-PTI",
* "eth DS-PTI").  Both vendor chipdefs (RTL9602C and RTL9603CVD, GTC block 0x70;
* the RTL9607C agrees) name them GPON_GTC_DS_TDM_PTI and GPON_GTC_DS_ETH_PTI,
* each with four fields like GPON_GTC_DS_OMCI_PTI, and `bare_offset_chip_audit.py
* --names` re-checks these names against the chipdefs on every run.  What the
* driver writes -- stock's 0x11 into all three -- is unchanged. */
```

### GPON DS Eth filter value 0x59

```c
/* stock O5 operating value (read live from an online stock ONU): BROADCAST_PASS(6)|NON_MULTICAST_PASS(4)|FCS_CHK_EN(3)|bit0. The earlier 0x18 (no broadcast/bit0) was a wrong "avoid US stall" guess — stock runs 0x59 stably online with OMCI flowing. */
```

### luna_gpon_chip register table

```c
/* ★ THE RTL9602C's GTC offsets AS DATA -- the first populated `struct
* gpon_chip` (flowcore/regtable.h), per that header's own brief: "ADDING A
* CHIP IS ADDING ONE INITIALISER BELOW".
*
* VALUES ARE LITERALS ON PURPOSE, the same idiom as rtl9602c_sw_map in
* luna_eth_regs.h: `regtable_vs_sdk.py` parses this initialiser structurally
* and confirms every address against the RTL9602C's OWN vendor chipdef
* (rtk_rtl9602c_reg_list.c: GTC block 0x70xxxx, array-offset 32 BITS = 4-byte
* stride, idx 0..127) -- a macro reference here would blind that oracle.  Each
* value is ALSO the value of the named #define above/beside it, and
* dev/rtl9607c-test/gpon_regtable_diff_test extracts BOTH spellings from this
* tree on every build and fails if they ever disagree, so the two copies
* cannot drift silently.
*
* Offsets are WITHIN the GTC block (base 0x1b700000 belongs to the shell's
* gpon_io), exactly as gpon_rd/gpon_wr already address it.
*
* ★★★ EVERY OFFSET IS SPELLED REG_AT(...) SINCE 2026-09-14, and the literal
* inside it is still the literal both oracles read.  The wrapper is what makes
* a FORGOTTEN field impossible to miss: C zero-fills any member a designated
* initialiser omits, and offset 0 is a real register on these parts, so before
* this the omission became "the register at 0" and reg_has() called it PRESENT.
* Now it decodes to UNSET and every helper refuses with -ENXIO.  The remaining
* plain values -- the two STRIDES and the three INDEX MASKS -- are not offsets;
* they are covered by reg_table_registered_guard.py, which requires every field
* of this initialiser to be written down (a stride left at 0 stamps every flow
* into ONE slot, which is the GEM_US_PORT_MAP regression in its other form). */
```

### PI_ literals named for gpon_pbo_init etc.

```c
/* ★ ADDED 2026-09-01, because bare_offset_chip_audit found these written as RAW
* LITERALS in gpon_pbo_init / gpon_install_tcont / rtl9602c_datapath_tables_init
* -- and every one of them names a DIFFERENT register on the RTL9603CVD. A
* literal bypasses the per-chip translation entirely, so the macro is not
* cosmetic here: it is the only thing that makes PI_X() see the address.
* Names and 9602C addresses are the vendor chipdef's own. */
```

### PON_TCONT_EN, PON_SCH_QMAP upstream-scheduler cluster

```c
/* ★★★ ADDED 2026-09-08 -- THE WHOLE UPSTREAM-SCHEDULER CLUSTER WAS STILL BARE.
* The two passes above caught the literals `bare_offset_chip_audit` could see;
* it audits only the functions it declares as SUBJECTS, so every literal in
* gpon_pbo_init / gpon_install_tcont / the /proc dump stayed invisible and the
* PON-IP move table -- which is GENERATED from the PI_* names in THIS header --
* could not contain a register this header never named.  A generator whose
* population is a header is blind to whatever the driver spells as a number.
*
* MEASURED on the G24W (RTL9603CVD), 2026-09-08, ours vs its own stock at O5:
*   PON_TCONT_EN   0x025fc   stock 0x00010001, ours 0x00000000
*   PON_SCH_QMAP   0x025e8   stock 0x0000000f, ours 0x00000000
* ...while OUR values sat at the RTL9602C addresses (0x023e4 = 0x00010001,
* 0x023a0 = 0x0000000f), which on this die are UNDECLARED holes.  The upstream
* T-CONT enable -- the register that lets the PON-IP scheduler drain T-CONT 16
* at all -- had never reached the silicon.  Right value, wrong register, and a
* read-back that agrees with itself because it reads the same wrong register.
*
* Names and RTL9602C addresses are the vendor chipdef's own (tier 3); the
* RTL9603CVD side is that chip's own chipdef, and the two live reads above are
* tier 1 on both firmwares. */
```

### Indirect counter muxes (GTC names vs PI_ names)

```c
/*
* The three flow-indexed indirect COUNTER muxes + the MISC mux -- names and
* addresses are the vendor chipdef's own, IDENTICAL on the RTL9602C and the
* RTL9603CVD (GPON_GEM_DS_RX_CNTR_IND 0x704040 / _STAT 0x704044,
* GPON_GEM_DS_FWD_CNTR_IND 0x70404C / _STAT 0x704050, GPON_GEM_DS_MISC_IND
* 0x704064 / GPON_GEM_DS_MISC_CNTR_STAT 0x704068, GPON_GTC_DS_PORT_CNTR_IND
* 0x701140 / _STAT 0x701144).  Until 2026-09-03 the driver reached the first
* two pairs through PI_TX_CFG_US / PI_RX_CFG_US / PI_CFG_US -- PON-IP-block
* names whose NUMERIC offsets coincide with these GTC-block registers -- and
* the last two pairs as bare hex.  A name from the wrong block is a defect:
* it reads as "the US NIC config is being polled" while the code is reading
* a DS counter.  The STAT registers are all CLEAR-ON-READ.
*/
```

### Optical-LOS forcing bits (GPON_CDR_LOS_EN, FIB_FP_CFG_FRC_SD, WSDS_*)

```c
/*
* ★ RESCUED FROM THE board/g24w MERGE, 2026-08-28. These five existed only
* on that branch: the extraction of the 180 offsets happened on board/x111w
* and the G24W work added these afterwards, so a merge that simply took the
* extracted header would have DROPPED them -- and they are the optical-LOS
* forcing path, which is exactly what a GPON board needs to detect a pulled
* fibre. Checked one by one against the header before resolving, rather than
* discovered later by a link error.
*/
```

### SOC_IO_GPIO_EN and other offsets moved to a per-chip table

```c
/*
* ★★★ 8 OFFSETS WERE REMOVED FROM THIS HEADER ON 2026-08-28, and the reason is
* the architecture, not a conflict resolution. They live in gpon-rtl960x.c as
* a RUNTIME lookup -- `SOC_IO_GPIO_EN` is `(swc->io_gpio_en)`, not 0x00048 --
* because the G24W work had already turned the SWCORE map into a per-chip
* TABLE while the extraction that created this header turned the same
* registers into per-chip LITERALS. Two branches, opposite directions, same
* registers; the merge had to pick one.
*
* The table wins, and it is the operator's own target: a compile-time header
* gives ONE chip per object, so shared logic is still compiled twice and can
* still diverge between the two builds. A table is chosen by pointer, so one
* compiled function serves every part and a new chip is an initialiser a human
* can diff against the last one -- which is what "a port is a list of
* registers" actually means.
*
* ⚠ THE DISAGREEMENT WAS SILENT AND THE BUILD FOUND IT: same names, different
* values (0x00048 against swc->io_gpio_en), and -Werror=macro-redefined is the
* only reason anybody looked.
*/
```

### Unused defines kept for the LOS path

```c
/*
* ★ 23 UNUSED DEFINES WERE DELETED ON 2026-08-28 (operator: "borrar los no
* usados"). Nothing in this directory referenced them.
*
* ⚠ FIVE WERE KEPT DESPITE BEING UNUSED: FIB_FP_CFG_FRC_SD, GPON_CDR_LOS_EN
* and the three WSDS optical-LOS forcing bits. They were rescued from the
* board/g24w merge hours earlier -- the code that uses them has not landed
* yet, and deleting them now would mean re-deriving the LOS path a second
* time. Unused TODAY is not the same as unused.
*/
```

### SMI/MDIO master sharing and the unchecked poll

```c
/* ---- the SMI/MDIO master, and the SWCORE proxy built on it -----------------
*
* ★★ FAMILY, TAKING THE MAPPED BASE -- the (hwio) conversion.  These four were
* written TWICE, in gpon-rtl960x.c and rtl9607c_gpon.c, and the two copies were
* identical: `sw_proxy_rd`/`sw_proxy_wr` byte for byte, and the two SMI halves
* differing ONLY in comments (verified by diff, 2026-08-28).  The single thing
* that kept them apart was the file-scope `swcore_base` each one closed over,
* so passing the base in is the whole of the fix.
*
* ⚠ AND THE CONSTANTS WERE DOUBLED TOO: this header already declared SMI_CTRL_0
* and its neighbours while rtl9607c_gpon.c carried its own `#define`s of the
* same addresses.  Two spellings of one register is how a corrected offset
* reaches half the family -- the defect this tree paid for twice today.
*
* ★ THE POLL IS BOUNDED AND ITS EXIT IS NOT CHECKED, exactly as both copies had
* it.  That is preserved deliberately: changing behaviour while moving code
* makes a later bisect blame the move.  It is recorded as OWED, not fixed here
* -- an SMI transaction that never clears CMD_EN currently returns whatever the
* data register held, and on this project a silent read is how a dead PHY bus
* survived five candidate fixes. */
```

### luna_smi_read refusal

```c
/**
* luna_smi_read - one SMI read through the switch's MDIO master
* @out: the 16-bit result, written ONLY when the transaction completed
*
* ★ IT REFUSES TO ANSWER WHEN IT DID NOT READ (2026-09-04).  Both vendor copies
* fell out of the poll on exhaustion and read SMI_CTRL_3 anyway, so a bus that
* never cleared CMD_EN handed back whatever that register still held -- the
* PREVIOUS transaction's data, presented as this one's.  This header's own
* comment recorded that as OWED rather than fixed, deliberately, because the
* repair should not ride along with a code move.
*
* It is the same defect repaired three times elsewhere in this tree on the same
* day (cortina_ni_rx_ind_entry, cortina_ni_rx_get_axi_attrib, the GTC CAM
* readback), and it is the worse one here: a stale value is not a degraded
* answer, it is a plausible wrong one, and every read would look healthy.
*
* Reachable only through luna_sw_proxy_rd(), whose two callers are the 9607C
* I2C-hole path in sw_rd() (gated on is_9607c, so unreachable on the 9602C and
* 9603CVD) and a self-checking diagnostic -- which is why the fix could land
* without a board.
*/
```

## luna_l34_acc.h

### luna_l34_tbl_op shared data bank

```c
/* ★★ A COMMAND THAT TIMED OUT CAN STILL OWN THE SHARED DATA BANK.  The
* bank is four registers, not a per-command buffer: the engine reads it
* while EXE is set, so writing the next op's words into it before BOTH
* request bits have cleared hands the engine a mixture of two entries
* and it will store one of them.  Nothing reports that -- the entry
* reads back exactly as written.  Wait for the previous op to let go,
* and refuse rather than proceed if it never does.
* ⚠ It is BOTH bits, not `exe`: a read left executing owns the bank just
* as a write does, and the poll below only ever clears our own. */
```

## rtl9602c_eth.c

### omci_tx_ring

```c
/* HW ring for the US-OMCI descriptor ring */
```

```c
/* 0 = LAN ring (GMAC FETCHES it; ring 4 dedicated does NOT fetch — dirty stays 0); 4 = dedicated */
```

```c
/* dedicated HW-descriptor TX ring (opts3 carries the SID). The ring0 SW 0x8899-tag path only works on the FPGA model — on real silicon the L2 switch strips the inline tag so US OMCI never reaches the US-NIC; the HW descriptor (word3=0x00b20040, DST_SID 64) is the only path that classifies to SID-64 group[4]. */
```

### gmac_reset

```c
/* now safe to reset: rtl9602c_uboot_swcore_bringup() re-runs U-Boot's GMAC<->switch resync after the IP-block reset, breaking the old "reset kills egress" catch-22, and the stock GMAC init re-establishes the GMAC0-TX->US-NIC direct link */
```

### SW_VLAN_CTRL_VAL (VLAN filtering on at init, hybrid plan)

```c
/* VLAN filtering ON at init — required during ranging/config-apply for
* reliable onlining (VLAN-off cold boots failed config-apply 4x; VLAN-on
* onlines + stays stable). BUT with filtering ON this switch does NOT pass
* LAN port<->CPU traffic (ping 192.168.1.1 fails), so LAN management access
* needs filtering OFF. HYBRID (gpon-rtl960x.c gpon_fsm_poll): keep 0x19 for
* config, then auto-clear bit0 (filtering off) once the ONU is stably at O5
* (config done) to open LAN; re-assert on any re-range. Proven viable: online
* with 0x19 then poke 0x13008=0 -> stays online 6h + LAN reachable. */
```

### CPUTAG registers (fragment; rest of the sentence is lost)

```c
/* THE SAME THREE ADDRESSES ARE SPELLED THREE TIMES IN */
```

### CPUTAG1_LOW (0x4070 vs 0x4002)

```c
/* SUPERSEDED: an earlier reading had stock OR-ing 0x4070 (bits 4/5/6 as the
* cpu-tag format/enable).  The live 9602C devmem read below is 0x4002. */
```

### hw_nat default and offload measurements

```c
/* ★★★ THE DEFAULT IS 1, AND IT IS THE OPERATOR'S CALL, NOT AN INFERENCE.
* (2026-09-15: *"=1 por defecto y corrigir"*.)  The standing objective has
* always been *"mantener el acelerador de hardware activo por defecto"*, and 0
* contradicted it silently: at 0 neither the TC lifecycle handle nor the
* /proc node exists, so the board forwarded in software at 11% of stock with
* its CPU at 100% and nothing could say why.
*
* MEASURED before flipping it, not after: `hwnat_wedge_ab.py` booted BOTH arms
* twice on the same image bytes and neither wedged -- 0/2 each, the host
* pinging the board's own LAN address as the deciding witness.  ⚠ That was at
* 10 pps, which is not traffic: it says the gate can be OPENED safely, never
* that the engine survives load.  The load question belongs to the benchmark.
*
* ⚠ AND OPENING IT IS NOT SUFFICIENT, which is why the old comment's "armed
* lazily on first offload" was wrong twice over: the thing it claimed to arm
* was CONSTRUCTED INSIDE THIS GATE, so nothing could arm it.
*
* ✔ MEASURED 2026-09-16, and this comment used to end "with the gate open the
* engine still reads provisioned 0 and offered=0 ... what installs a flow is
* still owed".  That was true for one day.  With a forwarded connection through
* the board the engine reads offered=28988 installed=372 hits=3012 -- it
* installs flows and the hardware path counts hits.  What is still owed is
* narrower and is NOT about this gate: the case's own forged flow is software
* forwarded, and the leading explanation is our INSTRUMENT (the NAPT hit
* bitmap is clear-on-read and has two readers).
*/
```

### Stall watchdog

```c
/* TX-DMA park watchdog (see tx_recover). stall_since = jiffies of the
* first tick that saw OWN stuck at the HW cursor with no dirty progress;
* 0 = healthy. recover_work runs the IP-block power-cycle + re-arm. */
```

### LAN SerDes mode (linked below rate, 2026-10-27)

```c
/* ---- the LAN SerDes: port 0 to the external PHY -----------------------------
* Its mode register. [4:0] is the SerDes mode in the family's coding (0x02
* SGMII, 0x12 HSGMII, 0x1f off), and 0x1f, every bit set, is also what a die
* with no SerDes port shows: the UNI gate reads it as PHY_PWRDN_GUARD.
*
* The sequences below are the stock U-Boot's `sdsmode` command (2011.12.NA
* for rtl9602C: sds_sgmii at 0x80d80854, sds_hisgmii at 0x80d808fc, read from
* the V2802RH's flash) and are, register for register, what the stock RTOS's
* _rtl9602c_lan_sds_mode_set writes for its modes 3 (SGMII) and 5 (HSGMII):
* park the mode, the analog block, the mode, the forced ability, a reset
* pulse, one misc word. U-Boot runs them only when the command is typed, so
* a Linux that does not run them leaves the SerDes at its reset defaults --
* which linked, and carried traffic, but not at the link's rate (2026-10-27).
* The stock RTOS's link task re-runs them, with the PHY's own SerDes option,
* whenever the copper speed crosses 1G (rtl9602c_port0_link_poll). */
```

### CFG_UNHIOL.IPG_COMPENSATION

```c
/* (b) CFG_UNHIOL.IPG_COMPENSATION — swcore 0x23040 bit0. ORACLE-CONFIRMED real
* value diff (ours read 0xa8, working stock 0xa9) that demonstrably changed the
* OMCI forwarding class when poked live. The stock switch init sets it. */
```

### LAN-ring HW-descriptor path (ring 4 observations)

```c
/* LAN-ring HW-descriptor path: the bare OMCI PDU on the proven-to-fetch LAN
* ring 0 (kick R_IO_CMD bit0) with the CORRECTED HW cpu-tag descriptor -- NOT
* the SW 0x8899 tag.  Isolates RING from DESCRIPTOR: HW ring 4 does fetch, yet
* RX_SID_GOOD stays 0. */
```

### OMCI PDU padding (48 bytes raw)

```c
/* Stock sends the OMCI PDU RAW (48 bytes, no Ethernet pad) — the
* working stock ref ONU's TX descriptor is len=0x30=48. The old
* pad-to-60 was a TX-stall workaround, now obsolete (the SW-follows-
* HW ring alignment fixed the runt stall). Send exactly what stock
* sends so the US-NIC sees the same frame. */
```

### rtl9602c_eth_omci_xmit ring-0 path

```c
/* omci_tx_ring==0: shared-ring-0 test path — enqueue on the proven LAN
* ring 0 with the OMCI steering descriptor (isolates steering from the
* "ring 4 won't fetch" problem). All ring instances 1..5 use the dedicated
* ring below. */
```

### rtl9602c_omci_doorbell derivation

```c
/* omci_tx_ring is the HW ring h directly — the SAME h used to arm R_TxFDP(h)
* in open(). Derive the doorbell from h so they can never disagree (the prior
* bug: ring armed at h=4/0x1340 but kicked R_IO_CMD bit0 = h=0). Stock kick
* behavior: h<4 -> R_IO_CMD |= 1<<h; h==4 -> R_IO_CMD1 |= 0x100. */
```

### TXD0_DESC_FLAGS and D_EOR

```c
/* Same fix as the ring-0 path: DO NOT OR TXD0_DESC_FLAGS (0xf8800000) — it sets
* D_EOR (bit30) on EVERY descriptor, so the GMAC treats each as end-of-ring and
* never drains the ring (OWN stuck, dirty stalls). D_EOR belongs ONLY on the wrap
* slot (set just below), exactly like the working LAN TX path. */
```

### rtl9602c_eth_omci_set_optical (fragment; rest of the sentence is lost)

```c
/* LAYER BOUNDARY -- RESOLVED 2026-09-02 */
```

### Last-resort TX recovery

```c
/* Last-resort path (off by default — resets kill the switch egress on
* this board): drop everything in flight, re-arm both rings from
* scratch (engine stopped => CDO genuinely resets), align at 0. */
```

### 0x230c4 SVLAN_UPLINK_PMSK vs SMI_CTRL_3

```c
/* ⚠ 0x230c4 STAYS A LITERAL: this chip's chipdef calls it SVLAN_UPLINK_PMSK,
* luna_gpon_regs.h calls the same address SMI_CTRL_3, and the two never run
* on one silicon -- dev/FINDING-one-address-two-blocks-smi-vs-svlan.md.
* Naming it either way would make the disagreement look settled. */
```

### Open: full switch bring-up cost (fragment; rest of the sentence is lost)

```c
/* THE FULL SWITCH BRING-UP IS A ONE-TIME COST, NOT A */
```

### GMAC keeps running across close (X111W, 2026-09-30)

```c
/* The GMAC keeps running: the DS/US OMCI and gpon0 ride its rings, and an OLT
* gives up on an ONU whose OMCI stops (the laptop X111W, 2026-09-30, O5 reached
* between preinit's close and netifd's open). */
```

### l34_proc registration

```c
/* ★★★ THE DIAGNOSTIC IS REGISTERED WHATEVER THE GATE DOES.  It used to sit
* inside the `if (hw_nat)` below, which put the one file that can say WHY
* the engine installed nothing behind the very thing it diagnoses: with
* the knob at its default 0 the node did not exist, and an absent file
* reads as "this image carries no accelerator instrumentation" rather than
* "the knob is off".  It is READ-ONLY and it changes no datapath
* behaviour; `l34_proc_show()` reports the gate's state and returns before
* touching a lock the closed gate never initialised.
*/
```

## rtl9602c_l34.c

### Indirect table access moved to luna_l34_acc.h

```c
/* ★ THE BODY MOVED TO FAMILY TIER, 2026-09-16 (luna_l34_acc.h).  This is the ONE
* thing this family's two L3/L4 engines share -- the NAPT model here and the
* FLOWBASED model on the RTL9603CVD -- and it lived as `static` inside the file
* belonging to exactly one of them, so a second die could only get it by copying.
* Nothing the engine sees changes: this chip's four addresses and its command-word
* layout are the same values, now expressed as a per-die TABLE. */
```

### L34_SWTCR0 V4FLRT_EN/V6FLRT_EN (stock vs driver differential)

```c
/* ★★★ THE FLOW-ROUTE PATH IS NOT OURS, AND WE WERE LEAVING IT ON.
* This is a read-modify-write, so every bit we do not name keeps whatever
* the reset/bootloader left -- and V4FLRT_EN/V6FLRT_EN arrive SET. Stock
* clears them: a SAME-DAY differential on this board (swcore_diff, both
* captures inside 15 minutes) reads SWTCR0 84801e10 on stock against
* 64000e20 on ours, with V4FLRT_EN and V6FLRT_EN the inverted pair.
* FLRT is the die's FLOW-ROUTE lookup and it has its own tables
* (FLOW_ROUTING_TABLE_IPV4, access type 12) which this driver never
* programs -- so with the bit set the engine consults a table we leave
* empty instead of the NAPT/NAPTR pair we fill. That is exactly the
* measured state: an entry installed, live, never removed and never hit
* while 272 MB of matching traffic passed.
* ⚠ The VALUE is not a datasheet reading: it is what the vendor's own
* firmware holds on this very board, captured the same hour. */
```

### L34_NAPT_HIT power-on state

```c
/* ⚠ DRAIN THE HIT BITMAP ONCE, AND DISCARD IT.  MEASURED 2026-09-16 on the
* X111W with ZERO flows installed: L34_NAPT_HIT read back 82 of 128 words
* NON-ZERO, 1278 of 2624 bits set (48.7%), no two words alike -- power-on
* state, or what the vendor firmware left behind on a TFTP->RAM boot.
* Either way it is not our traffic.
*
* Nothing cleared it, so the FIRST sweep OR'd all of it into `hit_shadow`
* -- which is STICKY (`|=`) and is only ever cleared one slot at a time by
* the liveness op.  A genuine hit is then invisible against a half-set
* background and `hits_seen` carries a constant that has nothing to do
* with packets, which is why `accel_witness_calibration` could not tell
* offload-ON from software forwarding.
*
* The register is CLEAR-ON-READ, so reading it IS the clear: this loop
* needs no write, and it deliberately does NOT touch `hit_shadow` or
* `hits_seen` -- draining into them is exactly the bug. */
```

### WAN /32 CPU self-route (l34_iface_blackholes)

```c
/* The /32 CPU self-route for the WAN address. The LAN side has always * written one and this side never did, so l34_iface_blackholes() -- our * own predicate, "the engine claims traffic for this interface and * nothing terminates its OWN address locally" -- was true for the WAN * netif on every boot, and /proc/flowdump printed `netif[0] blackholes * 1` while nobody acted on it. A NATed reply is addressed to this very * address. */
```

### rtl9602c_l34_wan_teardown (X111W DHCP OFFER)

```c
/* The WAN address the tables describe has left the kernel: invalidate every
* WAN row wan_setup() programs, the NETIF claim first. Left in place, NETIF[WAN]
* keeps claiming frames to the WAN MAC and routes a unicast DHCP OFFER away
* from the CPU -- and with no lease no flow re-provisions it (X111W after a
* PPPoE session, 2026-09-23). Refused while a flow still owns the tables. */
```

### HSB/HSA lookup log

```c
/* ★★★ THE DIE'S OWN LOOKUP LOG. HSB is the hash source block the engine
* BUILT from the last frame it looked up; HSA is what it DECIDED. Printed
* raw and whole: this is a POINTER for reading, not a verdict, and a field
* split invented here would be a decode nobody measured.
* ⚠ ALL-ZERO MEANS THE LOG IS OFF, NOT THAT NOTHING WAS LOOKED UP -- the
* mode lives in HSBA_CTRL and is printed beside it so the two can never be
* confused. ⚠ AND THE MODE MATTERS AS MUCH AS THE DATA: under LOG_ALL the
* last lookup is the ssh packet that carried the reading command, so the
* log answered with this host's own port 22 instead of the flow. FIRST_DROP
* latches. That confusion is why this surface sat unread while two
* register hypotheses were settled by rebuilding the image instead. */
```

### NAPT entry read-back in /proc/flowdump (2026-09-16)

```c
/* The NAPT entries this driver most recently INSTALLED, read BACK from the
* silicon and decoded.  Until 2026-09-16 this node showed the hit BITMAP and
* the interface tables and never an entry, so the one question a non-matching
* flow raises -- does the installed key match the traffic? -- could not be
* asked from the board.  Nothing could disagree with us, so no measurement
* could catch a wrong field.  MEASURED that day: installs=15, live=1, hits=11
* across a 105 Mbps flow, with the CPU still at 83%.
*
* It reads what the HARDWARE holds, never what the driver believes it wrote:
* an entry the silicon rejected or rewrote is exactly the case worth seeing.
*/
```

### NAPT_OUT vs NAPTR_IN index spaces

```c
/* ⚠⚠ TWO INDEX SPACES, AND CONFLATING THEM READS ZEROS FOREVER.
* `hw_index` addresses NAPT_OUT (the hash slot), and that word
* CARRIES the NAPTR_IN index -- L34_NAPT_HASHIN_IDX says so in
* as many words. The first cut of this renderer read NAPTR_IN
* AT the NAPT_OUT index and printed seven all-zero entries,
* which reads exactly like "the silicon holds nothing" and is
* a bug in the reader. The hit bitmap is indexed by hw_index
* too, which is the cross-check: hits were non-zero while this
* showed empty entries, and that contradiction is what caught
* it. */
```

### Hit-bitmap diag sweep order

```c
/* ★ A DIAG READS THE SHADOW AND CONSUMES NOTHING.  This used to read the
* clear-on-read register itself, so looking at the diagnostic cleared the
* bits the flowtable's own liveness op was about to test -- a witness
* that aged out the flows it was measuring.
*
* ⚠ AND IT SWEEPS HERE, NOT BESIDE THE BITMAP PRINT.  It used to run AFTER
* the flow-diag line was built, so that line reported `hits_seen` from
* BEFORE this read's own sweep -- always one sweep stale, and 0 on the
* first read however many bits the bitmap held.  MEASURED: a read with
* nothing installed printed `hits=0` and then dumped 1278 set bits. */
```

### /proc/flowdump counters (fragment; rest of the sentence is lost)

```c
/* THE THREE COUNTS THAT SEPARATE FOUR DIFFERENT */
```

## rtl9602c_l34.h

### L34_SWTCR0 V4FLRT/V6FLRT bit names (fragment; rest of the sentence is lost)

```c
/* ⚠⚠ THESE BIT NAMES WERE WRONG UNTIL 2026-09-12, AND */
```

### L34_BD_CFG WAN binding policy

```c
/* WAN BINDING policy: what the die does with a frame whose WAN binding does NOT
* match, one field per L2/L3/L34 combination. ⚠ THE ACTION VALUE 0 IS *DROP*
* (the vendor's own `rtk_l34_bindAct_t`: L34_BIND_ACT_DROP = 0), and this
* register reads ALL ZERO on our image while stock holds 0xa2400000 -- measured
* on this board, both captures the same hour. With the BINDING table unwritten
* nothing can ever match, so every unmatched combination takes the DROP action
* and the accelerated path discards what the CPU then forwards. */
```

### L34_HSBA_CTRL

```c
/* ★★★ THE DIE'S OWN VIEW OF THE LOOKUP -- the instrument that replaces guessing.
* HSB is what the engine SAW (the hash source block it built from the frame) and
* HSA is what it DECIDED. Stock runs with the log mode at LOG_ALL and its HSB
* descriptors are populated; ours ran at NO_LOG, which is why every descriptor
* read zero and two register hypotheses had to be settled by rebuilding the
* image instead of by reading what the die already knows.
* Mode values are the vendor's own (rtk_l34_hsba_mode_t): 0 BOTH_LOG, 1 NO_LOG,
* 2 LOG_ALL, 3 LOG_FIRST_DROP, 4 LOG_FIRST_PASS, 5 LOG_FIRST_TO_CPU, 6 LOG. */
```

### L34_HSBA_MODE_FIRST_DROP

```c
/* ⚠⚠ LOG_ALL IS THE WRONG MODE FOR US, MEASURED 2026-09-19. It keeps the LAST
* lookup, and the last lookup is always the ssh packet carrying the command that
* reads the log: the first capture came back holding 192.168.1.2 -> 192.168.1.12
* port 22, this host talking to the board. The instrument was measuring itself.
* LOG_FIRST_DROP LATCHES the first dropped frame instead, so the event survives
* every later lookup including the read's own. */
```

### L34_BD_CFG write attempt

```c
/* ⚠ NOT WRITTEN, and the refutation is why. Setting this to stock's value was
* TESTED on 2026-09-18 -- one build, one boot, one 20 s transit of 204 896
* frames -- and `hits_seen` moved from 0 to 1. One hit is not forwarding; it is
* the order of magnitude the liveness probe itself touches. A speculative write
* of a value only partly decoded, with no measured benefit, does not belong in
* the shipped path, so the ADDRESS and the DECODE stay here for the next
* attempt and the write does not. */
```

### L34_NAPT_HIT readers

```c
/* ★★ ONE READER FOR A CLEAR-ON-READ BITMAP.  L34_NAPT_HIT clears on
* read, and it had TWO readers: the flowtable's per-flow liveness op
* and /proc/flowdump.  Each consumed the other's evidence -- the
* diagnostic could age a live flow OUT by clearing the bit its own GC
* was about to look for, and the witness read zeros because the GC had
* just harvested them.  The bitmap is now swept ONCE per jiffy into a
* sticky shadow; liveness consumes only its OWN slot, and the
* diagnostic consumes nothing, because a DIAG may not destroy evidence
* a control path needs. */
```

### Install ring (2026-09-16 measurement)

```c
/* The last few NAPT indexes this driver INSTALLED, so the dump can read
* the entries BACK.  /proc/flowdump rendered the hit BITMAP and the
* interface tables and never the entries, so *does the installed key
* match the flow?* could not be asked from the board at all -- the shape
* this project calls a register the driver writes and cannot read back,
* where nothing can disagree with us.  MEASURED 2026-09-16: installs=15,
* live=1, hits=11 across a 105 Mbps flow, i.e. an entry exists and the
* traffic is not matching it, and no instrument could say which field is
* wrong.  A ring, not a list: it costs 16 bytes and answers the question
* for the flows that matter, which are the most recent. */
```

### Offered-key ring (2026-09-16 measurement)

```c
/* The last few flow keys the core OFFERED, recorded BEFORE any refusal so
* a declined one is visible too.  MEASURED 2026-09-16: offered=188,
* installed=7, live=1, why{dup-cookie=87 engine=94} -- and the single LIVE
* entry was UDP while the throughput legs were TCP, so what was being
* accelerated was the latency probe.  Whether a TCP 5-tuple was ever
* offered at all could not be asked: the counters name CAUSES, never the
* flow they were about. */
```

### Refusal tally

```c
/* ★ AND A TALLY PER REASON, because the LAST one answers the wrong
* question. MEASURED on the X111W: 722 engine refusals collapsed into one
* count and one string, so "why did 1.6% of upstream flows install" could
* not be asked at all -- the node named a cause and could not say whether
* it was 1 of 722 or 722 of 722.
* ⚠ The reasons are STATIC LITERALS (see l34_refuse), so the POINTER is the
* key: no allocation, no strcmp, and a slot per distinct site. */
```

## rtl9602c_l34_tc.c

### TC install path (fragment; rest of the sentence is lost)

```c
/* THE INSTALL REFUSED UNCONDITIONALLY UNTIL 2026-09-11 */
```

### rtl9602c_l34_is_lan_side VLAN uppers (X100DG)

```c
/* Structural, never a name.
* A VLAN upper is on its real device's side: the flowtable offers a tagged
* WAN->LAN leg from gpon0.<vid> when gpon0 itself is not a flowtable device
* (X100DG, 2026-10-05: every such leg was decoded as upstream and refused). */
```

### Refusal tally overflow

```c
/* Table full and this reason is new: still COUNTED, in the last slot,
* under a name saying the split stopped being exact there.
* ⚠ The first cut could never reach this -- the overflow branch sat
* inside a condition that had already excluded the only case that
* reaches it, so a ninth reason was dropped in SILENCE while the
* comment beside it promised the opposite. And the second cut returned
* early from the loop, which skipped the log line below. */
```

### last_hit seeding (installs == removals on X111W)

```c
/* ⚠⚠ SEED THE LIVENESS CLOCK, OR THE FLOW IS TORN DOWN BEFORE IT CAN EVER
* BE HIT.  `priv` arrives ZEROED, `op_stats` writes `last_hit` only inside
* its `if (active)` branch, and it reports `*lastused = p->last_hit` -- so
* a flow installed and polled before its first packet matched answers
* `lastused = 0`, which the flowtable GC reads as "idle since jiffies 0"
* and retires immediately.
*
* MEASURED on the X111W: `installs == removals` EXACTLY on every sitting
* (408/408, 30/30, 30/30) while the engine really was matching packets
* (`hits=414`).  The asymmetry is the evidence -- Cortina seeds this at
* install (`cortina-ni-flowoffload.c`, `entry->last_hit = jiffies` beside
* `installed_at`) and its offload has always held.  A newly installed flow
* is not an idle one. */
```

### hw_index vs the core's published idx

```c
/* ⚠ `idx` IS THE CORE'S PUBLISHED INDEX AND IT IS NOT AN IDENTITY.  This
* used to assign it over p->f.hw_index -- so a cleanup after an install
* that failed BEFORE publishing arrived with 0 and retired entry zero,
* which belongs to somebody else.  The flow's own retained indices are
* the only ones that name what it actually claimed. */
```

### rtl9602c_l34_op_note_vlan (fragment; rest of the sentence is lost)

```c
/* THE CORE OFFERS THIS ATTRIBUTION AND THIS FAMILY */
```
