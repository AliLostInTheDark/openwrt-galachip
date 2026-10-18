# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=epon-oamd
PKG_RELEASE:=22
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/epon-oamd
  SECTION:=net
  CATEGORY:=Network
  TITLE:=EPON OAM daemon (IEEE 802.3ah and CTC extended OAM)
endef

define Package/epon-oamd/description
  Answers the OLT's OAM on an EPON ONU whose MAC runs MPCP in hardware:
  802.3ah discovery, CTC extended discovery, LOID authentication, churning
  keys and the CTC attributes an OLT reads and writes while provisioning
  an SFU. Runs only while xpon.main.mode is epon.
endef

define Package/epon-oamd/conffiles
/etc/config/epon
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(PKG_BUILD_DIR)/epon-oamd ./src/epon-oamd.c
endef

define Package/epon-oamd/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/config $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/epon-oamd $(1)/usr/sbin/epon-oamd
	$(INSTALL_CONF) ./files/etc/config/epon $(1)/etc/config/epon
	$(INSTALL_BIN) ./files/etc/init.d/epon-oamd $(1)/etc/init.d/epon-oamd
endef

$(eval $(call BuildPackage,epon-oamd))
