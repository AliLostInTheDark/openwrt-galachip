// SPDX-License-Identifier: GPL-2.0-only
/* CRC-16 XMODEM: bounded byte waits, retry budget and duplicate validation. */
#include "common.h"
#define SOH 1
#define STX 2
#define EOT 4
#define ACK 6
#define NAK 21
#define CAN 24
static uint8_t block[1024];
struct xmodem_status xmodem_status;

static uint16_t crc16(uint16_t crc, uint8_t data)
{
	unsigned int i;

	crc ^= (uint16_t)data << 8;
	for (i = 0; i < 8; i++)
		crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
	return crc;
}

int xmodem_receive(void *destination, size_t capacity)
{
	uint8_t *dst = destination, expected = 1;
	size_t total = 0, count, last_count = 0, i;
	unsigned int errors = 0;
	uint16_t crc;
	int c, seq, inverse, hi, lo, previous;

	xmodem_status = (struct xmodem_status){0};
	uart_rx_errors = 0;

	while (errors < 64) {
		if (!total)
			uart_put('C');
		c = uart_get();
		if (c == CAN)
			return -1;
		if (c == EOT) {
			if (!total)
				return -1;
			uart_put(ACK);
			return total;
		}
		if (c != SOH && c != STX) {
			if (c < 0)
				xmodem_status.timeouts++;
			else
				xmodem_status.header_errors++;
			/* Before the first packet, request CRC mode without a NAK. */
			if (!total) {
				errors++;
				continue;
			}
			goto retry;
		}
		count = c == SOH ? 128 : 1024;
		xmodem_status.packets++;
		xmodem_status.bytes = 0;
		seq = uart_get();
		inverse = uart_get();
		if (seq < 0 || inverse < 0) {
			xmodem_status.timeouts++;
			goto retry;
		}
		xmodem_status.sequence = seq;
		for (i = 0; i < count; i++) {
			c = uart_get();
			if (c < 0) {
				xmodem_status.timeouts++;
				xmodem_status.bytes = i;
				goto retry;
			}
			block[i] = c;

		}
		xmodem_status.bytes = count;
		hi = uart_get();
		lo = uart_get();
		if (hi < 0 || lo < 0) {
			xmodem_status.timeouts++;
			goto retry;
		}
		if ((seq ^ inverse) != 255) {
			xmodem_status.header_errors++;
			goto retry;
		}
		/* Receive the complete burst before doing CRC work with caches off. */
		crc = 0;
		for (i = 0; i < count; i++)
			crc = crc16(crc, block[i]);
		xmodem_status.calculated_crc = crc;
		xmodem_status.received_crc = (hi << 8) | lo;
		if (crc != xmodem_status.received_crc) {
			xmodem_status.crc_errors++;
			goto retry;
		}
		previous = total && (uint8_t)seq == (uint8_t)(expected - 1);
		if (previous) {
			/* A duplicate may only match bytes from the last accepted block. */
			if (count != last_count || count > total)
				goto cancel;
			for (i = 0; i < count; i++)
				if (dst[total - count + i] != block[i])
					goto cancel;
			uart_put(ACK);
			/* Duplicate traffic must not reset the finite retry budget. */
			errors++;
			continue;
		}
		if ((uint8_t)seq != expected || count > capacity - total)
			goto cancel;
		for (i = 0; i < count; i++)
			dst[total + i] = block[i];
		total += count;
		last_count = count;
		expected++;
		errors = 0;
		uart_put(ACK);
		continue;
retry:
		uart_put(NAK);
		errors++;
	}
cancel:
	uart_put(CAN);
	uart_put(CAN);
	return -1;
}
