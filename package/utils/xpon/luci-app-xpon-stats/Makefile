# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_VERSION:=2.0.0
PKG_RELEASE:=4
LUCI_TITLE:=GPON/EPON ONU status for the router behind an OpenWrt xPON ONU
LUCI_PKGARCH:=all
PKG_LICENSE:=GPL-2.0-only
LUCI_DEPENDS:=+luci-base +uclient-fetch +jsonfilter

define Package/luci-app-xpon-stats/conffiles
/etc/config/xpon-stats
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
