# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_VERSION:=1.0.0
PKG_RELEASE:=1
LUCI_TITLE:=GPON ONU status for the router behind an OpenWrt OMCI ONU
LUCI_PKGARCH:=all
PKG_LICENSE:=GPL-2.0-only
LUCI_DEPENDS:=+luci-base +uclient-fetch +jsonfilter

define Package/luci-app-omci-stats/conffiles
/etc/config/omci-stats
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
