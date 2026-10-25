#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# What a GPON ONU tells about itself and the OLT it ranges on, collected read-only into one tar.
# Usage: sh gpon_bundle.sh [output-dir]   (default /tmp). OMCI-simulate/CONTRIBUTE-A-BUNDLE.md explains it.
#        sh gpon_bundle.sh --stdout > bundle.tar   the same tar on stdout (our image: gpon-bundle, the PON page)
#        sh gpon_bundle.sh --binaries > binaries.tar   the stock OMCI binaries, streamed: nothing staged in RAM
set -u
PREFIX=${PREFIX:-}
PROTOCOL="bin/omci* bin/europacli bin/pondetect bin/checkomci bin/checkpon bin/diag lib/libomci* lib/libmib.so* lib/librtk.so* lib/libgpon* lib/omci/* lib/features/* lib/modules/*.ko etc/omci* etc/runomci.sh"
protocol=$(cd "$PREFIX/" && ls -d $PROTOCOL 2> /dev/null)
if [ "${1:-}" = "--binaries" ]; then
	[ -n "$protocol" ] || { echo "no stock OMCI binary on this board" >&2; exit 1; }
	cd "$PREFIX/" && tar cf - $protocol
	exit $?
fi
STDOUT=
[ "${1:-}" = "--stdout" ] && { STDOUT=1; shift; }
OUT=${1:-/tmp}
B="$OUT/gpon-bundle.$$"
mkdir "$B" || exit 1
note() { echo "$*" >> "$B/commands.txt"; }
step() { name=$1; shift; "$@" > "$B/$name" 2>&1 < /dev/null; note "$name rc=$? $*"; }
grab() {
	if [ -r "$PREFIX$1" ]; then cat "$PREFIX$1" > "$B/$2" 2>&1; note "$2 rc=$? cat $1"; else note "$2 absent $1"; fi
}
echo "gpon-bundle 1" > "$B/manifest.txt"
step script.sh cat "$0"

step identity-uname.txt uname -a
for f in version cmdline cpuinfo meminfo; do
	grab "/proc/$f" "identity-proc-$f.txt"
done
grab /proc/mtd identity-proc-mtd.txt
grab /proc/onu_crashlog crash-records.txt
grab /proc/net/dev identity-proc-net-dev.txt
grab /etc/openwrt_release identity-etc-openwrt_release.txt
grab /etc/version identity-etc-version.txt
for i in $(ls "$PREFIX/sys/class/net" 2>/dev/null); do
	grab "/sys/class/net/$i/address" "identity-mac-$i.txt"
done
grab /sys/firmware/fdt dt.dtb

if command -v dmesg > /dev/null 2>&1; then
	step boot-log.txt dmesg
else
	cat "$PREFIX/dev/kmsg" > "$B/boot-log.txt" 2>&1 &
	reader=$!
	sleep 2
	kill "$reader" 2> /dev/null
	note "boot-log.txt read /dev/kmsg for 2 s"
fi

if [ -n "$protocol" ]; then (cd "$PREFIX/" && md5sum $protocol) > "$B/protocol.md5" 2>&1; fi

for f in gpon gpon_log gpon_timing gpon_health oltcap ploamcap gpon_prov gpon_unknown ploam_unknown swdump pidump bosadump; do
	grab "/proc/$f" "gpon-$f.txt"
done
if command -v gpon-omci-view > /dev/null 2>&1; then
	step omci-provisioning.txt gpon-omci-view
	step omci-not-identified.txt gpon-omci-view --unknown
fi
grab /tmp/gpon/wan-discover.txt wan-discover.txt
grab /tmp/gpon/bundle.json bundle.json
grab /proc/net/vlan/config network-vlan.txt
grab /proc/net/route network-route.txt
grab /proc/net/fib_trie network-addresses.txt

if command -v diag > /dev/null 2>&1; then
	step diag-gpon-onu-state.txt diag gpon get onu-state
	step diag-rt_gpon-onu-state.txt diag rt_gpon get onu-state
fi

if command -v omcicli > /dev/null 2>&1; then
	step omci-logfile-before.txt omcicli get logfile
	case "$(cat "$B/omci-logfile-before.txt")" in
		*"Raw Mode"*"Time Stamp"*) ;;
		*)
			step omci-logfile-enable.txt omcicli set logfile 7 0
			echo "runtime_log_enable omcicli set logfile 7 0" >> "$B/manifest.txt"
			;;
	esac
	step omci-logfile.txt omcicli get logfile
	log=
	found=
	while read -r line; do
		if [ -n "$found" ]; then log=$line; break; fi
		if [ "$line" = "Log File:" ]; then found=1; fi
	done < "$B/omci-logfile.txt"
	case "$log" in
		/tmp/*) grab "$log" omcilog.txt ;;
		*) note "omcilog.txt absent: log file '$log' is not under /tmp" ;;
	esac
	for what in devmode dmmode ponspeed; do
		step "omci-$what.txt" omcicli get "$what"
	done
fi

(cd "$B" && md5sum *) > "$B/manifest.md5" 2> /dev/null || note "manifest.md5 incomplete: no md5sum"
if [ -n "$STDOUT" ]; then
	tar cf - -C "$B" .
	rc=$?
	rm -r "$B"
	exit $rc
fi
NAME="gpon-bundle-$(date +%Y%m%d-%H%M%S 2> /dev/null)-$$.tar"
tar cf "$OUT/$NAME" -C "$B" . || { echo "no tar archive: copy the directory $B instead"; exit 1; }
rm -r "$B"
echo "$OUT/$NAME"
