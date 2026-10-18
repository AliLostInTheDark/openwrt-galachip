# SPDX-License-Identifier: GPL-2.0-only
CROSS_COMPILE ?= arm-openwrt-linux-muslgnueabi-
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
LOADER_VERSION ?= dev

CFLAGS := -march=armv5te -marm -Os -ffreestanding -fno-builtin -nostdlib \
	  -fno-pic -fno-stack-protector -ffunction-sections -fdata-sections \
	  -Wall -Wextra -DLOADER_VERSION='"$(LOADER_VERSION)"'
LDFLAGS := -nostdlib -Wl,--gc-sections -Wl,-T,loader.lds -Wl,--build-id=none

all: gc1601-loader.bin gc1601-loader-flash.bin

gc1601-loader.elf: start.S main.c loader.lds
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.S main.c -lgcc

gc1601-loader.bin: gc1601-loader.elf
	$(OBJCOPY) -O binary $< $@

gc1601-loader-flash.bin: gc1601-loader.bin mkspl.py
	python3 mkspl.py $< $@

clean:
	rm -f gc1601-loader.elf gc1601-loader.bin gc1601-loader-flash.bin

.PHONY: all clean
