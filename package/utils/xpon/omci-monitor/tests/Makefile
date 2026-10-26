# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
CFLAGS ?= -O2 -g
TEST_BIN ?= /tmp/omci-monitor-test

.PHONY: test
test:
	$(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -I../src/include \
		-o $(TEST_BIN) test.c ../src/wire.c ../src/netlink.c
	$(TEST_BIN)
	$(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -I../src/include \
		-o $(TEST_BIN)-transport transport-test.c
	$(TEST_BIN)-transport
	$(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -I../src/include \
		-o $(TEST_BIN)-capture capture-test.c ../src/wire.c ../src/netlink.c
	$(TEST_BIN)-capture
	$(CC) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -I../src/include \
		-o $(TEST_BIN)-monitor ../src/monitor.c ../src/wire.c ../src/netlink.c
	$(TEST_BIN)-monitor -r replay.capture
	! $(TEST_BIN)-monitor -r malformed.capture
