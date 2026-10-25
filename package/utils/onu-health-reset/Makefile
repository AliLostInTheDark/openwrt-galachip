#
# Clean-room OpenWrt package: restart an ONU that stays sick while its watchdog is still fed.
# SPDX-License-Identifier: GPL-2.0-or-later
#
include $(TOPDIR)/rules.mk

PKG_NAME:=onu-health-reset
PKG_VERSION:=1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=Confiared <brule.herman@confiared.com>

include $(INCLUDE_DIR)/package.mk

define Package/onu-health-reset
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Restart an ONU that stays sick while its watchdog is still fed
  PKGARCH:=all
endef

define Package/onu-health-reset/description
  Every minute: procd/ubus and netifd must answer, and major page faults must stay under
  100/s (a reclaim livelock re-reads code from flash without end). Five sick minutes in a
  row, after ten minutes of uptime, put the reason in the kernel log and panic, so the
  unit's crash records keep it; three resets without a healthy day stop the resets.
endef

define Build/Compile
endef

define Package/onu-health-reset/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d
	$(INSTALL_BIN) ./files/onu-health-reset $(1)/usr/sbin/onu-health-reset
	$(INSTALL_BIN) ./files/onu-health-reset.init $(1)/etc/init.d/onu-health-reset
endef

$(eval $(call BuildPackage,onu-health-reset))
