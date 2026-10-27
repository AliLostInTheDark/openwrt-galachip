// SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
static const int *input;
static size_t length, position, output_length;
static unsigned char output[100000];
void uart_script(const int *bytes, size_t size)
{
	input = bytes;
	length = size;
	position = output_length = 0;
}
int uart_get(void)
{
	return position < length ? input[position++] : -1;
}
void uart_put(int ch)
{
	if (output_length < sizeof(output))
		output[output_length++] = ch;
}
size_t uart_output_count(int ch)
{
	size_t count = 0, i;
	for (i = 0; i < output_length; i++)
		if (output[i] == ch)
			count++;
	return count;
}
