# SPDX-License-Identifier: GPL-2.0-only
# One behaviour on every ONU (operator, 2026-10-18): the new image replaces the running one in the
# same k0 and r0, the configuration kept. The build already refuses an image too big for them.

REQUIRE_IMAGE_METADATA=1

onu_member() {
	tar tf "$1" 2>/dev/null | grep -m 1 "^sysupgrade-[^/]*/$2\$"
}

onu_mtd() {
	local d

	for d in /sys/class/mtd/mtd[0-9]*; do
		[ "$(cat "$d/name" 2>/dev/null)" = "$1" ] && { echo "$d"; return 0; }
	done
	return 1
}

platform_check_image() {
	local img="$1" kernel root part dev size

	grep -qs ' /rom squashfs ' /proc/mounts || {
		echo "This ONU runs from RAM, not from its flash: the flash is not ours to replace."
		return 1
	}
	kernel=$(onu_member "$img" kernel)
	root=$(onu_member "$img" root)
	[ -n "$kernel" ] && [ -n "$root" ] || {
		echo "Not an ONU firmware image: no sysupgrade-<board>/kernel and root."
		return 1
	}
	[ "$(tar xf "$img" "$kernel" -O | dd bs=4 count=1 2>/dev/null | hexdump -v -e '1/1 "%02x"')" = 27051956 ] &&
		tar xf "$img" "$kernel" -O | dd bs=1 skip=32 count=32 2>/dev/null | grep -qs OpenWrt || {
		echo "The kernel is not an OpenWrt uImage."
		return 1
	}
	for part in k0:kernel r0:root; do
		dev=$(onu_mtd "${part%%:*}") || {
			echo "This ONU's flash has no ${part%%:*} partition."
			return 1
		}
		[ $(( $(cat "$dev/flags") & 0x400 )) -ne 0 ] || {
			echo "${part%%:*} is read-only on this ONU."
			return 1
		}
		size=$(tar xf "$img" "$(onu_member "$img" "${part#*:}")" -O | wc -c)
		[ "$size" -le "$(cat "$dev/size")" ] || {
			echo "The image's ${part#*:} ($size bytes) is larger than ${part%%:*}."
			return 1
		}
	done
	return 0
}

platform_do_upgrade() {
	local kernel root

	kernel=$(onu_member "$1" kernel)
	root=$(onu_member "$1" root)
	sync
	tar xf "$1" "$kernel" -O | mtd write - k0 || return 1
	tar xf "$1" "$root" -O | mtd ${UPGRADE_BACKUP:+-j "$UPGRADE_BACKUP"} write - r0
}
