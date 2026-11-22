# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=xpon-agent
PKG_VERSION:=1.0.0
PKG_RELEASE:=5
PKG_MAINTAINER:=Ifran Ali <khanu.i.i.k.2000@gmail.com>
PKG_LICENSE:=GPL-2.0-only LGPL-2.1-or-later

include $(INCLUDE_DIR)/package.mk

OMCID_SOURCES := main.c transport.c backend-client.c backend-layout.c checkpoint.c control.c \
	metadata.c agent.c me-kernel.c native-platform.c identity-native.c \
	olt-watch.c
PROTOCOL_SOURCES := me.c profiles.c wire.c attributes.c

define Package/xpon-agent
  SECTION:=utils
  CATEGORY:=Utilities
  SUBMENU:=xPON
  TITLE:=xPON userspace control plane (GPON OMCI and EPON OAM)
  DEPENDS:=@TARGET_galachip_gc1601 +ucode +libucode +ucode-mod-fs +ucode-mod-log +ucode-mod-math +ucode-mod-ubus +ucode-mod-uci +ucode-mod-uloop +rpcd +rpcd-mod-ucode +libuci +libubus
  PROVIDES:=omci-agent epon-oamd omcid libomci-protocol omci-vlanscan
endef

define Package/xpon-agent/description
 The userspace side of the PON port. For GPON: omcid, the OMCI responder
 with its managed-entity catalogue, attribute codec and OLT profiles; a
 ucode binding for the Linux xPON OMCI Generic Netlink API; the ubus
 service LuCI and omcictl use; and omci-vlanscan, which finds the internet
 VLANs behind the ONU. For EPON: epon-oamd, which answers the OLT's IEEE
 802.3ah and CTC extended OAM (discovery, LOID authentication, churning
 keys, SFU provisioning) while xpon.main.mode is epon.
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
		-I$(PKG_BUILD_DIR)/include \
		-o $(PKG_BUILD_DIR)/omci.so \
		$(PKG_BUILD_DIR)/omci_ucode.c $(PKG_BUILD_DIR)/xpon-nl.c \
		-shared $(TARGET_LDFLAGS) -lucode
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-I$(PKG_BUILD_DIR)/include \
		-o $(PKG_BUILD_DIR)/epon-oamd $(PKG_BUILD_DIR)/epon-oamd.c \
		$(PKG_BUILD_DIR)/xpon-nl.c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall \
		-I$(PKG_BUILD_DIR) -I$(PKG_BUILD_DIR)/include \
		-o $(PKG_BUILD_DIR)/omci-vlanscan ./vlanscan/src/omci-vlanscan.c \
		$(PKG_BUILD_DIR)/xpon-nl.c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-std=gnu11 -Wall -Wextra -Werror \
		-Wno-unused-parameter -Wno-pointer-sign -Wno-sign-compare \
		-I./omcid/src -I./protocol/src -I./protocol/src/include -I./src -I./src/include \
		-o $(PKG_BUILD_DIR)/omcid \
		$(addprefix ./omcid/src/,$(OMCID_SOURCES)) ./src/xpon-nl.c \
		$(addprefix ./protocol/src/,$(PROTOCOL_SOURCES)) \
		$(TARGET_LDFLAGS) -lm
endef

define Package/xpon-agent/install
	$(INSTALL_DIR) $(1)/usr/lib/ucode
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omci.so $(1)/usr/lib/ucode/omci.so

	$(INSTALL_DIR) $(1)/usr/share/ucode/omci
	$(INSTALL_DATA) ./files/usr/share/ucode/omci/mib.uc \
		$(1)/usr/share/ucode/omci/mib.uc
	$(INSTALL_DATA) ./files/usr/share/ucode/omci/g988.uc \
		$(1)/usr/share/ucode/omci/g988.uc
	$(INSTALL_DATA) ./files/usr/share/ucode/omci/voice.uc \
		$(1)/usr/share/ucode/omci/voice.uc
	$(INSTALL_DATA) ./files/usr/share/ucode/omci/mcast.uc \
		$(1)/usr/share/ucode/omci/mcast.uc

	$(INSTALL_DIR) $(1)/usr/share/rpcd/ucode
	$(INSTALL_BIN) ./files/usr/share/rpcd/ucode/xpon $(1)/usr/share/rpcd/ucode/xpon
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./files/usr/share/rpcd/acl.d/xpon-public.json $(1)/usr/share/rpcd/acl.d/

	$(INSTALL_DIR) $(1)/usr/share/ucode/xpon
	$(INSTALL_DATA) ./files/usr/share/ucode/xpon/summary.uc ./files/usr/share/ucode/xpon/vlan.uc \
		$(1)/usr/share/ucode/xpon/

	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omcid $(1)/usr/sbin/omcid
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/epon-oamd $(1)/usr/sbin/epon-oamd
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omci-vlanscan $(1)/usr/sbin/omci-vlanscan
	$(INSTALL_BIN) ./files/usr/sbin/omcictl $(1)/usr/sbin/omcictl
	$(INSTALL_BIN) ./files/usr/sbin/xponctl $(1)/usr/sbin/xponctl
	$(INSTALL_BIN) ./files/usr/sbin/omci-diag $(1)/usr/sbin/omci-diag
	$(INSTALL_BIN) ./files/usr/sbin/omci-report $(1)/usr/sbin/omci-report
	$(INSTALL_BIN) ./files/usr/sbin/omci-voice $(1)/usr/sbin/omci-voice
	$(INSTALL_BIN) ./files/usr/sbin/omci-mcast $(1)/usr/sbin/omci-mcast

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./omcid/files/etc/init.d/omcid $(1)/etc/init.d/omcid
	$(INSTALL_BIN) ./files/etc/init.d/epon-oamd $(1)/etc/init.d/epon-oamd
	$(INSTALL_BIN) ./files/etc/init.d/omci-voice $(1)/etc/init.d/omci-voice
	$(INSTALL_BIN) ./files/etc/init.d/omci-mcast $(1)/etc/init.d/omci-mcast

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/omci $(1)/etc/config/omci
	$(INSTALL_CONF) ./files/etc/config/epon $(1)/etc/config/epon

	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/etc/uci-defaults/80_omci_reboot \
		$(1)/etc/uci-defaults/80_omci_reboot
	$(INSTALL_BIN) ./files/etc/uci-defaults/81_omci_drop_dying_gasp \
		$(1)/etc/uci-defaults/81_omci_drop_dying_gasp
endef

define Package/xpon-agent/conffiles
/etc/config/omci
/etc/config/epon
/etc/omci/olt-profiles.conf
endef

define Package/xpon-agent/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || /etc/init.d/rpcd reload >/dev/null 2>&1 || true
endef

$(eval $(call BuildPackage,xpon-agent))
