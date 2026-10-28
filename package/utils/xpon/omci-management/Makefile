# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=omci-management
PKG_RELEASE:=3
PKG_LICENSE:=GPL-2.0-only
include $(INCLUDE_DIR)/package.mk

define Package/omci-management
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=xPON
  TITLE:=Independent untagged IPv4 ONU management
  DEPENDS:=+uci +ip-tiny +tc-tiny +kmod-veth +kmod-sched-flower +kmod-sched-act-vlan
endef

define Build/Compile
endef

define Package/omci-management/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d
	$(INSTALL_BIN) ./files/omci-management $(1)/usr/sbin/omci-management
	$(INSTALL_BIN) ./files/omci-management.init $(1)/etc/init.d/omci-management
endef

$(eval $(call BuildPackage,omci-management))
