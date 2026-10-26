/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ZTE Sanechips ZX279125 GPON MAC and Optical Controller definitions
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#ifndef _ZX279125_PON_H
#define _ZX279125_PON_H

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/soc/zte/zx279125-pp.h>
#include <net/xpon.h>
#include <net/xpon/omci.h>

/* Top Packet Engine / Interrupt Controller (0x92040000) */
#define ZX_GMAC_INT_STATUS		0x40
#define ZX_GMAC_INT_MASK		0x44
#define ZX_GMAC_INT_GPON		BIT(0)
#define ZX_GMAC_INT_ETH			BIT(2)

/* GPON MAC Core (0x920C0000) */
#define ZX_PON_INT_STATUS		0x00
#define ZX_PON_INT_MASK			0x04
#define ZX_PON_INT_PLOAM_RCV		BIT(7)
#define ZX_PON_INT_LOS			BIT(5)
#define ZX_PON_INT_LOF			BIT(6)
#define ZX_PON_INT_VALID			0x1ef

/* GPON Rx Config & Counters (0x920C4000) */
#define ZX_PON_RX_CFG			0x00
#define ZX_PON_RX_CFG_EN		BIT(0)
#define ZX_PON_RX_CFG_INV		BIT(1)
#define ZX_PON_RX_STAT			0x14
#define ZX_PON_RX_FEC_CFG		0x1c
#define ZX_PON_RX_FEC_EN		BIT(0)
#define ZX_PON_RX_PLOAM_CFG		0x24
#define ZX_PON_RX_SUPERFRAME_CNT	0x48
#define ZX_PON_RX_BIP8_ERR_CNT		0x70
#define ZX_PON_RX_PLOAM_BROAD_CNT	0xc0
#define ZX_PON_RX_PLOAM_ONUID_CNT	0xc4
#define ZX_PON_RX_PLOAM_CRC_ERR_CNT	0xd4

/* GEM Port & OMCI Engine (0x920D0000) */
#define ZX_GEM_OMCI_CTRL		0x00
#define ZX_GEM_OMCI_DROP_DIS		BIT(1)
#define ZX_GEM_ETH_LEN_CFG		0x04
#define ZX_GEM_OMCI_LEN_CFG		0x08
#define ZX_GEM_VALID_FRAMES_CNT		0x44
#define ZX_GEM_CRC_ERR_CNT		0x198

/* ONU State & Ranging Control (0x920D4000) */
#define ZX_ONU_STATE_CTRL		0x00
#define ZX_ONU_STATE_MASK		GENMASK(2, 0)
#define ZX_ONU_ID_MASK			GENMASK(23, 16)
#define ZX_ONU_ID_SHIFT			16
#define ZX_ONU_RESP_TIME		0x10
#define ZX_ONU_EQD_DELAY		0x14
#define ZX_ONU_PRE_EQD_DELAY		0x24

/* Upstream Tx & PLOAM (0x920D8000) */
#define ZX_PON_TX_CFG			0x00
#define ZX_PON_TX_FORCE_CLOSE		BIT(1)
#define ZX_PON_TX_BURST_EN		0x04
#define ZX_PON_SERIAL_NUM		0x40
#define ZX_PON_PLOAM_TX			0x48
#define ZX_PON_OVERHEAD_MSG		0x64

/* AES Encryption (0x92100000) */
#define ZX_AES_KEY_SWITCH_TIME		0x20

/* ITU-T G.984 ONU States */
enum zx_gpon_onu_state {
	ZX_ONU_STATE_O1_INITIAL		= 1,
	ZX_ONU_STATE_O2_STANDBY		= 2,
	ZX_ONU_STATE_O3_SERIAL_NUM	= 3,
	ZX_ONU_STATE_O4_RANGING		= 4,
	ZX_ONU_STATE_O5_OPERATION	= 5,
	ZX_ONU_STATE_O6_POPUP		= 6,
	ZX_ONU_STATE_O7_EMERGENCY_STOP	= 7,
};

/* Optical BOSA Subsystem (UX3320S / GN25L95 / ZX279511 via I2C at 0x51) */
#define ZX_BOSA_I2C_ADDR		0x51
#define ZX_BOSA_PAGE_SELECT		0x7f
#define ZX_BOSA_CTRL			0x6e
#define ZX_BOSA_SOFT_TX_DIS		BIT(6)

/* BOSA Telemetry Registers */
#define ZX_BOSA_REG_TEMP		0x60
#define ZX_BOSA_REG_VCC			0x62
#define ZX_BOSA_REG_TX_BIAS		0x64
#define ZX_BOSA_REG_TX_POWER		0x66
#define ZX_BOSA_REG_RX_POWER		0x68

/* Main Driver Context */
struct zx279125_pon {
	struct device		*dev;
	void __iomem		*base_gmac_int;
	void __iomem		*base_gpon;
	void __iomem		*base_rx;
	void __iomem		*base_drs;
	void __iomem		*base_ports;
	void __iomem		*base_gem;
	void __iomem		*base_onu;
	void __iomem		*base_tx;
	void __iomem		*base_aes;

	int			irq;
	spinlock_t		lock;
	struct mutex		bosa_lock;
	struct mutex		config_lock;
	struct work_struct	state_work;
	struct delayed_work	state_timeout;
	u32			pending_irqs;
	u8			preamble1, preamble2, preamble3;
	u16			preamble3_sync, preamble3_oper;
	bool			running, tx_hold, laser_allowed;
	u16			omci_port;
	u16			alloc_ids[32];
	unsigned long		alloc_valid;
	u32			delimiter;
	u8			delimiter_bits;
	bool			emergency_stop, overhead_valid;
	u16			tcont_entities[32];
	unsigned long		tcont_valid;
	u8			aes_key_index, random_delay;
	bool			aes_key_valid, serial_mask_enabled, serial_mask_match;
	u8			aes_shadow_key[16];
	struct {
		u16 entity, gem, tcont;
		bool valid;
	} gems[64];
	struct zx279125_pp_receiver receiver;

	struct i2c_adapter	*i2c_adap;
	struct net_device	*netdev;
	struct xpon_device	*xpon;
	struct omci_device	*odev;

	enum zx_gpon_onu_state	state;
	u8			onu_id;
	u8			serial_number[8];
	u8			password[10];

	u8			pon_mac[6];
	u8			base_mac[6];
	char			epon_sn[32];
	bool			has_pon_mac;
	bool			has_base_mac;
	bool			has_epon_sn;
	enum xpon_mode		mode_pref;

	bool			tx_allowed;
	bool			fec_enabled;
};

int zx279125_gpon_init(struct zx279125_pon *pon);
int zx279125_gpon_start(struct zx279125_pon *pon);
void zx279125_gpon_hold_tx(struct zx279125_pon *pon, bool hold);
int zx279125_gpon_set_alloc(struct zx279125_pon *pon, unsigned int index,
			    u16 alloc, bool valid);
int zx279125_gpon_set_omcc(struct zx279125_pon *pon, u16 gem, bool valid);
int zx279125_gpon_send_ploam(struct zx279125_pon *pon, u8 msg,
			     const u8 data[10], unsigned int repeat);
void zx279125_ploam_init(struct zx279125_pon *pon);
void zx279125_ploam_stop(struct zx279125_pon *pon);
int zx279125_gpon_request_key(struct zx279125_pon *pon);
int zx279125_gpon_encrypt_port(struct zx279125_pon *pon, const u8 data[10]);
int zx279125_gpon_switch_key(struct zx279125_pon *pon, const u8 data[10]);
void zx279125_gpon_crypto_reset(struct zx279125_pon *pon);
void zx279125_gpon_exit(struct zx279125_pon *pon);
int zx279125_gpon_set_state(struct zx279125_pon *pon, enum zx_gpon_onu_state state);
irqreturn_t zx279125_pon_isr(int irq, void *dev_id);
int zx279125_tagparam_load(struct zx279125_pon *pon, struct omci_identity *id);

#endif /* _ZX279125_PON_H */
