# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

ARCH:=arm
BOARD:=zte
BOARDNAME:=ZTE Sanechips
FEATURES:=squashfs dt gpio small_flash low_mem
SUBTARGETS:=zx279125
DEVICE_TYPE:=basic

KERNEL_PATCHVER:=6.18

define Target/Description
	Build firmware images for ZTE Sanechips ARM SoC based boards.
endef

KERNELNAME:=zImage

include $(INCLUDE_DIR)/target.mk

include $(PLATFORM_DIR)/ramroot.mk

DEFAULT_PACKAGES += \
	kmod-gpio-button-hotplug

$(eval $(call BuildTarget))
