// SPDX-License-Identifier: GPL-2.0-only
/* See luna_ponmac_logic.h. Moved verbatim; no line was rewritten.
 */
#include <linux/types.h>

#include "luna_ponmac_logic.h"

/* True for the SerDes offsets our golden table writes but the ...
 * dev/MEASURED-luna_ponmac_logic.c.md sec 1. */
bool c2_off_overconfig(u32 off)
{
	u32 a = off & 0xffffu;
	return (a >= 0x2608 && a <= 0x265c) ||	/* SDS_ANA_SPD page (REG34..55)     */
	       (a >= 0x2688 && a <= 0x26dc) ||	/* SDS_ANA_1P25G page (REG34..55)   */
	       (a >= 0x2788 && a <= 0x27dc) ||	/* SDS_ANA_EPON page (REG34..55)    */
	       (a >= 0x2c00 && a <= 0x2df8);	/* the 4 FIB-bank bodies            */
}
