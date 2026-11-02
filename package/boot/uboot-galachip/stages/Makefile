# SPDX-License-Identifier: GPL-2.0-only
# Run from an empty output directory: make -f <package>/stages/Makefile ...
STAGES := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
CFLAGS := -Os -march=armv5te -marm -ffreestanding -fno-builtin -fno-pic \
	-fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-ffunction-sections -fdata-sections -fstack-usage -Wstack-usage=512 \
	-Wall -Wextra -I$(STAGES) -I$(STAGES)compat -I$(UBOOT_SRC)/lib/lzma \
	-I$(UBOOT_OBJ)/include/generated
LDFLAGS := -nostdlib -static -Wl,--gc-sections,--build-id=none
COMMON := common.o uart.o runtime.o

all: wrapper.bin spl.bin
%.o: $(STAGES)%.c $(STAGES)common.h
	$(CC) $(CFLAGS) -c $< -o $@
wrapper-start.o: $(STAGES)start.S meta.bin proper.lzma
	$(CC) $(CFLAGS) -DWRAPPER -c $< -o $@
spl-start.o: $(STAGES)start.S
	$(CC) $(CFLAGS) -c $< -o $@
LzmaDec.o: $(UBOOT_SRC)/lib/lzma/LzmaDec.c
	$(CC) $(CFLAGS) -DZ7_LZMA_SIZE_OPT -c $< -o $@
wrapper.elf: wrapper-start.o wrapper.o decode.o xmodem.o LzmaDec.o $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-T,$(STAGES)wrapper.lds,-Map,wrapper.map -o $@ $^ -lgcc
spl.elf: spl-start.o spl.o xmodem.o $(COMMON)
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,-T,$(STAGES)spl.lds,-Map,spl.map -o $@ $^ -lgcc
%.bin: %.elf
	$(OBJCOPY) -O binary $< $@
.PHONY: all
