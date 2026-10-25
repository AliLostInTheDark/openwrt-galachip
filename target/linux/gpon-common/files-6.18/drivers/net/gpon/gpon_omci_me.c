// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/string.h>

#include "gpon_common.h"	/* GPON_GEM_BIDIR -- G.988 ME 268 direction 3 */
#include "gpon_omci_me.h"

/* ---- dynamic (OLT-provisioned) ME store ---- */

struct omci_me_inst *omci_store_find(struct omci_onu *o, u16 class_id,
				     u16 inst)
{
	u16 k;

	for (k = 0; k < OMCI_STORE_MAX; k++)
		if (o->store[k].used && o->store[k].class_id == class_id &&
		    o->store[k].inst == inst)
			return &o->store[k];
	return NULL;
}

/* Does the ONU hold ANY instance of @class_id?  Used to separate "I do not
 * know that class at all" (0x04) from "I know the class, not that instance"
 * (0x05) on a Set the OLT sends for something it never created. */
bool omci_store_has_class(struct omci_onu *o, u16 class_id)
{
	u16 k;

	for (k = 0; k < OMCI_STORE_MAX; k++)
		if (o->store[k].used && o->store[k].class_id == class_id)
			return true;
	return false;
}

/* idx-th used entry in array order — the MIB-Upload tail rows. */
struct omci_me_inst *omci_store_nth(struct omci_onu *o, u16 idx)
{
	u16 k, n = 0;

	for (k = 0; k < OMCI_STORE_MAX; k++)
		if (o->store[k].used) {
			if (n == idx)
				return &o->store[k];
			n++;
		}
	return NULL;
}

/* Insert one OLT-created instance. */
bool omci_store_put(struct omci_onu *o, u16 class_id, u16 inst,
		    const u8 *body, int blen)
{
	struct omci_me_inst *e = NULL;
	u16 k;

	for (k = 0; k < OMCI_STORE_MAX; k++)
		if (!o->store[k].used) {
			e = &o->store[k];
			break;
		}
	if (!e)
		return false;

	memset(e, 0, sizeof(*e));
	e->used = true;
	e->class_id = class_id;
	e->inst = inst;
	e->blen = 0;
	o->store_n++;
	if (body && blen > 0) {
		if (blen > (int)sizeof(e->body))
			blen = sizeof(e->body);
		memcpy(e->body, body, blen);
		e->blen = (u8)blen;
	}
	return true;
}

/* Apply a Set's attribute values to a provisioned instance. */
void omci_store_merge(struct omci_me_inst *e, const u8 *val, int vlen)
{
	if (vlen <= 0)
		return;
	if (vlen > (int)sizeof(e->body))
		vlen = sizeof(e->body);
	memcpy(e->body, val, vlen);
	if (e->blen < (u8)vlen)
		e->blen = (u8)vlen;
}

void omci_store_del(struct omci_onu *o, u16 class_id, u16 inst)
{
	struct omci_me_inst *e = omci_store_find(o, class_id, inst);

	if (e) {
		e->used = false;
		if (o->store_n)
			o->store_n--;
	}
}

/* ME attribute model.
 * struct omci_identity is in the header:
 * each ONU carries its own copy, seeded from this build default. */
static const struct omci_identity omci_id_default_bytes = {
	/* The HSGQ-G008 OLT recognizes HSGQ ONUs; "XPON" was rejected. */
	.vendor_id	  = { 'H', 'S', 'G', 'Q' },
	.onu_g_version	  = { '0', '2', 'A', '5', 'B', '1' },
	.sw_bank0_version = { 'M', '2', '2', '5', '-',
			      '2', '6', '0', '5', '2', '5' },
	.sw_bank1_version = { 'M', '2', '2', '5', '-',
			      '2', '6', '0', '5', '1', '5' },
	.equipment_id	  = { 'H', 'S', 'G', 'Q', '-',
			      'X', '4', '1', '1', 'A', 'X', 'F' },
	.product_code	  = { 0x00, 0x1f },	/* stock's MIB "31", as it reads it */
	.loid		  = { 'u', 's', 'e', 'r' },
	.operator_id	  = { 'C', 'T', 'C' },
	/* .omcc_version 0: the far end's (generic: 0x80, G.984.4 baseline, the stocks' own) */
	/* .zeros and the tails above are 0 by omission — C zero-fills what a
	 * designated initializer does not name. */
};

/* One assert per field, NAMING the field.  The descriptor table derives its
 * wire sizes from these members (A_ID), so shrinking or growing one is
 * refused here at build time, not discovered as a shifted reply at the OLT. */
#define OMCI_ID_SIZEOF(member)						\
	sizeof(((const struct omci_identity *)0)->member)
#define OMCI_ID_ASSERT(member, n)					\
	_Static_assert(OMCI_ID_SIZEOF(member) == (n),			\
		       "omci_identity." #member " must be " #n " octets")
OMCI_ID_ASSERT(vendor_id, 4);		/* ONU-G #1 / Circuit-Pack #5 */
OMCI_ID_ASSERT(onu_g_version, 14);	/* ONU-G #2 */
OMCI_ID_ASSERT(sw_bank0_version, 14);	/* SW-image #1 / Circuit-Pack #4 */
OMCI_ID_ASSERT(sw_bank1_version, 14);	/* SW-image #1, bank 1 */
OMCI_ID_ASSERT(equipment_id, 20);	/* ONU2-G #1 */
OMCI_ID_ASSERT(product_code, 2);	/* ONU2-G #3 */
OMCI_ID_ASSERT(loid, 24);		/* ONU-G #10 / CTC LoID #2 */
OMCI_ID_ASSERT(loid_passwd, 12);	/* ONU-G #11 / CTC LoID #3 */
OMCI_ID_ASSERT(operator_id, 4);		/* CTC #1 */
OMCI_ID_ASSERT(omcc_version, 1);	/* ONU2-G #2 */
OMCI_ID_ASSERT(farend, 8);
OMCI_ID_ASSERT(zeros, 25);		/* == its longest consumer, VEIP #3 */
/* every member is a u8 array, so equality here also proves no padding crept
 * in: the flat 119-byte pool plus product code (2), LoID password (12), OMCC
 * version (1) and the far-end choice (8) */
_Static_assert(sizeof(struct omci_identity) == 142,
	       "omci_identity is not the 142-byte pool the OLT reads");

/* Where a descriptor row takes its value bytes from. */
enum omci_attr_src {
	OMCI_SRC_CONST,		/* v = the value, big-endian in `size` bytes */
	OMCI_SRC_ID,		/* v = offsetof() into struct omci_identity */
	OMCI_SRC_SN,		/* the board serial number (8) */
	OMCI_SRC_MDS,		/* ME 2 #1 = the live MIB-Data-Sync */
	OMCI_SRC_DYN,		/* v = enum omci_attr_dyn */
	OMCI_SRC_STORE,		/* v = dense offset in the dynamic instance */
	/* A LONG attribute the 26-octet dense body cannot hold */
	OMCI_SRC_TBL,
	/* ★★ A COUNTER THIS MODEL DECLARES AND THIS ONU CANNOT ESTABLISH.
	 * It occupies its dense slot (so the layout stays stock's) and serves
	 * NO VALUE: the Get leaves its bit out of the response mask, which puts
	 * it in the ATTRIBUTES-FAILED word -- G.988's own "could not ask".
	 * A missing counter rendered as 0 is a measurement nobody took. */
	OMCI_SRC_CNT,
	OMCI_SRC_ANIG_THRESHOLD,
	OMCI_SRC_TEXT,		/* v = enum omci_text */
	OMCI_SRC_OLT_G,		/* v = offset into o->olt_g, OLT-writable */
	OMCI_SRC_INST0,		/* one octet: v >> 8 on instance 0, else v & 0xff */
};

/* Constant octet strings stock serves, zero-padded. */
enum omci_text {
	OMCI_TEXT_SPACES,	/* an equipment ID nobody set: 20 spaces */
	OMCI_TEXT_ZERO_DIGIT,	/* a version or address nobody set: "0" */
	/* the vendor MEs' own defaults, as stock's plugins and upload hold them */
	OMCI_TEXT_TWO,		/* ME 65363 #2 configuration version */
	OMCI_TEXT_WEP_KEY,	/* ME 65386 #6..#9: a WEP key nobody set */
	OMCI_TEXT_EXT_ONU_G7,	/* ME 65408 #7  (mib_ExtendedOnuG) */
	OMCI_TEXT_EXT_ONU_G9,	/* ME 65408 #9  region */
	OMCI_TEXT_EXT_ONU_G10,	/* ME 65408 #10 a masked password */
	OMCI_TEXT_EXT_ONU_G12,	/* ME 65408 #12 */
	OMCI_TEXT_HW_CAP,	/* ME 65427 #2 capability declaration */
	OMCI_TEXT_P2Q_CAP,	/* ME 350 #14 */
	OMCI_TEXT_CTC_OP,	/* ME 65529 #1 operation ID */
};

static const u8 omci_text[][26] = {
	[OMCI_TEXT_SPACES]	= "                    ",
	[OMCI_TEXT_ZERO_DIGIT]	= "0",
	[OMCI_TEXT_TWO]		= "2",
	[OMCI_TEXT_WEP_KEY]	= "0000000000",
	[OMCI_TEXT_EXT_ONU_G7]	= "12D7",
	[OMCI_TEXT_EXT_ONU_G9]	= "CHINA",
	[OMCI_TEXT_EXT_ONU_G10]	= "****************",
	[OMCI_TEXT_EXT_ONU_G12]	= "EeAa",
	[OMCI_TEXT_HW_CAP]	= "RSQQQSQSQSSSSQQS",
	[OMCI_TEXT_P2Q_CAP]	= "\x01\x08",
	[OMCI_TEXT_CTC_OP]	= "CTC",
};

/* Where an OMCI_SRC_TBL write goes. */
enum omci_attr_tbl {
	OMCI_TBL_EXT_VLAN_ROW,	/* ME 171 #6 -> the row table, decoded */
	OMCI_TBL_MAC_FILTER_ROW,/* ME 49 #1 -> the per-bridge-port MAC filter */
	OMCI_TBL_HELD,		/* accepted and COUNTED, not held: see
				 * gpon_vlan_model.attr_owed */
};

/* The few attributes whose value is derived from the ME INSTANCE. */
enum omci_attr_dyn {
	OMCI_DYN_UNI_ADMIN,	/* ME 11 #5: accepted reported admin state */
	/* TWO SELECTORS FOR ONE VALUE, ON PURPOSE. */
	OMCI_DYN_UNI_EXPECTED,	/* ME 11 #1: the expected plug-in type */
	OMCI_DYN_UNI_TYPE,	/* ME 11 #2: the SENSED type -- the capability */
	OMCI_DYN_UNI_MAX_FRAME,	/* ME 11 #8: as the OLT set it */
	OMCI_DYN_UNI_PAUSE,	/* ME 11 #10: as the OLT set it */
	OMCI_DYN_UNIG_ADMIN,	/* ME 264 #2: the same, for UNI-G */
	OMCI_DYN_UNIG_CAP,	/* ME 264 #3: management capability, per
				 * instance -- the extra UNI-G each board
				 * carries reports 0 where the others report 1 */
	OMCI_DYN_SW_VER,	/* ME 7 #1: per-bank version field */
	OMCI_DYN_SW_FLAG,	/* ME 7 #2/#3: bank 0 is the active+committed */
	OMCI_DYN_TCONT_ALLOC,	/* ME 262 #1: alloc-ID of this T-CONT */
	OMCI_DYN_PQ_PORT,	/* ME 277 #6: related port, counts DOWN in the
				 * 8-queue block (queue 0 -> 7, 1 -> 6, ...) */
	OMCI_DYN_TS_TCONT,	/* ME 278 #1: T-CONT pointer == the instance */
	OMCI_DYN_ANIG_RX,	/* ME 263 #10: live RX optical level */
	OMCI_DYN_ANIG_TX,	/* ME 263 #14: live TX optical level */
	/* ME 5 / ME 6, from the declared slot of that instance */
	OMCI_DYN_SLOT_TYPE,
	OMCI_DYN_SLOT_PORTS,
	OMCI_DYN_SLOT_TBUF,
	OMCI_DYN_SLOT_PRIQ,
	OMCI_DYN_SLOT_SCHED,
	OMCI_DYN_IP_HOST_MAC,	/* ME 134 #2: the WAN MAC, where declared */
	OMCI_DYN_TCONT_N,	/* ANI-G #2: the board's T-CONT count */
	OMCI_DYN_ONU2G_PQ,	/* ONU2-G #6 */
	OMCI_DYN_SCHED_N,	/* ONU2-G #7 */
	OMCI_DYN_GEM_N,		/* ONU2-G #9 */
	OMCI_DYN_WAN_MAC,	/* the WAN netdev's MAC, could-not-ask until set */
	OMCI_DYN_CTC,		/* ME 65529 #2..#5, as the board declares them */
	OMCI_DYN_OMCC,		/* ONU2-G #2: the identity's, else the far end's */
};

/* One modelled attribute. */
struct omci_attr {
	u16	class_id;
	u16	v;
	u8	attr;
	u8	size;
	u8	src;
	u8	access;	/* verified dynamic layout: read=1, write=2, create=4 */
};

/* G.988 calls the attribute writable and this ONU has */
#define OMCI_ACCESS_WRITE_UNCHANGED	8
/* Served by a Get and never uploaded: stock's optionType & 0x31A, read out of
 * its own ME plugins (OMCI-simulate/me_attr_table.py) and identical on four
 * Luna stocks; the field X111W's upload shows exactly that set. */
#define OMCI_ACCESS_HIDDEN		16

#define AT(cls, n, sz, s, arg)	{ (cls), (arg), (n), (sz), (s), 0 }
#define A_ST(cls, n, sz, off, acc) \
	{ (cls), (off), (n), (sz), OMCI_SRC_STORE, (acc) }
/* a LONG attribute the dense body cannot hold: @where is enum omci_attr_tbl */
#define A_TBL(cls, n, sz, where, acc) \
	{ (cls), (where), (n), (sz), OMCI_SRC_TBL, (acc) }
#define A_C(cls, n, sz, val)	AT(cls, n, sz, OMCI_SRC_CONST, val)
/* a constant G.988 declares writable: settable to the value it already serves */
#define A_CW(cls, n, sz, val)	{ (cls), (val), (n), (sz), OMCI_SRC_CONST,  \
				  OMCI_ACCESS_WRITE_UNCHANGED }
/* identity field: the wire size IS the named member's size — one source */
#define A_ID(cls, n, member)	AT(cls, n, OMCI_ID_SIZEOF(member),	\
				   OMCI_SRC_ID,				\
				   offsetof(struct omci_identity, member))
/* @sz octets of zeros, served from the zeros member (@sz <= 25 — see the
 * zeros member: a larger ask reads off the end, and only the x86 sweep
 * plus ASan police that bound) */
#define A_ZERO(cls, n, sz)	AT(cls, n, sz, OMCI_SRC_ID,		\
				   offsetof(struct omci_identity, zeros))
#define A_SN(cls, n)		AT(cls, n, 8, OMCI_SRC_SN, 0)
#define A_MDS(cls, n)		AT(cls, n, 1, OMCI_SRC_MDS, 0)
#define A_D(cls, n, sz, dyn)	AT(cls, n, sz, OMCI_SRC_DYN, dyn)
#define A_NO_ATTRS(cls)		AT(cls, 0, 0, OMCI_SRC_CONST, 0)
/* a read-only COUNTER at dense offset @off that nothing feeds: see
 * OMCI_SRC_CNT.  It keeps the dense layout stock's and answers COULD NOT ASK. */
#define A_CNT(cls, n, sz, off)	{ (cls), (off), (n), (sz), OMCI_SRC_CNT, 1 }
#define A_HIDE(cls, n, sz, src, v, acc)	\
	{ (cls), (v), (n), (sz), (src), (acc) | OMCI_ACCESS_HIDDEN }
#define A_TXT(cls, n, sz, which)	{ (cls), (which), (n), (sz), OMCI_SRC_TEXT, 0 }
#define A_OLTG(n, off, sz)	{ OMCI_ME_OLT_G, (off), (n), (sz), OMCI_SRC_OLT_G, 3 }
#define A_INST0(cls, n, first, rest) \
	{ (cls), ((first) << 8) | (rest), (n), 1, OMCI_SRC_INST0, 0 }
/* @sz octets of a unit identity member from its start */
#define A_IDN(cls, n, sz, member)	AT(cls, n, sz, OMCI_SRC_ID,	\
					   offsetof(struct omci_identity, member))

static const struct omci_attr omci_attrs[] = {
	/* ---- ME 2 ONU-Data (inst 0) ---- */
	A_MDS(2, 1),				/* #1  MIB-Data-Sync */

	/* ---- ME 256 ONU-G (inst 0).  ALL 14 attributes are servable: a
	 * missing one answers a short mask and the OLT re-GETs forever. ---- */
	A_ID(256,  1, vendor_id),		/* #1  Vendor ID */
	A_ID(256,  2, onu_g_version),		/* #2  Version */
	A_SN(256, 3),				/* #3  Serial number */
	A_C(256,  4,  1, 0x02),			/* #4  Traffic-mgmt option */
	A_C(256,  5,  1, 0x00),			/* #5  ATM CC option */
	A_C(256,  6,  1, 0x00),			/* #6  Battery backup */
	A_C(256,  7,  1, 0x00),			/* #7  Admin state */
	A_C(256,  8,  1, 0x00),			/* #8  Op state */
	A_HIDE(256, 9, 1, OMCI_SRC_CONST, 0x00, 0),	/* #9  Survival time */
	A_ID(256, 10, loid),			/* #10 Logical ONU ID */
	A_ID(256, 11, loid_passwd),		/* #11 Logical password */
	A_C(256, 12,  1, 0x00),			/* #12 Credentials status */
	A_HIDE(256, 13, 2, OMCI_SRC_CONST, 0x0000, 0),	/* #13 Ext TC-layer options */
	A_HIDE(256, 14, 1, OMCI_SRC_CONST, 0x01, 0),	/* #14 ONT state */

	/* ---- ME 257 ONU2-G (inst 0) ---- */
	A_ID(257,  1, equipment_id),		/* #1  Equipment ID */
	A_D(257,  2,  1, OMCI_DYN_OMCC),	/* #2  OMCC version: BASELINE
						 * only -- devid 0x0b is not
						 * served, omci_id_set refuses
						 * an extended code */
	A_ID(257,  3, product_code),		/* #3  Vendor product code */
	A_C(257,  4,  1, 0x01),			/* #4  Security capability */
	A_C(257,  5,  1, 0x01),			/* #5  Security mode */
	A_D(257,  6,  2, OMCI_DYN_ONU2G_PQ),	/* #6  Total priority queues */
	A_D(257,  7,  1, OMCI_DYN_SCHED_N),	/* #7  Total traffic scheds */
	A_C(257,  8,  1, 0x01),			/* #8  Mode */
	A_D(257,  9,  2, OMCI_DYN_GEM_N),	/* #9  Total GEM ports */
	A_C(257, 10,  4, 3600),			/* #10 SysUpTime — UINT32: two
						 * bytes here misaligns every
						 * later attr (proven bug) */
	A_C(257, 11,  2, 0x007f),		/* #11 Connectivity capability */
	A_C(257, 12,  1, 0x00),			/* #12 Current conn mode */
	A_C(257, 13,  2, 0x003b),		/* #13 QoS config flexibility */
	A_C(257, 14,  2, 0x0001),		/* #14 Priority-queue scale */

	/* ---- ME 5 Cardholder, one per declared slot ---- */
	A_D(5, 1, 1, OMCI_DYN_SLOT_TYPE),	/* #1  Actual type */
	A_D(5, 2, 1, OMCI_DYN_SLOT_TYPE),	/* #2  Expected type */
	A_D(5, 3, 1, OMCI_DYN_SLOT_PORTS),	/* #3  Expected port count */
	A_TXT(5, 4, 20, OMCI_TEXT_SPACES),	/* #4  Expected equipment ID */
	A_TXT(5, 5, 20, OMCI_TEXT_SPACES),	/* #5  Actual equipment ID */
	A_CW(5, 8, 1, 0),			/* #8  ARC */
	A_CW(5, 9, 1, 0),			/* #9  ARC interval */

	/* ---- ME 6 Circuit-Pack, one per declared slot ---- */
	A_D(6,  1,  1, OMCI_DYN_SLOT_TYPE),	/* #1  Type */
	A_D(6,  2,  1, OMCI_DYN_SLOT_PORTS),	/* #2  Number of ports */
	A_SN(6, 3),				/* #3  Serial number */
	A_ID(6,  4, sw_bank0_version),		/* #4  Version */
	A_ID(6,  5, vendor_id),			/* #5  Vendor ID */
	A_CW(6,  6,  1, 0),			/* #6  Admin state */
	A_C(6,  7,  1, 0),			/* #7  Op state */
	A_CW(6,  8,  1, 0),			/* #8  Bridged/IP ind */
	A_TXT(6, 9, 20, OMCI_TEXT_SPACES),	/* #9  Equipment ID */
	A_CW(6, 10,  1, 0),			/* #10 Card configuration */
	A_D(6, 11,  1, OMCI_DYN_SLOT_TBUF),	/* #11 Total T-CONT buffers */
	A_D(6, 12,  1, OMCI_DYN_SLOT_PRIQ),	/* #12 Total priority queues */
	A_D(6, 13,  1, OMCI_DYN_SLOT_SCHED),	/* #13 Total traffic schedulers */
	A_CW(6, 14,  4, 0),			/* #14 Power shed override */

	/* ---- ME 7 Software-Image, banks 0 (active) + 1 ---- */
	A_D(7, 1, 14, OMCI_DYN_SW_VER),		/* #1  Version */
	A_D(7, 2,  1, OMCI_DYN_SW_FLAG),	/* #2  Is committed */
	A_D(7, 3,  1, OMCI_DYN_SW_FLAG),	/* #3  Is active */
	A_C(7, 4,  1, 1),			/* #4  Is valid */
	/* ⚠ #5 AND #6 WERE ONE ROW UNTIL 2026-08-31 */
	A_HIDE(7, 5, 25, OMCI_SRC_ID, offsetof(struct omci_identity, zeros), 0),
						/* #5  Product code */
	A_HIDE(7, 6, 16, OMCI_SRC_ID, offsetof(struct omci_identity, zeros), 0),
						/* #6  Image hash */

	/* ---- ME 11 PPTP Ethernet UNI (inst 0x0101) — THE HGU gate ---- */
	A_D(11,  1, 1, OMCI_DYN_UNI_EXPECTED),	/* #1  Expected type */
	A_D(11,  2, 1, OMCI_DYN_UNI_TYPE),	/* #2  Sensed type */
	/* THE WRITABLE SET IS STOCK'S OWN, MEASURED */
	A_CW(11,  3, 1, 0),			/* #3  Auto-detect config */
	A_CW(11,  4, 1, 0),			/* #4  Eth loopback config */
	A_D(11,  5, 1, OMCI_DYN_UNI_ADMIN),	/* #5  Admin state */
	A_C(11,  6, 1, 1),			/* #6  Op state */
	A_C(11,  7, 1, 0),			/* #7  Config ind */
	A_D(11,  8, 2, OMCI_DYN_UNI_MAX_FRAME),	/* #8  Max frame size */
	A_CW(11,  9, 1, 0),			/* #9  DTE/DCE ind */
	A_D(11, 10, 2, OMCI_DYN_UNI_PAUSE),	/* #10 Pause time */
	A_CW(11, 11, 1, 2),			/* #11 Bridged/IP ind */
	A_CW(11, 12, 1, 0),			/* #12 ARC */
	A_CW(11, 13, 1, 0),			/* #13 ARC interval */
	A_CW(11, 14, 1, 0),			/* #14 PPPoE filter */
	A_CW(11, 15, 1, 0),			/* #15 Power control */

	/* ---- ME 53 PPTP POTS UNI, one per declared instance: what stock
	 * reports for a port no phone is wired to ---- */
	A_CW(53,  1, 1, 0),			/* #1  Admin state */
	A_CW(53,  2, 2, 0),			/* #2  Interworking TP pointer */
	A_CW(53,  3, 1, 0),			/* #3  ARC */
	A_CW(53,  4, 1, 0),			/* #4  ARC interval */
	A_CW(53,  5, 1, 0),			/* #5  Impedance */
	A_CW(53,  6, 1, 0),			/* #6  Transmission path */
	A_CW(53,  7, 1, 0),			/* #7  Rx gain */
	A_CW(53,  8, 1, 0),			/* #8  Tx gain */
	A_C(53,  9, 1, 0),			/* #9  Op state */
	A_C(53, 10, 1, 0),			/* #10 Hook state */
	A_CW(53, 11, 2, 0),			/* #11 POTS holdover time */
	A_CW(53, 12, 1, 0),			/* #12 Nominal feed voltage */

	/* ---- single MEs a board declares (extra-me-instances), each as the
	 * field X111W's stock uploads it (production OLT, 2026-10-02) ---- */
	A_CW(83, 1, 1, 0),			/* LCT UNI #1  Admin state */
	A_CW(133,  1, 2, 0),			/* power shedding #1 Restore timer */
	A_CW(133,  2, 2, 0),			/* #2  Data class interval */
	A_CW(133,  3, 2, 0),			/* #3  Voice class interval */
	A_CW(133,  4, 2, 0),			/* #4  Video overlay interval */
	A_CW(133,  5, 2, 0),			/* #5  Video return interval */
	A_CW(133,  6, 2, 0),			/* #6  DSL class interval */
	A_CW(133,  7, 2, 0),			/* #7  ATM class interval */
	A_CW(133,  8, 2, 0),			/* #8  CES class interval */
	A_CW(133,  9, 2, 0),			/* #9  Frame class interval */
	A_CW(133, 10, 2, 0),			/* #10 SDH-SONET interval */
	A_C(133, 11, 2, 0),			/* #11 Shedding status */
	A_CW(138, 1, 1, 1),			/* VoIP config #1 Available protocols */
	A_CW(138, 2, 1, 1),			/* #2  Protocol used */
	A_C(138, 3, 4, 5),			/* #3  Available config methods */
	A_CW(138, 4, 1, 0),			/* #4  Config method used */
	A_CW(138, 5, 2, 0xffff),		/* #5  Config address pointer */
	A_C(138, 6, 1, 0),			/* #6  Config state */
	A_CW(138, 7, 1, 0),			/* #7  Retrieve profile */
	A_TXT(138, 8, 25, OMCI_TEXT_ZERO_DIGIT),	/* #8  Profile version */
	A_C(141, 1, 2, 0),			/* VoIP line status #1 Codec used */
	A_C(141, 2, 1, 0),			/* #2  Voice server status */
	A_C(141, 3, 1, 0),			/* #3  Port session type */
	A_C(141, 4, 2, 0),			/* #4  Call 1 packet period */
	A_C(141, 5, 2, 0),			/* #5  Call 2 packet period */
	A_TXT(141, 6, 25, OMCI_TEXT_ZERO_DIGIT),	/* #6  Call 1 destination */
	A_TXT(141, 7, 25, OMCI_TEXT_ZERO_DIGIT),	/* #7  Call 2 destination */
	A_C(141, 8, 1, 0),			/* #8  Line state */
	A_C(141, 9, 1, 0),			/* #9  Emergency call status */
	A_CW(340, 1, 1, 1),			/* TR-069 server #1 Admin state */
	A_CW(340, 2, 2, 0xffff),		/* #2  ACS address pointer */
	A_CW(340, 3, 2, 0xffff),		/* #3  Associated tag */

	/* ---- ME 134 IP host config data, one per declared instance.  The
	 * current-address group (#9..#13) reads 0: stock mirrors the live WAN
	 * lease there, which this model does not know. ---- */
	A_CW(134,  1, 1, 0),			/* #1  IP options */
	A_D(134,  2, 6, OMCI_DYN_IP_HOST_MAC),	/* #2  MAC address */
	A_ZERO(134, 3, 25),			/* #3  ONU identifier */
	A_CW(134,  4, 4, 0),			/* #4  IP address */
	A_CW(134,  5, 4, 0),			/* #5  Mask */
	A_CW(134,  6, 4, 0),			/* #6  Gateway */
	A_CW(134,  7, 4, 0),			/* #7  Primary DNS */
	A_CW(134,  8, 4, 0),			/* #8  Secondary DNS */
	A_C(134,  9, 4, 0),			/* #9  Current address */
	A_C(134, 10, 4, 0),			/* #10 Current mask */
	A_C(134, 11, 4, 0),			/* #11 Current gateway */
	A_C(134, 12, 4, 0),			/* #12 Current primary DNS */
	A_C(134, 13, 4, 0),			/* #13 Current secondary DNS */
	A_ZERO(134, 14, 25),			/* #14 Domain name */
	A_ZERO(134, 15, 25),			/* #15 Host name */
	A_HIDE(134, 16, 2, OMCI_SRC_CONST, 0, OMCI_ACCESS_WRITE_UNCHANGED),
						/* #16 Relay agent options */

	/* ============ THE WAN SERVICE SPINE (G.988 clause 9.3) ============
	 * Every row's WIDTH and ACCESS below is EXTRACTED from the board's own
	 * stock plugin, never read off a spec from memory -- stock's per-ME
	 * shared object registers its attribute table and the `olt` field IS
	 * this `access` field (read 1 | write 2 | set-by-create 4), which the
	 * ME 268 rows further down already agree with field for field:
	 *   dev/re-tools/venv/bin/python3 dev/OMCI-simulate/me_attr_table.py \
	 *       --so <stock rootfs>/lib/omci/mib_<Name>.so
	 * The stock attribute NAME is quoted beside each row so a reader can
	 * re-run that command and check the row against its source.
	 *
	 * ⚠ THE DENSE OFFSETS ARE THE ONLY PLACE THEY ARE SPELLED.  Nothing
	 *   else in this tree reads these bodies by literal offset -- the
	 *   service resolver at the end of this file goes through the table --
	 *   so a width correction moves every reader with it.
	 *
	 * ---- ME 45 MAC bridge service profile (mib_MacBriServProf) ----
	 * The bridge instance every bridge port hangs off. */
	A_ST(45,  1, 1,  0, 7),		/* #1  spanning tree ind   SpanTreeInd */
	A_ST(45,  2, 1,  1, 7),		/* #2  learning ind        LearningInd */
	A_ST(45,  3, 1,  2, 7),		/* #3  port bridging ind   AtmBriInd */
	A_ST(45,  4, 2,  3, 7),		/* #4  priority            Priority */
	A_ST(45,  5, 2,  5, 7),		/* #5  max age             MaxAge */
	A_ST(45,  6, 2,  7, 7),		/* #6  hello time          HelloTime */
	A_ST(45,  7, 2,  9, 7),		/* #7  forward delay       ForwardDelay */
	A_ST(45,  8, 1, 11, 7),		/* #8  unknown MAC discard DiscardUnknow */
	A_ST(45,  9, 1, 12, 7),		/* #9  MAC learning depth  MacLearningDepth */
	A_ST(45, 10, 4, 13, 7),		/* #10 ageing time         DynamicFilteringAgeingTime */

	/* ME 47 MAC bridge port configuration */
	A_ST(47,  1, 2,  0, 7),		/* #1  bridge ID pointer   BridgeIdPtr */
	A_ST(47,  2, 1,  2, 7),		/* #2  port number         PortNum */
	A_ST(47,  3, 1,  3, 7),		/* #3  TP type             TPType */
	A_ST(47,  4, 2,  4, 7),		/* #4  TP pointer          TPPointer */
	A_ST(47,  5, 2,  6, 7),		/* #5  port priority       PortPriority */
	A_ST(47,  6, 2,  8, 7),		/* #6  port path cost      PortPathCost */
	A_ST(47,  7, 1, 10, 7),		/* #7  port spanning tree  PortSpanTreeInd */
	A_ST(47,  8, 1, 11, 7),		/* #8  deprecated          EncapMethod */
	A_ST(47,  9, 1, 12, 7),		/* #9  deprecated          LanFCSInd */
	A_ST(47, 10, 6, 13, 1),		/* #10 port MAC address    PortMacAddr (R) */
	A_ST(47, 11, 2, 19, 3),		/* #11 outbound TD ptr     OutboundTD */
	A_ST(47, 12, 2, 21, 3),		/* #12 inbound TD ptr      InboundTD */
	A_ST(47, 13, 1, 23, 7),		/* #13 MAC learning depth  NumOfAllowedMac */

	/* ME 49 MAC bridge port filter table */
	A_TBL(49, 1, GPON_MAC_FILTER_ROW_LEN, OMCI_TBL_MAC_FILTER_ROW, 3),
					/* #1 MAC filter table MACFilterTable ---- ME 50 MAC */
	A_NO_ATTRS(50),

	/* ME 52 MAC bridge port PM history */
	A_ST(52, 1, 1,  0, 1),		/* #1 interval end time    IntervalEndTime (R) */
	A_ST(52, 2, 2,  1, 7),		/* #2 threshold data 1/2   ThresholdData12Id */
	/* #3..#7 ARE COUNTERS AND NOTHING COUNTS. */
	A_CNT(52, 3, 4,  3),		/* #3 forwarded frames     ForwardedFrameCounter */
	A_CNT(52, 4, 4,  7),		/* #4 delay exceeded disc  DelayExceededDiscard */
	A_CNT(52, 5, 4, 11),		/* #5 MTU exceeded disc    MtuExceededDiscard */
	A_CNT(52, 6, 4, 15),		/* #6 received frames      ReceivedFrameCounter */
	A_CNT(52, 7, 4, 19),		/* #7 received+discarded   ReceivedAndDiscardedCounter */

	/* ME 78 VLAN tagging operation configuration */
	A_ST(78, 1, 1, 0, 7),		/* #1  upstream op mode    UsTagOpMode */
	A_ST(78, 2, 2, 1, 7),		/* #2  upstream TCI        UsTagTci */
	A_ST(78, 3, 1, 3, 7),		/* #3  downstream op mode  DsTagOpMode */
	A_ST(78, 4, 1, 4, 7),		/* #4  association type    Type */
	A_ST(78, 5, 2, 5, 7),		/* #5  associated ME ptr   Pointer */

	/* ME 79 MAC bridge port filter pre-assign */
	A_ST(79,  1, 1, 0, 3),		/* #1  IPv4 multicast      IPv4McastFilter */
	A_ST(79,  2, 1, 1, 3),		/* #2  IPv6 multicast      IPv6McastFilter */
	A_ST(79,  3, 1, 2, 3),		/* #3  IPv4 broadcast      IPv4BcastFilter */
	A_ST(79,  4, 1, 3, 3),		/* #4  RARP                RARPFilter */
	A_ST(79,  5, 1, 4, 3),		/* #5  IPX                 IPXFilter */
	A_ST(79,  6, 1, 5, 3),		/* #6  NetBEUI             NetBEUIFilter */
	A_ST(79,  7, 1, 6, 3),		/* #7  AppleTalk           AppleTalkFilter */
	A_ST(79,  8, 1, 7, 3),		/* #8  bridge management   BridgeManaInfofilter */
	A_ST(79,  9, 1, 8, 3),		/* #9  ARP                 ARPFilter */
	A_ST(79, 10, 1, 9, 3),		/* #10 PPPoE broadcast     PPPoeBcastFilter */

	/* ME 84 VLAN tagging filter data (mib_VlanTagFilterData) */
	A_ST(84, 1, 24,  0, 7),		/* #1  VLAN filter list    FilterTbl */
	A_ST(84, 2,  1, 24, 7),		/* #2  forward operation   FwdOp */
	A_ST(84, 3,  1, 25, 7),		/* #3  number of entries   NumOfEntries */

	/* ME 130 802.1p mapper service profile */
	A_ST(130,  1, 2,  0, 7),	/* #1  TP pointer          TPPtr */
	A_ST(130,  2, 2,  2, 7),	/* #2  P-bit 0 -> IW TP    IwTpPtrPbit0 */
	A_ST(130,  3, 2,  4, 7),	/* #3  P-bit 1             IwTpPtrPbit1 */
	A_ST(130,  4, 2,  6, 7),	/* #4  P-bit 2             IwTpPtrPbit2 */
	A_ST(130,  5, 2,  8, 7),	/* #5  P-bit 3             IwTpPtrPbit3 */
	A_ST(130,  6, 2, 10, 7),	/* #6  P-bit 4             IwTpPtrPbit4 */
	A_ST(130,  7, 2, 12, 7),	/* #7  P-bit 5             IwTpPtrPbit5 */
	A_ST(130,  8, 2, 14, 7),	/* #8  P-bit 6             IwTpPtrPbit6 */
	A_ST(130,  9, 2, 16, 7),	/* #9  P-bit 7             IwTpPtrPbit7 */
	A_ST(130, 10, 1, 18, 7),	/* #10 unmarked frame opt  UnmarkFrmOpt */
	A_TBL(130, 11, 24, OMCI_TBL_HELD, 3),
					/* #11 DSCP -> P-bit map   DscpMap2Pbit */
	A_ST(130, 12, 1, 19, 7),	/* #12 default P-bit mark  DefPbitMark */
	A_ST(130, 13, 1, 20, 7),	/* #13 TP type             TPType */

	/* ME 171 extended VLAN tagging operation configuration */
	A_ST(171, 1, 1, 0, 7),		/* #1  association type    AssociationType */
	A_C(171, 2, 2, GPON_EXT_VLAN_ROWS),
					/* #2  table max size (R)  our capacity */
	A_ST(171, 3, 2, 1, 3),		/* #3  input TPID          InputTPID */
	A_ST(171, 4, 2, 3, 3),		/* #4  output TPID         OutputTPID */
	A_ST(171, 5, 1, 5, 3),		/* #5  downstream mode     DsMode */
	A_TBL(171, 6, GPON_EXT_VLAN_ROW_LEN, OMCI_TBL_EXT_VLAN_ROW, 3),
					/* #6  the ROW TABLE       RxFrameVlanTagOperTable */
	A_ST(171, 7, 2, 6, 7),		/* #7  associated ME ptr   AssociatedMePoint */
	A_TBL(171, 8, 24, OMCI_TBL_HELD, 3),
					/* #8  DSCP -> P-bit map   DscpToPbitMapping */

	/* ---- ME 131 OLT-G: what the OLT writes, stock's values until it does */
	A_OLTG(1, 0, 4),			/* #1  OLT vendor ID */
	A_OLTG(2, 4, 20),			/* #2  Equipment ID */
	A_OLTG(3, 24, 14),			/* #3  Version */
	A_OLTG(4, 38, 14),			/* #4  Time of day */

	/* ---- ME 262 T-CONT (inst 0x8000..0x800b) ---- */
	A_D(262, 1, 2, OMCI_DYN_TCONT_ALLOC),	/* #1  Alloc-ID */
	A_C(262, 2, 1, 1),			/* #2  Mode indicator */
	A_C(262, 3, 1, 0),			/* #3  Policy */

	/* ---- ME 263 ANI-G (inst 0x8001) ---- */
	A_C(263,  1, 1, 1),			/* #1  SR indication */
	A_D(263,  2, 2, OMCI_DYN_TCONT_N),			/* #2  Total T-CONTs */
	A_CW(263,  3, 2, 48),			/* #3  GEM block length */
	A_CW(263,  4, 1, 0),			/* #4  Piggyback DBA */
	A_C(263,  5, 1, 0),			/* #5  (deprecated) */
	A_CW(263,  6, 1, 5),			/* #6  SF threshold */
	A_CW(263,  7, 1, 9),			/* #7  SD threshold */
	A_CW(263,  8, 1, 0),			/* #8  ARC */
	A_CW(263,  9, 1, 0),			/* #9  ARC interval */
	/* #10/#14 are the LIVE optical levels, sampled from the optic's
	 * SFF-8472 A2h diagnostics by the shell (see omci_onu_set_optical);
	 * until the first successful read — and after a failed one — they serve
	 * OMCI_ANIG_{RX,TX}_FALLBACK, because the OLT must never get silence. */
	A_D(263, 10, 2, OMCI_DYN_ANIG_RX),	/* #10 RX optical level */
	{ 263, 0, 11, 1, OMCI_SRC_ANIG_THRESHOLD, 3 },
	{ 263, 1, 12, 1, OMCI_SRC_ANIG_THRESHOLD, 3 },
	A_HIDE(263, 13, 2, OMCI_SRC_CONST, 0x0000, 0),	/* #13 ONU response time */
	A_D(263, 14, 2, OMCI_DYN_ANIG_TX),	/* #14 TX optical level */
	{ 263, 2, 15, 1, OMCI_SRC_ANIG_THRESHOLD, 3 },
	{ 263, 3, 16, 1, OMCI_SRC_ANIG_THRESHOLD, 3 },

	/* ---- ME 264 UNI-G (inst 0x0101) ---- */
	A_C(264, 1, 2, 0x0000),			/* #1  Config-option status */
	A_D(264, 2, 1, OMCI_DYN_UNIG_ADMIN),	/* #2  Admin state */
	A_D(264, 3, 1, OMCI_DYN_UNIG_CAP),	/* #3  Management capability */
	A_C(264, 4, 2, 0x0000),			/* #4  Non-OMCI mgmt ID */
	A_C(264, 5, 2, 0x0000),			/* #5  Relay-agent options */

	/* ME 266 GEM interworking termination point */
	A_ST(266, 1, 2,  0, 7),		/* #1 GEM port CTP pointer GemCtpPtr */
	A_ST(266, 2, 1,  2, 7),		/* #2 interworking option  IwOpt */
	A_ST(266, 3, 2,  3, 7),		/* #3 service profile ptr  ServProPtr */
	A_ST(266, 4, 2,  5, 7),		/* #4 interworking TP ptr  IwTpPtr */
	A_ST(266, 5, 1,  7, 1),		/* #5 PPTP counter         PptpCounter (R) */
	A_ST(266, 6, 1,  8, 1),		/* #6 operational state    OpState (R) */
	A_ST(266, 7, 2,  9, 7),		/* #7 GAL profile pointer  GalProfPtr */
	A_ST(266, 8, 1, 11, 3),		/* #8 GAL loopback config  GalLoopbackCfg */

	/* ME 268 GEM-port network CTP. Dense values differ from Create after
	 * attr 5: read-only #6 is absent from SBC, so #7 moves from 9 to 10.
	 * Width/access facts: own X111W mib_GemPortCtp.so mibTable_init.
	 * The full 16-byte value fits one baseline Get or Upload row. */
	A_ST(268,  1, 2,  0, 7),
	A_ST(268,  2, 2,  2, 7),
	A_ST(268,  3, 1,  4, 7),
	A_ST(268,  4, 2,  5, 7),
	A_ST(268,  5, 2,  7, 7),
	A_ST(268,  6, 1,  9, 1),
	A_ST(268,  7, 2, 10, 7),
	A_ST(268,  8, 1, 12, 1),
	A_ST(268,  9, 2, 13, 3),
	A_ST(268, 10, 1, 15, 3),

	/* ---- ME 272 GAL Ethernet profile (mib_GalEthProf) ----
	 * 266's companion, and one attribute wide: the maximum GEM payload the
	 * interworking point may emit. */
	A_ST(272, 1, 2, 0, 7),		/* #1 max GEM payload size MaxGemPayloadSize */

	/* ---- ME 277 Priority-Queue (inst 0..7) ---- */
	A_C(277, 1, 1, 1),			/* #1  Queue config option */
	A_C(277, 2, 2, 3276),			/* #2  Max queue size */
	A_C(277, 3, 2, 3276),			/* #3  Allocated queue size */
	A_C(277, 4, 2, 0),			/* #4  Discard reset interval */
	A_C(277, 5, 2, 0),			/* #5  Threshold value */
	A_D(277, 6, 4, OMCI_DYN_PQ_PORT),	/* #6  Related port */
	A_C(277, 7, 2, 0x0000),			/* #7  Traffic-sched pointer */
	A_C(277, 8, 1, 1),			/* #8  Weight */
	/* #9..#16 as stock's own /etc/omci_mib.cfg and its upload give them */
	A_C(277,  9, 2, 0),			/* #9  Back pressure operation */
	A_C(277, 10, 4, 0),			/* #10 Back pressure time */
	A_C(277, 11, 2, 0),			/* #11 Back pressure occur threshold */
	A_C(277, 12, 2, 0),			/* #12 Back pressure clear threshold */
	A_ZERO(277, 13, 8),			/* #13 Packet drop thresholds */
	A_C(277, 14, 2, 0x00ff),		/* #14 Packet drop max_p */
	A_C(277, 15, 1, 9),			/* #15 Queue drop w_q */
	A_C(277, 16, 1, 0),			/* #16 Drop precedence colour */

	/* ---- ME 278 Traffic-Scheduler (inst 0x8000..0x800b) ---- */
	A_D(278, 1, 2, OMCI_DYN_TS_TCONT),	/* #1  T-CONT pointer */
	A_C(278, 2, 2, 0x0000),			/* #2  Traffic-sched pointer */
	A_C(278, 3, 1, 2),			/* #3  Policy: WRR, as stock's
						 * omci_app creates every one */
	A_C(278, 4, 1, 0),			/* #4  Priority/weight */

	/* ME 280 GEM traffic descriptor */
	A_ST(280, 1, 4,  0, 7),		/* #1  CIR                 CIR */
	A_ST(280, 2, 4,  4, 7),		/* #2  PIR                 PIR */
	A_ST(280, 3, 4,  8, 7),		/* #3  CBS                 CBS */
	A_ST(280, 4, 4, 12, 7),		/* #4  PBS                 PBS */
	A_ST(280, 5, 1, 16, 7),		/* #5  colour mode         ColourMode */
	A_ST(280, 6, 1, 17, 7),		/* #6  ingress colour mark IngressColourMarking */
	A_ST(280, 7, 1, 18, 7),		/* #7  egress colour mark  EgressColourMarking */
	A_ST(280, 8, 1, 19, 7),		/* #8  meter type          MeterType */

	/* ME 281 multicast GEM interworking termination */
	A_ST(281, 1, 2, 0, 7),		/* #1  GEM CTP pointer     GemCtpPtr */
	A_ST(281, 2, 1, 2, 7),		/* #2  interworking option IwOpt */
	A_ST(281, 3, 2, 3, 7),		/* #3  service profile ptr ServProPtr */
	A_ST(281, 4, 2, 5, 7),		/* #4  IW TP pointer       IwTpPtr */
	A_C(281, 5, 1, 0),		/* #5  PPTP counter (R)    PptpCounter */
	A_C(281, 6, 1, 0),		/* #6  operational state   OpState (R) */
	A_ST(281, 7, 2, 7, 7),		/* #7  GAL profile pointer GalProfPtr */
	A_ST(281, 8, 1, 9, 7),		/* #8  GAL loopback config GalLoopbackCfg */
	A_TBL(281,  9, 12, OMCI_TBL_HELD, 3),
					/* #9  IPv4 multicast addr IPv4MCastAddrTable */
	A_TBL(281, 10, 24, OMCI_TBL_HELD, 3),
					/* #10 IPv6 multicast addr IPv6MCastAddrTable */

	/* ---- ME 329 VEIP (inst 0x0601) — the HGU marker ---- */
	A_C(329, 1, 1, 0),			/* #1  Admin state */
	A_C(329, 2, 1, 0),			/* #2  Op state */
	/* ⚠ THE SAME OFF-BY-ONE, and on the ME this port's WAN */
	A_ZERO(329, 3, 25),			/* #3  Interdomain name */
	A_C(329, 4,  2, 0x0000),		/* #4  TCP/UDP pointer */
	A_C(329, 5,  2, 0x0000),		/* #5  IANA assigned port */

	/* ==== VENDOR MEs a board declares (extra-me-instances), as the field
	 * X111W's stock uploads them (production OLT, 2026-10-02).  Widths are
	 * its own plugins' (OMCI-simulate/me_attr_table.py on M225-260618; ME
	 * 65294 from the G24W and X100DG stocks, whose X111W plugin has no
	 * table to decode); values are the plugins' defaults.  The subscriber's
	 * WiFi names and keys are NOT served here: zeros. ==== */
#ifdef CONFIG_OLT_ZTE
	/* ---- ME 247 extended ONU-G (mib_ExtendedOnuGZTE) ---- */
	A_C(247, 1, 2, 0),
	A_C(247, 2, 2, 0),
	A_IDN(247, 3, 14, equipment_id),	/* #3  Version: the model name */
	A_C(247, 4, 2, 0),
	A_C(247, 5, 2, 0),
	A_C(247, 6, 2, 0),
	A_C(247, 7, 2, 0),
	A_C(247, 8, 2, 0),
	A_C(247, 9, 2, 0),
	A_C(247, 10, 2, 0),
	A_C(247, 11, 2, 0),
	A_C(247, 12, 2, 0),
	A_C(247, 13, 2, 0),
	A_C(247, 14, 2, 0),
	A_C(247, 15, 2, 0),
	A_C(247, 16, 1, 0),			/* #16 Reset default */
#endif
	/* ---- ME 252 WLAN profile (mib_WlanCfgProfile) ---- */
	A_C(252, 1, 1, 0),			/* #1  WLAN type */
	A_C(252, 2, 1, 1),			/* #2  2.4 GHz admin */
	A_C(252, 3, 1, 6),			/* #3  2.4 GHz encryption */
	A_ZERO(252, 4, 24),			/* #4  2.4 GHz SSID: not served */
	A_ZERO(252, 5, 24),			/* #5  2.4 GHz key: never served */
	A_C(252, 6, 1, 0),			/* #6  5 GHz admin */
	A_C(252, 7, 1, 0),			/* #7  5 GHz encryption */
	A_TXT(252, 8, 24, OMCI_TEXT_ZERO_DIGIT),	/* #8  5 GHz SSID */
	A_TXT(252, 9, 24, OMCI_TEXT_ZERO_DIGIT),	/* #9  5 GHz key */
	A_C(252, 10, 1, 0),			/* #10 Commit */
	/* ---- ME 253 loop detection, per Ethernet UNI (mib_LoopDetect) ---- */
	A_CW(253, 1, 1, 0),			/* #1  Admin state */
	A_CW(253, 2, 1, 0),			/* #2  ARC */
	A_CW(253, 3, 1, 0),			/* #3  ARC interval */
	A_CW(253, 4, 1, 0),			/* #4  Loop detection config */
	/* ---- ME 350 (mib_Me350) ---- */
	A_C(350, 1, 1, 0),			/* #1  Flow mapping mode */
	A_C(350, 2, 1, 2),			/* #2  Traffic management */
	A_C(350, 3, 1, 1),			/* #3  Flow CAR */
	A_C(350, 4, 1, 2),			/* #4  ONT transparent */
	A_C(350, 5, 2, 0),			/* #5  Current MAC number */
	A_C(350, 6, 4, 0),			/* #6  MAC age time */
	A_C(350, 7, 2, 0x0200),
	A_C(350, 8, 2, 0),
	A_C(350, 9, 2, 0),
	A_C(350, 10, 2, 0),
	A_C(350, 11, 2, 0),
	A_C(350, 12, 2, 0),
	A_C(350, 13, 4, 0),
	A_TXT(350, 14, 4, OMCI_TEXT_P2Q_CAP),	/* #14 P2Q capability */
	A_ZERO(350, 15, 6),			/* #15 P2Q map */
	A_TXT(350, 16, 4, OMCI_TEXT_SPACES),	/* #16 four spaces */
	/* ---- ME 373 (mib_Me373) ---- */
	A_C(373, 1, 1, 1),
	A_C(373, 2, 1, 1),
	A_C(373, 3, 1, 0),
	A_C(373, 4, 1, 0),
	A_C(373, 5, 1, 1),
	A_C(373, 6, 1, 1),
	A_C(373, 7, 1, 1),
	A_C(373, 8, 1, 1),
	A_C(373, 9, 2, 0xff80),
	A_C(373, 10, 2, 0x8000),
	A_C(373, 11, 2, 0xe000),
	A_C(373, 12, 2, 0xc000),
	A_C(373, 13, 1, 1),
#ifdef CONFIG_OLT_ZTE
	/* ---- ME 65294 SNTP (mib_MeZteSntp) ---- */
	A_CW(65294, 1, 2, 0),			/* #1  Time zone */
	A_CW(65294, 2, 4, 0),			/* #2  Master server */
	A_CW(65294, 3, 4, 0),			/* #3  Slave server */
	A_CW(65294, 4, 4, 0),			/* #4  Interval */
	A_CW(65294, 5, 1, 0),
	A_CW(65294, 6, 1, 0),
#endif
	/* ---- ME 65352 ONU support (mib_hsg_onu_support) ---- */
	A_ZERO(65352, 1, 16),
	/* ---- ME 65363 ONU (mib_FHOnu) ---- */
	A_C(65363, 1, 1, 0x5e),			/* #1  OMCI flag */
	A_TXT(65363, 2, 12, OMCI_TEXT_TWO),	/* #2  Configuration version */
	A_C(65363, 3, 2, 0),			/* #3  OLT number */
	A_C(65363, 4, 2, 0),			/* #4  PON number */
	A_C(65363, 5, 2, 0),			/* #5  ONU number */
	A_D(65363, 6, 6, OMCI_DYN_WAN_MAC),	/* #6  ONU MAC */
	A_C(65363, 7, 2, 0),			/* #7  Sub-version */
	A_C(65363, 8, 1, 0x5e),			/* #8  VoIP mode */
	A_C(65363, 9, 1, 0),			/* #9  T-CONT mode */
	A_C(65363, 10, 1, 0),			/* #10 Upgrade mode */
	A_ID(65363, 11, equipment_id),		/* #11 Equipment ID */
	A_C(65363, 12, 4, 0),			/* #12 Special ability */
	A_ZERO(65363, 13, 24),			/* #13 Version info: stock's own build */
	A_C(65363, 14, 2, 0),			/* #14 Telnet */
	/* ---- ME 65385 WLAN common, per SSID (mib_WlanCommonCfg) ---- */
	A_C(65385, 1, 1, 4),			/* #1  Mode */
	A_C(65385, 2, 1, 0),			/* #2  Channel */
	A_ZERO(65385, 3, 24),			/* #3  SSID: not served */
	A_C(65385, 4, 1, 1),			/* #4  Bandwidth */
	A_C(65385, 5, 1, 0),			/* #5  Rate */
	A_C(65385, 6, 2, 1),			/* #6  Guard interval */
	A_C(65385, 7, 2, 100),			/* #7  Beacon interval */
	A_C(65385, 8, 2, 1),			/* #8  DTIM interval */
	A_C(65385, 9, 1, 0),			/* #9  AP isolation */
	A_C(65385, 10, 1, 0),			/* #10 Hidden SSID */
	A_C(65385, 11, 1, 1),			/* #11 SSID enable */
	A_INST0(65385, 12, 1, 0),		/* #12 Enable: the first SSID */
	/* ---- ME 65386 WLAN security, per SSID (mib_WlanSecurityCfg) ---- */
	A_INST0(65386, 1, 4, 0),		/* #1  Security mode */
	A_ZERO(65386, 2, 24),			/* #2  Shared key: never served */
	A_C(65386, 3, 1, 2),			/* #3  WEP authentication */
	A_C(65386, 4, 1, 0),			/* #4  WEP key bits */
	A_C(65386, 5, 1, 0),			/* #5  Key index */
	A_TXT(65386, 6, 26, OMCI_TEXT_WEP_KEY),	/* #6..#9 WEP keys */
	A_TXT(65386, 7, 26, OMCI_TEXT_WEP_KEY),
	A_TXT(65386, 8, 26, OMCI_TEXT_WEP_KEY),
	A_TXT(65386, 9, 26, OMCI_TEXT_WEP_KEY),
	A_INST0(65386, 10, 2, 1),		/* #10 WPA encryption */
	/* ---- ME 65408 extended ONU-G (mib_ExtendedOnuG) ---- */
	A_C(65408, 1, 1, 0),			/* #1  Reset default */
	A_C(65408, 2, 1, 1),
	A_C(65408, 3, 1, 0x3d),
	A_C(65408, 4, 1, 3),
	A_C(65408, 5, 2, 0x3800),
	A_C(65408, 6, 4, 0),
	A_TXT(65408, 7, 16, OMCI_TEXT_EXT_ONU_G7),
	A_C(65408, 8, 1, 0),
	A_TXT(65408, 9, 24, OMCI_TEXT_EXT_ONU_G9),
	A_TXT(65408, 10, 16, OMCI_TEXT_EXT_ONU_G10),
	A_C(65408, 11, 1, 0),
	A_TXT(65408, 12, 25, OMCI_TEXT_EXT_ONU_G12),
	A_C(65408, 13, 1, 0),
	A_C(65408, 14, 1, 0),
	A_C(65408, 15, 4, 0x13),
	A_C(65408, 16, 4, 0),
	/* ---- ME 65417 MAC per PPTP (mib_mac_pptp) ---- */
	A_C(65417, 1, 1, 0),
	A_ZERO(65417, 2, 25),
	A_ZERO(65417, 3, 25),
	A_ZERO(65417, 4, 25),
	A_ZERO(65417, 5, 25),
	A_ZERO(65417, 6, 25),
	A_ZERO(65417, 7, 25),
	A_C(65417, 8, 1, 0),
	/* ---- ME 65427 proprietary capability (mib_HwProprietaryCapabilityDeclare) */
	A_C(65427, 1, 2, 0xffff),
	A_TXT(65427, 2, 16, OMCI_TEXT_HW_CAP),
	A_C(65427, 3, 1, 0x90),
	A_C(65427, 4, 1, 0x6f),

	/* ---- ME 65529 CTC ONU capability (inst 0, mib_OnuCapability): the
	 * HSGQ OLT takes the ISP ONU type from #3 before it counts VEIPs ---- */
	A_TXT(65529, 1, 4, OMCI_TEXT_CTC_OP),	/* #1  Operation ID */
	A_D(65529, 2, 1, OMCI_DYN_CTC),		/* #2  CTC spec version */
	A_D(65529, 3, 1, OMCI_DYN_CTC),		/* #3  ONU type: 1 = HGU */
	A_D(65529, 4, 1, OMCI_DYN_CTC),		/* #4  TX power supply control */
	A_HIDE(65529, 5, 1, OMCI_SRC_DYN, OMCI_DYN_CTC, 1),	/* #5  customized type, Get only */

	/* ---- ME 65530 CTC LoID authentication (inst 0) ---- */
	A_ID(65530, 1, operator_id),		/* #1  Operation ID */
	A_ID(65530, 2, loid),			/* #2  LoID */
	A_ID(65530, 3, loid_passwd),		/* #3  Password (MUST be
						 * servable, proven) */
	A_C(65530, 4,  1, 0x01),		/* #4  Auth status = success */

	{ 0, 0, 0, 0, 0, 0 },			/* terminator (class 0 is not a
						 * G.988 class ID) */
};

/* Is @class_id in a G.988 vendor-reserved range */
static bool omci_vendor_class(u16 class_id)
{
	return (class_id >= 240 && class_id <= 255) ||
	       (class_id >= 350 && class_id <= 399) ||
	       class_id >= 65280;
}

/* First descriptor row of @class_id, or NULL if the model does not carry it. */
static const struct omci_attr *omci_me_find(u16 class_id)
{
	const struct omci_attr *a;

	for (a = omci_attrs; a->class_id; a++)
		if (a->class_id == class_id)
			return a;
	return NULL;
}

/* Which slot of @inv holds @inst, or -1 when this board has no such instance.
 * The comparison is on the FULL u16: the G24W's fourth PPTP Ethernet UNI is
 * 0x0401 and its first is 0x0101, so a low-byte port index matches both. */
int omci_uni_slot(const struct omci_uni_inv *inv, u16 inst)
{
	u8 i;

	for (i = 0; i < inv->n; i++)
		if (inv->inst[i] == inst)
			return i;
	return -1;
}

/* The reported administrative state of @inst.  An instance this board does not
 * have reads 0 -- G.988 unlocked, and what the model answered for every
 * instance but one before the inventory existed. */
static u8 omci_uni_admin(const struct omci_uni_inv *inv, u16 inst)
{
	int slot = omci_uni_slot(inv, inst);

	return slot < 0 ? 0 : inv->admin[slot];
}

/* Record an administrative state the model has accepted, flagging the slot
 * only when the state actually MOVED: a re-Set of the value already held must
 * not make the family touch a port that is already where the OLT wants it. */
static void omci_uni_accept(struct omci_uni_inv *inv, int slot, u8 admin)
{
	if (inv->admin[slot] == admin)
		return;
	inv->admin[slot] = admin;
	inv->changed |= (u8)(1u << slot);
}

u8 omci_uni_take_changed(struct omci_uni_inv *inv)
{
	u8 changed = inv->changed;

	inv->changed = 0;
	return changed;
}

void omci_uni_mark_changed(struct omci_uni_inv *inv, u8 slot)
{
	if (slot < inv->n)
		inv->changed |= (u8)(1u << slot);
}

bool omci_uni_apply_run(const struct omci_uni_apply_ops *ops, void *sh,
			u8 changed, u8 n, const u8 *admin)
{
	bool retry = false;
	u8 i;

	if (!ops || !ops->apply || !ops->rearm || !admin)
		return false;
	if (n > OMCI_UNI_MAX)
		n = OMCI_UNI_MAX;
	for (i = 0; i < n; i++) {
		enum omci_uni_apply_rc rc;

		if (!(changed & (u8)(1u << i)))
			continue;
		rc = ops->apply(sh, i, admin[i] != 0);
		if (rc == OMCI_UNI_APPLIED)
			continue;
		/* NOT APPLIED ⇒ STILL OWED, both ways.  take_changed() has
		 * already cleared the word, so anything not put back here is
		 * gone: no later Set of the same value re-arms it and a
		 * MIB-Reset only covers the unlock direction. */
		ops->rearm(sh, i);
		if (rc != OMCI_UNI_NO_PORT)
			retry = true;
	}
	return retry;
}

/* A MIB-Reset returns every UNI to unlocked.  ⚠ AND FLAGS THE ONES THAT WERE
 * LOCKED: a port the OLT held down and then reset the MIB out from under must
 * come back up, and the changed word is the only thing that says so. */
/* ME 11 #1/#8/#10 back to what the ONU reports before any OLT wrote them. */
static void omci_uni_defaults(struct omci_onu *o)
{
	u8 i;

	for (i = 0; i < OMCI_UNI_MAX; i++) {
		o->uni_expected[i] = o->uni_type[i];
		o->uni_max_frame[i] = OMCI_UNI_MAX_FRAME_DEFAULT;
		o->uni_pause[i] = OMCI_UNI_PAUSE_DEFAULT;
	}
}

static void omci_uni_reset(struct omci_uni_inv *inv)
{
	u8 i;

	for (i = 0; i < inv->n; i++) {
		if (!inv->admin[i])
			continue;
		inv->admin[i] = 0;
		inv->changed |= (u8)(1u << i);
	}
}

/* One list's declaration rules.  A duplicate or a zero instance id would put
 * the MIB and the OLT's copy of it permanently out of step, so the whole
 * declaration is judged before any of it is installed. */
static bool omci_uni_list_ok(const u16 *inst, u8 n)
{
	u8 i, j;

	if (n > OMCI_UNI_MAX || (n && !inst))
		return false;
	for (i = 0; i < n; i++) {
		if (!inst[i])
			return false;
		for (j = 0; j < i; j++)
			if (inst[j] == inst[i])
				return false;
	}
	return true;
}

static void omci_uni_install(struct omci_uni_inv *inv, const u16 *inst, u8 n)
{
	u8 i;

	memset(inv, 0, sizeof(*inv));
	for (i = 0; i < n; i++)
		inv->inst[i] = inst[i];
	inv->n = n;
}

/* Does a Get validate the instance?  Every mutable class, and the classes
 * whose instances a board declares: equipment, queues, IP hosts, POTS. */
bool omci_get_checks_inst(u16 class_id)
{
	return omci_me_mutable(class_id) || class_id == OMCI_ME_CARDHOLDER ||
	       class_id == OMCI_ME_CIRCUIT_PACK ||
	       class_id == OMCI_ME_PRIORITY_QUEUE ||
	       class_id == OMCI_ME_IP_HOST ||
	       class_id == OMCI_ME_PPTP_POTS_UNI ||
	       /* a board declares these one by one, so another board has none */
	       class_id == OMCI_ME_LCT_UNI || class_id == OMCI_ME_POWER_SHED ||
	       class_id == OMCI_ME_VOIP_CONFIG ||
	       class_id == OMCI_ME_VOIP_LINE_STATUS ||
	       class_id == OMCI_ME_TR069_SERVER;
}

/* Only these classes have a verified mutable layout in this model. Other
 * classes retain their existing compatibility handling in the message layer. */
bool omci_me_mutable(u16 class_id)
{
	switch (class_id) {
	case OMCI_ME_ANI_G:
	case OMCI_ME_OLT_G:
	case OMCI_ME_GEM_CTP:
	case OMCI_ME_PPTP_ETH_UNI:
	case OMCI_ME_TCONT:
	case OMCI_ME_UNI_G:
	/* The WAN service spine.  ME 50 is here with no modelled attribute:
	 * its INSTANCES are tracked (the ONU creates one per bridge port), so a
	 * Get must still validate the instance rather than answer about a
	 * bridge port that does not exist. */
	case OMCI_ME_MAC_BRIDGE_SVC:
	case OMCI_ME_MAC_BRIDGE_PORT:
	case OMCI_ME_MAC_BRIDGE_FILTER:
	case OMCI_ME_MAC_BRIDGE_TABLE:
	case OMCI_ME_MAC_BRIDGE_PM:
	case OMCI_ME_GEM_IW_TP:
	case OMCI_ME_GAL_ETH_PROF:
	/* The VLAN / classification half. */
	case OMCI_ME_VLAN_TAG_OP:
	case OMCI_ME_PREASSIGN_FILTER:
	case OMCI_ME_VLAN_TAG_FILTER:
	case OMCI_ME_PBIT_MAPPER:
	case OMCI_ME_EXT_VLAN:
	case OMCI_ME_GEM_TRAFFIC_DESC:
	case OMCI_ME_MCAST_GEM_IW_TP:
		return true;
	default:
		return false;
	}
}

static void omci_anig_alarms_refresh(struct omci_onu *o)
{
	const u8 *t = o->anig_threshold;
	s32 rx = (s16)o->anig_rx_level, tx = (s16)o->anig_tx_level;
	u16 bits = 0;

	if (!o->anig_live)
		return;
	/* Threshold LSB = 0.5 dB; level LSB = 0.002 dB. No guessed vendor policy. */
	if (t[0] != 0xff && rx < -(s32)t[0] * 250)
		bits |= OMCI_ANIG_RX_LOW;
	if (t[1] != 0xff && rx > -(s32)t[1] * 250)
		bits |= OMCI_ANIG_RX_HIGH;
	if (t[2] != 0x81 && tx < (s32)(s8)t[2] * 250)
		bits |= OMCI_ANIG_TX_LOW;
	if (t[3] != 0x81 && tx > (s32)(s8)t[3] * 250)
		bits |= OMCI_ANIG_TX_HIGH;
	o->alarm_class = OMCI_ME_ANI_G;
	o->alarm_inst = 0x8001;
	o->alarm_active = (o->alarm_active & ~(OMCI_ANIG_RX_LOW |
		OMCI_ANIG_RX_HIGH | OMCI_ANIG_TX_LOW | OMCI_ANIG_TX_HIGH)) | bits;
}

static const struct { u8 off, size; bool numeric; } omci_id_fields[OMCI_ID_FIELDS] = {
#define OMCI_ID_ROW(id, name, member, numeric)				\
	[OMCI_ID_##id] = { offsetof(struct omci_identity, member),	\
			   OMCI_ID_SIZEOF(member), numeric },
	OMCI_ID_LIST(OMCI_ID_ROW)
#undef OMCI_ID_ROW
};

const char *const omci_id_names[OMCI_ID_FIELDS] = {
#define OMCI_ID_NAME(id, name, member, numeric) [OMCI_ID_##id] = #name,
	OMCI_ID_LIST(OMCI_ID_NAME)
#undef OMCI_ID_NAME
};

/* G.988 ONU2-G #2 codes that announce the baseline message set only. */
static bool omci_omcc_baseline(u8 code)
{
	return (code >= 0x80 && code <= 0x86) || (code >= 0xa0 && code <= 0xa3);
}

/* ---- Far-end adaptations: the ONE table (gpon_omci_me.h enum omci_farend) ---- */
static const struct omci_farend_profile {
	const char *name;
	u8 vendor[4];		/* the OLT-G / identity vendor that selects it */
	u8 omcc_version;	/* ONU2-G #2 when the identity leaves it to the far end */
	u16 me[2][2];		/* class, instance this far end provisions on every ONU */
} omci_farends[OMCI_FAREND_N] = {
	[OMCI_FAREND_GENERIC] = { "generic", { 0 }, 0x80 },
#ifdef CONFIG_OLT_ZTE
	/* AXS's ZTE OLT (DC3, 2026-10-17) Sets ME 138 #0 and ME 340 on the VEIP of a
	 * ZTE F670L, whose own binaries announce OMCC 0xa2 (V1.1.20P3N4D/P3N6B). */
	[OMCI_FAREND_ZTE] = { "zte", { 'Z', 'T', 'E', 'G' }, 0xa2,
		{ { OMCI_ME_VOIP_CONFIG, 0 }, { OMCI_ME_TR069_SERVER, OMCI_VEIP_INST } } },
#endif
};

static enum omci_farend omci_farend_detect(const struct omci_onu *o)
{
	unsigned int f;

	for (f = 0; f < OMCI_FAREND_N; f++)
		if (o->id.farend[0] && !strncmp((const char *)o->id.farend, omci_farends[f].name,
						sizeof(o->id.farend)))
			return f;
	for (f = OMCI_FAREND_GENERIC + 1; f < OMCI_FAREND_N; f++)
		if (!memcmp(o->id.vendor_id, omci_farends[f].vendor, 4) ||
		    !memcmp(o->olt_vendor_seen, omci_farends[f].vendor, 4))
			return f;
	return OMCI_FAREND_GENERIC;
}

const char *omci_farend_name(const struct omci_onu *o)
{
	return omci_farends[o->farend < OMCI_FAREND_N ? o->farend : 0].name;
}

/* Every family's /proc/gpon: the profile in force, every name the identity
 * "farend" accepts, and the vendor the OLT last wrote in ME 131. */
int omci_farend_fmt(const struct omci_onu *o, char *out, size_t sz)
{
	unsigned int f;
	int pos = scnprintf(out, sz, "omci_farend: %s\nomci_farends:",
			    o ? omci_farend_name(o) : "-");

	for (f = 0; f < OMCI_FAREND_N; f++)
		pos += scnprintf(out + pos, sz - pos, " %s", omci_farends[f].name);
	if (o && o->olt_vendor_seen[0])
		return pos + scnprintf(out + pos, sz - pos, "\nomci_olt_vendor: %.4s\n",
				       (const char *)o->olt_vendor_seen);
	return pos + scnprintf(out + pos, sz - pos, "\nomci_olt_vendor: -\n");
}

static bool omci_farend_known(const u8 *val, unsigned int len)
{
	unsigned int f;

	if (!len)
		return true;
	for (f = 0; f < OMCI_FAREND_N; f++)
		if (strlen(omci_farends[f].name) == len && !memcmp(val, omci_farends[f].name, len))
			return true;
	return false;
}

void omci_id_default(struct omci_identity *id)
{
	*id = omci_id_default_bytes;
}

bool omci_id_set(struct omci_identity *id, enum omci_id_field f,
		 const u8 *val, unsigned int len)
{
	u8 *dst;

	if ((unsigned int)f >= OMCI_ID_FIELDS || len > omci_id_fields[f].size ||
	    (omci_id_fields[f].numeric && len != omci_id_fields[f].size) ||
	    (f == OMCI_ID_OMCC_VERSION && val[0] && !omci_omcc_baseline(val[0])) ||
	    (f == OMCI_ID_FAREND && !omci_farend_known(val, len)))
		return false;
	if (!id)
		return true;
	dst = (u8 *)id + omci_id_fields[f].off;
	memset(dst, 0, omci_id_fields[f].size);
	if (len)
		memcpy(dst, val, len);
	return true;
}

bool omci_id_set_str(struct omci_identity *id, enum omci_id_field f,
		     const char *s)
{
	unsigned int len = 0, code = 0, base = 10, digit, size, i;
	u8 be[2];

	while (s[len] && s[len] != '\n')
		len++;
	if ((unsigned int)f >= OMCI_ID_FIELDS || !omci_id_fields[f].numeric)
		return omci_id_set(id, f, (const u8 *)s, len);
	/* strtol(s, NULL, 0), as stock's omci_app reads OMCI_VENDOR_PRODUCT_CODE:
	 * "31" is 0x001f on the field X111W's wire. */
	size = omci_id_fields[f].size;
	if (s[0] == '0' && (s[1] | 0x20) == 'x') {
		base = 16;
		s += 2;
	} else if (s[0] == '0' && len > 1) {
		base = 8;
		s++;
	}
	if (!*s || *s == '\n')
		return false;
	for (; *s && *s != '\n'; s++) {
		char c = *s;

		if (c >= '0' && c <= '9')
			digit = (unsigned int)(c - '0');
		else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
			digit = (unsigned int)((c | 0x20) - 'a' + 10);
		else
			return false;
		if (digit >= base)
			return false;
		code = code * base + digit;
		if (code >> (8 * size))
			return false;
	}
	for (i = 0; i < size; i++)
		be[i] = (u8)(code >> (8 * (size - 1 - i)));
	return omci_id_set(id, f, be, size);
}

enum omci_id_field omci_id_set_named(struct omci_identity *id, const char *name,
				     const char *s)
{
	unsigned int f;

	for (f = 0; f < OMCI_ID_FIELDS; f++)
		if (!strcmp(name, omci_id_names[f]))
			return omci_id_set_str(id, f, s) ? f : OMCI_ID_FIELDS;
	return OMCI_ID_FIELDS;
}

void omci_onu_set_optical(struct omci_onu *o, u16 rx_level, u16 tx_level)
{
	o->anig_rx_level = rx_level;
	o->anig_tx_level = tx_level;
	o->anig_live = true;
	omci_anig_alarms_refresh(o);
}

static void omci_build_mib(struct omci_onu *o);

/* The far end is chosen at each MIB-Reset, and again when the OLT names itself
 * in ME 131: AXS's ZTE OLT writes ZTEG there only after its one MIB-Reset, then
 * Sets the MEs its profile adds (DC3 capture, 2026-10-18). */
static void omci_farend_refresh(struct omci_onu *o)
{
	enum omci_farend f = omci_farend_detect(o);

	if (f == o->farend)
		return;
	o->farend = f;
	if (o->nrows)
		omci_build_mib(o);
}

void omci_me_reset_values(struct omci_onu *o)
{
	u16 i;

	omci_uni_reset(&o->pptp_eth_uni);
	omci_uni_reset(&o->uni_g);
	omci_uni_defaults(o);
	/* A MIB-Reset drops the provisioned store, and the ME 171 rows live
	 * OUTSIDE it: leaving them would serve the previous session's
	 * subscriber VLAN to an OLT that has just told us to forget
	 * everything. */
	gpon_vlan_model_reset(&o->vlan);
	o->tcont_alloc_written = 0;
	/* G.988's unassigned 0x00ff on every T-CONT, as stock's omci_app creates
	 * them and the field X111W uploads them */
	for (i = 0; i < OMCI_TCONT_MAX; i++)
		o->tcont_alloc[i] = 0x00ff;
	memset(o->olt_g, 0, sizeof(o->olt_g));
	memcpy(o->olt_g + 4, omci_text[OMCI_TEXT_SPACES], 20);
	o->olt_g[24] = '0';
	o->anig_threshold[0] = o->anig_threshold[1] = 0xff;
	o->anig_threshold[2] = o->anig_threshold[3] = 0x81;
	omci_anig_alarms_refresh(o);
	omci_farend_refresh(o);
	o->omcc_farend = o->farend;
}

/* How many octets of DENSE attribute body */
u8 omci_me_dense_len(u16 class_id)
{
	const struct omci_attr *a;
	unsigned int end = 0;

	for (a = omci_me_find(class_id); a && a->class_id == class_id; a++)
		if ((a->src == OMCI_SRC_STORE || a->src == OMCI_SRC_CNT) &&
		    (unsigned int)a->v + a->size > end)
			end = (unsigned int)a->v + a->size;
	return end > OMCI_STORE_BODY ? 0 : (u8)end;
}

/* Create carries only SBC attributes, with no mask. */
bool omci_store_create(struct omci_onu *o, u16 class_id, u16 inst,
		       const u8 *body, unsigned int blen)
{
	const struct omci_attr *a;
	u8 dense[OMCI_STORE_BODY] = { 0 };
	u8 dlen = omci_me_dense_len(class_id);
	unsigned int pos = 0;

	if (!dlen)
		return omci_store_put(o, class_id, inst, body,
				      blen > sizeof(dense) ? (int)sizeof(dense)
							   : (int)blen);
	if (class_id == OMCI_ME_GEM_CTP) {
		/* The stock default downstream priority-queue pointer is
		 * unassigned. */
		dense[10] = 0xff;
		dense[11] = 0xff;
	}
	for (a = omci_me_find(class_id); a && a->class_id == class_id; a++) {
		if (a->src != OMCI_SRC_STORE || !(a->access & 4))
			continue;
		if (!body || pos + a->size > blen) {
			if (class_id == OMCI_ME_GEM_CTP)
				return false;
			break;
		}
		memcpy(dense + a->v, body + pos, a->size);
		pos += a->size;
	}
	return omci_store_put(o, class_id, inst, dense, dlen);
}

/* Can this ONU actually realise the value the OLT asks for */
static bool omci_set_value_realisable(const struct omci_onu *o, int pptp_slot,
				      const struct omci_attr *a, const u8 *v)
{
	u32 want = 0;
	unsigned int i;

	/* ME 11, G.988 writable: #1 autosense (0) or the type this port is, #8
	 * any size the datapath carries whole, #10 any pause time. */
	if (a->class_id == OMCI_ME_PPTP_ETH_UNI && a->attr == 1)
		return v[0] == 0 || (pptp_slot >= 0 && v[0] == o->uni_type[pptp_slot]);
	if (a->class_id == OMCI_ME_PPTP_ETH_UNI && a->attr == 8) {
		want = ((u32)v[0] << 8) | v[1];
		return want >= OMCI_UNI_MAX_FRAME_DEFAULT && want <= OMCI_UNI_MAX_FRAME_LIMIT;
	}
	if (a->class_id == OMCI_ME_PPTP_ETH_UNI && a->attr == 10)
		return true;
	/* An administrative state is locked or unlocked, and nothing else. */
	if ((a->class_id == OMCI_ME_PPTP_ETH_UNI && a->attr == 5) ||
	    (a->class_id == OMCI_ME_UNI_G && a->attr == 2))
		return v[0] <= 1;
	if (!(a->access & OMCI_ACCESS_WRITE_UNCHANGED))
		return true;
	if (a->size > sizeof(want))
		return false;		/* wider than we can compare: refuse */
	for (i = 0; i < a->size; i++)
		want = (want << 8) | v[i];
	return want == a->v;
}

/* Apply ONE already-validated attribute value. */
static void omci_set_apply_one(struct omci_onu *o, const struct omci_attr *a,
			       u16 inst, int pptp_slot, int unig_slot,
			       struct omci_me_inst *e, const u8 *v)
{
	if (a->access & OMCI_ACCESS_WRITE_UNCHANGED)
		return;			/* validated equal to what we serve */
	if (a->src == OMCI_SRC_STORE)
		memcpy(e->body + a->v, v, a->size);
	else if (a->src == OMCI_SRC_OLT_G) {
		memcpy(o->olt_g + a->v, v, a->size);
		if (a->v == 0 && a->size >= sizeof(o->olt_vendor_seen)) {
			memcpy(o->olt_vendor_seen, v, sizeof(o->olt_vendor_seen));
			omci_farend_refresh(o);
		}
	}
	else if (a->src == OMCI_SRC_ANIG_THRESHOLD)
		o->anig_threshold[a->v] = v[0];
	else if (a->src == OMCI_SRC_TBL) {
		/* TWO table writes this model HOLDS -- the subscriber VLAN and
		 * the per-bridge-port MAC filter; the rest are accepted so the
		 * burst completes and COUNTED so the acceptance is not a
		 * silence. */
		if (a->v == OMCI_TBL_EXT_VLAN_ROW)
			gpon_ext_vlan_row_set(&o->vlan, inst, v);
		else if (a->v == OMCI_TBL_MAC_FILTER_ROW)
			gpon_mac_filter_row_set(&o->vlan, inst, v);
		else
			o->vlan.attr_owed++;
	} else if (a->class_id == OMCI_ME_PPTP_ETH_UNI && a->attr == 1)
		o->uni_expected[pptp_slot] = v[0];
	else if (a->class_id == OMCI_ME_PPTP_ETH_UNI && (a->attr == 8 || a->attr == 10)) {
		u16 val = ((u16)v[0] << 8) | v[1];
		u16 *slot = a->attr == 8 ? &o->uni_max_frame[pptp_slot] : &o->uni_pause[pptp_slot];

		/* accepted and served; the switch is not reprogrammed: COUNTED */
		if (val != (a->attr == 8 ? OMCI_UNI_MAX_FRAME_DEFAULT : OMCI_UNI_PAUSE_DEFAULT))
			o->vlan.attr_owed++;
		*slot = val;
	} else if (a->class_id == OMCI_ME_PPTP_ETH_UNI)
		omci_uni_accept(&o->pptp_eth_uni, pptp_slot, v[0]);
	else if (a->class_id == OMCI_ME_UNI_G)
		omci_uni_accept(&o->uni_g, unig_slot, v[0]);
	else {
		o->tcont_alloc[inst - 0x8000] = ((u16)v[0] << 8) | v[1];
		o->tcont_alloc_written |= 1u << (inst - 0x8000);
	}
}

/* Atomic masked Set: validate the entire request before changing any value.
 * Unsupported bits name absent attributes; failed bits name requested known
 * attributes left unapplied. This also reports the valid part of a rejected
 * mixed request as failed, rather than claiming partial application. */
u8 omci_me_set(struct omci_onu *o, u16 class_id, u16 inst, u16 mask,
	       const u8 *values, unsigned int len, u16 *unsupported, u16 *failed)
{
	const struct omci_attr *first = omci_me_find(class_id), *a;
	struct omci_me_inst *e = omci_store_find(o, class_id, inst);
	/* Which inventory slot this Set addresses, or -1.  Resolved once, on the
	 * full u16, and it is what makes the attribute writable below. */
	int pptp_slot = class_id == OMCI_ME_PPTP_ETH_UNI ?
			omci_uni_slot(&o->pptp_eth_uni, inst) : -1;
	int unig_slot = class_id == OMCI_ME_UNI_G ?
			omci_uni_slot(&o->uni_g, inst) : -1;
	u8 dlen = omci_me_dense_len(class_id);
	u16 known = 0, writable = 0;
	unsigned int need = 0, pos = 0;

	for (a = first; a && a->class_id == class_id; a++) {
		u16 bit;

		if (!a->attr)
			continue;
		bit = (u16)OMCI_ATTR_BIT(a->attr);
		known |= bit;
		if ((a->access & 2) ||
		    (a->access & OMCI_ACCESS_WRITE_UNCHANGED) ||
		    /* ME 11 #5 and ME 264 #2, the administrative states. */
		    (pptp_slot >= 0 && (a->attr == 1 || a->attr == 5 ||
					a->attr == 8 || a->attr == 10)) ||
		    (unig_slot >= 0 && a->attr == 2) ||
		    (class_id == OMCI_ME_TCONT && inst >= 0x8000 &&
		     inst < 0x8000 + o->cap.tcont_n && a->attr == 1))
			writable |= bit;
		if (mask & bit)
			need += a->size;
	}
	*unsupported = (u16)(mask & ~known);
	*failed = 0;
	/* A dense class is written THROUGH its instance, so the instance must
	 * exist and hold the full dense body its table describes -- otherwise
	 * omci_set_apply_one would write at a table offset into a shorter
	 * blob.  Derived from the table (16 for ME 268, as it always was). */
	if (*unsupported || (mask & ~writable) || need > len ||
	    (need && !values) ||
	    (dlen && (!e || e->blen != dlen))) {
		*failed = (u16)(mask & known);
		return OMCI_RC_ATTR_FAILED;
	}
	/* EVERY requested value is checked before ANY is applied — the whole
	 * point of an atomic Set, and the reason this is a separate pass. */
	for (a = first, pos = 0; a && a->class_id == class_id; a++) {
		if (!a->attr || !(mask & OMCI_ATTR_BIT(a->attr)))
			continue;
		if (!omci_set_value_realisable(o, pptp_slot, a, values + pos)) {
			*failed = mask;
			return OMCI_RC_ATTR_FAILED;
		}
		pos += a->size;
	}
	for (a = first, pos = 0; a && a->class_id == class_id; a++) {
		if (!a->attr || !(mask & OMCI_ATTR_BIT(a->attr)))
			continue;
		omci_set_apply_one(o, a, inst, pptp_slot, unig_slot, e,
				   values + pos);
		pos += a->size;
	}
	if (class_id == OMCI_ME_ANI_G)
		omci_anig_alarms_refresh(o);
	return OMCI_RC_OK;
}

/* Does the ONU hold a descriptor of this class (not just the vendor range policy)? */
bool omci_class_described(u16 class_id)
{
	return omci_me_find(class_id) != NULL;
}

/* Does the ONU model this class at all (either a descriptor or the vendor
 * range policy)? */
bool omci_class_modelled(u16 class_id)
{
	return omci_me_find(class_id) || omci_vendor_class(class_id);
}

static bool omci_row_listed(const struct omci_onu *o, u16 class_id, u16 inst)
{
	u16 i;

	for (i = 0; i < o->nrows; i++)
		if (o->rows[i].class_id == class_id && o->rows[i].inst == inst)
			return true;
	return false;
}

bool omci_vendor_absent(const struct omci_onu *o, u16 class_id, u16 inst)
{
	/* CTC LoID authentication: served by Get, never uploaded, as stock's */
	return omci_vendor_class(class_id) && class_id != OMCI_ME_CTC_LOID_AUTH &&
	       !omci_row_listed(o, class_id, inst);
}

/* The bytes of one attribute.  Integers are big-endian, right-aligned in
 * @size octets; @scratch must hold 4 bytes. */
static const struct omci_slot *omci_slot_find(const struct omci_onu *o,
					      u16 inst)
{
	u8 i;

	for (i = 0; i < o->slots_n; i++)
		if (o->slots[i].inst == inst)
			return &o->slots[i];
	return NULL;
}

static int omci_ip_host_index(const struct omci_onu *o, u16 inst)
{
	int i;

	for (i = 0; i < o->ip_host_n; i++)
		if (o->ip_host[i] == inst)
			return i;
	return -1;
}

static u8 omci_slot_value(const struct omci_slot *s, u16 dyn)
{
	switch (dyn) {
	case OMCI_DYN_SLOT_TYPE:
		return s->type;
	case OMCI_DYN_SLOT_PORTS:
		return s->ports;
	case OMCI_DYN_SLOT_TBUF:
		return s->tcont_buf;
	case OMCI_DYN_SLOT_PRIQ:
		return s->priq;
	default:		/* OMCI_DYN_SLOT_SCHED */
		return s->scheds;
	}
}

static u8 omci_usq_of(const struct omci_onu *o, u8 t)
{
	return o->cap.usq_per[t] ? o->cap.usq_per[t] : 8;
}

static u16 omci_usq_total(const struct omci_onu *o)
{
	u16 n = 0;
	u8 t;

	for (t = 0; t < o->cap.tcont_n; t++)
		n += omci_usq_of(o, t);
	return n;
}

/* ME 277 #6 as stock numbers its queues: per port, counting DOWN (its first
 * queue holds the highest priority).  Downstream block b of 8 is the board's
 * b-th declared queue port; upstream queues fill T-CONT 0x8000, 0x8001, ... in
 * turn, each with its declared count. */
static u32 omci_pq_related_port(const struct omci_onu *o, u16 inst)
{
	u16 q = inst & 0x7fff, port;
	u8 t, n = 8;

	if (inst & 0x8000) {
		for (t = 0; t + 1 < o->cap.tcont_n && q >= (n = omci_usq_of(o, t)); t++)
			q -= n;
		n = omci_usq_of(o, t);
		port = (u16)(0x8000 + t);
	} else {
		port = q / 8 < o->cap.dsq_n ? o->cap.dsq_port[q / 8] : 0;
		q %= 8;
	}
	return ((u32)port << 16) | (u32)(n - 1 - q);
}

static const u8 *omci_attr_bytes(struct omci_onu *o,
				 const struct omci_attr *a, u16 inst,
				 u8 *scratch)
{
	u32 val;

	switch (a->src) {
	case OMCI_SRC_ANIG_THRESHOLD:
		return o->anig_threshold + a->v;
	case OMCI_SRC_TEXT:
		return omci_text[a->v];
	case OMCI_SRC_INST0:
		val = inst ? (a->v & 0xff) : (a->v >> 8);
		break;
	case OMCI_SRC_OLT_G:
		return o->olt_g + a->v;
	case OMCI_SRC_STORE: {
		struct omci_me_inst *e = omci_store_find(o,
						      a->class_id, inst);

		return e && a->v + a->size <= e->blen ? e->body + a->v : NULL;
	}
	case OMCI_SRC_TBL:
		/* A table attribute read as a scalar. */
		return a->size <= OMCI_ID_SIZEOF(zeros) ? o->id.zeros : NULL;
	case OMCI_SRC_CNT:
		return NULL;		/* could not ask -- never a zero */
	case OMCI_SRC_ID:
		return (const u8 *)&o->id + a->v;
	case OMCI_SRC_SN:
		return o->sn;
	case OMCI_SRC_MDS:
		val = o->mds;
		break;
	case OMCI_SRC_DYN:
		switch (a->v) {
		case OMCI_DYN_UNI_ADMIN:
			val = omci_uni_admin(&o->pptp_eth_uni, inst);
			break;
		case OMCI_DYN_UNIG_ADMIN:
			val = omci_uni_admin(&o->uni_g, inst);
			break;
		case OMCI_DYN_UNI_EXPECTED:
		case OMCI_DYN_UNI_TYPE: {
			int slot = omci_uni_slot(&o->pptp_eth_uni, inst);

			/* Equal until an OLT writes #1 -- see the note on the selectors above. */
			val = slot < 0 ? OMCI_UNI_TYPE_DEFAULT :
			      a->v == OMCI_DYN_UNI_EXPECTED ? o->uni_expected[slot] : o->uni_type[slot];
			break;
		}
		case OMCI_DYN_UNI_MAX_FRAME:
		case OMCI_DYN_UNI_PAUSE: {
			int slot = omci_uni_slot(&o->pptp_eth_uni, inst);

			val = a->v == OMCI_DYN_UNI_MAX_FRAME ?
			      (slot < 0 ? OMCI_UNI_MAX_FRAME_DEFAULT : o->uni_max_frame[slot]) :
			      (slot < 0 ? OMCI_UNI_PAUSE_DEFAULT : o->uni_pause[slot]);
			break;
		}
		case OMCI_DYN_UNIG_CAP: {
			int slot = omci_uni_slot(&o->uni_g, inst);

			val = slot < 0 ? 1 : o->uni_g_mgmt_cap[slot];
			break;
		}
		case OMCI_DYN_SW_VER:
			return inst ? o->id.sw_bank1_version :
				      o->id.sw_bank0_version;
		case OMCI_DYN_SW_FLAG:
			val = inst ? 0 : 1;
			break;
		case OMCI_DYN_TCONT_ALLOC:
			val = inst >= 0x8000 && inst < 0x8000 + o->cap.tcont_n ?
				o->tcont_alloc[inst - 0x8000] : 0x00ff;
			break;
		case OMCI_DYN_PQ_PORT:
			val = omci_pq_related_port(o, inst);
			break;
		case OMCI_DYN_TCONT_N:
			val = o->cap.tcont_n;
			break;
		case OMCI_DYN_ONU2G_PQ:
			val = o->cap.onu2g_pq;
			break;
		case OMCI_DYN_SCHED_N:
			val = o->cap.sched_n;
			break;
		case OMCI_DYN_GEM_N:
			val = o->cap.gem_ports;
			break;
		case OMCI_DYN_CTC:
			val = o->cap.ctc[a->attr - 2];
			break;
		case OMCI_DYN_OMCC:
			val = o->id.omcc_version[0] ? o->id.omcc_version[0] :
			      omci_farends[o->omcc_farend].omcc_version;
			break;
		case OMCI_DYN_IP_HOST_MAC: {
			int i = omci_ip_host_index(o, inst);

			if (i < 0)
				return NULL;
			if (!o->ip_host_mac[i])
				return o->id.zeros;
			return o->wan_mac_set ? o->wan_mac : NULL;
		}
		case OMCI_DYN_SLOT_TYPE:
		case OMCI_DYN_SLOT_PORTS:
		case OMCI_DYN_SLOT_TBUF:
		case OMCI_DYN_SLOT_PRIQ:
		case OMCI_DYN_SLOT_SCHED: {
			const struct omci_slot *slot = omci_slot_find(o, inst);

			if (!slot)
				return NULL;
			val = omci_slot_value(slot, a->v);
			break;
		}
		case OMCI_DYN_WAN_MAC:
			return o->wan_mac_set ? o->wan_mac : NULL;
		case OMCI_DYN_ANIG_RX:
			val = o->anig_rx_level;
			break;
		case OMCI_DYN_ANIG_TX:
			val = o->anig_tx_level;
			break;
		default:	/* OMCI_DYN_TS_TCONT */
			val = inst;
			break;
		}
		break;
	default:		/* OMCI_SRC_CONST */
		val = a->v;
		break;
	}
	scratch[0] = (u8)(val >> 24);
	scratch[1] = (u8)(val >> 16);
	scratch[2] = (u8)(val >> 8);
	scratch[3] = (u8)val;
	return scratch + 4 - a->size;
}

/* the ONE generic attribute filler ---- Shared by GET */
u8 omci_me_fill(struct omci_onu *o, u16 class_id, u16 inst, u16 mask,
		u8 *v, const u8 *end, u16 *rmask_out, u16 *known_out)
{
	const struct omci_attr *a = omci_me_find(class_id);
	u16 rmask = 0, known = 0;
	bool over = false;

	*rmask_out = 0;
	*known_out = 0;
	if (!a)
		return omci_vendor_class(class_id) ? OMCI_RC_OK :
						     OMCI_RC_UNKNOWN_ME;

	for (; a->class_id == class_id; a++) {
		u16 bit;
		u8 scratch[4];
		const u8 *bytes;

		if (!a->attr)			/* marker row: no attributes */
			continue;
		bit = (u16)OMCI_ATTR_BIT(a->attr);
		known |= bit;
		if (!(mask & bit))
			continue;
		if (v + a->size > end) {
			over = true;
			continue;
		}
		bytes = omci_attr_bytes(o, a, inst, scratch);
		if (!bytes) {
			over = true;
			continue;
		}
		memcpy(v, bytes, a->size);
		v += a->size;
		rmask |= bit;
	}

	*rmask_out = rmask;
	*known_out = known;
	return over ? OMCI_RC_ATTR_FAILED : OMCI_RC_OK;
}

/* Where the instances of one auto-instantiated class come from. */
enum omci_mi {
	OMCI_MI_ONE,		/* the single instance named beside it */
	OMCI_MI_SLOTS,		/* the declared equipment slots */
	OMCI_MI_PPTP,		/* the declared Ethernet UNIs: THE HGU gate */
	OMCI_MI_POTS,
	OMCI_MI_IP_HOST,
	OMCI_MI_UNI_G,		/* its OWN inventory, not the PPTP list */
	OMCI_MI_TCONT,		/* 0x8000 + i */
	OMCI_MI_SCHED,		/* 0x8000 + i */
	OMCI_MI_QUEUE,		/* 8 per downstream port from 0, then 8 per
				 * T-CONT from 0x8000 when the board declares
				 * its capacity */
	OMCI_MI_EXTRA,		/* the board's further single MEs, any class */
	OMCI_MI_FAREND,		/* the far end's MEs a board does not carry */
};

/* Every auto-instantiated ME the OLT reads back.  The OLT counts the ME 11
 * instances to classify the ONU as HGU, and this is also the ONU's statement
 * of WHICH instances exist: a Set of one not here (and never created) is
 * answered 0x05.  ME 65530 is served by Get and never uploaded, as stock. */
static const struct {
	u16 class_id, inst;
	u8 from;
} omci_mib_classes[] = {
	/* in class order, so the sort below has little to move */
	{ OMCI_ME_ONU_DATA, 0, OMCI_MI_ONE },
	{ OMCI_ME_CARDHOLDER, 0, OMCI_MI_SLOTS },
	{ OMCI_ME_CIRCUIT_PACK, 0, OMCI_MI_SLOTS },
	{ OMCI_ME_SW_IMAGE, 0, OMCI_MI_ONE },
	{ OMCI_ME_SW_IMAGE, 1, OMCI_MI_ONE },
	{ OMCI_ME_PPTP_ETH_UNI, 0, OMCI_MI_PPTP },
	{ OMCI_ME_PPTP_POTS_UNI, 0, OMCI_MI_POTS },
	{ OMCI_ME_OLT_G, 0, OMCI_MI_ONE },
	{ OMCI_ME_IP_HOST, 0, OMCI_MI_IP_HOST },
	{ OMCI_ME_ONU_G, 0, OMCI_MI_ONE },
	{ OMCI_ME_ONU2_G, 0, OMCI_MI_ONE },
	{ OMCI_ME_TCONT, 0, OMCI_MI_TCONT },
	{ OMCI_ME_ANI_G, 0x8001, OMCI_MI_ONE },
	{ OMCI_ME_UNI_G, 0, OMCI_MI_UNI_G },
	{ OMCI_ME_PRIORITY_QUEUE, 0, OMCI_MI_QUEUE },
	{ OMCI_ME_TRAFFIC_SCHED, 0, OMCI_MI_SCHED },
	{ OMCI_ME_VEIP, OMCI_VEIP_INST, OMCI_MI_ONE },
	{ 0, 0, OMCI_MI_EXTRA },
	{ 0, 0, OMCI_MI_FAREND },
};

static u16 omci_mi_count(const struct omci_onu *o, u8 from)
{
	switch (from) {
	case OMCI_MI_SLOTS:
		return o->slots_n;
	case OMCI_MI_PPTP:
		return o->pptp_eth_uni.n;
	case OMCI_MI_POTS:
		return o->pots_uni_n;
	case OMCI_MI_IP_HOST:
		return o->ip_host_n;
	case OMCI_MI_UNI_G:
		return o->uni_g.n;
	case OMCI_MI_TCONT:
		return o->cap.tcont_n;
	case OMCI_MI_SCHED:
		return o->cap.sched_n;
	case OMCI_MI_QUEUE:
		return (u16)(8 * o->cap.dsq_n + (o->cap.usq ? omci_usq_total(o) : 0));
	case OMCI_MI_EXTRA:
		return o->extra_me_n;
	case OMCI_MI_FAREND:
		return omci_farends[o->farend].me[1][0] ? 2 : omci_farends[o->farend].me[0][0] ? 1 : 0;
	default:
		return 1;
	}
}

static u16 omci_mi_inst(const struct omci_onu *o, u8 from, u16 inst, u16 i)
{
	u16 ds_q = (u16)(8 * o->cap.dsq_n);

	switch (from) {
	case OMCI_MI_SLOTS:
		return o->slots[i].inst;
	case OMCI_MI_PPTP:
		return o->pptp_eth_uni.inst[i];
	case OMCI_MI_POTS:
		return o->pots_uni[i];
	case OMCI_MI_IP_HOST:
		return o->ip_host[i];
	case OMCI_MI_UNI_G:
		return o->uni_g.inst[i];
	case OMCI_MI_TCONT:
	case OMCI_MI_SCHED:
		return (u16)(0x8000 + i);
	case OMCI_MI_QUEUE:
		return i < ds_q ? i : (u16)(0x8000 + i - ds_q);
	case OMCI_MI_EXTRA:
		return o->extra_me[i][1];
	case OMCI_MI_FAREND:
		return omci_farends[o->farend].me[i][1];
	default:
		return inst;
	}
}

#define OMCI_UPLOAD_VALUE_LEN 26

static void omci_row_add(struct omci_onu *o, u16 class_id, u16 inst, u16 mask)
{
	if (o->nrows < OMCI_MIB_ROWS_MAX) {
		o->rows[o->nrows].class_id = class_id;
		o->rows[o->nrows].inst = inst;
		o->rows[o->nrows].mask = mask;
		o->nrows++;
	} else {
		o->rows_dropped++;
	}
}

/* Stock's packing of one instance: its uploaded attributes in order, a new row
 * whenever the next would overflow the 26-octet value area. */
static void omci_pack_rows(struct omci_onu *o, u16 class_id, u16 inst)
{
	const struct omci_attr *a;
	unsigned int used = 0;
	u16 mask = 0;

	for (a = omci_me_find(class_id); a && a->class_id == class_id; a++) {
		if (!a->attr || (a->access & OMCI_ACCESS_HIDDEN))
			continue;
		if (mask && used + a->size > OMCI_UPLOAD_VALUE_LEN) {
			omci_row_add(o, class_id, inst, mask);
			mask = 0;
			used = 0;
		}
		mask |= (u16)OMCI_ATTR_BIT(a->attr);
		used += a->size;
	}
	if (mask)
		omci_row_add(o, class_id, inst, mask);
}

static u32 omci_row_key(const struct omci_mib_row *r)
{
	return (u32)r->class_id << 16 | r->inst;
}

/* Build the static MIB-Upload row table, in stock's order: by class, then by
 * instance, an instance's rows in attribute order. */
static void omci_build_mib(struct omci_onu *o)
{
	unsigned int c;
	u16 i, j;

	o->nrows = 0;
	o->rows_dropped = 0;
	for (c = 0; c < sizeof(omci_mib_classes) / sizeof(omci_mib_classes[0]); c++) {
		u8 from = omci_mib_classes[c].from;

		for (i = 0; i < omci_mi_count(o, from); i++) {
			u16 cls = from == OMCI_MI_EXTRA ? o->extra_me[i][0] :
				  from == OMCI_MI_FAREND ? omci_farends[o->farend].me[i][0] :
				  omci_mib_classes[c].class_id;
			u16 inst = omci_mi_inst(o, from, omci_mib_classes[c].inst, i);

			if (from != OMCI_MI_FAREND || !omci_row_listed(o, cls, inst))
				omci_pack_rows(o, cls, inst);
		}
	}
	for (i = 1; i < o->nrows; i++) {
		struct omci_mib_row r = o->rows[i];

		for (j = i; j && omci_row_key(&o->rows[j - 1]) > omci_row_key(&r); j--)
			o->rows[j] = o->rows[j - 1];
		o->rows[j] = r;
	}
}

bool omci_onu_declare_unis(struct omci_onu *o,
			   const u16 *pptp_inst, const u8 *pptp_type, u8 pptp_n,
			   const u16 *unig_inst, const u8 *unig_mgmt_cap,
			   u8 unig_n)
{
	/* The panel as it stands, so a refusal can put it */
	struct omci_uni_inv was_pptp = o->pptp_eth_uni, was_unig = o->uni_g;
	u8 was_cap[OMCI_UNI_MAX], was_type[OMCI_UNI_MAX];
	u8 i;

	if (!omci_uni_list_ok(pptp_inst, pptp_n) ||
	    !omci_uni_list_ok(unig_inst, unig_n))
		return false;
	memcpy(was_cap, o->uni_g_mgmt_cap, sizeof(was_cap));
	memcpy(was_type, o->uni_type, sizeof(was_type));
	omci_uni_install(&o->pptp_eth_uni, pptp_inst, pptp_n);
	omci_uni_install(&o->uni_g, unig_inst, unig_n);
	for (i = 0; i < OMCI_UNI_MAX; i++) {
		o->uni_g_mgmt_cap[i] = i < unig_n && unig_mgmt_cap ?
				       unig_mgmt_cap[i] : 1;
		o->uni_type[i] = i < pptp_n && pptp_type ?
				 pptp_type[i] : OMCI_UNI_TYPE_DEFAULT;
	}
	omci_uni_defaults(o);
	omci_build_mib(o);
	if (o->rows_dropped) {
		/* ⚠ A PANEL THAT DOES NOT FIT IS PUT BACK, NOT LEFT */
		o->pptp_eth_uni = was_pptp;
		o->uni_g = was_unig;
		memcpy(o->uni_g_mgmt_cap, was_cap, sizeof(was_cap));
		memcpy(o->uni_type, was_type, sizeof(was_type));
		omci_uni_defaults(o);
		omci_build_mib(o);
		return false;
	}
	return true;
}

bool omci_onu_declare_slots(struct omci_onu *o, const struct omci_slot *slot,
			    u8 n)
{
	struct omci_slot was[OMCI_SLOT_MAX];
	u8 was_n = o->slots_n, i, j;

	if (n > OMCI_SLOT_MAX || (n && !slot))
		return false;
	for (i = 0; i < n; i++) {
		if (!slot[i].inst)
			return false;
		for (j = 0; j < i; j++)
			if (slot[j].inst == slot[i].inst)
				return false;
	}
	memcpy(was, o->slots, sizeof(was));
	if (n)
		memcpy(o->slots, slot, n * sizeof(*slot));
	o->slots_n = n;
	omci_build_mib(o);
	if (o->rows_dropped) {
		memcpy(o->slots, was, sizeof(was));
		o->slots_n = was_n;
		omci_build_mib(o);
		return false;
	}
	return true;
}

/* The inventory every board on the bench has at minimum, and exactly what this
 * model carried before the inventory existed. */
static const u16 omci_uni_default[] = {
	0x0101 /* default PPTP Ethernet UNI instance */
};

/* One declared 16-bit list: even byte count, at most OMCI_UNI_MAX entries,
 * decoded big-endian.  Empty is legal and means "this board has none". */
static bool omci_uni_decode_list(const void *be, int len, u16 *out, u8 *n)
{
	const u8 *b = be;
	int i;

	if (len < 0 || (len & 1) || len > 2 * OMCI_UNI_MAX || (len && !b))
		return false;
	for (i = 0; i < len / 2; i++)
		out[i] = (u16)((b[2 * i] << 8) | b[2 * i + 1]);
	*n = (u8)(len / 2);
	return true;
}

enum omci_uni_decl omci_onu_declare_unis_be(struct omci_onu *o,
					    const void *pptp_be, int pptp_len,
					    const void *type, int type_len,
					    const void *unig_be, int unig_len,
					    const void *cap, int cap_len,
					    const char **why)
{
	u16 pptp[OMCI_UNI_MAX], unig[OMCI_UNI_MAX];
	u8 caps[OMCI_UNI_MAX], types[OMCI_UNI_MAX];
	u8 pptp_n = 0, unig_n = 0;
	const char *unused;
	int i;

	if (!why)
		why = &unused;
	if (pptp_len < 0 && unig_len < 0) {
		/* ⚠ A CAPABILITY LIST ON ITS OWN IS NOT "nothing declared".  It is
		 * a node that says something about UNI-Gs it never listed, and
		 * discarding it silently would accept a panel nobody can read. */
		if (cap_len >= 0) {
			*why = "a management-capability list with no UNI-G list";
			return OMCI_UNI_DECL_BAD;
		}
		if (type_len >= 0) {
			*why = "an Ethernet-UNI type list with no UNI list";
			return OMCI_UNI_DECL_BAD;
		}
		*why = "no panel declared";
		return OMCI_UNI_DECL_ABSENT;
	}
	/* Half a declaration is refused rather than completed: a board that
	 * lists its Ethernet UNIs and not its UNI-Gs has not said it has none,
	 * it has left one out. */
	if (pptp_len < 0 || unig_len < 0) {
		*why = "one UNI class declared and not the other";
		return OMCI_UNI_DECL_BAD;
	}
	if (!omci_uni_decode_list(pptp_be, pptp_len, pptp, &pptp_n)) {
		*why = "the Ethernet-UNI list is not an even count of 16-bit instances, or holds more than the model does";
		return OMCI_UNI_DECL_BAD;
	}
	if (!omci_uni_decode_list(unig_be, unig_len, unig, &unig_n)) {
		*why = "the UNI-G list is not an even count of 16-bit instances, or holds more than the model does";
		return OMCI_UNI_DECL_BAD;
	}
	for (i = 0; i < OMCI_UNI_MAX; i++) {
		caps[i] = 1;
		types[i] = OMCI_UNI_TYPE_DEFAULT;
	}
	if (type_len < 0) {
		/* absent: every UNI keeps the type this model used to hardcode */
	} else if (type_len != pptp_n || (type_len && !type)) {
		*why = "the Ethernet-UNI type list is present and is not one byte per Ethernet UNI";
		return OMCI_UNI_DECL_BAD;
	} else {
		for (i = 0; i < type_len && i < OMCI_UNI_MAX; i++)
			types[i] = ((const u8 *)type)[i];
	}
	if (cap_len < 0) {
		/* absent: every UNI-G keeps the 1 the model used to hardcode */
	} else if (cap_len != unig_n || (cap_len && !cap)) {
		*why = "the management-capability list is present and is not one byte per UNI-G";
		return OMCI_UNI_DECL_BAD;
	} else {
		/* Bounded by the ARRAY as well as by the equality above: a rule
		 * enforced only somewhere else is one a later edit can remove,
		 * and this loop writes into a fixed-size object. */
		for (i = 0; i < cap_len && i < OMCI_UNI_MAX; i++)
			caps[i] = ((const u8 *)cap)[i];
	}
	if (!omci_onu_declare_unis(o, pptp, types, pptp_n, unig, caps, unig_n)) {
		*why = "a duplicate or zero instance, or more MIB rows than the table holds";
		return OMCI_UNI_DECL_BAD;
	}
	*why = "declared";
	return OMCI_UNI_DECL_OK;
}

/* One plain Ethernet pack with its 8 downstream queues: what this model
 * carried before a board could declare its slots. */
static const struct omci_slot omci_slot_default = {
	.inst = 0x0101, .type = OMCI_UNI_TYPE_DEFAULT, .ports = 1, .priq = 8,
};

/* ME 65529 #2..#5 as stock X100DG and G24W serve them: spec 0, HGU, TX power
 * supply control 2, customized type 1. */
static const u8 omci_ctc_default[4] = { 0, 1, 2, 1 };

#define OMCI_SLOT_BE_FIELDS 6

enum omci_uni_decl omci_onu_declare_slots_be(struct omci_onu *o,
					     const void *be, int len,
					     const char **why)
{
	struct omci_slot slot[OMCI_SLOT_MAX];
	u16 v[OMCI_SLOT_BE_FIELDS];
	const u8 *b = be;
	const char *unused;
	int i, f, n;

	if (!why)
		why = &unused;
	if (len < 0) {
		*why = "no slots declared";
		return OMCI_UNI_DECL_ABSENT;
	}
	if ((len && !b) || len % (2 * OMCI_SLOT_BE_FIELDS) ||
	    len > 2 * OMCI_SLOT_BE_FIELDS * OMCI_SLOT_MAX) {
		*why = "the slot list is not 0..8 sextuples of 16-bit cells";
		return OMCI_UNI_DECL_BAD;
	}
	n = len / (2 * OMCI_SLOT_BE_FIELDS);
	for (i = 0; i < n; i++) {
		for (f = 0; f < OMCI_SLOT_BE_FIELDS; f++, b += 2)
			v[f] = (u16)((b[0] << 8) | b[1]);
		for (f = 1; f < OMCI_SLOT_BE_FIELDS; f++)
			if (v[f] > 0xff) {
				*why = "a slot value wider than its one-octet attribute";
				return OMCI_UNI_DECL_BAD;
			}
		slot[i] = (struct omci_slot){ .inst = v[0], .type = (u8)v[1],
			.ports = (u8)v[2], .priq = (u8)v[3],
			.tcont_buf = (u8)v[4], .scheds = (u8)v[5] };
	}
	if (!omci_onu_declare_slots(o, slot, (u8)n)) {
		*why = "a zero or duplicate instance, or more MIB rows than the table holds";
		return OMCI_UNI_DECL_BAD;
	}
	*why = "declared";
	return OMCI_UNI_DECL_OK;
}

enum omci_uni_decl omci_onu_declare_ip_hosts_be(struct omci_onu *o,
						const void *inst_be, int inst_len,
						const void *mac, int mac_len,
						const char **why)
{
	u16 inst[OMCI_IP_HOST_MAX], was[OMCI_IP_HOST_MAX];
	u8 was_mac[OMCI_IP_HOST_MAX], was_n = o->ip_host_n;
	const u8 *b = inst_be;
	const char *unused;
	int i, j, n;

	if (!why)
		why = &unused;
	if (inst_len < 0 && mac_len < 0) {
		*why = "no IP hosts declared";
		return OMCI_UNI_DECL_ABSENT;
	}
	n = inst_len / 2;
	if (!b || inst_len <= 0 || (inst_len & 1) || n > OMCI_IP_HOST_MAX ||
	    !mac || mac_len != n) {
		*why = "the IP-host list is not 1..4 16-bit instances with one MAC flag each";
		return OMCI_UNI_DECL_BAD;
	}
	for (i = 0; i < n; i++) {
		inst[i] = (u16)((b[2 * i] << 8) | b[2 * i + 1]);
		for (j = 0; j < i; j++)
			if (inst[j] == inst[i]) {
				*why = "a duplicate IP-host instance";
				return OMCI_UNI_DECL_BAD;
			}
	}
	memcpy(was, o->ip_host, sizeof(was));
	memcpy(was_mac, o->ip_host_mac, sizeof(was_mac));
	memcpy(o->ip_host, inst, n * sizeof(*inst));
	memcpy(o->ip_host_mac, mac, n);
	o->ip_host_n = (u8)n;
	omci_build_mib(o);
	if (o->rows_dropped) {
		memcpy(o->ip_host, was, sizeof(was));
		memcpy(o->ip_host_mac, was_mac, sizeof(was_mac));
		o->ip_host_n = was_n;
		omci_build_mib(o);
		*why = "more MIB rows than the table holds";
		return OMCI_UNI_DECL_BAD;
	}
	*why = "declared";
	return OMCI_UNI_DECL_OK;
}

#define OMCI_CAP_BE_FIELDS 4

enum omci_uni_decl omci_onu_declare_queues_be(struct omci_onu *o,
					      const void *cap, int cap_len,
					      const void *dsq, int dsq_len,
					      const char **why)
{
	struct omci_capacity was = o->cap;
	const u8 *b = cap;
	u16 v[OMCI_CAP_BE_FIELDS], port[OMCI_DSQ_MAX];
	u8 n = 0;
	const char *unused;
	int f;

	if (!why)
		why = &unused;
	if (cap_len < 0 && dsq_len < 0) {
		*why = "no capacity or queue order declared";
		return OMCI_UNI_DECL_ABSENT;
	}
	if (cap_len >= 0) {
		if (!b || cap_len != 2 * OMCI_CAP_BE_FIELDS) {
			*why = "the capacity is not <T-CONTs priority-queues schedulers GEM-ports>";
			return OMCI_UNI_DECL_BAD;
		}
		for (f = 0; f < OMCI_CAP_BE_FIELDS; f++)
			v[f] = (u16)((b[2 * f] << 8) | b[2 * f + 1]);
		if (!v[0] || v[0] > OMCI_TCONT_MAX || !v[2] || v[2] > OMCI_TCONT_MAX) {
			*why = "a T-CONT or scheduler count outside 1..32";
			return OMCI_UNI_DECL_BAD;
		}
		o->cap.tcont_n = (u8)v[0];
		o->cap.onu2g_pq = v[1];
		o->cap.sched_n = (u8)v[2];
		o->cap.gem_ports = v[3];
		o->cap.usq = true;
	}
	if (dsq_len >= 0) {
		if (!dsq_len || !omci_uni_decode_list(dsq, dsq_len, port, &n) ||
		    !omci_uni_list_ok(port, n)) {
			o->cap = was;
			*why = "the downstream queue ports are not 1..8 distinct non-zero instances";
			return OMCI_UNI_DECL_BAD;
		}
		memcpy(o->cap.dsq_port, port, n * sizeof(*port));
		o->cap.dsq_n = n;
	}
	omci_build_mib(o);
	if (o->rows_dropped) {
		o->cap = was;
		omci_build_mib(o);
		*why = "more MIB rows than the table holds";
		return OMCI_UNI_DECL_BAD;
	}
	*why = "declared";
	return OMCI_UNI_DECL_OK;
}

/* One decimal cell of a unit capacity, up to @max. -> the next cell or NULL. */
static const char *omci_cap_cell(const char *s, unsigned int max, unsigned int *v)
{
	unsigned int n = 0, digits = 0;

	for (; *s >= '0' && *s <= '9'; s++, digits++) {
		n = n * 10 + (unsigned int)(*s - '0');
		if (n > max)
			return NULL;
	}
	*v = n;
	return digits ? s : NULL;
}

enum omci_uni_decl omci_onu_declare_unit_capacity(struct omci_onu *o,
						  const char *s, const char **why)
{
	struct omci_capacity was_cap;
	struct omci_slot was_slots[OMCI_SLOT_MAX];
	unsigned int tconts, pq, scheds, i;
	const char *unused;

	if (!why)
		why = &unused;
	if (!s || !*s || *s == '\n') {
		*why = "no unit capacity";
		return OMCI_UNI_DECL_ABSENT;
	}
	s = omci_cap_cell(s, OMCI_TCONT_MAX, &tconts);
	s = s && *s == ',' ? omci_cap_cell(s + 1, 255, &pq) : NULL;
	s = s && *s == ',' ? omci_cap_cell(s + 1, OMCI_TCONT_MAX, &scheds) : NULL;
	if (!s || (*s && *s != '\n') || !tconts || !scheds) {
		*why = "the unit capacity is not <T-CONTs 1..32>,<priority queues 0..255>,<schedulers 1..32>";
		return OMCI_UNI_DECL_BAD;
	}
	if (!o)
		return OMCI_UNI_DECL_OK;
	was_cap = o->cap;
	memcpy(was_slots, o->slots, sizeof(was_slots));
	o->cap.tcont_n = (u8)tconts;
	o->cap.onu2g_pq = (u16)pq;
	o->cap.sched_n = (u8)scheds;
	/* the pack that holds the T-CONT buffers is the upstream side */
	for (i = 0; i < o->slots_n; i++)
		if (o->slots[i].tcont_buf) {
			o->slots[i].tcont_buf = (u8)tconts;
			o->slots[i].priq = (u8)pq;
			o->slots[i].scheds = (u8)scheds;
		}
	omci_build_mib(o);
	if (o->rows_dropped) {
		o->cap = was_cap;
		memcpy(o->slots, was_slots, sizeof(was_slots));
		omci_build_mib(o);
		*why = "more MIB rows than the table holds";
		return OMCI_UNI_DECL_BAD;
	}
	*why = "declared";
	return OMCI_UNI_DECL_OK;
}

enum omci_uni_decl omci_onu_declare_pots_be(struct omci_onu *o, const void *be,
					    int len, const char **why)
{
	u16 inst[OMCI_UNI_MAX], was[OMCI_UNI_MAX];
	u8 n = 0, was_n = o->pots_uni_n;
	const char *unused;

	if (!why)
		why = &unused;
	if (len < 0) {
		*why = "no POTS UNIs declared";
		return OMCI_UNI_DECL_ABSENT;
	}
	if (!len || !omci_uni_decode_list(be, len, inst, &n) ||
	    !omci_uni_list_ok(inst, n)) {
		*why = "the POTS-UNI list is not 1..8 distinct non-zero 16-bit instances";
		return OMCI_UNI_DECL_BAD;
	}
	memcpy(was, o->pots_uni, sizeof(was));
	memcpy(o->pots_uni, inst, n * sizeof(*inst));
	o->pots_uni_n = n;
	omci_build_mib(o);
	if (o->rows_dropped) {
		memcpy(o->pots_uni, was, sizeof(was));
		o->pots_uni_n = was_n;
		omci_build_mib(o);
		*why = "more MIB rows than the table holds";
		return OMCI_UNI_DECL_BAD;
	}
	*why = "declared";
	return OMCI_UNI_DECL_OK;
}


enum omci_uni_decl omci_onu_declare_extra_me_be(struct omci_onu *o,
						const void *be, int len,
						const char **why)
{
	u16 was[OMCI_EXTRA_ME_MAX][2];
	u8 was_n = o->extra_me_n, n, i;
	const u8 *b = be;
	const char *unused;

	if (!why)
		why = &unused;
	if (len < 0) {
		*why = "no further MEs declared";
		return OMCI_UNI_DECL_ABSENT;
	}
	if (!b || !len || len % 4 || len > 4 * OMCI_EXTRA_ME_MAX) {
		*why = "the further MEs are not 1..8 <class instance> pairs";
		return OMCI_UNI_DECL_BAD;
	}
	n = (u8)(len / 4);
	memcpy(was, o->extra_me, sizeof(was));
	o->extra_me_n = 0;
	omci_build_mib(o);
	for (i = 0; i < n; i++) {
		u16 c = (u16)((b[4 * i] << 8) | b[4 * i + 1]);
		u16 inst = (u16)((b[4 * i + 2] << 8) | b[4 * i + 3]);

		if (!omci_me_find(c) || omci_row_listed(o, c, inst)) {
			o->extra_me_n = was_n;
			omci_build_mib(o);
			*why = "a further ME this model does not carry, or one already listed";
			return OMCI_UNI_DECL_BAD;
		}
		o->extra_me[i][0] = c;
		o->extra_me[i][1] = inst;
		o->extra_me_n = i + 1;
		omci_build_mib(o);
	}
	if (o->rows_dropped) {
		memcpy(o->extra_me, was, sizeof(was));
		o->extra_me_n = was_n;
		omci_build_mib(o);
		*why = "more MIB rows than the table holds";
		return OMCI_UNI_DECL_BAD;
	}
	*why = "declared";
	return OMCI_UNI_DECL_OK;
}

enum omci_uni_decl omci_onu_declare_ctc_be(struct omci_onu *o, const void *be,
					   int len, const char **why)
{
	if (len < 0) {
		*why = "the core's 0 1 2 1";
		return OMCI_UNI_DECL_ABSENT;
	}
	if (!be || len != sizeof(o->cap.ctc)) {
		*why = "the CTC capability is not <spec-version onu-type tx-power customized>";
		return OMCI_UNI_DECL_BAD;
	}
	memcpy(o->cap.ctc, be, sizeof(o->cap.ctc));
	*why = "declared";
	return OMCI_UNI_DECL_OK;
}

enum omci_uni_decl omci_onu_declare_usq_be(struct omci_onu *o, const void *per,
					   int len, const char **why)
{
	u8 was[OMCI_TCONT_MAX];
	const u8 *b = per;
	int t;

	if (len < 0) {
		*why = "every T-CONT holds 8 queues";
		return OMCI_UNI_DECL_ABSENT;
	}
	if (!b || len != o->cap.tcont_n) {
		*why = "the upstream queue counts are not one per T-CONT";
		return OMCI_UNI_DECL_BAD;
	}
	for (t = 0; t < len; t++) {
		if (!b[t] || b[t] > 8) {
			*why = "an upstream queue count outside 1..8";
			return OMCI_UNI_DECL_BAD;
		}
	}
	memcpy(was, o->cap.usq_per, sizeof(was));
	memcpy(o->cap.usq_per, b, len);
	omci_build_mib(o);
	if (o->rows_dropped) {
		memcpy(o->cap.usq_per, was, sizeof(was));
		omci_build_mib(o);
		*why = "more MIB rows than the table holds";
		return OMCI_UNI_DECL_BAD;
	}
	*why = "declared";
	return OMCI_UNI_DECL_OK;
}

const char *omci_onu_declare_equipment(struct omci_onu *o, omci_prop_fn prop,
				       void *ctx, const char **why)
{
	const char *first = NULL, *reason = NULL;
	const void *a, *b;
	int alen, blen;

#define OMCI_DECLARED(call, name) do {					\
		if ((call) == OMCI_UNI_DECL_BAD && !first) {			\
			first = (name);						\
			*why = reason;						\
		}								\
	} while (0)
	a = prop(ctx, "circuit-packs", &alen);
	OMCI_DECLARED(omci_onu_declare_slots_be(o, a, alen, &reason), "circuit-packs");
	a = prop(ctx, "ip-host-instances", &alen);
	b = prop(ctx, "ip-host-wan-mac", &blen);
	OMCI_DECLARED(omci_onu_declare_ip_hosts_be(o, a, alen, b, blen, &reason),
		      "ip-host-instances");
	a = prop(ctx, "pots-uni-instances", &alen);
	OMCI_DECLARED(omci_onu_declare_pots_be(o, a, alen, &reason),
		      "pots-uni-instances");
	a = prop(ctx, "onu-capacity", &alen);
	b = prop(ctx, "downstream-queue-ports", &blen);
	OMCI_DECLARED(omci_onu_declare_queues_be(o, a, alen, b, blen, &reason),
		      "onu-capacity");
	a = prop(ctx, "upstream-queues-per-tcont", &alen);
	OMCI_DECLARED(omci_onu_declare_usq_be(o, a, alen, &reason),
		      "upstream-queues-per-tcont");
	a = prop(ctx, "ctc-onu-capability", &alen);
	OMCI_DECLARED(omci_onu_declare_ctc_be(o, a, alen, &reason),
		      "ctc-onu-capability");
	a = prop(ctx, "extra-me-instances", &alen);
	OMCI_DECLARED(omci_onu_declare_extra_me_be(o, a, alen, &reason),
		      "extra-me-instances");
#undef OMCI_DECLARED
	return first;
}

struct omci_prop_view {
	omci_prop_fn prop;
	void *ctx;
};

/* The equipment readers take a NULL value as absent. */
static const void *omci_prop_null_absent(void *view, const char *name, int *len)
{
	const struct omci_prop_view *v = view;
	const void *p = v->prop(v->ctx, name, len);

	if (!p)
		*len = -1;
	return p;
}

void omci_onu_declare_panel(struct omci_onu *o, omci_prop_fn prop, void *ctx,
			    u8 *port, unsigned int port_cap, u8 *port_n,
			    struct omci_panel_decl *d)
{
	const void *pptp, *type, *unig, *cap, *ports;
	int pptp_len, type_len, unig_len, cap_len;
	struct omci_prop_view view = { prop, ctx };
	enum omci_uni_decl decl;
	const char *why = NULL;

	pptp = prop(ctx, "ethernet-uni-instances", &pptp_len);
	type = prop(ctx, "ethernet-uni-types", &type_len);
	unig = prop(ctx, "uni-g-instances", &unig_len);
	cap = prop(ctx, "uni-g-management-capability", &cap_len);
	ports = prop(ctx, "ethernet-uni-ports", &d->ports_len);
	decl = omci_onu_declare_unis_be(o, pptp, pptp_len, type, type_len, unig,
					unig_len, cap, cap_len, &why);
	d->unis_why = decl == OMCI_UNI_DECL_BAD ? why : NULL;
	d->unis = decl == OMCI_UNI_DECL_OK && pptp_len > 0 ? pptp_len / 2 : 0;
	*port_n = 0;
	if (d->unis && ports && d->ports_len == d->unis &&
	    d->ports_len <= (int)port_cap) {
		memcpy(port, ports, d->ports_len);
		*port_n = (u8)d->ports_len;
	}
	d->part_why = NULL;
	d->part = omci_onu_declare_equipment(o, omci_prop_null_absent, &view, &d->part_why);
}

void omci_onu_set_wan_mac(struct omci_onu *o, const u8 mac[6])
{
	memcpy(o->wan_mac, mac, sizeof(o->wan_mac));
	o->wan_mac_set = true;
}

void omci_onu_init(struct omci_onu *o, const u8 sn[8], u8 mds_seed)
{
	/* ⚠ THIS READS NOTHING OUT OF @o. It is a COLD init */
	memset(o, 0, sizeof(*o));
	memcpy(o->sn, sn, 8);
	omci_id_default(&o->id);
	omci_me_reset_values(o);
	o->mds = mds_seed;
	/* Seed the ANI-G optical levels with the static fallback */
	o->mds_adapt = true;
	o->mds_adapt_reads = OMCI_MDS_ADAPT_READS;
	o->anig_rx_level = OMCI_ANIG_RX_FALLBACK;
	o->anig_tx_level = OMCI_ANIG_TX_FALLBACK;
	o->slots[0] = omci_slot_default;
	o->slots_n = 1;
	o->cap.tcont_n = OMCI_TCONT_COUNT;
	o->cap.sched_n = OMCI_TCONT_COUNT;
	o->cap.onu2g_pq = 0x0060;
	o->cap.gem_ports = 0x0040;
	memcpy(o->cap.ctc, omci_ctc_default, sizeof(o->cap.ctc));
	o->cap.dsq_port[0] = 0x0101;
	o->cap.dsq_n = 1;
	omci_onu_declare_unis(o, omci_uni_default, NULL, 1,
			      omci_uni_default, NULL, 1);
}

/* Everything the family still owes a port after the model was reset under it:
 * every slot that WAS locked, because the reset only unlocks the report, plus
 * anything that was flagged and not yet drained, because a later same-value
 * Set or a replayed PDU will never recreate a consumed bit. */
static void omci_uni_carry_obligations(struct omci_uni_inv *now,
				       const struct omci_uni_inv *was)
{
	u8 i;

	now->changed |= was->changed;
	for (i = 0; i < now->n && i < was->n; i++)
		if (was->admin[i])
			now->changed |= (u8)(1u << i);
}

void omci_onu_reinit(struct omci_onu *o, const u8 sn[8], u8 mds_seed)
{
	/* Safe to read: @o is LIVE here by this function's contract, which is
	 * the whole reason it is not the same function as omci_onu_init(). */
	struct omci_uni_inv pptp = o->pptp_eth_uni, unig = o->uni_g;
	struct omci_identity id = o->id;	/* the unit's, not the session's */
	struct omci_slot slots[OMCI_SLOT_MAX];
	u8 slots_n = o->slots_n;
	u16 ip_host[OMCI_IP_HOST_MAX];
	u8 ip_host_mac[OMCI_IP_HOST_MAX], ip_host_n = o->ip_host_n;
	u16 pots_uni[OMCI_UNI_MAX];
	u8 pots_uni_n = o->pots_uni_n;
	u16 extra_me[OMCI_EXTRA_ME_MAX][2];
	u8 extra_me_n = o->extra_me_n;
	struct omci_capacity capacity = o->cap;
	u8 wan_mac[6];
	bool wan_mac_set = o->wan_mac_set;
	u8 cap[OMCI_UNI_MAX], type[OMCI_UNI_MAX];

	memcpy(cap, o->uni_g_mgmt_cap, sizeof(cap));
	/* ★ THE TYPE IS CARRIED TOO.  It is the panel's, not the session's: an
	 *   identity change does not turn an FE port into a GE one. */
	memcpy(type, o->uni_type, sizeof(type));
	memcpy(slots, o->slots, sizeof(slots));
	memcpy(ip_host, o->ip_host, sizeof(ip_host));
	memcpy(ip_host_mac, o->ip_host_mac, sizeof(ip_host_mac));
	memcpy(pots_uni, o->pots_uni, sizeof(pots_uni));
	memcpy(extra_me, o->extra_me, sizeof(extra_me));
	memcpy(wan_mac, o->wan_mac, sizeof(wan_mac));
	omci_onu_init(o, sn, mds_seed);
	o->id = id;
	memcpy(o->slots, slots, sizeof(slots));
	o->slots_n = slots_n;
	memcpy(o->ip_host, ip_host, sizeof(ip_host));
	memcpy(o->ip_host_mac, ip_host_mac, sizeof(ip_host_mac));
	o->ip_host_n = ip_host_n;
	memcpy(o->pots_uni, pots_uni, sizeof(pots_uni));
	o->pots_uni_n = pots_uni_n;
	memcpy(o->extra_me, extra_me, sizeof(extra_me));
	o->extra_me_n = extra_me_n;
	o->cap = capacity;
	memcpy(o->wan_mac, wan_mac, sizeof(wan_mac));
	o->wan_mac_set = wan_mac_set;
	/* ⚠ UNCONDITIONALLY, INCLUDING WITH BOTH LISTS EMPTY.  Guarding this on
	 * "something was declared" made a board that legally declares no UNI at
	 * all come back with the default 0x0101 on both classes -- an instance
	 * the board does not have, invented by the identity change. */
	omci_onu_declare_unis(o, pptp.inst, type, pptp.n, unig.inst, cap, unig.n);
	omci_uni_carry_obligations(&o->pptp_eth_uni, &pptp);
	omci_uni_carry_obligations(&o->uni_g, &unig);
}

/* Does this ONU's MIB hold any instance of the class: an upload row or one the
 * OLT created?  Then a Get on another instance is answered "unknown instance". */
bool omci_mib_has_class(struct omci_onu *o, u16 class_id)
{
	u16 i;

	if (omci_store_has_class(o, class_id))
		return true;
	for (i = 0; i < o->nrows; i++)
		if (o->rows[i].class_id == class_id)
			return true;
	return false;
}

/* Is (class, inst) a MIB instance this ONU holds?  Three sources: the static
 * auto-instantiated set (== the MIB-Upload rows, which is what the OLT learned
 * from us), any vendor-reserved class (we model no attributes but the OLT is
 * entitled to address them), and anything the OLT itself created. */
bool omci_inst_exists(struct omci_onu *o, u16 class_id, u16 inst)
{
	u16 i;

	if (omci_vendor_class(class_id))
		return true;
	if (omci_store_find(o, class_id, inst))
		return true;
	for (i = 0; i < o->nrows; i++)
		if (o->rows[i].class_id == class_id && o->rows[i].inst == inst)
			return true;
	return false;
}

/* THE WAN DATA GEM */

enum omci_dgem omci_dgem_classify(const u8 *body, u8 blen,
				  u16 omcc_gem, u16 mcast_gem, u16 *port_id)
{
	u16 g;

	/* attr 1..3 need 5 octets; a runt Create carries no direction and must
	 * never be read past -- an unfuzzable implicit length is precisely what
	 * this project refuses. */
	if (!body || blen < 5)
		return OMCI_DGEM_RUNT;

	/* explicit byte math: big-endian on the wire, and this same source is
	 * compiled for MIPS-BE, ARM64-LE and x86. */
	g = (u16)(((u16)body[0] << 8) | body[1]) & 0x0fff;

	if (!g)
		return OMCI_DGEM_ZERO;
	if (g == (omcc_gem & 0x0fff))
		return OMCI_DGEM_IS_OMCC;
	if (g == (mcast_gem & 0x0fff))
		return OMCI_DGEM_IS_MCAST;
	if (body[4] != GPON_GEM_BIDIR)
		return OMCI_DGEM_NOT_BIDIR;

	if (port_id)
		*port_id = g;
	return OMCI_DGEM_YES;
}

const char *omci_dgem_name(enum omci_dgem v)
{
	switch (v) {
	case OMCI_DGEM_YES:		return "data GEM";
	case OMCI_DGEM_RUNT:		return "runt Create";
	case OMCI_DGEM_ZERO:		return "Port-ID 0";
	case OMCI_DGEM_IS_OMCC:		return "the OMCC GEM";
	case OMCI_DGEM_IS_MCAST:	return "the multicast GEM";
	case OMCI_DGEM_NOT_BIDIR:	return "uni-directional CTP";
	}
	return "?";
}

void omci_data_binding_snapshot(const struct omci_onu *o, u16 omcc_gem,
				u16 mcast_gem, struct omci_data_binding *binding)
{
	u16 i;

	if (!binding)
		return;
	memset(binding, 0, sizeof(*binding));
	if (!o)
		return;
	for (i = 0; i < OMCI_STORE_MAX; i++) {
		const struct omci_me_inst *e = &o->store[i];
		u16 port_id, tcont;

		if (!e->used || e->class_id != OMCI_ME_GEM_CTP ||
		    omci_dgem_classify(e->body, e->blen, omcc_gem, mcast_gem,
				       &port_id) != OMCI_DGEM_YES)
			continue;
		binding->gem_present = true;
		binding->gem_inst = e->inst;
		binding->gem_port = port_id;
		binding->direction = e->body[4];
		tcont = ((u16)e->body[2] << 8) | e->body[3];
		binding->tcont_inst = tcont;
		if (tcont >= 0x8000 && tcont < 0x8000 + o->cap.tcont_n &&
		    (o->tcont_alloc_written & (1u << (tcont - 0x8000)))) {
			binding->alloc_known = true;
			binding->alloc_id = o->tcont_alloc[tcont - 0x8000];
		}
		return;
	}
}

bool omci_data_gem_port(struct omci_onu *o, u16 omcc_gem, u16 mcast_gem,
			u16 *port_id)
{
	struct omci_data_binding binding;

	omci_data_binding_snapshot(o, omcc_gem, mcast_gem, &binding);
	if (binding.gem_present && port_id)
		*port_id = binding.gem_port;
	return binding.gem_present;
}

/* ME 262 T-CONT snoop: the parse (Set-with-mask vs Create SBC layout) and the
 * against-the-shadow decision, hoisted from cortina-gpon.c cg_rx_omci Stage D
 * bit-for-bit (its guard was len >= 12, i.e. blen >= 4 -- kept exactly, so a
 * 2-octet Create is refused, never read). */
enum omci_tcont_verdict omci_tcont_snoop(u8 mt, const u8 *body,
					 unsigned int blen, u16 inst,
					 u16 cur_alloc, u16 cur_inst,
					 u16 *alloc)
{
	u8 m = mt & 0x1f;
	u16 a = 0;

	if (!body || blen < 4)
		return OMCI_TCONT_NONE;

	if (m == OMCI_MT_SET && ((((u16)body[0] << 8) | body[1]) & 0x8000))
		a = ((u16)body[2] << 8) | body[3];	/* attr 1, via the mask */
	else if (m == OMCI_MT_CREATE)
		a = ((u16)body[0] << 8) | body[1];	/* SBC: alloc first */

	/* 0xffff IS THE G.988 DEALLOCATE, NOT NOISE (2026-08-05). */
	if (a == 0xffff && cur_alloc && (!cur_inst || inst == cur_inst))
		return OMCI_TCONT_DEALLOC;
	if (a && a != 0xffff && a != cur_alloc) {
		if (alloc)
			*alloc = a;
		return OMCI_TCONT_ALLOC;
	}
	return OMCI_TCONT_NONE;
}

bool omci_dgem_delete(u8 mt, u16 inst, u16 cur_gem, u16 cur_inst)
{
	if ((mt & 0x1f) != OMCI_MT_DELETE)
		return false;
	return cur_gem && (!cur_inst || inst == cur_inst);
}

/* THE WAN SERVICE SPINE */
static bool omci_store_attr(struct omci_onu *o, u16 class_id, u16 inst,
			    u8 attr, u32 *out)
{
	const struct omci_attr *a;
	struct omci_me_inst *e = omci_store_find(o, class_id, inst);
	unsigned int i;
	u32 v = 0;

	if (!e)
		return false;
	for (a = omci_me_find(class_id); a && a->class_id == class_id; a++) {
		if (a->attr != attr || a->src != OMCI_SRC_STORE)
			continue;
		if (a->size > sizeof(v) || (unsigned int)a->v + a->size > e->blen)
			return false;
		for (i = 0; i < a->size; i++)
			v = (v << 8) | e->body[a->v + i];
		*out = v;
		return true;
	}
	return false;
}

/* The first instance of @class_id whose attribute @attr holds @val. */
static bool omci_store_by_attr(struct omci_onu *o, u16 class_id, u8 attr,
			       u32 val, u16 *inst_out)
{
	u16 k;

	for (k = 0; k < OMCI_STORE_MAX; k++) {
		u32 got;

		if (!o->store[k].used || o->store[k].class_id != class_id)
			continue;
		if (omci_store_attr(o, class_id, o->store[k].inst, attr, &got) &&
		    got == val) {
			*inst_out = o->store[k].inst;
			return true;
		}
	}
	return false;
}

u8 omci_service_resolve(struct omci_onu *o, u16 gem_port,
			struct omci_service_path *p)
{
	u32 v;

	if (!o || !p)
		return 0;
	memset(p, 0, sizeof(*p));
	p->gem_port = gem_port;

	if (!omci_store_by_attr(o, OMCI_ME_GEM_CTP, 1, gem_port, &p->gem_ctp))
		return p->have;
	p->have |= OMCI_SVC_GEM_CTP;

	if (!omci_store_by_attr(o, OMCI_ME_GEM_IW_TP, 1, p->gem_ctp, &p->iw_tp))
		return p->have;
	p->have |= OMCI_SVC_IW_TP;
	if (omci_store_attr(o, OMCI_ME_GEM_IW_TP, p->iw_tp, 2, &v))
		p->iw_option = (u16)v;
	if (omci_store_attr(o, OMCI_ME_GEM_IW_TP, p->iw_tp, 7, &v)) {
		p->gal_prof = (u16)v;
		if (omci_store_attr(o, OMCI_ME_GAL_ETH_PROF, p->gal_prof, 1, &v)) {
			p->gal_payload = (u16)v;
			p->have |= OMCI_SVC_GAL;
		}
	}

	if (!omci_store_by_attr(o, OMCI_ME_MAC_BRIDGE_PORT, 4, p->iw_tp,
				&p->bridge_port)) {
		/* ★ THE MAPPER ROUTE.  No bridge port names this ME 266
		 *   directly, so look for an 802.1p mapper service profile that
		 *   does: ME 130's eight P-bit pointers are attributes #2..#9,
		 *   and a bridge port then points at the MAPPER instead. */
		u8 pbit;

		for (pbit = 2; pbit <= 9; pbit++)
			if (omci_store_by_attr(o, OMCI_ME_PBIT_MAPPER, pbit,
					       p->iw_tp, &p->mapper))
				break;
		if (pbit > 9 ||
		    !omci_store_by_attr(o, OMCI_ME_MAC_BRIDGE_PORT, 4,
					p->mapper, &p->bridge_port)) {
			p->mapper = 0;
			return p->have;
		}
		p->pbit = (u8)(pbit - 2);	/* #2 is P-bit 0 */
		p->have |= OMCI_SVC_PBIT_MAPPER;
	}
	p->have |= OMCI_SVC_BRIDGE_PORT;
	if (omci_store_attr(o, OMCI_ME_MAC_BRIDGE_PORT, p->bridge_port, 3, &v))
		p->tp_type = (u16)v;

	if (!omci_store_attr(o, OMCI_ME_MAC_BRIDGE_PORT, p->bridge_port, 1, &v))
		return p->have;
	p->bridge = (u16)v;
	/* ★ THE BRIDGE MUST EXIST, not merely be POINTED AT.  An ME 47 whose
	 *   bridge-ID pointer names an ME 45 the OLT never created is a dangling
	 *   link, and reporting it as a resolved bridge would be this model
	 *   asserting a service the OLT has not finished expressing. */
	if (omci_store_find(o, OMCI_ME_MAC_BRIDGE_SVC, p->bridge))
		p->have |= OMCI_SVC_BRIDGE;
	return p->have;
}

u16 omci_service_bridge_of_gem(struct omci_onu *o, u16 gem_port)
{
	struct omci_service_path p;

	if (omci_service_resolve(o, gem_port, &p) & OMCI_SVC_BRIDGE)
		return p.bridge;
	return 0;
}
