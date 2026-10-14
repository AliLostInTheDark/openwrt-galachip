# SPDX-License-Identifier: GPL-2.0-only

define Image/Prepare
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
	# A source tree checked out with umask 002 gives group-writable
	# directories, and dropbear refuses public keys below one of them
	find $(TARGET_DIR) ! -type l -perm /022 ! -perm -1000 -exec chmod go-w {} +
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
  DEVICE_PACKAGES := kmod-gc1601-gpon-diag kmod-gc1601-epon-diag xpon-report \
    omci-agent luci-app-omci \
    epon-oamd luci-app-epon \
    -uclient-fetch -urandom-seed -urngd -ca-bundle \
    -libustream-mbedtls -rpcd-mod-iwinfo -libiwinfo-data -libiwinfo \
    -luci-lib-uqr -odhcpd -odhcpd-ipv6only -odhcp6c -dnsmasq \
    -firewall4 -nftables -ppp -ppp-mod-pppoe
endef
TARGET_DEVICES += tplink_xz000-g7
