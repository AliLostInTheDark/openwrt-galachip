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
  NAME:=TP-Link XZ000-G7 (RAM-only development)
  BUILD_TARGET:=galachip
  BUILD_SUBTARGET:=gc1601
  DEFAULT:=n
  UBOOT_IMAGE:=output/artifacts/*
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
		--localversion "-OpenWrt-$(REVISION)"
endef

# Binary build artifacts only. No image recipe, sysupgrade hook or flash command.
define Package/u-boot/install
	$(INSTALL_DIR) $(1)
	$(CP) $(PKG_BUILD_DIR)/output/artifacts/. $(1)/
endef

$(eval $(call BuildPackage/U-Boot))
