// SPDX-License-Identifier: GPL-2.0-only
#include <command.h>
#include <asm/io.h>

void gc1601_eth_diagnostic(void);

static int do_g7diag(struct cmd_tbl *cmd, int flag, int argc, char *const argv[])
{
	printf("scu=%08x/%08x/%08x ddr=%08x/%08x\n",
	       readl((void *)0x90a00020), readl((void *)0x90a00030),
	       readl((void *)0x90a00810), readl((void *)0x90b00004),
	       readl((void *)0x90b00020));
	printf("gmac=%08x/%08x pm=%08x/%08x/%08x pool=%08x/%08x/%08x\n",
	       readl((void *)0x90e00000), readl((void *)0x90e00008),
	       readl((void *)0x90dc0020), readl((void *)0x90dc0024),
	       readl((void *)0x90dc0074), readl((void *)0x90dc0028),
	       readl((void *)0x90dc0034), readl((void *)0x90dc0030));
	printf("rx=%08x/%08x/%08x tx=%08x/%08x/%08x/%08x/%08x/%08x\n",
	       readl((void *)0x90dd4000), readl((void *)0x90dd4018),
	       readl((void *)0x90dd4020), readl((void *)0x90ddc040),
	       readl((void *)0x90ddc000), readl((void *)0x90ddc008),
	       readl((void *)0x90ddc00c), readl((void *)0x90ddc010),
	       readl((void *)0x90ddc014));
	gc1601_eth_diagnostic();
	return 0;
}
U_BOOT_CMD(g7diag, 1, 1, do_g7diag, "read GC1601 handoff registers", "");
