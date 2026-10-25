# SPDX-License-Identifier: GPL-2.0-only
#
# The GPON protocol core: PLOAM, OMCI, GEM and flow offload. It touches no
# registers; the Luna shell (drivers/net/ethernet/realtek) drives the MAC.
# The datapath keeps -O2 under the size-optimized kernel.

ccflags-y += -O2
obj-y += gpon_ddm.o
obj-y += gpon_sn.o
obj-y += gpon_hwaddr.o
obj-y += gpon_range_gate.o
obj-y += gpon_gem_us.o
obj-y += gpon_omci_core.o
obj-y += gpon_omci_me.o
obj-y += gpon_omci_trace.o
obj-$(CONFIG_GPON_PLOAM_DIAG) += gpon_ploam_diag.o
obj-$(CONFIG_GPON_OMCI_DIAG) += gpon_omci_diag.o
obj-$(CONFIG_GPON_GEM_DIAG) += gpon_gem_diag.o
obj-$(CONFIG_GPON_OLT_DIAG) += gpon_olt_diag.o
obj-y += gpon_ploam.o
obj-y += gpon_regseq.o
obj-$(CONFIG_GPON_FLOW_OFFLOAD) += gpon_flow.o
obj-$(CONFIG_GPON_FLOW_OFFLOAD) += gpon_flow_offload.o
obj-$(CONFIG_GPON_FLOW_DIAG) += gpon_flow_diag.o
obj-$(CONFIG_GPON_FLOW_OFFLOAD) += gpon_edge.o
