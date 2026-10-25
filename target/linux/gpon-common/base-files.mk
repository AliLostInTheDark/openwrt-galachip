# SPDX-License-Identifier: GPL-2.0-or-later
GPON_BASE_FILES := $(TOPDIR)/target/linux/gpon-common/base-files
PKG_FILE_DEPENDS += $(GPON_BASE_FILES)/

define Package/base-files/install-target
	$(CP) $(GPON_BASE_FILES)/. $(1)/
endef
