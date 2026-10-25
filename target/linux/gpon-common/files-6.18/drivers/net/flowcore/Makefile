# SPDX-License-Identifier: GPL-2.0-only
#
# drivers/net/flowcore -- the hardware-decoupled half of L3/L4 flow offload.
#
# A SIBLING OF drivers/net/gpon, NOT part of it: that directory is the GPON
# PROTOCOL core, and flow offload is not GPON. Authored once in
# target/linux/gpon-common/files-6.18/ and reaching both targets through the
# FILES_DIR they already carry, so a second common layer costs one obj- line in
# drivers/net/Makefile and no change to any target Makefile.
obj-y += flowcore_hash.o

# ── hoisted logic, one object per shell it left (bulk_hoist.py) ──────────
# Each object is gated on the SAME config symbol as the shell it was hoisted
# from, so a target never compiles (or links) logic for a driver it does not
# build -- the lean-kernel-per-model rule.  The functions kept here passed the
# hoist rule for real: no MMIO, no kernel service, no shell file-scope state.
# Everything the mechanical pass moved that failed that rule (I2C/MMIO helpers,
# net/debugfs/param callbacks, MIPS arch glue) was moved back into its shell.
#
# cg_sn_parse wraps the common core's gpon_sn_parse(), hence the include path
# (same spelling as both shells' own -I for the shared GPON core).
ccflags-y += -I$(srctree)/drivers/net/gpon
obj-$(CONFIG_CORTINA_GPON) += cortina_gpon_logic.o
obj-$(CONFIG_CORTINA_NI) += cortina_ni_rx_logic.o cortina_ni_tx_logic.o
obj-$(CONFIG_CORTINA_NI_FLOWOFFLOAD) += cortina_ni_flowoffload_logic.o \
	cortina_l3fe_logic.o
obj-$(CONFIG_LUNA_GPON) += luna_gpon_logic.o luna_ponmac_logic.o
# ★ REMOVED 2026-09-10: CONFIG_RTL9607C_GPON is not a Kconfig symbol -- no
#   `config RTL9607C_GPON` exists anywhere in target/linux, so this line was
#   always empty. It was harmless (line 27 already builds the object under
#   the gate that DOES exist, and the .o is present in all three subtargets'
#   build_dir) but it read as "also built for the 9607C", which is a claim
#   about a symbol nobody declares.
#   ⚠ If an RTL9607C gate is ever added, add it HERE and to the Kconfig in
#     the same change -- a dependency on an undeclared symbol is silent.
obj-$(CONFIG_RTL9602C_ETH) += rtl9602c_l34_logic.o
# ★ luna_gmac_logic.o under BOTH Ethernet gates (the luna_ponmac_logic.o
# shape): the family's two Ethernet shells are gated on DISJOINT symbols
# (taroko: RTL9602C_ETH only; interaptiv: LUNA_ETH only), so family GMAC
# logic listed under one symbol alone links on one board and breaks the
# other -- the measured crossconfig_call_guard defect.
obj-$(CONFIG_RTL9602C_ETH) += luna_gmac_logic.o
obj-$(CONFIG_LUNA_ETH) += luna_gmac_logic.o
obj-$(CONFIG_LUNA_ETH) += luna_flow_logic.o

# The GN25L95 BOSA calibration: the ORDERED sequence and a fail-first executor,
# both HW-decoupled.  Under BOTH GPON gates for the luna_gmac_logic.o reason --
# the part is on the Cortina board today and on the Luna ones tomorrow, and an
# object listed under one family's symbol alone links on one and breaks the
# other.  Proven against stock on the real part by
# dev/rtl9607c-test/gn25l95_cal_diff_test.c.
obj-$(CONFIG_CORTINA_GPON) += gn25l95_cal_logic.o
obj-$(CONFIG_LUNA_GPON) += gn25l95_cal_logic.o

# The OMCI WAN service model's VLAN half: the core DECIDES (drivers/net/gpon/
# gpon_omci_vlan.h) and these two FILL struct gpon_vlan_ops.  Gated on each
# family's GPON symbol rather than its Ethernet one: the caller is the OMCI
# side, and CONFIG_LUNA_GPON is the one symbol both Luna subtargets set (the
# two Luna Ethernet shells are gated on DISJOINT symbols, which is the measured
# crossconfig_call_guard defect).  Neither object names a register ADDRESS --
# those arrive in the per-die table the shell fills -- so both build on x86 and
# their write SEQUENCE is asserted by dev/rtl9607c-test/gpon_vlan_install_diff_test.
obj-$(CONFIG_CORTINA_GPON) += cortina_vlan_install.o
obj-$(CONFIG_LUNA_GPON) += luna_vlan_install.o
