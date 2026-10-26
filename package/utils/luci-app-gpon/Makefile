#
# Clean-room LuCI app: GPON ONU provisioning UI.
# SPDX-License-Identifier: GPL-2.0-or-later
#
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-gpon
PKG_VERSION:=1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=Confiared <brule.herman@confiared.com>

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-gpon
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=PON page: state, diagnosis, identity, far end, captures
  DEPENDS:=+luci-base +gpon-provision +luci-mod-status
  PKGARCH:=all
endef

define Package/luci-app-gpon/description
  The PON page, the same on every ONU family: fiber state and why the ONU is not
  (or not steadily) at O5, the editable GPON identity and OLT mode, the driver's
  state by category, and the OMCI/PLOAM captures a new OLT is supported from.
  Its ubus backend `gpon` also feeds the status page card.
endef

define Build/Compile
endef

define Package/luci-app-gpon/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/gpon
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/gpon/pon.js \
		$(1)/www/luci-static/resources/view/gpon/pon.js
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/status/include
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/status/include/35_gpon.js \
		$(1)/www/luci-static/resources/view/status/include/35_gpon.js
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/status/include/36_onu_health.js \
		$(1)/www/luci-static/resources/view/status/include/36_onu_health.js
	$(INSTALL_DIR) $(1)/www/luci-static/resources/tools
	$(INSTALL_DATA) ./htdocs/luci-static/resources/tools/onu-health.js \
		$(1)/www/luci-static/resources/tools/onu-health.js
	$(INSTALL_DIR) $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) ./root/usr/libexec/rpcd/onu-health $(1)/usr/libexec/rpcd/onu-health
	$(INSTALL_BIN) ./root/usr/libexec/rpcd/gpon $(1)/usr/libexec/rpcd/gpon
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./root/usr/share/luci/menu.d/luci-app-gpon.json \
		$(1)/usr/share/luci/menu.d/luci-app-gpon.json
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./root/usr/share/rpcd/acl.d/luci-app-gpon.json \
		$(1)/usr/share/rpcd/acl.d/luci-app-gpon.json
endef

$(eval $(call BuildPackage,luci-app-gpon))
