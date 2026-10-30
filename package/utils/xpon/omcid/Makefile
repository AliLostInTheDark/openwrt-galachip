# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=omcid
PKG_RELEASE:=15
PKG_LICENSE:=GPL-2.0-only LGPL-2.1-or-later
PKG_BUILD_DEPENDS:=libomci-protocol

include $(INCLUDE_DIR)/package.mk

define Package/omcid
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=xPON
  TITLE:=Userspace GPON OMCI responder
  DEPENDS:=@(TARGET_galachip_gc1601||TARGET_zte_zx279125||TARGET_realtek_luna) +libomci-protocol
endef

define Package/omcid/description
 The existing OMCI engine running in a single userspace event loop, with
 live MIB administration, OLT profiles and exclusive typed kernel hardware
 operations. Started at boot in GPON mode; the kernel has no OMCI engine
 of its own.
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-std=gnu11 -Wall -Wextra -Werror \
		-Wno-unused-parameter -Wno-pointer-sign -Wno-sign-compare \
		-I./src -I$(STAGING_DIR)/usr/include/omci \
		-o $(PKG_BUILD_DIR)/omcid ./src/main.c ./src/transport.c \
		./src/backend-client.c ./src/checkpoint.c ./src/control.c ./src/metadata.c \
		./src/agent.c ./src/me-kernel.c ./src/native-platform.c \
		./src/identity-native.c ./src/int-log-native.c ./src/olt-watch.c \
		$(TARGET_LDFLAGS) -lomci-protocol
endef

define Package/omcid/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omcid $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/omcid $(1)/etc/init.d/
endef

$(eval $(call BuildPackage,omcid))
