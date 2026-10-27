/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __XZ000_G7_H
#define __XZ000_G7_H
#define CFG_SYS_SDRAM_BASE 0x40000000
#define CFG_SYS_INIT_RAM_ADDR 0x40000000
#define CFG_SYS_INIT_RAM_SIZE 0x02000000
#define CFG_EXTRA_ENV_SETTINGS \
	"ipaddr=192.168.1.1\0" \
	"serverip=192.168.1.10\0" \
	"loadaddr=41000000\0" \
	"autostart=no\0" \
	"autoload=no\0"
#endif
