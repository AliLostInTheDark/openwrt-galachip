# SPDX-License-Identifier: GPL-2.0-only
#
# Per-board image definitions for the `taroko` subtarget (RLX/Lexra "Taroko"
# core; renamed from rtl960x 2026-08-29 -- the subtarget selects a CORE).
# Boards are brought up run-from-RAM first: the initramfs uImage is TFTP'd
# into RAM and bootm'd by the vendor U-Boot (no flash write during bring-up).

# The V2802RH is a 4 MiB L2 bridge: LuCI keeps only the views and menus that
# apply to one (as the XZ000-G7 does). The rootfs is shared by every device in
# a build, so this runs only when the V2802RH is the one selected.
define Image/Prepare
	$(if $(CONFIG_TARGET_realtek_luna_taroko_DEVICE_vsol_v2802rh), \
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
	fi)
endef

define Device/hsgq_x111w
  DEVICE_VENDOR := HSGQ
  DEVICE_MODEL := X111W
  DEVICE_DTS := rtl9602c_x111w
  DEVICE_DTS_DIR := $(DTS_DIR)/realtek-luna
  SOC := rtl9602c
  # NOR persistent image (raw MTD; the vendor fwu.sh flashes a uImage -> k0 and a
  # squashfs -> r0). Partition sizes from the live device /proc/mtd:
  #   k0 = 0x2f0000 (3008k) kernel, its bank's last 64k the crashlog; r0 = 0x4c0000 (4864k) rootfs.
  # SIZE GUARD: KERNEL_SIZE auto-fires check-size on the kernel (image.mk), and the
  # explicit check-size on kernel.bin/rootfs.bin makes the BUILD fail if either
  # overflows its partition -- an oversize image is never written to the device.
  KERNEL_SIZE := $(call flash_kernel_size,3008k)
  IMAGE_SIZE := 4864k
  IMAGES := kernel.bin rootfs.bin sysupgrade.bin
  IMAGE/kernel.bin := append-kernel | check-size $$(KERNEL_SIZE)
  IMAGE/rootfs.bin := append-rootfs | pad-rootfs | check-size
  IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
  # luci-app-gpon builds (validated) but its luci-base dep needs cgi-io from the
  # 'packages' feed; add "luci-app-gpon luci" here once that feed is installed.
  # Router stack so the ONU actually routes/NATs LAN<->WAN and serves DHCP:
  #   dnsmasq           - LAN DHCPv4 + DNS server
  #   firewall4(+nft)   - lan/wan zones, lan->wan forward + masquerade (NAT)
  #   odhcpd-ipv6only   - LAN IPv6 RA/DHCPv6 server (distributes the WAN PD)
  #   odhcp6c           - WAN DHCPv6 client (PD request toward the OLT)
  #   ppp ppp-mod-pppoe - PPPoE WAN client (alternative WAN proto to IPoE/DHCP)
  #   wpad-basic-mbedtls - WiFi AP/STA daemon (hostapd+wpa_supplicant) for the
  #     rtl8192fe AP: WPA2-PSK(psk2) + WPA3-SAE personal, no enterprise/EAP, no
  #     mesh; mbedtls TLS (libmbedtls is already pulled by libustream-mbedtls, so
  #     this adds only the ~1MB wpad binary, ~350k squashfs-compressed). This is
  #     the smallest wpad variant that still does psk2 AP + PMF; wpad-mini lacks
  #     SAE/802.11w. Listed here so 'make defconfig' keeps it tied to the device
  #     profile (a bare .config regen would otherwise drop a manual selection).
  # (urngd is excluded target-wide in target/linux/realtek-luna/Makefile: it
  #  burned ~60s of boot CPU on this core for nothing - crng seeds itself.)
  DEVICE_PACKAGES := gpon-provision luci-app-gpon luci-base luci-mod-admin-full luci-theme-bootstrap uhttpd uhttpd-mod-ubus rpcd rpcd-mod-file \
	dnsmasq firewall4 odhcpd-ipv6only odhcp6c ppp ppp-mod-pppoe wpad-basic-mbedtls
endef
TARGET_DEVICES += hsgq_x111w

define Device/vsol_v2801rgw
  DEVICE_VENDOR := VSOL
  DEVICE_MODEL := V2801RGW
  DEVICE_DTS := rtl9602c_v2801rgw
  DEVICE_DTS_DIR := $(DTS_DIR)/realtek-luna
  SOC := rtl9602c
  # One bank on an 8 MiB Winbond EF4017, from the unit's own /proc/mtd and U-Boot env:
  #   k0 = 0x290000 (2624k) kernel at 0xa0000, the 64k crashlog at 0x330000,
  #   r0 = 0x4c0000 (4864k) rootfs at 0x340000 (operator 2026-10-18: a crashlog on every model).
  # check-size fails the BUILD when either overflows: an oversize image never reaches a unit.
  KERNEL_SIZE := $(call flash_kernel_size,2624k)
  IMAGE_SIZE := 4864k
  IMAGES := kernel.bin rootfs.bin sysupgrade.bin
  IMAGE/kernel.bin := append-kernel | check-size $$(KERNEL_SIZE)
  IMAGE/rootfs.bin := append-rootfs | pad-rootfs | check-size
  IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
  # The same router stack as the X111W (see its profile for the why of each package).
  DEVICE_PACKAGES := gpon-provision luci-app-gpon luci-base luci-mod-admin-full luci-theme-bootstrap uhttpd uhttpd-mod-ubus rpcd rpcd-mod-file \
	dnsmasq firewall4 odhcpd-ipv6only odhcp6c ppp ppp-mod-pppoe wpad-basic-mbedtls
endef
TARGET_DEVICES += vsol_v2801rgw

define Device/hsgq_x100dg
  DEVICE_VENDOR := HSGQ
  DEVICE_MODEL := X100DG
  DEVICE_DTS := rtl9601d_x100dg
  DEVICE_DTS_DIR := $(DTS_DIR)/realtek-luna
  SOC := rtl9601d
  # 32 MB of RAM cannot unpack a 10 MB initramfs: the dev loop boots the squashfs pair from RAM
  # through phram. The same pair is its flash image: one bank over both of the vendor's 8 MiB,
  #   k0 = 0x220000 (2176k) at 0x80000, the 64k crashlog at 0x2a0000, r0 = 0x550000 (5440k) at 0x2b0000.
  KERNEL_SIZE := $(call flash_kernel_size,2176k)
  IMAGE_SIZE := 5440k
  IMAGES := kernel.bin rootfs.bin sysupgrade.bin
  IMAGE/kernel.bin := append-kernel | check-size $$(KERNEL_SIZE)
  IMAGE/rootfs.bin := append-rootfs | check-size
  IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
  # gpon-provision: its SN, MAC and RTL8290B laser cal come from its own config partition.
  DEVICE_PACKAGES := -iw gpon-provision
endef
TARGET_DEVICES += hsgq_x100dg

define Device/vsol_v2802rh
  DEVICE_VENDOR := VSOL
  DEVICE_MODEL := V2802RH
  DEVICE_ALT0_VENDOR := VSOL
  DEVICE_ALT0_MODEL := V2801RD
  DEVICE_DTS := rtl9601d_v2802rh
  DEVICE_DTS_DIR := $(DTS_DIR)/realtek-luna
  SOC := rtl9601d
  # 4 MiB Zbit ZB25VQ32 with the stock bootloader, env and hardware MIB below
  # 0x27000 and the RTOS's OSK1FS files above 0x3e0000. U-Boot boots the uImage
  # at 0x27000 in place; the firmware partition (0x3b9000, 3812k) holds it, the
  # rootfs right behind it and the overlay in what is left.
  IMAGE_SIZE := 3812k
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
  # LuCI and the overlay.
  DEVICE_PACKAGES := -iw -dnsmasq -firewall4 -nftables -kmod-nft-offload \
	-odhcpd-ipv6only -odhcp6c -ppp -ppp-mod-pppoe -wpad-basic-mbedtls \
	-procd-ujail -libustream-mbedtls -uclient-fetch -ca-bundle \
	gpon-provision
endef
TARGET_DEVICES += vsol_v2802rh
