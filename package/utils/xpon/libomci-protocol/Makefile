# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=libomci-protocol
PKG_RELEASE:=3
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/libomci-protocol
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=Shared OMCI catalogue, attribute codec and OLT profiles
  DEPENDS:=@TARGET_galachip_gc1601
endef

define Package/libomci-protocol/description
 Native userspace library shared with the Linux OMCI agent. Provides the
 managed-entity catalogue, attribute packing, wire framing and OLT profiles.
 This library does not start a daemon or take over the live OMCC.
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -fPIC -shared \
		-Wall -Wextra -Werror -I./src/include \
		-Wl,-soname,libomci-protocol.so.1 \
		-o $(PKG_BUILD_DIR)/libomci-protocol.so.1 \
		./src/me.c ./src/profiles.c ./src/wire.c ./src/attributes.c $(TARGET_LDFLAGS)
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/omci/linux $(1)/usr/lib
	$(CP) ./src/me.h ./src/wire.h ./src/attributes.h $(1)/usr/include/omci/
	$(CP) ./src/include/linux/*.h $(1)/usr/include/omci/linux/
	$(CP) $(PKG_BUILD_DIR)/libomci-protocol.so.1 $(1)/usr/lib/
	$(LN) libomci-protocol.so.1 $(1)/usr/lib/libomci-protocol.so
endef

define Package/libomci-protocol/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/libomci-protocol.so.1 $(1)/usr/lib/
	$(LN) libomci-protocol.so.1 $(1)/usr/lib/libomci-protocol.so
endef

$(eval $(call BuildPackage,libomci-protocol))
