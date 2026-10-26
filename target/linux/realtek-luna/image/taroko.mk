# SPDX-License-Identifier: GPL-2.0-only
#
# The VSOL V2802RH, the one board of this target.

# The V2802RH is a 4 MiB L2 bridge: LuCI keeps only the views and menus that
# apply to one.
define Image/Prepare
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
		$(TARGET_DIR)/www/luci-static/resources/view/system/leds.js \
		$(TARGET_DIR)/www/luci-static/resources/icons/wireguard* \
		$(TARGET_DIR)/www/luci-static/resources/icons/wifi* \
		$(TARGET_DIR)/www/luci-static/resources/icons/signal* \
		$(TARGET_DIR)/www/luci-static/resources/icons/tunnel* \
		$(TARGET_DIR)/www/luci-static/resources/icons/vrf* \
		$(TARGET_DIR)/www/luci-static/resources/icons/port_pse*; \
	rm -rf $(TARGET_DIR)/www/luci-static/resources/view/system/led-trigger; \
	if [ -d $(TARGET_DIR)/usr/share/luci/menu.d ]; then \
		$(CP) $(TOPDIR)/target/linux/realtek-luna/taroko/v2802rh/menu.d/* \
			$(TARGET_DIR)/usr/share/luci/menu.d/; \
	fi
endef

define Device/vsol_v2802rh
  DEVICE_VENDOR := VSOL
  DEVICE_MODEL := V2802RH
  DEVICE_ALT0_VENDOR := VSOL
  DEVICE_ALT0_MODEL := V2801RD
  DEVICE_DTS := rtl9601d_v2802rh
  DEVICE_DTS_DIR := $(DTS_DIR)/realtek-luna
  SOC := rtl9601d
  SUPPORTED_DEVICES := vsol,v2802rh vsol,v2802rh-chainload-v1
  # 4 MiB Zbit ZB25VQ32 with the stock bootloader, env and hardware MIB below
  # 0x27000, the chainloaded U-Boot at 0x3c0000 and the RTOS's OSK1FS files
  # above 0x3e0000. U-Boot boots the uImage at 0x27000 in place; the firmware
  # partition (0x399000, 3684k) holds it, the rootfs right behind it and the
  # overlay in what is left.
  IMAGE_SIZE := 3684k
  # The flash erases in 4k. The jffs2 marker lets the first boot from flash
  # format the free space as the overlay instead of falling back to tmpfs.
  BLOCKSIZE := 4k
  IMAGES := kernel.bin rootfs.bin firmware.bin sysupgrade.bin
  # kernel.bin and rootfs.bin are for a RAM boot (bootm + phram).
  IMAGE/kernel.bin := append-kernel
  IMAGE/rootfs.bin := append-rootfs | check-size 2048k
  IMAGE/firmware.bin := append-kernel | pad-to $$(BLOCKSIZE) | \
	append-rootfs | pad-rootfs | check-size
  IMAGE/sysupgrade.bin := append-kernel | pad-to $$(BLOCKSIZE) | \
	append-rootfs | pad-rootfs | check-size | append-metadata
  # An L2 bridge: no routing, NAT, DHCP server, firewall or WiFi. Nor TLS,
  # package fetching or process jails, which would leave no room in r0 for
  # LuCI and the overlay. OMCI runs in omcid through the RTL9601D backend.
  DEVICE_PACKAGES := -iw -dnsmasq -firewall4 -nftables -kmod-nft-offload \
	-odhcpd-ipv6only -odhcp6c -ppp -ppp-mod-pppoe -wpad-basic-mbedtls \
	-procd-ujail -libustream-mbedtls -uclient-fetch -ca-bundle \
	gpon-provision omcid omci-agent omci-monitor luci-app-omci
endef
TARGET_DEVICES += vsol_v2802rh
