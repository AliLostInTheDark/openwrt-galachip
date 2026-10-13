# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_VERSION:=0.1
PKG_RELEASE:=7
LUCI_TITLE:=LuCI support for EPON (epon-oamd)
LUCI_PKGARCH:=all
PKG_LICENSE:=GPL-2.0-only
LUCI_DEPENDS:=+luci-base +epon-oamd +luci-app-xpon

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
