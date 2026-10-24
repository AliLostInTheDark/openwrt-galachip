# SPDX-License-Identifier: GPL-2.0-only

CC ?= gcc
CFLAGS ?= -O2 -Wall
LDFLAGS ?=
LIBS ?= -lm

all: bosa-info

bosa-info: bosa-info.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f bosa-info *.o

.PHONY: all clean
