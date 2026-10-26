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

# Development and recovery tools below are not selected by default.
define U-Boot/vsol_v2801sg-chainload
  NAME:=VSOL V2801SG RAM chainload test
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
  UBOOT_CONFIG:=zx279125_vsol_v2801sg
  UBOOT_IMAGE:=u-boot.bin
  UBOOT_CUSTOMIZE_CONFIG := $(ZX279125_SMALL_CONFIG) \
    --disable ZX279125_SERIAL_INIT --enable CMD_TFTPPUT \
    --set-val BOOTDELAY -1 --set-str BOOTCOMMAND "" \
    --set-str IDENT_STRING " RAM-snapshot-export"
endef

UBOOT_TARGETS += vsol_v2801sg-dump

# Packed into the firmware's kernel slot by mkzxicheader --loader, so stock
# U-Boot boots it after checking the image; it boots the kernel stored after
# it. Stock has already set up the UART and DDR.
define U-Boot/vsol_v2801sg-stockchain
  NAME:=VSOL V2801SG monitor chainloaded by stock U-Boot
  BUILD_DEVICES:=vsol_v2801sg
  UBOOT_CONFIG:=zx279125_vsol_v2801sg
  UBOOT_IMAGE:=u-boot.bin
  UBOOT_CUSTOMIZE_CONFIG := $(ZX279125_SMALL_CONFIG) \
    --disable ZX279125_SERIAL_INIT \
    --set-val BOOTDELAY 2 \
    --set-str BOOTCOMMAND "sf probe; sf read 0x40600000 0x70000 0x200000; bootm 0x40600000" \
    --set-str BOOTARGS "console=ttyS0,115200n8 root=/dev/mtdblock7 ro rootfstype=squashfs mem=32M" \
    --set-str IDENT_STRING " stock-chain"
endef

UBOOT_TARGETS += vsol_v2801sg-stockchain

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./files/. $(PKG_BUILD_DIR)/
endef

# Variants whose monitor can follow the cold first stage (UART initialization).
ZX279125_COLD_VARIANTS := vsol_v2801sg vsol_v2801sg-uart-test

define Build/Compile
	$(call Build/Compile/U-Boot)
	$(STAGING_DIR_HOST)/bin/mkimage -A arm -O u-boot -T firmware -C none \
		-a 0x40400000 -e 0x40400000 \
		-n 'U-Boot $(PKG_VERSION) for VSOL V2801SG' \
		-d $(PKG_BUILD_DIR)/u-boot.bin $(PKG_BUILD_DIR)/u-boot.img
	$(if $(filter $(ZX279125_COLD_VARIANTS),$(BUILD_VARIANT)), \
		python3 $(CURDIR)/tools/build-first-stage.py \
			--payload $(PKG_BUILD_DIR)/u-boot.bin \
			--payload-map $(PKG_BUILD_DIR)/System.map \
			--uboot-tree $(PKG_BUILD_DIR) --cross $(TARGET_CROSS) \
			--out $(PKG_BUILD_DIR)/first-stage)
	# Stock bootm checks the CRC and unpacks it, so a damaged copy is refused.
	$(if $(filter vsol_v2801sg-stockchain,$(BUILD_VARIANT)), \
		gzip -9nc $(PKG_BUILD_DIR)/u-boot.bin > $(PKG_BUILD_DIR)/u-boot.bin.gz && \
		$(STAGING_DIR_HOST)/bin/mkimage -A arm -O linux -T kernel -C gzip \
			-a 0x40400000 -e 0x40400000 \
			-n 'U-Boot $(PKG_VERSION) stock-chain' \
			-d $(PKG_BUILD_DIR)/u-boot.bin.gz $(PKG_BUILD_DIR)/u-boot-stockchain.uimg)
endef

# The combined cold image has not booted from physical reset; never flash it
# without a tested external recovery path.
define Package/u-boot/install
	$(CP) \
		$(PKG_BUILD_DIR)/u-boot.bin \
		$(PKG_BUILD_DIR)/u-boot.img \
		$(1)/
	$(if $(filter $(ZX279125_COLD_VARIANTS),$(BUILD_VARIANT)), \
		$(CP) $(PKG_BUILD_DIR)/first-stage/combined-OFFLINE-NOT-FOR-FLASH.bin \
			$(1)/u-boot-cold-UNTESTED.bin; \
		$(CP) $(PKG_BUILD_DIR)/first-stage/manifest.json \
			$(1)/u-boot-cold-manifest.json)
	$(if $(filter vsol_v2801sg-stockchain,$(BUILD_VARIANT)), \
		$(CP) $(PKG_BUILD_DIR)/u-boot-stockchain.uimg $(1)/)
endef

define Build/InstallDev
	$(if $(filter vsol_v2801sg-stockchain,$(BUILD_VARIANT)), \
		$(INSTALL_DIR) $(STAGING_DIR_IMAGE) && \
		$(CP) $(PKG_BUILD_DIR)/u-boot-stockchain.uimg \
			$(STAGING_DIR_IMAGE)/vsol_v2801sg-u-boot-stockchain.uimg)
endef

$(eval $(call BuildPackage/U-Boot))
