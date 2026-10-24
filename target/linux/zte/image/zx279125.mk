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

# The loader is the u-boot-vsol_v2801sg-stockchain package's uImage.
define Build/zxic-header
	$(TOPDIR)/scripts/mkzxicheader.py \
		--loader $(STAGING_DIR_IMAGE)/vsol_v2801sg-u-boot-stockchain.uimg $@ $@.new
	@mv $@.new $@
endef

# Also save the unappended zImage and the DTB, so U-Boot can fix up a separate
# DTB for the packet-engine cursor handoff (bootz <zImage> - <dtb>).
define Build/export-zimage-dtb
	mkdir -p $(BIN_DIR)
	cp $@ $(BIN_DIR)/$(KERNEL_INITRAMFS_PREFIX)-zImage.bin
	cp $(KDIR)/image-$(firstword $(DEVICE_DTS)).dtb $(BIN_DIR)/$(DEVICE_IMG_PREFIX).dtb
endef

define Device/vsol_v2801sg
  DEVICE_VENDOR := VSOL
  DEVICE_MODEL := V2801SG
  DEVICE_DTS := zx279125-vsol-v2801sg
  # Earlier images used the unregistered v-sol vendor prefix.
  SUPPORTED_DEVICES += v-sol,v2801sg
  SOC := zx279125
  BLOCKSIZE := 64k
  IMAGE_SIZE := 7424k
  KERNEL := kernel-bin | append-dtb | uImage none
  IMAGES := sysupgrade.bin
  IMAGE/sysupgrade.bin := append-kernel | pad-to $$(BLOCKSIZE) | \
    append-rootfs | pad-rootfs | zxic-header | check-size | append-metadata
  KERNEL_INITRAMFS := kernel-bin | export-zimage-dtb | append-dtb | uImage none
  DEVICE_PACKAGES := kmod-zx279125-ethernet xpon-report bosa-tools \
    omci-agent omcid luci-app-omci luci-app-xpon \
    luci-mod-network luci-mod-status luci-mod-system luci-theme-slate \
    uhttpd uhttpd-mod-ubus uhttpd-mod-ucode rpcd \
    -luci -luci-light -luci-mod-admin-full -luci-app-package-manager \
    -luci-app-firewall -luci-proto-ppp -luci-proto-ipv6 -rpcd-mod-rrdns \
    -epon-oamd -luci-app-epon -luci-app-xpon-stats \
    -apk-mbedtls -apk-openssl -opkg \
    -uclient-fetch -urandom-seed -urngd -ca-bundle \
    -libustream-mbedtls -rpcd-mod-iwinfo -libiwinfo-data -libiwinfo \
    -luci-lib-uqr -odhcpd -odhcpd-ipv6only -odhcp6c -dnsmasq \
    -firewall -firewall4 -uci-firewall -iptables -nftables -ppp -ppp-mod-pppoe \
    -kmod-zram -zram-swap
endef
TARGET_DEVICES += vsol_v2801sg
