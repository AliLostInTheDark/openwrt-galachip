# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
TEST_BIN ?= /tmp/omcid-engine-test
LIB := ../../protocol/src
# The kernel's backend operations, run against the engine in the tests
KERNEL_BACKEND := ../../../../../../target/linux/galachip/files/net/xpon/omci/backend.c
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
WARN := -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-pointer-sign -Wno-sign-compare
SOURCES := ../src/agent.c ../src/me-kernel.c ../src/native-platform.c ../src/identity-native.c
LIB_SOURCES := $(LIB)/me.c $(LIB)/profiles.c $(LIB)/wire.c $(LIB)/attributes.c

.PHONY: test check-sync
test: check-sync
	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-o $(TEST_BIN) engine-test.c $(SOURCES) $(LIB_SOURCES) -lm
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)
	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-o $(TEST_BIN)-recovery recovery-test.c $(SOURCES) $(LIB_SOURCES) \
		$(KERNEL_BACKEND) ../src/control.c ../src/metadata.c ../src/transport.c -lm
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-recovery
	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB)/include \
		-o $(TEST_BIN)-transport transport-test.c
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-transport
	node ownership-test.js | $(CC) $(CFLAGS) $(WARN) -I$(LIB)/include \
		-x c - -o $(TEST_BIN)-ownership
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-ownership
	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-I../../src -I../../src/include \
		-o $(TEST_BIN)-lifecycle lifecycle-test.c $(SOURCES) $(LIB_SOURCES) \
		../src/backend-client.c ../src/backend-layout.c ../src/checkpoint.c ../src/control.c \
		../src/metadata.c ../src/transport.c ../src/olt-watch.c ../../src/xpon-nl.c -lm
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-lifecycle
	$(CC) $(CFLAGS) $(WARN) -I$(LIB)/include -o $(TEST_BIN)-proxy proxy-test.c
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-proxy
	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-o $(TEST_BIN)-client backend-client-test.c $(SOURCES) $(LIB_SOURCES) \
		../src/backend-client.c ../src/backend-layout.c $(KERNEL_BACKEND) ../src/checkpoint.c \
		../src/olt-watch.c -lm
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-client

	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-o $(TEST_BIN)-seed profile-seed-test.c $(SOURCES) $(LIB_SOURCES) -lm
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-seed
	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-o $(TEST_BIN)-qos qos-compat-test.c $(SOURCES) $(LIB_SOURCES) -lm
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-qos

	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-o $(TEST_BIN)-olt-watch olt-watch-test.c ../src/olt-watch.c
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-olt-watch

	$(CC) $(CFLAGS) $(WARN) -I../src -I$(LIB) -I$(LIB)/include \
		-o $(TEST_BIN)-replay replay-test.c $(SOURCES) $(LIB_SOURCES) -lm
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BIN)-replay sessions/genexis-gnxs-boot.txt genexis

check-sync:
	sh check-sync.sh
