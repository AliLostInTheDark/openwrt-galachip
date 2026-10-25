# SPDX-License-Identifier: GPL-2.0-only

ARCH:=mips
SUBTARGET:=taroko
BOARD:=realtek-luna
BOARDNAME:=Realtek Luna - RLX Taroko core (RTL9602C, RTL9603C)
CPU_TYPE:=mips32

# Lexra MDU erratum mitigation (RLX/Taroko cores only): a memory load issued in
# the div/mult -> mflo/mfhi shadow silently corrupts HI/LO. -fno-schedule-insns2
# stops gcc's sched2 pass from filling that shadow with loads (measured: musl and
# the -O2 datapath drop to 0 hazard windows). CFLAGS here becomes the dumped
# Target-Optimization, i.e. the CONFIG_TARGET_OPTIMIZATION default - pinned in
# the subtarget so a `make defconfig` cannot silently drop the flag (it did once).
# Complete fix = a `-mfix-lexra` gcc div-fusion (TODO). Do NOT add this to the
# interaptiv subtarget (interAptiv core, unaffected).
# -mfix-rlx (toolchain/gcc 990-mips-fix-rlx.patch): an RLX branch whose delay slot
# is a three-operand mul writing the branch's register decides on the mul result.
CFLAGS:=-Os -pipe -mno-branch-likely -mips32 -mtune=mips32 -fno-schedule-insns2 -mfix-rlx

# THE SUBTARGET IS THE CORE, AND THE NAME NOW SAYS SO.  A part belongs here when
# its own boot banner and /proc/cpuinfo say RLX / Taroko -- nothing about its
# part number decides it.  MEASURED 2026-08-19 on the LANLY G24W: the RTL9603CVD
# reports "MIPS interAptiv V2.0", isa mips32r2, 64 TLB entries, so it is an
# `interaptiv` part despite the RTL9603 name, and landing it here would give it
# the Lexra MDU erratum toolchain and an R3000 ASID layout it does not want.
# That used to need a paragraph of warning because this subtarget was called
# "rtl960x"; it is now just a reading.
define Target/Description
	Build firmware images for Realtek RTL960xC GPON ONU boards based on
	the RLX "Taroko" core (RTL9602C, RTL9603C, ...). Big-endian MIPS,
	16 MB SPI-NOR, run-from-RAM bring-up via TFTP/initramfs.
	NOTE: the RTL9603CVD is an interAptiv part -- see the interaptiv subtarget.
endef
