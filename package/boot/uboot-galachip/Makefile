# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_VERSION:=2026.10
PKG_RELEASE:=1
PKG_HASH:=3f856da3cbbc93af51ff95fde18588a348b4a26723b01d9841e999d988481015

include $(INCLUDE_DIR)/u-boot.mk
include $(INCLUDE_DIR)/package.mk

$(eval $(call TestHostCommand,python3-lzma,Please install Python 3 with lzma support,python3 -c 'import lzma'))

PKG_LICENSE:=GPL-2.0-only GPL-2.0-or-later LZMA-SDK-9.22
PKG_LICENSE_FILES:=Licenses/README lib/lzma/LzmaDec.c

define U-Boot/xz000_g7
  NAME:=TP-Link XZ000-G7
  BUILD_TARGET:=galachip
  BUILD_SUBTARGET:=gc1601
  BUILD_DEVICES:=tplink_xz000-g7
  UBOOT_IMAGE:=output/artifacts/xz000-g7-uboot-proper.img
endef

UBOOT_TARGETS:=xz000_g7

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./files/. $(PKG_BUILD_DIR)/
endef

# build.py configures separate RAM and production output directories.
define Build/Configure
endef

define Build/Compile
	+python3 ./tools/build.py \
		--source $(PKG_BUILD_DIR) \
		--output $(PKG_BUILD_DIR)/output \
		--cross-compile "$(TARGET_CROSS)" \
		--jobs $(or $(patsubst -j%,%,$(filter -j%,$(PKG_JOBS))),1) \
		--revision "$(shell git rev-parse HEAD 2>/dev/null || echo unknown)" \
		--localversion="-OpenWrt-$(REVISION)"
endef

# Upstream names: u-boot-with-spl.bin is the whole boot region for
# upgrade_uboot, u-boot.bin the RAM image chainloaded with go, u-boot.img
# proper alone and u-boot-spl.bin the ROM-headed SPL.
define Build/InstallDev
	$(INSTALL_DIR) $(STAGING_DIR_IMAGE)
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/output/artifacts/xz000-g7-bootregion.bin \
		$(STAGING_DIR_IMAGE)/$(BUILD_DEVICES)-u-boot-with-spl.bin
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/$(UBOOT_IMAGE) \
		$(STAGING_DIR_IMAGE)/$(BUILD_DEVICES)-u-boot.img
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/output/artifacts/xz000-g7-spl-rom.bin \
		$(STAGING_DIR_IMAGE)/$(BUILD_DEVICES)-u-boot-spl.bin
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/output/artifacts/xz000-g7-uboot-ram.bin \
		$(STAGING_DIR_IMAGE)/$(BUILD_DEVICES)-u-boot.bin
endef

define Package/u-boot/install/default
endef

$(eval $(call BuildPackage/U-Boot))
