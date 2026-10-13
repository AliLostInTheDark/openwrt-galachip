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
