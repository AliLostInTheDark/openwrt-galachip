#!/bin/sh
# GC1601 packet-engine / GPON register dump, identical on stock and OpenWrt.
#   sh pe-dump.sh regs  > dump.txt     one full snapshot (idle, then under load)
#   sh pe-dump.sh watch > watch.txt    queue + counter samples about every 1-2 s (Ctrl-C)
#
# Read-only, and only registers without read side effects. Skipped on
# purpose: RX +0x0c/+0x10/+0x14 (allocator response/release/request),
# buffers +0x40 (IRQ status), descriptor ACK +0x60/+0x64 and the TX ring
# completion words: reading those on stock steals buffers or IRQs.

d() {
	echo "## $1 .. $2"
	a=$(($1))
	while [ $a -le $(($2)) ]; do
		v=$(devmem $(printf 0x%08x $a) 32)
		[ "$v" != "0x00000000" ] && printf "%08x %s\n" $a $v
		a=$((a + 4))
	done
}

regs() {
	echo "# $(cat /proc/version)"
	echo "# uptime $(cat /proc/uptime)"
	# Buffer manager, minus +0x40
	d 0x90dc0000 0x90dc003c
	d 0x90dc0044 0x90dc0074
	d 0x90dc4000 0x90dc4000		# buffer control
	d 0x90dc8000 0x90dc8000		# buffer enable
	d 0x90dcc054 0x90dcc054
	# Descriptor bank: controls and limit, not the table window or ACKs
	d 0x90dd0000 0x90dd0010
	d 0x90dd00b8 0x90dd00b8
	# RX gate / pool: +0x00..+0x08, +0x18..+0x38
	d 0x90dd4000 0x90dd4008
	d 0x90dd4018 0x90dd4038
	# TX config, scheduler, OS (shaper/scheduler) banks
	d 0x90ddc000 0x90ddc014
	d 0x90de0000 0x90de0040
	d 0x90de8000 0x90de8010
	d 0x90dec000 0x90dec050
	# GMAC config and pause/flow control, then its counters
	d 0x90e00000 0x90e00010
	d 0x90e000e0 0x90e000e4
	d 0x90e00300 0x90e00304
	d 0x90e00700 0x90e0081c
	# Switch source port controls
	d 0x90f90000 0x90f90010
	d 0x90f94198 0x90f94198
	# GPON MAC, SCU and global (ranges from the October stock capture)
	d 0x90c80000 0x90c8017c
	d 0x90c80400 0x90c80840
	d 0x90c81000 0x90c810b8
	d 0x90c40000 0x90c40024
	d 0x90c00000 0x90c00010
	d 0x90a00800 0x90a0081c
	queues
	echo "## done"
}

queues() {
	for f in /sys/bus/platform/devices/90c00000.xpon_sdk/debug/dbg_bufcfg \
		 /sys/kernel/debug/*/dbg_bufcfg \
		 /sys/class/net/eth0/device/queue_snapshot; do
		[ -r "$f" ] && { echo "## $f"; cat "$f"; }
	done
	[ -r /sys/bus/platform/devices/90cc0000.pon/tcont_stats ] &&
		{ echo "## tcont_stats"; cat /sys/bus/platform/devices/90cc0000.pon/tcont_stats; }
}

watch() {
	while :; do
		echo "#### $(cut -d' ' -f1 /proc/uptime)"
		queues
		d 0x90e00714 0x90e0081c
		sleep 1
	done
}

case "$1" in
regs) regs ;;
watch) watch ;;
*) echo "usage: $0 regs|watch" >&2; exit 1 ;;
esac
