# SPDX-License-Identifier: GPL-2.0-or-later
# A unit's MAC identity: six lowercase octets, unicast, and none of the
# addresses every unit of this family carries (Realtek's LAN and WLAN defaults).

ONU_MAC_PLACEHOLDERS="00:00:00:00:00:00 00:e0:4c:86:70:01 00:e0:4c:07:68:02"

unit_mac_valid() {
	case "$1" in
	??:??:??:??:??:??) ;;
	*) return 1 ;;
	esac
	case "$1" in
	?[13579bdf]:*) return 1 ;;
	esac
	case " $ONU_MAC_PLACEHOLDERS " in
	*" $1 "*) return 1 ;;
	esac
	return 0
}

# A `<module>.mac=` boot parameter (a flasher's per-unit pin), lowercased; the
# module is matched, not spelled: rtl960x_eth, rtl9602c_eth and luna_eth all exist.
cmdline_mac() {
	sed -n 's/.*[ \t][A-Za-z0-9_]\{1,\}\.mac=\([0-9A-Fa-f:]\{17\}\).*/\1/p' \
		"${ONU_CMDLINE:-/proc/cmdline}" 2>/dev/null | tr 'A-F' 'a-f'
}

# This unit's MAC in the operator's order (2026-10-01): its factory config
# partition, else a boot parameter a flasher pinned, else none -- nothing is
# invented, and never eth0's current address (without a pin, the driver's
# random). Sets UNIT_MAC and UNIT_MAC_SRC; returns 1 when no source has one.
unit_mac() {
	UNIT_MAC=$(${ONU_RTK_FACTORY:-/usr/sbin/rtk_factory} -p config mac 2>/dev/null | tr 'A-F' 'a-f')
	UNIT_MAC_SRC="this unit's factory config partition"
	unit_mac_valid "$UNIT_MAC" && return 0
	UNIT_MAC=$(cmdline_mac)
	UNIT_MAC_SRC="the mac= boot parameter"
	unit_mac_valid "$UNIT_MAC" && return 0
	UNIT_MAC= UNIT_MAC_SRC=
	return 1
}

# The software version the OLT reads (OMCI SW-image), operator 2026-09-30:
# OWRT-<short commit> of the build in /etc/build-id, "+" for a dirty tree.
owrt_version() {
	local id rest
	read -r id rest < "${ONU_BUILD_ID:-/etc/build-id}" || return 1
	case "$id" in
	*-dirty) id=${id%-dirty}; rest=+ ;;
	*) rest= ;;
	esac
	case "$id" in
	[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) ;;
	*) return 1 ;;
	esac
	echo "OWRT-$(echo "$id" | cut -c1-7)$rest"
}

# The OMCI identity fields the GPON driver takes, read from the driver itself (the
# core's OMCI_ID_LIST): a field added there needs nothing in any script.
omci_id_fields() {
	sed -n 's/^omci_id_fields: //p' "${GPON_PROC:-/proc/gpon}" 2>/dev/null
}

# One field to whichever driver runs: Luna's onu_<name> parameter, else the
# "id <name> <value>" line of Cortina's /proc/gpon.
omci_id_write() {
	local param="${ONU_OMCI_ID_DIR:-/sys/module/luna_gpon/parameters}/onu_$1"
	if [ -w "$param" ]; then
		printf '%s' "$2" > "$param"
	else
		echo "id $1${2:+ $2}" > "${GPON_PROC:-/proc/gpon}"
	fi
}

# Every field the driver lists, LuCI/UCI first, else `$1 <name>` (the unit's own
# MIB value), the software version ours. Prints what it could not apply; an
# empty value keeps the driver's build default.
omci_id_apply() {
	local mib="$1" names name val
	names=$(omci_id_fields)
	[ -n "$names" ] || { echo "the driver lists no OMCI identity field: the build default stands"; return 1; }
	for name in $names; do
		val=$(uci -q get "gpon.identity.$name")
		case "$name" in
		sw_ver1|sw_ver2) [ -n "$val" ] || val=$(owrt_version 2>/dev/null) ;;
		farend) ;;	# empty = the driver detects it; written even empty, so clearing works
		*) [ -n "$val" ] || val=$("$mib" "$name") ;;
		esac
		[ -n "$val" ] || [ "$name" = farend ] || continue
		omci_id_write "$name" "$val" 2>/dev/null || echo "the driver refused OMCI $name '$val'"
	done
	return 0
}

# The serial this unit ranges with: the operator's under source=manual (a clone),
# else the factory one ($1).
gpon_serial() {
	local sn
	if [ "$(uci -q get gpon.identity.source)" = manual ] &&
		sn=$(uci -q get gpon.identity.sn) && [ -n "$sn" ]; then
		printf '%s' "$sn"
	else
		printf '%s' "$1"
	fi
}

# The PLOAM password stock sends: LuCI/UCI first, else the MIB's $2 when the MIB holds
# the key ($1 = 1; an empty one is sent as ten zero octets, as stock does). Returns 1
# when neither holds one: the driver keeps its default.
gpon_ploam_password() {
	local pw
	pw=$(uci -q get gpon.identity.ploam_passwd)
	[ -n "$pw" ] || { [ "$1" = 1 ] || return 1; pw=$2; }
	printf '%s' "$pw"
}
