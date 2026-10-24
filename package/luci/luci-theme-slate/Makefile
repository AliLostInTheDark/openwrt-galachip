# SPDX-License-Identifier: Apache-2.0

include $(TOPDIR)/rules.mk

PKG_VERSION:=1.0.0
PKG_RELEASE:=14
LUCI_TITLE:=Slate Theme (sidebar layout, light and dark)
LUCI_PKGARCH:=all
LUCI_DEPENDS:=+luci-base
PKG_LICENSE:=Apache-2.0
# csstidy's highest template merges rules with equal declarations and so
# reorders the cascade; the image is xz-compressed anyway.
LUCI_MINIFY_CSS:=0

define Package/luci-theme-slate/postrm
#!/bin/sh
case "$$1" in *upgrade*) exit 0 ;; esac
[ "$${PKG_UPGRADE}" = 1 ] && exit 0
[ -n "$${IPKG_INSTROOT}" ] || {
	uci -q delete luci.themes.Slate
	uci -q delete luci.themes.SlateDark
	uci -q delete luci.themes.SlateLight
	case "$$(uci -q get luci.main.mediaurlbase)" in
		/luci-static/slate|/luci-static/slate-dark|/luci-static/slate-light)
			# Let LuCI select another registered theme after removal.
			uci -q delete luci.main.mediaurlbase
			;;
	esac
	uci commit luci
}
endef

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
