PART_NAME=firmware
REQUIRE_IMAGE_METADATA=1

platform_check_image() {
	[ "$(get_magic_long "$1")" = "03000003" ] || {
		echo "Invalid image type."
		return 1
	}
}

platform_do_upgrade() {
	default_do_upgrade "$1"
}
