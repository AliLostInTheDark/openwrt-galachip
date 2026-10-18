# SPDX-License-Identifier: GPL-2.0-only

define KernelPackage/gc1601-gpon-diag
  SUBMENU:=$(OTHER_MENU)
  TITLE:=GalaChip GC1601 GPON diagnostics
  DEPENDS:=@TARGET_galachip
  KCONFIG:=CONFIG_GC1601_GPON_DIAG
  FILES:=$(LINUX_DIR)/drivers/soc/galachip/gc1601-gpon-diag.ko
endef

define KernelPackage/gc1601-gpon-diag/description
  Logs the GPON activation, PLOAMs, OMCI messages, T-CONT, GEM port and
  service set-up and the switch flows to the kernel log. Not loaded at
  boot: run "modprobe gc1601-gpon-diag" on the OLT in question and send
  the output of xpon-report.
endef

$(eval $(call KernelPackage,gc1601-gpon-diag))

define KernelPackage/gc1601-epon-diag
  SUBMENU:=$(OTHER_MENU)
  TITLE:=GalaChip GC1601 EPON diagnostics
  DEPENDS:=@TARGET_galachip
  KCONFIG:=CONFIG_GC1601_EPON_DIAG
  FILES:=$(LINUX_DIR)/drivers/soc/galachip/gc1601-epon-diag.ko
endef

define KernelPackage/gc1601-epon-diag/description
  Logs EPON MAC registers, registration events and the first downstream
  and upstream frames to the kernel log. Not loaded at boot: run
  "modprobe gc1601-epon-diag" on an EPON OLT and send the dmesg.
endef

$(eval $(call KernelPackage,gc1601-epon-diag))
