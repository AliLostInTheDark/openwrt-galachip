# SPDX-License-Identifier: GPL-2.0-only
#
# OpenWrt package for BOSA / SFF-8472 Optical Transceiver Management
# Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>

include $(TOPDIR)/rules.mk

PKG_NAME:=bosa-tools
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/bosa-tools
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=Telephony
  TITLE:=Optical BOSA Transceiver (SFF-8472) DDMI Diagnostic Utility
  DEPENDS:=+kmod-i2c-core
  MAINTAINER:=Ifran Ali <khanu.i.i.k.2000@gmail.com>
endef

define Package/bosa-tools/description
  Command-line diagnostics and control utility for on-board optical
  transceivers (BOSA / SFF-8472 DDMI) on ZTE Sanechips GPON/EPON ONUs.
  Monitors laser temperature, supply rail voltage, laser bias current,
  optical TX/RX power in uW and dBm, and controls laser safety gating.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Package/bosa-tools/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bosa-info $(1)/usr/sbin/bosa-info
	$(LN) bosa-info $(1)/usr/sbin/bosa-ctl
endef

$(eval $(call BuildPackage,bosa-tools))
