# SPDX-License-Identifier: GPL-2.0-or-later
# Copy, validate and publish this unit's calibration before driver activation.
gpon_cal_stage() {
	local src="$1" checksum="$2" dst="$3" expected="$4"
	local want="" sum="" len="" tmp="" rc=1

	if [ -n "$checksum" ] && [ -e "$checksum" ]; then
		want=$(cat "$checksum") || return 1
		want=${want%%[[:space:]]*}
	fi
	mkdir -p "${dst%/*}" || return 1
	tmp=$(mktemp "$dst.XXXXXX") || return 1
	if cp "$src" "$tmp"; then
		len=$(wc -c < "$tmp") || len=""
		if [ "$len" = "$expected" ]; then
			if [ -n "$want" ]; then
				sum=$(md5sum "$tmp") || sum=""
				sum=${sum%%[[:space:]]*}
			else
				sum=""
			fi
			if [ "$sum" = "$want" ] && [ ! -d "$dst" ]; then
				mv "$tmp" "$dst" && return 0
			fi
		fi
	fi
	echo "gpon-cal: refusing calibration from $src: copy, size, checksum or publication failed" >&2
	rm -f "$tmp" || echo "gpon-cal: could not remove $tmp" >&2
	return "$rc"
}
