# SPDX-License-Identifier: GPL-2.0-only

define Image/Prepare
	# LuCI views, icons and menus that do not apply to an SFU bridge
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
	if [ -d $(TARGET_DIR)/usr/share/luci/menu.d ]; then \
		$(CP) $(TOPDIR)/target/linux/zte/zx279125/menu.d/* $(TARGET_DIR)/usr/share/luci/menu.d/; \
	fi
	find $(TARGET_DIR) ! -type l -perm /022 ! -perm -1000 -exec chmod go-w {} +
endef

define Device/vsol_v2801sg
  DEVICE_VENDOR := V-SOL
  DEVICE_MODEL := V2801SG
  DEVICE_DTS := zx279125-vsol-v2801sg
  SOC := zx279125
  BLOCKSIZE := 64k
  IMAGE_SIZE := 3648k
  KERNEL := kernel-bin | append-dtb | uImage none
  IMAGES := sysupgrade.bin
  IMAGE/sysupgrade.bin := append-kernel | pad-to $$(BLOCKSIZE) | \
    append-rootfs | pad-rootfs | check-size | append-metadata
  DEVICE_PACKAGES := xpon-report \
    omci-agent omcid luci-app-omci luci-theme-slate uhttpd-mod-ucode \
    epon-oamd luci-app-epon \
    -uclient-fetch -urandom-seed -urngd -ca-bundle \
    -libustream-mbedtls -rpcd-mod-iwinfo -libiwinfo-data -libiwinfo \
    -luci-lib-uqr -odhcpd -odhcpd-ipv6only -odhcp6c -dnsmasq \
    -firewall4 -nftables -ppp -ppp-mod-pppoe
endef
TARGET_DEVICES += vsol_v2801sg
