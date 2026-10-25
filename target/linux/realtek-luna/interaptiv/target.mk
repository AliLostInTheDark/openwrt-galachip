# SPDX-License-Identifier: GPL-2.0-only

ARCH:=mips
SUBTARGET:=interaptiv
BOARD:=realtek-luna
BOARDNAME:=Realtek Luna - MIPS interAptiv core (RTL9607C, RTL9603CVD)
# interAptiv is a standard MIPS32 Release 2 core: build userspace as r2 (24kc:
# -mips32r2) so the C library uses the standard rdhwr/UserLocal thread pointer
# and lwl/lwr unaligned access, not the RLX/Lexra-only encodings the r1 (mips32)
# `taroko` subtarget needs. This also gives this subtarget its own toolchain so
# the two cores never share an ISA-incompatible libc.
CPU_TYPE:=24kc

# This subtarget is keyed on the CORE -- which is now what it is NAMED after,
# so the routing is a reading of the board's own banner and not an argument.
# Any Luna part with a
# MIPS interAptiv (MIPS32 R2) core belongs here and shares this toolchain.
# Members today: RTL9607C (engineering board) and RTL9603CVD (LANLY G24W,
# core MEASURED from its own /proc/cpuinfo 2026-08-19). Per-chip differences
# live in that chip's .dtsi and in the driver's DT match, never in a second
# subtarget -- a fork of this file would only duplicate one toolchain build.
define Target/Description
	Build firmware images for Realtek Luna GPON ONU SoCs built on a
	MIPS interAptiv core (MIPS32 R2, big-endian) in a Coherent
	Processing System (GIC + CM + CPC). DRAM and flash are per-board; run-from-RAM
	bring-up via TFTP/initramfs. Kernel debugging (ftrace/kprobes) follows the
	build config's CONFIG_KERNEL_* options, off in the shipped config.
endef
