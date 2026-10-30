/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * What the omcid backend (luna_xpon.c) asks of the GPON MAC (luna_gpon.c),
 * and the activation events it is told about. A header of its own because
 * the two sides cannot share one translation unit: the in-kernel responder's
 * gpon_omci_core.h and net/xpon/omci.h both define struct omci_identity.
 */
#ifndef _LUNA_XPON_HW_H
#define _LUNA_XPON_HW_H

#include <linux/types.h>

/* Data T-CONT slots the backend may bind, and their GEM flows. */
#define LUNA_XPON_TCONTS	8
#define LUNA_XPON_GEMS		16
/* Their shared upstream queue bank: one queue per slot, then a pool. */
#define LUNA_XPON_BANK_QUEUES	32
#define LUNA_XPON_PIR_MAX	0x3ffff

struct luna_xpon_optics {
	s32 temp_mc;
	u32 vcc_uv;
	u32 bias_ua;
	u32 tx_nw;
	u32 rx_nw;
};

/* Downstream PON counters since boot, folded from clear-on-read registers. */
struct luna_xpon_pm {
	u64 bip_errors;
	u64 fec_corrected_bytes;
	u64 fec_corrected_codewords;
	u64 fec_uncorrectable_codewords;
	u64 fec_total_codewords;
	u64 fec_seconds;
	bool ds_fec;
	bool us_fec;
};

u8 luna_xpon_hw_state(void);
int luna_xpon_hw_los(void);
u8 luna_xpon_hw_onu_id(void);
int luna_xpon_hw_omcc_gem(void);
bool luna_xpon_hw_alloc_known(u16 alloc);
u8 luna_xpon_hw_omcc_qid(void);
u8 luna_xpon_hw_tcont_qid(unsigned int slot);
int luna_xpon_hw_tcont_install(unsigned int slot, u16 alloc);
void luna_xpon_hw_tcont_remove(unsigned int slot);
u8 luna_xpon_hw_gem_flow(unsigned int slot);
int luna_xpon_hw_gem_install(unsigned int slot, u16 gem, int qid);
void luna_xpon_hw_gem_remove(unsigned int slot);
u8 luna_xpon_hw_bank_qid(void);
int luna_xpon_hw_queue_set(u8 qid, bool wfq, u16 weight, u32 pir);
int luna_xpon_hw_queue_drain(u8 qid);
int luna_xpon_hw_tcont_queues(unsigned int slot, u32 mask);
int luna_xpon_hw_gem_queue(unsigned int slot, u8 qid);
bool luna_xpon_hw_mic_valid(const u8 *msg, unsigned int len);
int luna_xpon_hw_optics(struct luna_xpon_optics *o);
void luna_xpon_hw_pm(struct luna_xpon_pm *pm);
int luna_xpon_hw_a2h(u8 off, u8 *buf, unsigned int len);
bool luna_xpon_hw_allow_tx(void);
int luna_xpon_hw_set_allow_tx(bool on);
void luna_xpon_hw_get_identity(u8 sn[8], u8 password[10]);
int luna_xpon_hw_set_identity(const u8 *sn, const u8 *password);

#if IS_ENABLED(CONFIG_XPON_OMCI)
void luna_xpon_report_state(u8 state);
void luna_xpon_report_onu_id(u8 onu_id);
void luna_xpon_report_channel(u16 gem);
void luna_xpon_report_alloc(void);
void luna_xpon_report_identity(void);
/* The receiver's optical loss of signal, as the GTC samples it: net/xpon
 * shows it on the LOS lamp and reports it to omcid. */
void luna_xpon_report_los(bool los);
#else
static inline void luna_xpon_report_state(u8 state) { }
static inline void luna_xpon_report_onu_id(u8 onu_id) { }
static inline void luna_xpon_report_channel(u16 gem) { }
static inline void luna_xpon_report_alloc(void) { }
static inline void luna_xpon_report_identity(void) { }
static inline void luna_xpon_report_los(bool los) { }
#endif

#endif /* _LUNA_XPON_HW_H */
