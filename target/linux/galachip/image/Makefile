# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

# 512-byte tag read by the stock loader: kernel length at 0x78, zImage at 0x200.
define Build/gc1601-tag
	perl -e '$$l = -s $$ARGV[0]; $$t = "\0" x 512; substr($$t, $$_->[0], 4) = pack("V", $$_->[1]) for [0x00, 0x03000003], [0x68, 0xc0000000], [0x6c, 0xc0008000], [0x70, $$l], [0x78, $$l]; print $$t' $@ > $@.new
	cat $@ >> $@.new
	mv $@.new $@
endef

# The loader leaves the packet engine DMAing into 0x40800000, so the
# kernel image (linked at 0xc0008000) must end below 0xc0800000.
define Build/check-dma-gap
	@end="$$($(TARGET_CROSS)nm $(KDIR)/$(1) | awk '$$3 == "_end" { print $$1 }')"; \
	if [ -z "$$end" ] || [ $$((0x$$end)) -ge $$((0xc0800000)) ]; then \
		echo "$(1): _end 0x$$end reaches the loader DMA area at 0x40800000" >&2; \
		rm -f $@; exit 1; \
	fi
endef

define Device/Default
  PROFILES := Default
  DEVICE_DTS_DIR := ../dts
  KERNEL_NAME := zImage
  KERNEL := kernel-bin | append-dtb
  KERNEL_INITRAMFS := kernel-bin | append-dtb | check-dma-gap vmlinux-initramfs.debug
  KERNEL_INITRAMFS_SUFFIX := -zImage
  IMAGES :=
endef

include $(SUBTARGET).mk

$(eval $(call BuildImage))
