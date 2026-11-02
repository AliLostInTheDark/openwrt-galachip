# SPDX-License-Identifier: GPL-2.0-only

define Image/Prepare
	# The PON as a fiber optic port, with its icon and optical line rate
	$(SHELL) $(TOPDIR)/target/linux/galachip/gc1601/luci/install.sh \
		$(TARGET_DIR) $(TOPDIR)/feeds/luci $(STAGING_DIR_HOSTPKG)/bin/jsmin
	# LuCI views, icons and menus that do not apply to a 4 MiB bridge
	rm -f $(TARGET_DIR)/www/luci-static/resources/view/network/wireless.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/network/dhcp.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/network/dns.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/network/switch* \
	      $(TARGET_DIR)/www/luci-static/resources/view/network/routes.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/network/diagnostics.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/status/wireless.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/status/channel_analysis.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/status/nftables.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/status/iptables.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/status/connections.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/status/syslog.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/status/routes* \
	      $(TARGET_DIR)/www/luci-static/resources/view/status/load.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/status/bandwidth.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/status/include/30_network.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/status/include/40_dhcp.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/status/include/50_dsl.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/status/include/60_wifi.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/system/plugins.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/system/repokeys.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/system/mounts.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/system/crontab.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/system/pwpolicy.js \
	      $(TARGET_DIR)/www/luci-static/resources/view/system/leds.js
	rm -rf $(TARGET_DIR)/www/luci-static/resources/view/system/led-trigger
	rm -f $(TARGET_DIR)/www/luci-static/resources/icons/wireguard* \
	      $(TARGET_DIR)/www/luci-static/resources/icons/wifi* \
	      $(TARGET_DIR)/www/luci-static/resources/icons/signal* \
	      $(TARGET_DIR)/www/luci-static/resources/icons/tunnel* \
	      $(TARGET_DIR)/www/luci-static/resources/icons/vrf* \
	      $(TARGET_DIR)/www/luci-static/resources/icons/port_pse*
	# Menus without the entries whose views are removed above
	if [ -d $(TARGET_DIR)/usr/share/luci/menu.d ]; then \
		$(CP) $(TOPDIR)/target/linux/galachip/gc1601/menu.d/* $(TARGET_DIR)/usr/share/luci/menu.d/; \
	fi
	# No package manager in the image; sysupgrade only reads lib/apk/packages.
	rm -rf $(TARGET_DIR)/lib/apk/db
	# Nothing left calls these: ucode's debugger, the removed graphs' collector,
	# RPS tuning on a single core, GPIO switches this board does not have.
	rm -f $(TARGET_DIR)/usr/bin/udbg $(TARGET_DIR)/usr/bin/luci-bwc \
		$(TARGET_DIR)/etc/init.d/packet_steering $(TARGET_DIR)/etc/rc.d/*packet_steering \
		$(TARGET_DIR)/etc/init.d/gpio_switch $(TARGET_DIR)/etc/rc.d/*gpio_switch
	# omci-management needs these of kmod-sched-core; the rest would only load
	# at boot and hold RAM.
	if [ -f $(TARGET_DIR)/etc/modules.d/70-sched-core ]; then \
		cd $(TARGET_DIR)/lib/modules/$(LINUX_VERSION) && rm -f act_skbedit.ko cls_basic.ko \
			cls_flow.ko cls_fw.ko cls_route.ko cls_u32.ko em_u32.ko sch_hfsc.ko sch_htb.ko sch_tbf.ko; \
		printf '%s\n' act_gact act_mirred cls_matchall sch_ingress > $(TARGET_DIR)/etc/modules.d/70-sched-core; \
	fi
	# A source tree checked out with umask 002 gives group-writable
	# directories, and dropbear refuses public keys below one of them
	find $(TARGET_DIR) ! -type l -perm /022 ! -perm -1000 -exec chmod go-w {} +
endef

# U-Boot proper for NOR 0x2000-0x20000: install with run upgrade_uboot.
define Build/gc1601-u-boot-proper
	cat $(STAGING_DIR_IMAGE)/$(DEVICE_NAME)-u-boot-proper.img >> $@
	[ "$$(stat -c %s $@)" -eq 122880 ]
endef

# RAM U-Boot: tftpboot 41000000 and go 41000000 from the stock U-Boot.
define Build/gc1601-u-boot-ram
	cat $(STAGING_DIR_IMAGE)/$(DEVICE_NAME)-u-boot-ram.bin >> $@
endef

# ROM-headed SPL for NOR 0-0x2000: install with an SPI programmer only.
define Build/gc1601-u-boot-spl
	cat $(STAGING_DIR_IMAGE)/$(DEVICE_NAME)-u-boot-spl.bin >> $@
	[ "$$(stat -c %s $@)" -le 8192 ]
endef

define Device/tplink_xz000-g7
  DEVICE_VENDOR := TP-Link
  DEVICE_MODEL := XZ000-G7
  DEVICE_VARIANT := v1
  DEVICE_DTS := gc1601-tplink-xz000-g7
  SOC := gc1601
  BLOCKSIZE := 64k
  # 3776k firmware partition; keep at least four erase blocks for the overlay.
  IMAGE_SIZE := 3520k
  KERNEL := kernel-bin | append-dtb | check-dma-gap vmlinux.debug | gc1601-tag
  IMAGES := sysupgrade.bin
  IMAGE/sysupgrade.bin := append-kernel | pad-to $$(BLOCKSIZE) | \
    append-rootfs | pad-rootfs | check-size | append-metadata
  ARTIFACTS := u-boot-proper.img u-boot-spl.bin u-boot-ram.bin
  ARTIFACT/u-boot-proper.img := gc1601-u-boot-proper
  ARTIFACT/u-boot-spl.bin := gc1601-u-boot-spl
  ARTIFACT/u-boot-ram.bin := gc1601-u-boot-ram
  DEVICE_PACKAGES := kmod-gc1601-gpon-diag kmod-gc1601-epon-diag xpon-report \
    omci-agent omcid luci-app-omci luci-app-onu-system luci-theme-bootstrap uhttpd-mod-ucode \
    epon-oamd luci-app-epon \
    -uclient-fetch -urandom-seed -urngd -ca-bundle \
    -libustream-mbedtls -rpcd-mod-iwinfo -libiwinfo-data -libiwinfo \
    -luci-lib-uqr -odhcpd -odhcpd-ipv6only -odhcp6c -dnsmasq \
    -firewall4 -nftables -ppp -ppp-mod-pppoe -procd-ujail
endef
TARGET_DEVICES += tplink_xz000-g7
