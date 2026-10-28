/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GC1601_STAGE_COMMON_H
#define GC1601_STAGE_COMMON_H
#include <stdint.h>
#include <stddef.h>
#define STAGE_SIZE 0x1e000U
#define WRAPPER_ENTRY 0x40100000U
#define PROPER_ENTRY 0x40010000U
#define PROPER_MAX 0xe0000U
#define UART_MAX (PROPER_MAX + 1024U)
#define STAGE_MAGIC 0x31503747U
#define LZMA_MAGIC 0x315a3747U
struct stage_footer {
	uint32_t magic, version, length, entry, crc, reserved[2], header_crc;
};
struct lzma_header {
	uint32_t magic, compressed, expanded, compressed_crc;
	uint32_t expanded_crc, destination, entry, header_crc;
};
uint32_t stage_crc(const void *data, size_t length);
int stage_valid(const void *data, size_t length);
int lzma_header_valid(const struct lzma_header *h, size_t compressed_size);
int uart_image_valid(const void *data, size_t received);
extern uint32_t uart_rx_errors;
struct xmodem_status {
	uint32_t packets, timeouts, crc_errors, header_errors;
	uint32_t bytes, sequence, calculated_crc, received_crc;
};
extern struct xmodem_status xmodem_status;
int uart_get(void);
void uart_put(int ch);
int xmodem_receive(void *destination, size_t capacity);
void stage_jump(uint32_t entry) __attribute__((noreturn));
void fail(const char *message) __attribute__((noreturn));
#endif
