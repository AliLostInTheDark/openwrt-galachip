# SPDX-License-Identifier: GPL-2.0-only
#
# drivers/net/flowcore: the register-free halves of the Luna drivers (flow
# hashing, PON-MAC sequencing, L3/L4 table logic, GMAC rings, the GN25L95
# calibration sequence and the OMCI VLAN install), each built with the driver
# that calls it.

ccflags-y += -I$(srctree)/drivers/net/gpon

obj-y += flowcore_hash.o
obj-$(CONFIG_LUNA_GPON) += luna_ponmac_logic.o
obj-$(CONFIG_RTL9602C_ETH) += rtl9602c_l34_logic.o
obj-$(CONFIG_RTL9602C_ETH) += luna_gmac_logic.o
obj-$(CONFIG_LUNA_GPON) += gn25l95_cal_logic.o
obj-$(CONFIG_LUNA_GPON) += luna_vlan_install.o
