/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __ZX279125_PP_H
#define __ZX279125_PP_H

#include <linux/types.h>

#define ZX279125_PP_RESERVED_BASE 0x41000000U
#define ZX279125_PP_RESERVED_SIZE 0x00a00000U
#define ZX279125_PP_CURSORS 0x41840000U
#define ZX279125_PP_CURSORS_SIZE 128U

/* No writes to the engine: validate the stopped owner's retained table. */
int zx279125_pp_handoff(u32 *cursor_phys);

#endif
