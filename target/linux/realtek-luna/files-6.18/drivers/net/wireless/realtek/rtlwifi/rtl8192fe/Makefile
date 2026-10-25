# SPDX-License-Identifier: GPL-2.0-only
# Realtek RTL8192F 802.11n PCIe WiFi — clean-room mac80211 sub-driver.
# Reuses the rtlwifi PCI core (rtlwifi.ko + rtl_pci.ko); see DESIGN.md.

rtl8192fe-objs :=		\
		dm.o		\
		fw.o		\
		hw.o		\
		led.o		\
		phy.o		\
		pwrseq.o	\
		rf.o		\
		sw.o		\
		table.o		\
		trx.o

obj-$(CONFIG_RTL8192FE) += rtl8192fe.o

ccflags-y += -I$(srctree)/$(src)/..
