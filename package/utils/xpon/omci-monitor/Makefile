# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=omci-monitor
PKG_RELEASE:=4
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/omci-monitor
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=xPON
  TITLE:=Read-only OMCI monitor and userspace wire core
  DEPENDS:=@TARGET_galachip_gc1601
endef

define Package/omci-monitor/description
 Native, read-only OMCI RX/TX diagnostics and offline capture replay.
 Live capture needs the OMCI channel free (omcid stopped); with omcid
 running, replay the trace omcid writes on SIGUSR1. Never takes over OMCI
 provisioning.
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -Wall -Wextra -Werror \
		-I./src/include -o $(PKG_BUILD_DIR)/omci-monitor \
		./src/monitor.c ./src/netlink.c ./src/wire.c $(TARGET_LDFLAGS)
endef

define Package/omci-monitor/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omci-monitor $(1)/usr/sbin/omci-monitor
endef

$(eval $(call BuildPackage,omci-monitor))
