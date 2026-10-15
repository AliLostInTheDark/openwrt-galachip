# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_VERSION:=0.9.2
PKG_RELEASE:=16
LUCI_TITLE:=LuCI support for GC1601 OMCI
LUCI_PKGARCH:=all
PKG_MAINTAINER:=Matheus Sampaio Queiroga <srherobrine20@gmail.com>
PKG_LICENSE:=GPL-2.0-only
LUCI_DEPENDS:=@TARGET_galachip_gc1601 +luci-base +omci-agent +ucode-mod-math +omci-vlanscan +luci-app-xpon

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
