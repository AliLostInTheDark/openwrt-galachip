#
# Copyright (C) 2026 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_VERSION:=2026.10
PKG_RELEASE:=1

PKG_HASH:=3f856da3cbbc93af51ff95fde18588a348b4a26723b01d9841e999d988481015

include $(INCLUDE_DIR)/u-boot.mk
include $(INCLUDE_DIR)/package.mk

define U-Boot/Default
  BUILD_TARGET:=zte
  BUILD_SUBTARGET:=zx279125
endef

define U-Boot/vsol_v2801sg
  NAME:=VSOL V2801SG
  BUILD_DEVICES:=vsol_v2801sg
  UBOOT_CONFIG:=zx279125_vsol_v2801sg
  UBOOT_IMAGE:=u-boot.bin
endef

UBOOT_TARGETS := vsol_v2801sg

define Build/Compile
	$(call Build/Compile/U-Boot)
	$(STAGING_DIR_HOST)/bin/mkimage -A arm -O u-boot -T firmware -C none \
		-a 0x40400000 -e 0x40400000 \
		-n 'U-Boot $(PKG_VERSION) for VSOL V2801SG' \
		-d $(PKG_BUILD_DIR)/u-boot.bin $(PKG_BUILD_DIR)/u-boot.img
endef

define Package/u-boot/install
	$(CP) \
		$(PKG_BUILD_DIR)/u-boot.bin \
		$(PKG_BUILD_DIR)/u-boot.img \
		$(1)/
endef

$(eval $(call BuildPackage/U-Boot))
