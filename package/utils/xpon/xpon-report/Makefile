# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=xpon-report
PKG_RELEASE:=3
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/xpon-report
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Collect a GPON/EPON diagnostics report
  PKGARCH:=all
endef

define Package/xpon-report/description
  xpon-report writes the PON state (GPON or EPON), PLOAM, OMCI and OAM,
  T-CONTs, GEM ports, services, switch flows, optics, packet engine,
  kernel log and settings into one file, with serial numbers, passwords
  and LOIDs masked.
endef

define Build/Compile
endef

define Package/xpon-report/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/xpon-report $(1)/usr/sbin/xpon-report
endef

$(eval $(call BuildPackage,xpon-report))
