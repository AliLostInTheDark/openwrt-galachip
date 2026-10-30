#!/bin/sh
# Apply board presentation changes to fresh LuCI sources, then retain minification.
set -eu
root=$1
feed=$2
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
resources=$root/www/luci-static/resources
[ -d "$resources" ] || exit 0
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/view/network" "$work/view/status/include"
cp "$feed/modules/luci-mod-network/htdocs/luci-static/resources/view/network/interfaces.js" "$work/view/network/"
cp "$feed/modules/luci-mod-status/htdocs/luci-static/resources/view/status/include/29_ports.js" "$work/view/status/include/"
patch --batch --fuzz=0 -d "$work" -p1 < "$here/link-display.patch"
mkdir -p "$resources/v2802rh"
cp "$here/link.js" "$work/link.js"
for file in view/network/interfaces.js view/status/include/29_ports.js link.js; do
	if [ "$file" = link.js ]; then
		dest=$resources/v2802rh/link.js
	else
		dest=$resources/$file
		[ -f "$dest" ] || continue
	fi
	if command -v jsmin >/dev/null 2>&1; then
		jsmin < "$work/$file" > "$work/min.js"
		cp "$work/min.js" "$dest"
	else
		cp "$work/$file" "$dest"
	fi
done
