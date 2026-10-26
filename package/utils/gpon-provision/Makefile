#
# Clean-room OpenWrt package: per-board GPON ONU provisioning.
# SPDX-License-Identifier: GPL-2.0-or-later
#
include $(TOPDIR)/rules.mk

PKG_NAME:=gpon-provision
PKG_VERSION:=1
PKG_RELEASE:=5
PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=Confiared <brule.herman@confiared.com>

include $(INCLUDE_DIR)/package.mk

define Package/gpon-provision
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Per-board GPON ONU provisioning (MAC/SN from factory partition)
  DEPENDS:=+zlib
endef

define Package/gpon-provision/description
  Reads this unit's factory (HW_SETTING) and SW config blocks from the Realtek
  apmib config MTD partition and applies the per-board MAC to the netdev plus the
  GPON identity (SN/LOID/PLOAM/OMCI-OLT-mode) at early boot. One identical
  firmware image serves an entire fleet; each board self-provisions from its own
  flash. The factory partition is only ever read here, never rewritten.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(PKG_BUILD_DIR)/rtk_factory $(PKG_BUILD_DIR)/rtk_factory.c -lz
endef

define Package/gpon-provision/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/rtk_factory $(1)/usr/sbin/rtk_factory
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/gpon_provision.init $(1)/etc/init.d/gpon_provision
endef

$(eval $(call BuildPackage,gpon-provision))
