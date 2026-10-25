# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

ARCH:=mips
BOARD:=realtek-luna
BOARDNAME:=Realtek Luna GPON ONU
DEVICE_TYPE:=basic
FEATURES:=ramdisk squashfs gpio
SUBTARGETS:=taroko interaptiv

KERNEL_PATCHVER:=6.18

define Target/Description
	Build firmware images for Realtek "Luna" GPON ONU SoCs.  The SUBTARGET
	IS THE CPU CORE, and it is named after it: `taroko` for the RLX/Lexra
	"Taroko" core (RTL9602C, RTL9603C) and `interaptiv` for the licensed
	MIPS interAptiv core (RTL9607C, RTL9603CVD).  They were called rtl960x
	and rtl9607x until 2026-08-29, and both names were wrong in the same
	way: they claimed a PART-NUMBER RANGE while selecting a CORE, so a
	part had to be routed against its name rather than by it -- the
	RTL9603CVD is an interAptiv part and had to live in "rtl9607x", which
	each target.mk needed a warning paragraph to explain.  A board now
	goes to the subtarget its own boot banner names.
endef

include $(INCLUDE_DIR)/target.mk

# --- the SHARED GPON protocol core ------------------------------------------
# A THIRD files- tree, appended to FILES_DIR, so drivers/net/gpon/ is authored
# ONCE and compiled by BOTH GPON targets (realtek-luna MIPS32 big-endian and
# realtek-elnath aarch64 little-endian).  Operator, 2026-08-05: "la idea es
# poner en comun el codigo que corresponde para no tener mucho duplicado".
#
# This is not a new mechanism -- the build ALREADY concatenates two trees:
# include/quilt.mk:93 is
#   kernel_files=$(foreach fdir,$(GENERIC_FILES_DIR) $(FILES_DIR),$(fdir)/.)
# and both are recursively expanded, so one more element is all it takes.
#
# MUST be AFTER the target.mk include and use `+=`, never `:=` before it:
# include/kernel.mk:44 sets FILES_DIR with `?=`, so assigning earlier would
# stop that from firing and DROP this target's own files-6.18 entirely.
# Guarded offline by target/linux/gpon-common/build_wiring_guard.py.
#
# Symlinking the shared files into each target instead is REFUSED on two
# measurements: rules.mk:314 CP:=cp -fpR copies the LINK (whose relative path
# cannot resolve inside build_dir), and scripts/timestamp.pl:21 does
# `next if -l $file`, so editing the shared core would never re-trigger the
# copy -- the kernel would silently build a stale core.
ifeq ($(TARGET_BUILD),1)
FILES_DIR += "$(TOPDIR)/target/linux/gpon-common/files-$(KERNEL_PATCHVER)"
# ★★ AND THE COPY COMMAND MUST BE RE-EXPANDED, OR THE `+=` ABOVE IS INERT.
# MEASURED 2026-08-05, and it failed in the reassuring direction: with the `+=`
# alone, `make val.FILES_DIR` and `val.kernel_files` BOTH printed the shared
# tree, the offline wiring guard passed 35/35 -- and drivers/net/gpon/ was never
# copied into the kernel. The build then failed far away, on a missing header.
#
# WHY. include/quilt.mk:93-97 builds the copy out of $(kernel_files), but
# include/kernel-defaults.mk:15 freezes it with a SIMPLY-EXPANDED assignment:
#     Kernel/Patch := $(Kernel/Patch/Default)
# and that line runs during `include $(INCLUDE_DIR)/target.mk` above
# (target.mk:381 kernel.mk -> :383 kernel-build.mk -> :17 kernel-defaults.mk).
# So the literal `cp -fpR ... ` string is fixed BEFORE this file's body is read,
# while $(FILES_DIR) itself stays recursively expanded -- which is exactly why
# every late query showed the right value and the build used the wrong one.
# Proven by reading make's own variable database (`make -p`): the frozen
# Kernel/Patch listed generic/files and this target's files-6.18, and NOT
# gpon-common/files-6.18.
#
# There is no ordering that fixes this on its own: kernel.mk:44's `FILES_DIR ?=`
# and kernel-defaults.mk:15's freeze both happen inside that one include, so
# assigning before it drops this target's own files- tree and appending after it
# is too late for the copy. Re-running upstream's OWN assignment is the minimum
# that works, and it stays correct if upstream changes what the copy does --
# only the two names are depended on. (Kernel/Patch/Default is defined in
# include/quilt.mk:94, included at kernel-build.mk:16, one line before the
# freeze, so it is in scope here.)
Kernel/Patch := $(Kernel/Patch/Default)
endif

# No urngd: the jitter-entropy daemon burned ~60s of CPU at every boot on the
# 624-BogoMIPS RLX core (measured on the X111W, 2026-07-08) while the kernel
# crng already seeds itself from interrupt entropy by ~6s. (A "-urngd" negation
# in DEVICE_PACKAGES is NOT honored by the rootfs assembly - filter it here.)
DEFAULT_PACKAGES := $(filter-out urngd,$(DEFAULT_PACKAGES))

DEFAULT_PACKAGES += \
	kmod-gpio-button-hotplug \
	uboot-envtools \
	iw

$(eval $(call BuildTarget))
