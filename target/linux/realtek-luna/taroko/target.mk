# SPDX-License-Identifier: GPL-2.0-only

ARCH:=mips
SUBTARGET:=taroko
BOARD:=realtek-luna
BOARDNAME:=VSOL V2802RH (RTL9601D)
CPU_TYPE:=mips32

# Lexra MDU erratum mitigation (RLX/Taroko cores only): a memory load issued in
# the div/mult -> mflo/mfhi shadow silently corrupts HI/LO. -fno-schedule-insns2
# stops gcc's sched2 pass from filling that shadow with loads (measured: musl and
# the -O2 datapath drop to 0 hazard windows). CFLAGS here becomes the dumped
# Target-Optimization, i.e. the CONFIG_TARGET_OPTIMIZATION default - pinned in
# the subtarget so a `make defconfig` cannot silently drop the flag (it did once).
# Complete fix = a `-mfix-lexra` gcc div-fusion (TODO).
# -mfix-rlx (toolchain/gcc 990-mips-fix-rlx.patch): an RLX branch whose delay slot
# is a three-operand mul writing the branch's register decides on the mul result.
CFLAGS:=-Os -pipe -mno-branch-likely -mips32 -mtune=mips32 -fno-schedule-insns2 -mfix-rlx

define Target/Description
	VSOL V2802RH: RTL9601D, RLX Taroko core, big-endian MIPS, 4 MiB SPI-NOR.
endef
