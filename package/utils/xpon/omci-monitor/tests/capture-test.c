// SPDX-License-Identifier: GPL-2.0-only
#define main omci_monitor_main
#include "../src/monitor.c"
#undef main
#include <assert.h>

int main(void)
{
	unsigned char bytes[4];
	FILE *fp = tmpfile();

	assert(fp);
	assert(!capture_flush(fp));
	assert(!fseeko(fp, 1024 * 1024, SEEK_SET));
	assert(fputc('x', fp) != EOF);
	assert(capture_flush(fp) == -EFBIG);
	assert(!fclose(fp));
	assert(decode_hex("00aaff", bytes, sizeof(bytes)) == 3);
	assert(bytes[1] == 0xaa && bytes[2] == 0xff);
	assert(decode_hex("0z", bytes, sizeof(bytes)) == -EINVAL);
	assert(decode_hex("0", bytes, sizeof(bytes)) == -EINVAL);
	assert(decode_hex("0001", bytes, 1) == -EINVAL);
	puts("Capture-bound and hex checks passed");
	return 0;
}
