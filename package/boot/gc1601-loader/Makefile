# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=gc1601-loader
PKG_VERSION:=0.1
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
PKG_FLAGS:=nonshared

include $(INCLUDE_DIR)/package.mk

define Package/gc1601-loader
  SECTION:=boot
  CATEGORY:=Boot Loaders
  DEPENDS:=@TARGET_galachip
  TITLE:=First-stage loader for GalaChip GC1601 boards
endef

define Package/gc1601-loader/description
  Replaces the stock U-Boot SPL at flash offset 0: DDR set-up, then the
  OpenWrt kernel from flash, with a serial menu for XMODEM recovery and
  for starting the stock U-Boot. Not installed by sysupgrade; flashing
  it is a separate, manual step.
endef

define Build/Prepare
	$(CP) ./src/. $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		LOADER_VERSION="$(PKG_VERSION)-$(PKG_RELEASE)"
endef

define Build/InstallDev
	$(INSTALL_DIR) $(STAGING_DIR_IMAGE) $(BIN_DIR)
	$(CP) $(PKG_BUILD_DIR)/gc1601-loader.bin $(PKG_BUILD_DIR)/gc1601-loader-flash.bin \
		$(STAGING_DIR_IMAGE)/
	$(CP) $(PKG_BUILD_DIR)/gc1601-loader.bin $(PKG_BUILD_DIR)/gc1601-loader-flash.bin \
		$(BIN_DIR)/
endef

$(eval $(call BuildPackage,gc1601-loader))
