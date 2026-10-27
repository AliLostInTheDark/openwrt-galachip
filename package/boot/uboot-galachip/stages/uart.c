// SPDX-License-Identifier: GPL-2.0-only
#include "common.h"
#define REG(a) (*(volatile uint32_t *)(a))
#define UART 0x90400000U
#define POLLS 3000000U

void uart_init(void)
{
	REG(UART + 0x0c) = 0x83;
	REG(UART) = 18;
	REG(UART + 4) = 0;
	REG(UART + 0x0c) = 3;
	REG(UART + 0x10) = 3;
	REG(UART + 8) = 7;
}

void uart_put(int ch)
{
	uint32_t n;

	for (n = POLLS; n; n--)
		if (REG(UART + 0x14) & 0x20) {
			REG(UART) = ch;
			return;
		}
}

int uart_get(void)
{
	uint32_t n;

	for (n = POLLS; n; n--)
		if (REG(UART + 0x14) & 1)
			return REG(UART) & 255;
	return -1;
}

void uart_puts(const char *s)
{
	while (*s) {
		if (*s == '\n')
			uart_put('\r');
		uart_put(*s++);
	}
}

void fail(const char *s)
{
	uart_puts(s);
	uart_puts("\nSRAM/DDR diagnostic halt; power-cycle.\n");
	for (;;)
		;
}
