# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=xpon-diagnostics
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/xpon-diagnostics
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=xPON
  TITLE:=Bounded GPON/EPON datapath diagnostic collector
  DEPENDS:=@TARGET_galachip_gc1601
endef

define Build/Compile
endef

define Package/xpon-diagnostics/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/xpon-diagnostics $(1)/usr/sbin/xpon-diagnostics
endef

$(eval $(call BuildPackage,xpon-diagnostics))
