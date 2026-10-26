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
