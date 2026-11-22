# SPDX-License-Identifier: GPL-2.0-or-later

obj-y += lowlevel_init.o

ifdef CONFIG_TPL_BUILD
obj-y += tpl.o sdram.o spl_spi.o boot-device.o reset.o
# The write-buffer drain is a CP15 operation, which Thumb-1 cannot encode
CFLAGS_sdram.o := -marm
CFLAGS_REMOVE_sdram.o := $(LTO_CFLAGS)
else ifdef CONFIG_SPL_BUILD
obj-y += spl.o cache.o boot-device.o reset.o
else
obj-y += board.o boot.o ethdiag.o cache.o mmu.o
obj-$(CONFIG_DISPLAY_CPUINFO) += cpu-info.o
endif
