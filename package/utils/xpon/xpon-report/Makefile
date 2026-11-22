# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=xpon-report
PKG_RELEASE:=9
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/xpon-report
  SECTION:=net
  CATEGORY:=Network
  TITLE:=GPON/EPON diagnostics: report, datapath collector and OMCI monitor
  DEPENDS:=@TARGET_galachip_gc1601 +uci +ubus +jsonfilter
  PROVIDES:=xpon-diagnostics omci-monitor
endef

define Package/xpon-report/description
  xpon-report writes the PON state (GPON or EPON), PLOAM, OMCI and OAM,
  T-CONTs, GEM ports, services, switch flows, optics, packet engine,
  kernel log and settings into one file, with serial numbers, passwords
  and LOIDs masked. xpon-diagnostics collects a bounded datapath snapshot,
  and omci-monitor decodes OMCI traffic read-only, live while omcid is
  stopped or offline from the trace omcid writes on SIGUSR1.
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -Wall -Wextra -Werror \
		-I./monitor/src/include -o $(PKG_BUILD_DIR)/omci-monitor \
		./monitor/src/monitor.c ./monitor/src/netlink.c ./monitor/src/wire.c \
		$(TARGET_LDFLAGS)
endef

define Package/xpon-report/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/xpon-report $(1)/usr/sbin/xpon-report
	$(INSTALL_BIN) ./files/xpon-diagnostics $(1)/usr/sbin/xpon-diagnostics
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omci-monitor $(1)/usr/sbin/omci-monitor
endef

$(eval $(call BuildPackage,xpon-report))
