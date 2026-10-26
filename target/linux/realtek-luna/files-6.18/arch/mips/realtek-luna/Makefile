# SPDX-License-Identifier: GPL-2.0-only
# Realtek "Luna" GPON ONU (RTL960xC / RLX Taroko) platform support.

obj-y := prom.o setup.o irq-luna.o
obj-$(CONFIG_PCI) += pcie-luna.o
obj-$(CONFIG_CPU_HAS_WB) += wbflush.o
obj-$(CONFIG_CPU_R3K_TLB) += cache-luna.o
