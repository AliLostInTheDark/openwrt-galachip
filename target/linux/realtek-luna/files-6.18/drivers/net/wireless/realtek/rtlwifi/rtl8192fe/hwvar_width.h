/* SPDX-License-Identifier: GPL-2.0 */
/* Width-exact reads of set_hw_reg/get_hw_reg values, shared by hw.c and the host test
 * (dev/rtl9607c-test/rtl8192fe_hwvar_be_test, run big-endian under qemu-mips).
 * These hosts are big-endian: a u32 read through a u16 pointer is its HIGH half. */
#ifndef __RTL92FE_HWVAR_WIDTH_H__
#define __RTL92FE_HWVAR_WIDTH_H__

/* HW_VAR_BASIC_RATE: `val` points at a u32 basic-rate bitmap (mac->basic_rates).
 * -> the RRSR low 16 bits: CCK 1/5.5/11 always, 2M never, OFDM 6/12/24 when basic. */
static inline u16 rtl92fe_rrsr_from_basic(const u8 *val)
{
	u16 cfg = (u16)(*(const u32 *)val & 0x15f);

	return (u16)((cfg | 0x01 | 0x0d) & ~0x02);
}

/* HW_VAR_CORRECT_TSF: the 64-bit TSF composed by VALUE from its two register halves. */
static inline u64 rtl92fe_tsf_compose(u32 high, u32 low)
{
	return ((u64)high << 32) | low;
}

#endif
