# SPDX-License-Identifier: GPL-2.0-only
# Platform upgrade routines for ZTE Sanechips ZX279125 platforms

PART_NAME=firmware
REQUIRE_IMAGE_METADATA=1

platform_check_image() {
	local board=$(board_name)

	case "$board" in
	vsol,v2801sg)
		return 0
		;;
	*)
		echo "Unsupported board: $board"
		return 1
		;;
	esac
}

platform_do_upgrade() {
	default_do_upgrade "$1"
}
