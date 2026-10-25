#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# build_wiring_x86_check.sh -- offline proof, on the x86 host, that the SHARED
# GPON build wiring is not merely present but VALID.  No board, no OpenWrt
# build, no rig: it runs in seconds and gates a boot, it never proves hardware.
#
# WHY IT IS COMMON -- and which targets it covers
#   Operator, 2026-08-05: "la idea es poner en comun el codigo que corresponde
#   para no tener mucho duplicado".  The files it checks live in ONE shared
#   files- tree and are compiled by realtek-luna (MIPS32, big-endian) and
#   realtek-elnath (aarch64, little-endian).  This script asks the questions a
#   green build would not answer.
#
# THE CORE/SHELL RULE IT OBEYS
#   Build-wiring check only: it opens no device and no register, and none of
#   the files it guards may ever gain an MMIO access (proven separately by
#   gpon_core_purity_test in dev/rtl9607c-test/).
#
# WHAT IT PROVES, and each arm can fail:
#   1. Kconfig GRAMMAR, using the kernel's OWN x86-64 parser -- plus BOTH
#      halves of the GPON_CORE contract: a consumer's `select` turns it on,
#      AND nothing else does (a stray `default` would make the select vacuous).
#   2. GNU make GRAMMAR of every Makefile, with a positive control: a
#      deliberately broken fragment must be REJECTED, or the passes mean
#      nothing.  ("No targets" is the expected outcome for a kbuild fragment,
#      which defines variables and no rules -- not an error.)
#   3. that every .c in the shared directory compiles on x86 through the test
#      suite's shims.  This is the compile coverage that lets the objects stay
#      `# gpon-pending:` in the Makefile instead of being built into a shipping
#      kernel just to find out whether they compile.
#
# Companion: build_wiring_guard.py (the structural invariants, mutation-proven).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)          # target/linux/gpon-common
W=$(cd "$HERE/../../.." && pwd)              # openwrt tree root
G=$HERE/files-6.18/drivers/net
TMP=$(mktemp -d -t gpon-wiring-x86-XXXXXX)
trap 'rm -rf "$TMP"' EXIT
rc=0

# The kernel's own kconfig parser, built for the host by any prepared target.
CONF=""
for c in "$W"/build_dir/target-*/linux-*/linux-*/scripts/kconfig/conf; do
	[ -x "$c" ] && CONF=$c && break
done
SHIMS=${GPON_FUZZ_SHIMS:-/home/user/Desktop/CatchChallenger/dev/rtl9607c-test/fuzz_shims}

echo "=============== 1. Kconfig grammar + the GPON_CORE contract ==============="
if [ -z "$CONF" ]; then
	echo "BLOCKED: no host scripts/kconfig/conf found under build_dir."
	echo "         That is 'could not ask', NOT 'it passed'.  Prepare either"
	echo "         target once (make target/linux/prepare) and re-run."
	rc=1
else
	T=$TMP/kctree; mkdir -p "$T/drivers/net/gpon"
	cp "$G/Kconfig" "$T/drivers/net/Kconfig"
	cp "$G/gpon/Kconfig" "$T/drivers/net/gpon/Kconfig"
	# stub every OTHER sourced Kconfig, so ours is the only thing under test
	grep -oP '(?<=^source ")[^"]+' "$T/drivers/net/Kconfig" | while read -r p; do
		[ "$p" = "drivers/net/gpon/Kconfig" ] && continue
		mkdir -p "$T/$(dirname "$p")"; : > "$T/$p"
	done
	mkkc() {   # $1 = "select" | "noselect"
		cat > "$T/Kconfig" <<EOF
mainmenu "gpon build-wiring check"
config NET
	bool
	default y
config MY_GPON_DRIVER
	tristate "stand-in for CORTINA_GPON / LUNA_GPON"
	default y
$([ "$1" = select ] && printf '\tselect GPON_CORE')
source "drivers/net/Kconfig"
EOF
	}
	runkc() {  # $1 = config file name
		( cd "$T" && srctree=. ARCH=x86 SRCARCH=x86 KERNELVERSION=0 \
			CC=gcc LD=ld KCONFIG_CONFIG="$1" "$CONF" --olddefconfig Kconfig ) \
			>"$T/log.$1" 2>&1
	}
	mkkc select
	if runkc .cfg_sel; then
		echo "PASS: the kconfig parser accepts drivers/net/Kconfig + gpon/Kconfig"
		if grep -q '^CONFIG_GPON_CORE=y$' "$T/.cfg_sel"; then
			echo "PASS: a consumer's select turns GPON_CORE on"
		else
			echo "FAIL: select did not yield CONFIG_GPON_CORE=y"; rc=1
		fi
	else
		echo "FAIL: the kconfig parser rejected the tree"; sed -n '1,20p' "$T/log..cfg_sel"; rc=1
	fi
	# the negative half: without a consumer it must stay OFF, or `select` is
	# not really the only way in and W9 in the guard is vacuous.
	mkkc noselect
	runkc .cfg_no
	if grep -q '^CONFIG_GPON_CORE=y$' "$T/.cfg_no"; then
		echo "FAIL: GPON_CORE is on with nothing selecting it (stray default?)"; rc=1
	else
		echo "PASS: with no consumer GPON_CORE stays off -- select is the only way in"
	fi
fi

echo
echo "=============== 2. GNU make grammar ==============="
# A kbuild fragment defines variables and no rules, so "No targets" IS success.
mkparse() {
	local out
	out=$(make -qp -f "$1" 2>&1 >/dev/null | grep -v '^make: \*\*\* No targets\.  Stop\.$')
	[ -z "$out" ] && return 0
	echo "$out" | head -3 | sed 's/^/        /'; return 1
}
for f in "$G/Makefile" "$G/gpon/Makefile" \
	"$W/target/linux/realtek-luna/files-6.18/drivers/net/ethernet/realtek/Makefile" \
	"$W/target/linux/realtek-elnath/files-6.18/drivers/net/ethernet/cortina/Makefile"; do
	[ -f "$f" ] || { echo "FAIL missing: $f"; rc=1; continue; }
	if mkparse "$f"; then echo "PASS parse: ${f#$W/}"; else echo "FAIL parse: ${f#$W/}"; rc=1; fi
done
printf 'obj-y += a.o\nifeq ($(X),1)\n' > "$TMP/broken.mk"
if mkparse "$TMP/broken.mk" >/dev/null 2>&1; then
	echo "FAIL control: a truncated ifeq was ACCEPTED -- the parse check is vacuous"; rc=1
else
	echo "PASS control: a deliberately broken Makefile is rejected"
fi
echo "--- what the shared gpon/Makefile declares (empty obj-y == all pending) ---"
make -f "$G/gpon/Makefile" -p 2>/dev/null | grep -E '^(obj-y|ccflags-y) :?=' || true

echo
echo "=============== 3. x86 compile of every shared source ==============="
if [ ! -d "$SHIMS" ]; then
	echo "BLOCKED: shim dir absent: $SHIMS (set GPON_FUZZ_SHIMS)"; rc=1
else
	for c in "$G"/gpon/*.c; do
		[ -e "$c" ] || continue
		if gcc -fsyntax-only -std=gnu11 -I"$SHIMS" -I"$G/gpon" -D__KERNEL__ "$c" \
			2>"$TMP/cc.log"; then
			echo "PASS compile: $(basename "$c")"
		else
			echo "FAIL compile: $(basename "$c")"; head -3 "$TMP/cc.log" | sed 's/^/        /'
			rc=1
		fi
	done
fi

echo
echo "rc=$rc"
exit $rc
