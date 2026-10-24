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

ZX279125_SMALL_CONFIG := --disable ENV_IS_IN_SPI_FLASH \
  --enable ENV_IS_NOWHERE --disable CMD_SAVEENV \
  --disable SPI_FLASH_UNLOCK_ALL \
  --disable CMD_MTD --disable CMD_MD5SUM --disable CMD_ELF \
  --disable CMD_LOADB --disable CMD_LOADS --disable CMD_SPI \
  --disable CMD_DHCP --disable CMD_BOOTP --disable CMD_XIMG

define U-Boot/vsol_v2801sg
  NAME:=VSOL V2801SG
  BUILD_DEVICES:=vsol_v2801sg
  UBOOT_CONFIG:=zx279125_vsol_v2801sg
  UBOOT_IMAGE:=u-boot.bin
  UBOOT_CUSTOMIZE_CONFIG := $(ZX279125_SMALL_CONFIG)
endef

UBOOT_TARGETS := vsol_v2801sg

define U-Boot/vsol_v2801sg-chainload
  NAME:=VSOL V2801SG RAM chainload test
  BUILD_DEVICES:=vsol_v2801sg
  UBOOT_CONFIG:=zx279125_vsol_v2801sg
  UBOOT_IMAGE:=u-boot.bin
  UBOOT_CUSTOMIZE_CONFIG := $(ZX279125_SMALL_CONFIG) \
    --disable ZX279125_SERIAL_INIT \
    --set-val BOOTDELAY -1 --set-str BOOTCOMMAND "" \
    --set-str IDENT_STRING " RAM-chainload"
endef

UBOOT_TARGETS += vsol_v2801sg-chainload

define U-Boot/vsol_v2801sg-uart-test
  NAME:=VSOL V2801SG RAM UART initialization test
  BUILD_DEVICES:=vsol_v2801sg
  UBOOT_CONFIG:=zx279125_vsol_v2801sg
  UBOOT_IMAGE:=u-boot.bin
  UBOOT_CUSTOMIZE_CONFIG := $(ZX279125_SMALL_CONFIG) \
    --enable ZX279125_SERIAL_INIT \
    --set-val BOOTDELAY -1 --set-str BOOTCOMMAND "" \
    --set-str IDENT_STRING " RAM-UART-init-test"
endef

UBOOT_TARGETS += vsol_v2801sg-uart-test

define U-Boot/vsol_v2801sg-dump
  NAME:=VSOL V2801SG RAM snapshot export
  BUILD_DEVICES:=vsol_v2801sg
  UBOOT_CONFIG:=zx279125_vsol_v2801sg
  UBOOT_IMAGE:=u-boot.bin
  UBOOT_CUSTOMIZE_CONFIG := $(ZX279125_SMALL_CONFIG) \
    --disable ZX279125_SERIAL_INIT --enable CMD_TFTPPUT \
    --set-val BOOTDELAY -1 --set-str BOOTCOMMAND "" \
    --set-str IDENT_STRING " RAM-snapshot-export"
endef

UBOOT_TARGETS += vsol_v2801sg-dump

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./files/. $(PKG_BUILD_DIR)/
endef

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
