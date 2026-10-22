# SPDX-License-Identifier: GPL-2.0-only

define KernelPackage/zx279125-ethernet
  SUBMENU:=$(NETWORK_DEVICES_MENU)
  TITLE:=ZTE ZX279125 Ethernet
  DEPENDS:=@TARGET_zte_zx279125 +kmod-mii
  KCONFIG:=CONFIG_ZX279125_ETHERNET
  FILES:=$(LINUX_DIR)/drivers/net/ethernet/zte/zx279125-ethernet.ko
  AUTOLOAD:=$(call AutoLoad,16,zx279125-ethernet)
endef

define KernelPackage/zx279125-ethernet/description
  Ethernet UNI and shared CPU packet-engine driver for the ZX279125.
endef

$(eval $(call KernelPackage,zx279125-ethernet))
