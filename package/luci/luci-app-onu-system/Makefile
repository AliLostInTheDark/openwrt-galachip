# SPDX-License-Identifier: Apache-2.0

include $(TOPDIR)/rules.mk

PKG_VERSION:=0.1
PKG_RELEASE:=1
LUCI_TITLE:=LuCI LED control and a simpler router password page for ONUs
LUCI_DESCRIPTION:=System -> LEDs: every LED's mode, including the PON and LOS \
	LEDs that the xPON driver drives. Router Password: offers only accounts \
	that can log in instead of every /etc/passwd entry.
LUCI_PKGARCH:=all
LUCI_DEPENDS:=+luci-base +luci-mod-system
PKG_LICENSE:=Apache-2.0

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
