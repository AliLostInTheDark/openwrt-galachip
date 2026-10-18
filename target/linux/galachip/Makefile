# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

ARCH:=arm
BOARD:=galachip
BOARDNAME:=GalaChip
FEATURES:=squashfs dt gpio small_flash low_mem
SUBTARGETS:=gc1601
DEVICE_TYPE:=basic

KERNEL_PATCHVER:=6.18

define Target/Description
	Build firmware images for GalaChip xPON SoC based boards.
endef

KERNELNAME:=zImage

include $(INCLUDE_DIR)/target.mk

DEFAULT_PACKAGES += \
	kmod-gpio-button-hotplug

$(eval $(call BuildTarget))
