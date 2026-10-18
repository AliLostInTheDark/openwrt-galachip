# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
TEST_BIN ?= /tmp/gc1601-sfu-collect-test
CFLAGS ?= -O1 -g -fsanitize=address,undefined

.PHONY: test
test:
	node sfu-collect.mjs | $(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		-Wno-unused-parameter -x c - -o $(TEST_BIN)
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)
	$(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		-Wno-unused-parameter gpon-mac-test.c -o /tmp/gc1601-gpon-mac-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/gc1601-gpon-mac-test
	node gpon-dbru.mjs | $(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		-Wno-unused-parameter -x c - -o /tmp/gc1601-gpon-dbru-test
	ASAN_OPTIONS=detect_leaks=0 /tmp/gc1601-gpon-dbru-test
