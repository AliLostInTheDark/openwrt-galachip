# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_VERSION:=0.9.2
PKG_RELEASE:=48
LUCI_TITLE:=LuCI support for OMCI
LUCI_PKGARCH:=all
PKG_MAINTAINER:=Matheus Sampaio Queiroga <srherobrine20@gmail.com>
PKG_LICENSE:=GPL-2.0-only
LUCI_DEPENDS:=@(TARGET_galachip_gc1601||TARGET_zte_zx279125||TARGET_realtek_luna) +luci-base +omci-agent +omci-vlanscan +luci-app-xpon +TARGET_realtek_luna:omci-management

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
