# Keep the embedded RAM root compressed while the full DMA guard is retained.
ifneq ($(CONFIG_TARGET_ROOTFS_INITRAMFS_SEPARATE),y)
ifeq ($(call qstrip,$(CONFIG_EXTERNAL_CPIO)),)
override INITRAMFS_INIT_FILE := $(PLATFORM_DIR)/other-files/init

override define Kernel/Configure/Initramfs
	rm -rf $(KERNEL_BUILD_DIR)/ramroot && \
	mkdir -p $(addprefix $(KERNEL_BUILD_DIR)/ramroot/,bin lib dev rom overlay new_root) && \
	$(INSTALL_BIN) $(1)/bin/busybox $(KERNEL_BUILD_DIR)/ramroot/bin/busybox && \
	ln -s busybox $(KERNEL_BUILD_DIR)/ramroot/bin/sh && \
	$(CP) $(1)/lib/libc.so $(1)/lib/libgcc_s.so.1 \
		$(1)/lib/ld-musl-*.so.1 $(KERNEL_BUILD_DIR)/ramroot/lib/ && \
	$(STAGING_DIR_HOST)/bin/mksquashfs4 $(1) \
		$(KERNEL_BUILD_DIR)/ramroot/root.squashfs \
		-noappend -all-root -comp xz -b 65536 -processors 1 && \
	$(call Kernel/SetInitramfs,$(KERNEL_BUILD_DIR)/ramroot,$(2))
endef
endif
endif
