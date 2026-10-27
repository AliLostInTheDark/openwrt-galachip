# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_VERSION:=0.1
PKG_RELEASE:=9
LUCI_TITLE:=LuCI PON mode selection (GPON or EPON)
LUCI_PKGARCH:=all
PKG_LICENSE:=GPL-2.0-only
LUCI_DEPENDS:=+luci-base +xpon-report

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
