# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

# Realtek "Luna" U-Boot expects a legacy uImage: MIPS/Linux/kernel, LZMA
# compressed, load+entry at 0x80000000 (per vendor fwu.sh / on-board uImage).
# Bring-up is run-from-RAM: a uImage carrying an embedded initramfs is
# TFTP'd into RAM and bootm'd; no flash write during bring-up.
KERNEL_LOADADDR:=0x80000000

define Device/Default
  PROFILES := Default
  KERNEL_NAME := vmlinux
  # Legacy vendor U-Boot passes no DT, so append the board DTB to the kernel
  # (CONFIG_MIPS_RAW_APPENDED_DTB); head.S finds it and sets fw_passed_dtb.
  KERNEL_INITRAMFS := kernel-bin | append-dtb | lzma | uImage lzma
  KERNEL := kernel-bin | append-dtb | lzma | uImage lzma
  IMAGE_NAME = $$(IMAGE_PREFIX)-$$(2)
  IMAGES :=
endef

# A RAM-only memory-diagnosis kernel (build-openwrt --variant memory-debug / slab-debug) is
# never flashed, so it is held to no partition: the TFTP boot is its only route to a unit.
flash_kernel_size = $(if $(CONFIG_KERNEL_MEM_ALLOC_PROFILING)$(CONFIG_KERNEL_SLUB_DEBUG),,$(1))

include $(SUBTARGET).mk

$(eval $(call BuildImage))
