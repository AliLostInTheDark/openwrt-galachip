# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_VERSION:=1.0.0
PKG_RELEASE:=1
LUCI_TITLE:=LuCI support for the PON port (PON mode, GPON OMCI and EPON OAM)
LUCI_PKGARCH:=all
PKG_MAINTAINER:=Ifran Ali <khanu.i.i.k.2000@gmail.com>
PKG_LICENSE:=GPL-2.0-only
LUCI_DEPENDS:=@TARGET_galachip_gc1601 +luci-base +xpon-agent +xpon-report
PKG_PROVIDES:=luci-app-omci luci-app-epon

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
