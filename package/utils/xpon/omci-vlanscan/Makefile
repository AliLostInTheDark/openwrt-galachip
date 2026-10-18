# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=omci-vlanscan
PKG_RELEASE:=4
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/omci-vlanscan
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Find the internet VLANs behind a GPON ONU
endef

define Package/omci-vlanscan/description
  Send a PPPoE PADI and a DHCP DISCOVER on the PON interface tagged with
  every candidate VLAN, and report the VLANs a PPPoE or DHCP server answers
  on, like the vendor vlan_search.
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall \
		-o $(PKG_BUILD_DIR)/omci-vlanscan ./src/omci-vlanscan.c
endef

define Package/omci-vlanscan/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omci-vlanscan $(1)/usr/sbin/omci-vlanscan
endef

$(eval $(call BuildPackage,omci-vlanscan))
