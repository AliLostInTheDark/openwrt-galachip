# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
TEST_BIN ?= /tmp/omci-protocol-test
KERNEL_FILES := ../../../../../target/linux/galachip/files

.PHONY: test check-sync
test: check-sync
	$(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -I../src/include \
		-o $(TEST_BIN) protocol-test.c ../src/me.c ../src/profiles.c ../src/wire.c
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)
	$(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -I../src/include \
		-o $(TEST_BIN)-attributes attributes-test.c ../src/attributes.c
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-attributes

# The protocol code is userspace only; the kernel shares just the headers.
check-sync:
	cmp ../src/include/linux/omci-protocol.h $(KERNEL_FILES)/include/linux/omci-protocol.h
	cmp ../src/include/linux/omci.h $(KERNEL_FILES)/include/uapi/linux/omci.h
	cmp ../src/include/linux/omci-backend.h $(KERNEL_FILES)/include/uapi/linux/omci-backend.h
