# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
CFLAGS ?= -O1 -g
CFLAGS += -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer
TEST_BIN ?= /tmp/epon-oamd-test

.PHONY: test
test:
	$(CC) $(CFLAGS) oam-test.c -o $(TEST_BIN)
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)
