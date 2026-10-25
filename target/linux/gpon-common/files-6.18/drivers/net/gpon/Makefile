# SPDX-License-Identifier: GPL-2.0-only
#
# gpon/Makefile -- builds the hardware-decoupled GPON protocol core that is
# common to every OpenWrt GPON target in this tree.
#
# WHAT THIS IS
#   The Makefile half of the build wiring for drivers/net/gpon/.  The parent
#   drivers/net/Makefile descends here only when CONFIG_GPON_CORE is set, so
#   everything below is unconditional obj-y: if we are here, the core is being
#   built in.
#
# WHY IT IS COMMON -- and which targets and architectures compile it
#   Operator, 2026-08-05: "en openwrt deberia estar estructurado algo asi:
#   rtl960x* para la familia para tener codigo comun" ... "la idea es poner en
#   comun el codigo que corresponde para no tener mucho duplicado", and on the
#   two per-target monoliths that each carried a private copy of the protocol:
#   "mal, poner en comun".
#   This file is authored ONCE, in target/linux/gpon-common/files-6.18/, and
#   reaches both targets through FILES_DIR (see each target's Makefile), so the
#   two can never drift.  Compiled by:
#     - realtek-elnath  RTL9607F (Cortina)  aarch64, LITTLE-endian
#     - realtek-luna    RTL960xC (Luna)     MIPS32,  BIG-endian
#     - dev/rtl9607c-test on x86-64 through fuzz_shims/, no board in the loop
#   One source, three word sizes and both byte orders: that is why every wire
#   parser here uses explicit byte math and never a struct or pointer cast.
#
# THE CORE/SHELL RULE IT OBEYS
#   Every object listed below is FUNCTIONAL CORE: it decides, it never does.
#   ==> NO OBJECT BUILT HERE MAY EVER GAIN AN MMIO ACCESS.  No readl/writel, no
#   ioremap, no device pointer, no sleeping, no kernel timers -- time is an
#   explicit input.  A file that needs any of those is shell and belongs in the
#   target's own files- tree (drivers/net/ethernet/{realtek,cortina}/).
#   The re-runnable proof is gpon_core_purity_test in dev/rtl9607c-test/; this
#   comment is not evidence.
#
# NOT A PATCH, BY HARD RULE
#   Driver wiring lives in a browsable source file under files-<ver>/, never in
#   a patches-<ver>/*.patch -- not even for Kconfig or Makefile.

# --- B-BUILD-2: this directory MUST carry its own -O2 -------------------------
# realtek-luna forces -Os KERNEL-WIDE (patches-6.18/100-cc-optimize-for-size.patch
# rewrites the CC_OPTIMIZE_FOR_PERFORMANCE arm to -Os because the NAND kernel
# partition k0 is 3 MB), and exempts its datapath objects one at a time with
# CFLAGS_<obj>.o += -O2 in drivers/net/ethernet/realtek/Makefile.
#
# So moving protocol code OUT of gpon-rtl9602c.o and INTO this directory would
# silently drop it from -O2 to -Os.  That is a real behaviour change hiding
# inside a "pure code motion" refactor, and it would surface only as a
# throughput or latency number -- on a driver that is at stock parity.
#
# ccflags-y (whole directory) rather than one CFLAGS_<obj>.o line per file, on
# purpose: a per-object list is a list somebody forgets to extend, and the next
# object added would lose -O2 with nothing to see.  It works because of
# scripts/Makefile.lib:26,
#   _c_flags = ... $(KBUILD_CPPFLAGS) $(KBUILD_CFLAGS) $(ccflags-y)) $(CFLAGS_<obj>.o)
# so ccflags-y lands AFTER the kernel-wide -Os, and the LAST -O wins.
#
# MEASURED 2026-08-05 on the compiled objects of BOTH targets -- not on the
# monolith alone.  Re-check any line with:
#   tr ' ' '\n' < <kbuild>/drivers/net/gpon/.gpon_omci_core.o.cmd | grep '^-O'
#   luna    build_dir/target-mips_mips32_musl/linux-realtek-luna_rtl960x/linux-6.18.31
#     gpon_{gem_us,omci_core,omci_me}.o  -fno-schedule-insns2 -Os -O2  <- this line
#     gpon-rtl9602c.o                    -fno-schedule-insns2 -Os -O2  (exempted)
#     drivers/net/loopback.o             -fno-schedule-insns2 -Os      <- CONTROL:
#        a plain drivers/net object, i.e. exactly what this directory would get
#        if the line at the bottom of this block were dropped.
#   elnath  build_dir/target-aarch64_generic_musl/linux-realtek-elnath_rtl9607f/linux-6.18.31
#     gpon_{gem_us,omci_core,omci_me}.o  -O2 -O2   (kernel-wide, then this line)
#     cortina-gpon.o, drivers/net/loopback.o       -O2
#
# ⇒ ELNATH IS NOT AFFECTED -- checked, not assumed symmetric with luna:
# target/linux/realtek-elnath/patches/ is EMPTY, its .config keeps
# CONFIG_CC_OPTIMIZE_FOR_PERFORMANCE=y against an UNPATCHED top-level Makefile
# (linux-6.18.31/Makefile:874 -> KBUILD_CFLAGS += -O2, vs luna's :884 -> -Os), and
# its whole files- tree carries no CFLAGS_/ccflags-y -O exemption.  So the line
# below is redundant there, which is exactly why elnath's objects read "-O2 -O2".
# It stays unconditional so this directory compiles the same way on every target
# whatever that target's global default is.
# (The previous note here cited "omci_responder.o -> -O2".  That object was
# deleted BY this refactor, so its .cmd is gone and a reader cannot re-check it.
# Its surviving same-directory siblings above carry the same proof, since on
# elnath the -O comes from the kernel-wide default and from nothing else.)
#
# ⚠ THE HAZARD THIS LINE COULD HAVE CREATED IS CORRECTNESS, NOT SPEED -- and it
# had not been checked.  On luna the -O2 below lands AFTER the
# -fno-schedule-insns2 that patch 100 adds for the Lexra RLX MDU erratum (a load
# scheduled into a div/mult shadow silently corrupts HI/LO).  Were a later -O to
# re-enable that pass, this line would re-arm silent data corruption in every
# object here.  It does not: GCC applies -O level defaults FIRST and explicit -f
# options afterwards, whatever their order on the command line.  Measured with
# luna's own compiler (staging_dir/toolchain-mips_mips32_gcc-14.3.0_musl):
#   $CC -fno-schedule-insns2 -Os -O2 -Q --help=optimizers | grep fschedule-insns2
#        -> [disabled]      and without the -fno- prefix -> [enabled]  (control)
#   $CC -Os -O2 ... -falign-loops -> [enabled];  -O2 -Os -> [disabled]
#        -> the last -O wins, so this -O2 does beat the kernel-wide -Os.
# ⇒ do NOT "defensively" repeat -fno-schedule-insns2 here: it is a per-target
# erratum flag, it already reaches this directory through luna's KBUILD_CFLAGS
# (visible on loopback.o above), and it means nothing on aarch64.
#
# ★ KEEP THE NEXT LINE SPELLED EXACTLY AS IT IS.  build_wiring_guard.py W3.o2
# requires it, and that guard's --self-check mutation matches the literal
# '^ccflags-y \+= -O2$'.  A trailing comment, or a switch to subdir-ccflags-y,
# would leave the guard reporting PASS while its mutation quietly became a no-op
# -- a check that cannot fail, which is the failure direction this tree keeps
# paying for.  (Verified 2026-08-05: 34 checks, 0 failed; W3.o2 mutation -> RED.)
ccflags-y += -O2

# --- the core objects ---------------------------------------------------------
# BEFORE ADDING AN OBJECT HERE, CHECK IT IS THE RIGHT TIER.  This directory is
# the CORE tier (prefix gpon_): protocol only, and it must never touch hardware
# -- no register access, no clock, no lock, no allocator, no device pointer.
# Family hardware belongs in luna_ or cortina- files, and a single part's
# registers in rtl9602c_, rtl9607c_, rtl9607f_ or cortina-gpon.c.  The rule, the
# file map and the guard are written out ONCE: see "THE THREE TIERS" in
# gpon_common.h, in this directory.  The guard that enforces purity is
# dev/rtl9607c-test/gpon_layer_hostbuild_test.sh -- it compiles this directory
# against stubs with no hardware in them, so an impure core cannot build.
#
# A source file in this directory is in exactly ONE of three states, and the
# third one does not exist -- build_wiring_guard.py (W4) fails on any .c that is
# neither built nor listed pending:
#
#   BUILT     an `obj-y += <name>.o` line below.
#   PENDING   a `# gpon-pending: <name>.o -- <reason>` line below.  Carved into
#             this tree, compiled and fuzzed on x86 by dev/rtl9607c-test, but
#             deliberately NOT in the kernel image yet, with the reason stated.
#   (silent)  present and mentioned nowhere == dead source that never compiles
#             while every test of it passes.  That is the failure direction this
#             project keeps paying for, so the guard refuses it.
#
# THE RULE THAT DECIDES WHICH STATE A FILE IS IN: an object is turned on by the
# step that migrates its CONSUMER, in the same change that retires the duplicate
# it replaces -- never before.  Until then compile coverage comes from the x86
# suite, not from bloating a shipping kernel.
#
# ★ THE ELNATH SHELL MIGRATED (measured 2026-08-05 16:10-16:11), so three of the
# four are now BUILT.  What changed, and how it was checked rather than assumed:
#   - cortina-gpon.c:98-100 now includes gpon_omci_core.h, gpon_omci_me.h and
#     gpon_gem_us.h, and no longer includes "omci_responder.h";
#   - cortina/Makefile dropped omci_responder.o and added
#     ccflags-y += -I$(srctree)/drivers/net/gpon;
#   - cortina/omci_responder.{c,h} are DELETED from that tree.
# So the collision this list was guarding against is gone -- there is no second
# definition of omci_onu_input, omci_onu_emit_veip_up_avc or omci_onu_init left
# to clash with.  The symbols the migrated shell actually references, counted in
# cortina-gpon.c rather than inferred from the includes:
#     omci_onu_input x2  omci_onu_emit_veip_up_avc x1      -> gpon_omci_core.o
#     omci_onu_init x1   omci_onu_set_optical x1           -> gpon_omci_me.o
#     gpon_gem_us_tcont_decide x2                          -> gpon_gem_us.o
# Leaving any of those pending is not a conservative choice: it is a link error.
#
# ★ AND WHAT IT COSTS realtek-luna: MEASURED AT ZERO IMAGE BYTES, 2026-08-05.
# Footprint is a hard constraint here (<=64 MB RAM, 3 MB NAND kernel partition),
# so this was checked rather than assumed -- and the first version of this very
# comment asserted the opposite ("dead .text until luna's shell is migrated"),
# which the measurement refuted.
# luna's GPON symbols also `select GPON_CORE`, so luna DOES compile these three
# while its own shell (gpon-rtl9602c.c) still carries a private OMCI responder
# and calls none of them.  It links cleanly -- luna's own omci_* definitions are
# all `static`, so nothing collides, and CONFIG_CRC32=y is already set there.
# But luna sets CONFIG_LD_DEAD_CODE_DATA_ELIMINATION=y, so the objects are built
# -ffunction-sections -fdata-sections and the link runs --gc-sections
# (Makefile:1004), and the unreferenced core is discarded before vmlinux:
#     omci_store_find, omci_onu_input, gpon_gem_us_tcont_decide
#       present in vmlinux.o (pre-GC)   3/3
#       present in vmlinux  (post-GC)   0/3
# On elnath, which does NOT set that symbol, the same three ARE in System.map --
# as they must be, since that shell calls them.  ⇒ turning an object on here
# costs a target that does not use it compile time, not flash.
# (For scale if that ever changes: on luna the three objects are 248 + 2148 +
# 3548 = 5944 bytes of .text at -O2.)
#
# ⚠ LATENT, recorded not fixed: gpon_omci_core.c calls crc32_be but GPON_CORE
# does not `select CRC32`.  It builds today only because BOTH vendor Kconfigs
# happen to select it.  A third target that does not would fail at link, far
# from the cause.
#
# Kept in alphabetical order, one concern per line, so turning one on is a
# one-line edit and a reviewer sees at a glance what the core consists of.

# The UPSTREAM half of GEM provisioning: T-CONT/alloc and GEM-port lifecycle.
# (The GEM FRAMER is deliberately absent -- it is silicon on both shipping
# targets, so there is nothing to share.)
# ITU-T G.984.3 ONU Serial Number codec. TWO consumers from the day it lands:
# the Elnath GPON shell calls it directly, and the Luna shell is rebased onto
# it -- so unlike gpon_ploam.o below, this one de-duplicates a real pair. The
# two copies it replaces did not merely differ in style: the Elnath decoder
# refuses a malformed string while the Luna one turned hex_to_bin()'s -1 into
# 0xff bytes without a word, so the promoted copy is the VERIFIED one.
obj-y += gpon_ddm.o
obj-y += gpon_sn.o

# The netdev station-address ladder. Four consumers: luna_eth.c, rtl9602c_eth.c,
# cortina-ni-tx.c, cortina-gpon.c.
obj-y += gpon_hwaddr.o
# May this ONU transmit, and what must change if not. Pure policy, no MMIO --
# both families ask it, and the LIVE re-provision path is where its four
# defects lived, so it is decided once and walked exhaustively on x86
# (dev/rtl9607c-test/gpon_range_gate_test.c).
obj-y += gpon_range_gate.o

obj-y += gpon_gem_us.o

# ITU-T G.988 OMCI baseline MESSAGE layer: parse, dispatch, response, MIC.
obj-y += gpon_omci_core.o

# ITU-T G.988 MANAGED-ENTITY model and MIB store, called by the message layer.
obj-y += gpon_omci_me.o

# ★ AND THE VLAN / CLASSIFICATION SERVICE MODEL (ME 171, 78, 84, 79, 130, 280,
# 281) HAS NO LINE HERE ON PURPOSE: gpon_omci_vlan.h is header-only static
# inline, the shape gpon_data_plan.h already uses in this directory.  Its host
# coverage is dev/rtl9607c-test/gpon_omci_vlan_test.c, which compiles the REAL
# header on x86 -- that binary IS the no-MMIO proof, because the .c-walking
# host-build gate cannot reach a header.

# ITU-T G.988 message DECODE for the log: what a downstream PDU IS, in words.
# ★ A PROMOTION, not a merge: this existed only in the Cortina family and was
# ABSENT from Luna, so the Luna boards gain a decode they never had.  It only
# ever formats INTO A BUFFER -- the caller owns the rate limit and the level,
# because how chatty a board may be on its console is that board's fact.
obj-y += gpon_omci_trace.o

# ITU-T G.984.3 ACTIVATION diagnostics: the grant-acceptance counters and the two
# ONU-ID readbacks at Assign_ONU-ID / Ranging_Time / Deactivate_ONU-ID -- the
# read that split the O4 wall on 2026-09-06, promoted from a hand-added probe
# so no board has to re-derive it.  Formats INTO A BUFFER only; the family reads
# its own registers and owns the level.  Gated so a footprint-bound build can
# drop it (CONFIG_GPON_PLOAM_DIAG, default y: see the Kconfig help).
obj-$(CONFIG_GPON_PLOAM_DIAG) += gpon_ploam_diag.o

# ITU-T G.988 per-message OMCI diagnostics: the ONE trace line every family
# emits.  It is the LINE that stayed behind when the DECODE was promoted on
# 2026-09-10 -- two spellings of one idea, and the suite could read only the
# Cortina one, so every Luna board's OMCI trace was invisible with the lines in
# dmesg.  Formats INTO A BUFFER only; the family owns the rate limit and the
# level.  Gated so a footprint-bound build can drop it (CONFIG_GPON_OMCI_DIAG,
# default y: see the Kconfig help).
obj-$(CONFIG_GPON_OMCI_DIAG) += gpon_omci_diag.o

# The WAN DATA PATH's own line: what the OLT provisioned, what is armed, the
# plan, and WHICH precondition is missing.  The third member of the
# GPON_<SUBSYSTEM>_DIAG family; the verdicts come from the pure functions in
# gpon_data_plan.h so the line cannot disagree with the behaviour.  Formats
# INTO A BUFFER only; the family owns the rate limit and the level.
obj-$(CONFIG_GPON_GEM_DIAG) += gpon_gem_diag.o

# The FAR END's own conversation, recorded so it can be replayed off the board:
# the fifth GPON_<SUBSYSTEM>_DIAG member and the only one whose output is meant
# for a MACHINE.  Renderer and parser sit together on purpose -- the host replay
# (rtl9607c-test step 32) loads a capture through the same code the board wrote
# it with, so no lossy step can sit between the two halves of the differential.
# Gated (CONFIG_GPON_OLT_DIAG, default y: see the Kconfig help, which states the
# 16 KiB of BSS it costs).
obj-$(CONFIG_GPON_OLT_DIAG) += gpon_olt_diag.o

# ITU-T G.984.3 PLOAM activation state machine (O1..O7) and message builders.
# ONE kernel consumer today (realtek-luna): the Elnath PON-MAC runs activation
# in silicon, so there is no second copy this de-duplicates.  It is here for the
# roadmap -- the future ARM OLT, other brands, and the offline adversarial
# fuzzing that needs the FSM decoupled from any register.
# ★★★ BUILT AS OF 2026-08-27, DELIBERATELY WITHOUT A CONSUMER YET (operator:
# *"al menos quitarlo de especifico si es generico, para forzar ir usando en
# generico y no hacer codigo duplicado"*).
#
# It used to say "no consumer either until gpon-rtl9602c.c is migrated", and
# that is exactly how a common file rots: unbuilt, it compiles for nobody, and
# meanwhile the shell's private FSM keeps being extended because it is the one
# that works. Building it makes the family copy the one that CANNOT silently
# break -- every target compiles it on every build -- so the shell's copy is
# now provably the redundant one rather than the living one.
#
# ⚠ THE MIGRATION ITSELF IS NOT IN THIS STEP, and the reason is measured: the
# X400AXF -- the only board that boots and therefore the only place a change
# gets verified -- does activation IN SILICON and never calls a software PLOAM
# FSM. So swapping the Luna onto this file could be proven by the offline suite
# and by NOTHING on hardware, on a board that currently wedges at ~30 s. Build
# it, freeze the duplicate, migrate when the X111W boots.
obj-y += gpon_ploam.o

# ─────────────────────────────────────────────────────────────────────────────
# gpon_regseq.o -- a bring-up expressed as DATA, and the interpreter that runs it.
#
# ★ WHY IT IS HERE AND NOT IN A FAMILY.  The register addresses and values of a
# bring-up are silicon facts and stay with the silicon; "write, set these bits,
# wait, poll until it clears or give up" is not one, and it had been written per
# family only because the interpreter and the tables shared a file.
#
# ★ IT IS WHAT MADE THE MOVE POSSIBLE: the family version called mdelay() and
# udelay() directly, which this tier forbids.  With the two delays as ops the
# same interpreter runs on silicon and against a FIXTURE clock on a host, which
# is this project's stated preference over one ~200 s board boot.
obj-y += gpon_regseq.o

# gpon_flow.o -- "which flow is this", decided once for every family.
# ⚠ NOT in the strict host-buildable subset: it uses Linux's flow-offload API
# deliberately, which the widened core admits (the line is the REGISTER, not
# Linux) and the strict subset does not.
# ⚠ GATED, and the build is what taught it: the Luna targets have no
# net/flow_offload.h at all, so an unconditional obj-y here is a fatal error on
# every board without an accelerator.  The operator's call (2026-08-28): a
# Kconfig symbol per target, so a board with no engine carries ZERO bytes of
# this -- which matters under the <=64 MB / small-NAND rule.
obj-$(CONFIG_GPON_FLOW_OFFLOAD) += gpon_flow.o

# gpon_flow_offload.o -- the TC offload LIFECYCLE: the cookie->flow map, the
# entry and its unwind, destroy, stats, flush.  A family with an accelerator
# supplies five ops and writes none of this; a family without one compiles
# neither object.
obj-$(CONFIG_GPON_FLOW_OFFLOAD) += gpon_flow_offload.o

# gpon_flow_diag.o -- WHY a flow was not accelerated, named where the refusal is
# actually decided.  Sixteen causes that share three errnos; the families used to
# count them by errno and could name none of the ten that are -EOPNOTSUPP.
# Formats INTO A BUFFER only; the family owns the node and the level.
obj-$(CONFIG_GPON_FLOW_DIAG) += gpon_flow_diag.o

# gpon_edge.o -- the router EDGE (WAN address/MAC, next hop, LAN subnet) read
# out of the LIVE kernel, once for every family.  Gated with the lifecycle:
# only a board with an accelerator has anything to tell, and this tree ships to
# parts with <= 64 MB of RAM.
obj-$(CONFIG_GPON_FLOW_OFFLOAD) += gpon_edge.o
