# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
TEST_BIN ?= /tmp/omcid-engine-test
LIB := ../../libomci-protocol/src
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
WARN := -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-pointer-sign -Wno-sign-compare
SOURCES := ../src/agent.c ../src/me-kernel.c ../src/native-platform.c ../src/identity-native.c ../src/int-log-native.c
LIB_SOURCES := $(LIB)/me.c $(LIB)/profiles.c $(LIB)/wire.c $(LIB)/attributes.c

.PHONY: test check-sync
test: check-sync
	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-o $(TEST_BIN) engine-test.c $(SOURCES) $(LIB_SOURCES)
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)
	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-o $(TEST_BIN)-recovery recovery-test.c $(SOURCES) $(LIB_SOURCES) \
		../src/backend.c ../src/control.c ../src/metadata.c ../src/transport.c
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-recovery
	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB)/include \
		-o $(TEST_BIN)-transport transport-test.c
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-transport
	node ownership-test.js | $(CC) $(CFLAGS) $(WARN) -I$(LIB)/include \
		-x c - -o $(TEST_BIN)-ownership
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-ownership
	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-o $(TEST_BIN)-lifecycle lifecycle-test.c $(SOURCES) $(LIB_SOURCES) \
		../src/backend-client.c ../src/checkpoint.c ../src/control.c \
		../src/metadata.c ../src/transport.c ../src/olt-watch.c
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-lifecycle
	$(CC) $(CFLAGS) $(WARN) -I$(LIB)/include -o $(TEST_BIN)-proxy proxy-test.c
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-proxy
	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-o $(TEST_BIN)-client backend-client-test.c $(SOURCES) $(LIB_SOURCES) \
		../src/backend-client.c ../src/backend.c ../src/checkpoint.c
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-client

	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-o $(TEST_BIN)-seed seed-service-test.c $(SOURCES) $(LIB_SOURCES)
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-seed

	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-o $(TEST_BIN)-olt-watch olt-watch-test.c ../src/olt-watch.c
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-olt-watch

check-sync:
	sh check-sync.sh
