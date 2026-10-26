# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

define Device/Default
  PROFILES := Default
  DEVICE_DTS_DIR := ../dts
  KERNEL_NAME := zImage
  KERNEL := kernel-bin | append-dtb | uImage none
  KERNEL_INITRAMFS := kernel-bin | append-dtb | uImage none
  KERNEL_INITRAMFS_SUFFIX := -uImage
  KERNEL_LOADADDR := 0x40008000
  KERNEL_ENTRY := 0x40008000
  IMAGES :=
endef

include $(SUBTARGET).mk

$(eval $(call BuildImage))
