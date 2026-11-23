#!/bin/sh
# Apply board presentation changes to fresh LuCI sources, then retain minification.
set -eu
root=$1
feed=$2
jsmin=$3
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
resources=$root/www/luci-static/resources
[ -d "$resources" ] || exit 0
[ -x "$jsmin" ] || { echo "install.sh: no jsmin at $jsmin" >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/view/network" "$work/view/status/include"
cp "$feed/modules/luci-mod-network/htdocs/luci-static/resources/view/network/interfaces.js" "$work/view/network/"
cp "$feed/modules/luci-mod-status/htdocs/luci-static/resources/view/status/include/29_ports.js" "$work/view/status/include/"
patch --batch --fuzz=0 -d "$work" -p1 < "$here/link-display.patch"
# Keep the menu tree in uhttpd's parent instead of parsing it on every request.
dispatcher=usr/share/ucode/luci/dispatcher.uc
if [ -f "$root/$dispatcher" ]; then
	mkdir -p "$work/${dispatcher%/*}"
	cp "$feed/modules/luci-base/ucode/dispatcher.uc" "$work/$dispatcher"
	patch --batch --fuzz=0 -d "$work" -p1 < "$here/dispatcher.patch"
	cp "$work/$dispatcher" "$root/$dispatcher"
fi
# Probe luci.getFeatures once per rpcd start instead of on every page load.
rpcd_luci=usr/share/rpcd/ucode/luci
if [ -f "$root/$rpcd_luci" ]; then
	mkdir -p "$work/${rpcd_luci%/*}"
	cp "$feed/modules/luci-base/root/$rpcd_luci" "$work/$rpcd_luci"
	patch --batch --fuzz=0 -d "$work" -p1 < "$here/rpcd-luci.patch"
	cp "$work/$rpcd_luci" "$root/$rpcd_luci"
fi
mkdir -p "$resources/gc1601"
cp "$here/pon_up.svg" "$here/pon_down.svg" "$resources/gc1601/"
cp "$here/link.js" "$work/link.js"
for file in view/network/interfaces.js view/status/include/29_ports.js link.js; do
	if [ "$file" = link.js ]; then
		dest=$resources/gc1601/link.js
	else
		dest=$resources/$file
		[ -f "$dest" ] || continue
	fi
	"$jsmin" < "$work/$file" > "$work/min.js"
	cp "$work/min.js" "$dest"
done
