# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=omci-agent
PKG_VERSION:=0.9.2
PKG_RELEASE:=16
PKG_MAINTAINER:=Matheus Sampaio Queiroga <srherobrine20@gmail.com>
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/omci-agent
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=xPON
  TITLE:=GC1601 OMCI userspace control plane
  DEPENDS:=@TARGET_galachip_gc1601 +ucode +libucode +ucode-mod-ubus +ucode-mod-uci +ucode-mod-uloop +rpcd +rpcd-mod-ucode +libuci +libubus
endef

define Package/omci-agent/description
 Native C ucode binding for the Linux xPON OMCI Generic Netlink API, an rpcd
 ucode service exposing omci.agent over ubus, and an omcictl ubus client
 for the GalaChip GC1601.
 No helper process is used between rpcd and the kernel.
endef

define Build/Prepare
	rm -rf $(PKG_BUILD_DIR)
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/. $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-fPIC -Wall -Wextra -Werror \
		-I$(STAGING_DIR)/usr/include \
		-o $(PKG_BUILD_DIR)/omci.so \
		$(PKG_BUILD_DIR)/omci_ucode.c \
		-shared $(TARGET_LDFLAGS) -lucode
endef

define Package/omci-agent/install
	$(INSTALL_DIR) $(1)/usr/lib/ucode
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omci.so $(1)/usr/lib/ucode/omci.so

	$(INSTALL_DIR) $(1)/usr/share/rpcd/ucode
	$(INSTALL_BIN) ./files/usr/share/rpcd/ucode/omci.agent \
		$(1)/usr/share/rpcd/ucode/omci.agent

	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/usr/sbin/omcictl $(1)/usr/sbin/omcictl

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/omci $(1)/etc/config/omci

	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/etc/uci-defaults/80_omci_reboot \
		$(1)/etc/uci-defaults/80_omci_reboot
	$(INSTALL_BIN) ./files/etc/uci-defaults/81_omci_drop_dying_gasp \
		$(1)/etc/uci-defaults/81_omci_drop_dying_gasp
endef

define Package/omci-agent/conffiles
/etc/config/omci
endef

define Package/omci-agent/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || /etc/init.d/rpcd reload >/dev/null 2>&1 || true
endef

$(eval $(call BuildPackage,omci-agent))
