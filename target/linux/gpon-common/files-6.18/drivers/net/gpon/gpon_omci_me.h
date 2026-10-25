/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TIER: CORE (prefix gpon_) — decides, never touches hardware, and compiles for
 * MIPS-BE, ARM64-LE and x86.  Canonical tier rule and guard: see "THE THREE
 * TIERS" in gpon_common.h (this directory).
 *
 * gpon_omci_me.h — the ITU-T G.988 MANAGED-ENTITY model and MIB store: which
 * managed entities this ONU holds and what their attribute bytes are.  Three
 * things, and nothing else: the TABLE-DRIVEN attribute descriptors (one row per
 * class+attribute) with the one generic filler that walks them; the STATIC
 * MIB-Upload row table; and the DYNAMIC store of the instances the OLT itself
 * created, plus the context struct that holds all of it.
 *
 * It does NOT parse a PDU, dispatch a message type, build an envelope, stamp a
 * trailer or compute a MIC — that is gpon_omci_core.{h,c}, which CALLS this
 * one.  G.988 message rules are identical on every ONU ever built; the set of
 * MEs a product serves is a property of the PRODUCT.
 *
 * Table-driven is a FOOTPRINT decision (<=64 MB RAM, small NAND) and it makes
 * three cross-vendor invariants STRUCTURAL rather than per-case: every reply
 * bounded by the Get value area; the attribute set of an ME derivable, so a Get
 * NAMES what it does not support and what did not fit instead of answering
 * success with a short mask and generating OLT re-GET churn; and one policy per
 * vendor-reserved class RANGE instead of a list of the class IDs one OLT asked
 * for.  That policy is the unit's STOCK answer: result 0, every mask zero
 * (field X111W on a production OLT, 2026-10-02).
 *   Nothing here was altered to accommodate it.
 *
 * ENDIANNESS: every attribute value is emitted big-endian by explicit byte math
 * (omci_attr_bytes() in the .c), never a cast over wire bytes.
 */
#ifndef GPON_OMCI_ME_H
#define GPON_OMCI_ME_H

#include <linux/types.h>

/* The MESSAGE layer's facts: OMCI_LEN, the result codes omci_me_fill() returns,
 * OMCI_ATTR_BIT().  It forward-declares struct omci_onu, which this header then
 * defines, so the include runs one way only and the two never cycle. */
#include "gpon_omci_core.h"

/* The VLAN / classification model this store feeds: ME 171's row table needs
 * storage of its own (one instance holds MANY 16-octet rows, which no dense
 * body can), and the other six decode straight out of their dense bodies. */
#include "gpon_omci_vlan.h"

/* A capacity is a per-board value; the logic is common.
 * ★★ 64 WAS TOO SMALL AND IT COST THE X400AXF ITS WAN (MEASURED 2026-09-15).
 * The lab OLT's script creates 54 instances, SEVEN of them MAC bridge ports, and
 * a bridge port costs FOUR slots (ME 47 + its ME 50/79/49 companions) -- 75,
 * which the board's own `store=75` confirms.  At 64 the sixth Create is NAKed
 * rc=9, the OLT abandons provisioning THERE and never reaches the data
 * T-CONT/GEM, so the board holds O5 with no WAN while the OLT shows
 * `fail`/`Initial`/`Laser out`.  One number for both families now: the store is
 * 32 bytes an entry, so the whole array is 4 kB.
 * ⚠ It said 47 instances / six ports / 65 slots and "ME 50/79/84" until later
 * the same day: both read off the capture the overflow itself TRUNCATED.
 * Pinned by rtl9607c-test/omci_olt_script_fits_test, whose fixture is GENERATED
 * from the capture and refuses one that records a failed script. */
#ifndef OMCI_STORE_MAX
#define OMCI_STORE_MAX 128
#endif

/* A dynamic ME instance the OLT provisioned (Create). */
#define OMCI_STORE_BODY 26

struct omci_me_inst {
	u16	class_id;
	u16	inst;
	u8	body[OMCI_STORE_BODY];	/* dense attributes for a modelled
					 * class, opaque for the rest */
	u8	blen;
	bool	used;
};

/* One MIB-Upload-Next row: (class, instance, attr-mask) whose selected
 * attributes fit the 26-byte Upload-Next value area. */
struct omci_mib_row {
	u16	class_id;
	u16	inst;
	u16	mask;
};

/* Overridable for the same reason as OMCI_STORE_MAX above.  Stock uploads a
 * priority queue in TWO rows, so the X400AXF's 40 DS + 240 US queues alone
 * take 560, beside its 30 T-CONTs and schedulers; 6 octets a row. */
#ifndef OMCI_MIB_ROWS_MAX
#define OMCI_MIB_ROWS_MAX 768
#endif

/* The auto-instantiated T-CONT range: the default count, and the most a board
 * may declare (its stock's ANI-G #2). */
#define OMCI_TCONT_COUNT 12
#define OMCI_TCONT_MAX 32
/* Downstream priority-queue blocks a board may declare, 8 queues each. */
#define OMCI_DSQ_MAX 8

/* The capacity the OLT reads, as the board's stock declares it: T-CONTs (ME
 * 262 rows, ANI-G #2), schedulers (ME 278 rows, ONU2-G #7), ONU2-G #6 and
 * #9, and the priority queues: 8 per downstream port in dsq_port order, and 8
 * per T-CONT upstream when usq is set. */
struct omci_capacity {
	u8	tcont_n;
	u8	sched_n;
	u16	onu2g_pq;
	u16	gem_ports;
	u16	dsq_port[OMCI_DSQ_MAX];
	u8	dsq_n;
	bool	usq;
	u8	usq_per[OMCI_TCONT_MAX];	/* upstream queues per T-CONT, 0 = 8 */
	u8	ctc[4];			/* ME 65529 #2..#5 */
};

/* The one VEIP every board's model carries. */
#define OMCI_VEIP_INST 0x0601

/* One equipment slot, as the board's own stock reports it: a cardholder (ME 5)
 * and its circuit pack (ME 6).  The OLT counts the board's Ethernet and POTS
 * ports from these, and its priority queues from the ME 277 rows they promise:
 * a pack with T-CONT buffers is the upstream side, so its queues are the
 * upstream ones.  Nothing here is computed from a model name. */
struct omci_slot {
	u16	inst;
	u8	type;		/* G.988 plug-in unit type */
	u8	ports;
	u8	priq;		/* ME 6 #12 */
	u8	tcont_buf;	/* ME 6 #11 */
	u8	scheds;		/* ME 6 #13 */
};
#define OMCI_SLOT_MAX 8

/* ME 134 IP host config data instances a board declares. */
#define OMCI_IP_HOST_MAX 8

/* Further single MEs a board's stock reports, as (class, instance) pairs. */
#define OMCI_EXTRA_ME_MAX 32

/* ME 131 OLT-G as the OLT last wrote it: vendor (4), equipment ID (20),
 * version (14), time of day (14). */
#define OMCI_OLT_G_LEN 52

/* What this UNIT tells the OLT it is.  A production OLT may match any of these
 * against its provisioning (equipment ID -> ONT type, LOID -> subscriber), so
 * they are per unit, from the unit's own factory MIB exactly like stock's
 * omci_app (runomci.sh: `mib get` of the key named on each line).  Every
 * member is zero-padded to its G.988 wire size; `zeros` must stay LAST. */
struct omci_identity {
	u8 vendor_id[4];	/* ONU-G #1, Circuit-Pack #5   PON_VENDOR_ID */
	u8 onu_g_version[14];	/* ONU-G #2                    HW_HWVER */
	u8 sw_bank0_version[14];/* SW-image 0 #1, C-Pack #4    OMCI_SW_VER1 */
	u8 sw_bank1_version[14];/* SW-image 1 #1               OMCI_SW_VER2 */
	u8 equipment_id[20];	/* ONU2-G #1                   GPON_ONU_MODEL */
	u8 product_code[2];	/* ONU2-G #3, big-endian       OMCI_VENDOR_PRODUCT_CODE */
	u8 loid[24];		/* ONU-G #10, CTC LoID #2      LOID */
	u8 loid_passwd[12];	/* ONU-G #11, CTC LoID #3      LOID_PASSWD */
	u8 operator_id[4];	/* CTC LoID #1 */
	u8 omcc_version[1];	/* ONU2-G #2, baseline codes only; 0 = the far end's */
	u8 farend[8];		/* "" = detected, else a far-end profile's name */
	u8 zeros[25];		/* all-zero source, == its longest consumer */
};

/* THE list of what a unit provisions: the enum, the wire layout, the names and
 * each family's interface (/proc/gpon "id", Luna's onu_<name> parameters, the
 * omci_id_fields line userspace iterates) all derive from it.
 * X(id, name, member, numeric) */
#define OMCI_ID_LIST(X)							\
	X(VENDOR,	vendor_id,	vendor_id,		false)	\
	X(HW_VERSION,	hw_ver,		onu_g_version,		false)	\
	X(SW_VERSION0,	sw_ver1,	sw_bank0_version,	false)	\
	X(SW_VERSION1,	sw_ver2,	sw_bank1_version,	false)	\
	X(EQUIPMENT,	model,		equipment_id,		false)	\
	X(PRODUCT_CODE,	product_code,	product_code,		true)	\
	X(LOID,		loid,		loid,			false)	\
	X(LOID_PASSWD,	loid_passwd,	loid_passwd,		false)	\
	X(OMCC_VERSION,	omcc_version,	omcc_version,		true)	\
	X(FAREND,	farend,		farend,			false)

enum omci_id_field {
#define OMCI_ID_ENUM(id, name, member, numeric) OMCI_ID_##id,
	OMCI_ID_LIST(OMCI_ID_ENUM)
#undef OMCI_ID_ENUM
	OMCI_ID_FIELDS
};

/* The /proc/gpon line userspace reads the field names from, in list order. */
#define OMCI_ID_WORD(id, name, member, numeric) " " #name
#define OMCI_ID_FIELDS_LINE "omci_id_fields:" OMCI_ID_LIST(OMCI_ID_WORD)

/* How many instances of ONE administratively-controlled UNI class this model
 * carries.  MEASURED in the two boards' own captured stock MIBs: ME 11 has 4
 * rows on the G24W and 4 on the X400AXF, ME 264 has 6 and 5. */
#define OMCI_UNI_MAX 8

/* The instances of ONE UNI class the ONU presents, and */
struct omci_uni_inv {
	u16	inst[OMCI_UNI_MAX];	/* instance ids, [0, n) valid */
	u8	admin[OMCI_UNI_MAX];	/* G.988: 0 unlocked, 1 locked */
	u8	n;
	/* Bit i: slot i's administrative state MOVED and the family has not
	 * drained it yet.  A separate word rather than a compare against a
	 * shadow copy, because a re-Set to the value already held must NOT
	 * re-apply and a MIB-Reset back to unlocked MUST. */
	u8	changed;
};

struct omci_onu {
	u8	sn[8];			/* PLOAM serial number (vendor+VSSN) */
	struct omci_identity	id;	/* survives omci_onu_reinit() */
	u8	mds;			/* ME 2 attr 1: MIB-Data-Sync */
	/* The two UNI inventories. */
	struct omci_uni_inv	pptp_eth_uni;	/* ME 11, attribute 5 */
	struct omci_uni_inv	uni_g;		/* ME 264, attribute 2 */
	/* ME 264 #3 management capability, per uni_g slot. */
	u8	uni_g_mgmt_cap[OMCI_UNI_MAX];
	/* ME 11 #1/#2, PER INSTANCE. */
	u8	uni_type[OMCI_UNI_MAX];
	/* ME 11 #1/#8/#10 as the OLT last wrote them, per instance; a MIB reset
	 * restores expected = sensed, 1518 and 0xffff. */
	u8	uni_expected[OMCI_UNI_MAX];
	u16	uni_max_frame[OMCI_UNI_MAX];
	u16	uni_pause[OMCI_UNI_MAX];
	/* the equipment slots; survives omci_onu_reinit() like the panel */
	struct omci_slot	slots[OMCI_SLOT_MAX];
	u8	slots_n;
	/* ME 134 instances, and which report the WAN MAC (stock's do) */
	u16	ip_host[OMCI_IP_HOST_MAX];
	u8	ip_host_mac[OMCI_IP_HOST_MAX];
	u8	ip_host_n;
	u16	pots_uni[OMCI_UNI_MAX];	/* ME 53, as stock reports them */
	u8	pots_uni_n;
	u16	extra_me[OMCI_EXTRA_ME_MAX][2];	/* class, instance */
	u8	extra_me_n;
	u8	olt_g[OMCI_OLT_G_LEN];
	u8	olt_vendor_seen[4];	/* ME 131 #1 as last written; survives MIB-Reset */
	u8	farend;			/* enum omci_farend: its MEs; MIB-Reset and ME 131 */
	u8	omcc_farend;		/* the one whose OMCC version was uploaded */
	u8	wan_mac[6];		/* the WAN netdev's, set by the shell */
	bool	wan_mac_set;		/* unset: ME 134 #2 is not answered */
	/* MIB-Upload rows the row table had no room for.  ⚠ THE BUILDER DROPS
	 * THEM SILENTLY, and a dropped row is an instance the OLT never learns
	 * exists and therefore never provisions -- a whole subscriber port
	 * missing, with every message on the wire succeeding. */
	u16	rows_dropped;
	u16	tcont_alloc[OMCI_TCONT_MAX];
	u32	tcont_alloc_written;	/* accepted attr-1 Set, not a boot default */
	struct omci_capacity	cap;	/* survives omci_onu_reinit() */
	/* ⚠ THE ADAPTIVE MDS WALK's state.  It lived in the Luna shell and was
	 * DELETED by the 2026-08-27 responder deduplication (836b76be01) --
	 * deleted, not moved -- so omci_mds_provisionable_test has been RED on
	 * twelve arms ever since.  A container moved without its consumers. */
	u16	audit_reads;		/* DS OMCI reads since the OLT last PROVISIONED */
	u8	mds_tries;		/* how far the adaptive MDS walk has stepped */
	/* The walk's knobs, set by the shell at init so a bisect can turn the
	 * shipped behaviour off.  They live HERE, not in the shell's call, so
	 * the "GET arm and nowhere else" property is inside the core where a
	 * guard can read it. */
	bool	mds_adapt;
	u16	mds_adapt_reads;
	u16	nrows;			/* static MIB row count */
	u16	store_n;		/* provisioned-ME count */
	struct omci_mib_row	rows[OMCI_MIB_ROWS_MAX];
	struct omci_me_inst	store[OMCI_STORE_MAX];
	/* ME 171's row table.  It is NOT in the store above because one ME 171
	 * instance carries many 16-octet rows and the dense body holds 26
	 * octets total -- the subscriber VLAN simply does not fit the shape
	 * every other modelled ME uses. */
	struct gpon_vlan_model	vlan;
	/* G.988 11.2.2.1 and B.2.1 retained last response, one per priority
	 * (TCI bit 15): each priority is stop-and-wait, so a byte-identical
	 * repeat is REPLAYED from here instead of re-executed -- otherwise a
	 * lost US response bumps MIB-Data-Sync twice for one OLT transaction. */
	u8	last_req[2][40];	/* bytes 0..39 (trailer+MIC derived) */
	u8	last_resp[2][OMCI_LEN];
	bool	have_last[2];
	/* spy counters (dump/probe capability is first-class, project rule) */
	u32	unhandled;		/* DS message types with no ONU action */
	u32	dup_replay;		/* retransmissions served from the cache */
	u32	rx_extended;		/* devid 0x0b frames seen (not served) */
	u32	rx_bad_mic;		/* DS frames DISCARDED on an invalid MIC */
	u32	rx_runt;		/* DS frames shorter than a 48-byte baseline PDU */
	u32	rx_total;		/* DS frames handed to the responder */
	u32	mic_conv_ok;		/* first OMCI_MIC_SELFCHECK_N: AAL5-BE MIC */
	u32	mic_conv_bad;		/* first OMCI_MIC_SELFCHECK_N: any other */
	u32	no_ack;			/* requests with AR clear: applied, not
					 * answered — a silent path must still be
					 * countable */
	u32	avc_count;		/* autonomous AVC frames emitted */
	bool	avc_veip_up_sent;
	/* First 16 G.988 alarm bits, MSB first; one reporting ME. */
	u16	alarm_class;	/* the ME the conditions belong to (0 = none) */
	u16	alarm_inst;
	u16	alarm_active;	/* what the SILICON asserts right now */
	u16	alarm_told;	/* what the OLT has been told -- the EDGE */
	u8	alarm_seq;	/* G.988 alarm sequence number, wraps at 255 */
	u32	alarm_emitted;	/* spy counter: autonomous alarms sent */
	u16	alarm_snapshot_class;
	u16	alarm_snapshot_inst;
	u16	alarm_snapshot_bits;
	/* ME 263 ANI-G #10 RX / #14 TX optical level in the */
	u16	anig_rx_level;
	u16	anig_tx_level;
	bool	anig_live;
	u8	anig_threshold[4];	/* RX low/high, TX low/high */
	/* ANI-G self test acknowledged and not yet answered with its result */
	bool	selftest_pending;
	u16	selftest_tci;
	u16	selftest_inst;
	u32	selftest_sent;
};

/* The static ANI-G optical levels served until (and after a failed) DDM read:
 * -8.77 dBm received, +2.47 dBm launched.  Both plausible for this class-B+
 * optic, which is exactly why they must be labelled — a fabricated value that
 * looks right is the hardest kind to notice. */
#define OMCI_ANIG_RX_FALLBACK	0xeedc
#define OMCI_ANIG_TX_FALLBACK	0x04d7

/* Publish a live optical measurement into ME 263 #10/#14.  The caller does the
 * SLEEPING i2c read outside whatever lock guards the responder and passes the
 * two already-converted wire values in. */
void omci_onu_set_optical(struct omci_onu *o, u16 rx_level, u16 tx_level);

/* G.988 9.2.1: SF/SD are BER alarms, not LOS/LOF. */
#define OMCI_ANIG_RX_LOW		0x8000u
#define OMCI_ANIG_RX_HIGH		0x4000u
#define OMCI_ANIG_TX_LOW		0x0800u
#define OMCI_ANIG_TX_HIGH		0x0400u

/* Re-provision the G.984.3 ONU serial after init. */
static inline void omci_onu_set_sn(struct omci_onu *o, const u8 sn[8])
{
	unsigned int i;	/* a loop, not memcpy(): this header includes only
			 * <linux/types.h>, the same declared deviation the
			 * flowcore tier already takes for ether_addr_copy */
	for (i = 0; i < 8; i++)
		o->sn[i] = sn[i];
}

/* The build default every ONU model is seeded with until a unit provisions. */
void omci_id_default(struct omci_identity *id);

/* Provision one identity member from @len bytes, zero-padded to its wire size.
 * Refused (false, member unchanged) when @len exceeds it, when a numeric member
 * is not given its exact size, or an OMCC version announces the extended
 * message set this core does not serve.  @id NULL only validates. */
bool omci_id_set(struct omci_identity *id, enum omci_id_field f,
		 const u8 *val, unsigned int len);

/* The same from text, as a provisioning script hands it over: @s up to its
 * first newline, a numeric member as strtol(s, NULL, 0) reads it (stock's MIB
 * product code "31" is 0x001f). */
bool omci_id_set_str(struct omci_identity *id, enum omci_id_field f,
		     const char *s);

/* What a vendor's OLT needs of the ONU beyond G.988 -- the OMCC version it is
 * shown, the MEs it provisions on every ONU -- in ONE table for every board,
 * chosen at each MIB-Reset, and again for its MEs when the OLT writes ME 131,
 * from, in order: the unit's identity "farend" (the operator's choice), its own
 * vendor (a clone), the vendor the OLT last wrote in ME 131.  A board never
 * carries a protocol adaptation of its own. */
enum omci_farend {
	OMCI_FAREND_GENERIC,
#ifdef CONFIG_OLT_ZTE
	OMCI_FAREND_ZTE,
#endif
	OMCI_FAREND_N
};

/* The far end this ONU answers as, by name ("generic", "zte"). */
const char *omci_farend_name(const struct omci_onu *o);
int omci_farend_fmt(const struct omci_onu *o, char *out, size_t sz);

/* The field named @name provisioned from text. -> the field, or OMCI_ID_FIELDS
 * when the name is unknown or the value refused.  @id NULL only validates. */
enum omci_id_field omci_id_set_named(struct omci_identity *id, const char *name,
				     const char *s);

/* Each field's name, the rtk_factory verb that reads it from the MIB. */
extern const char *const omci_id_names[OMCI_ID_FIELDS];

/* ME class IDs presented in the MIB upload */
#define OMCI_ME_ONU_DATA	2
#define OMCI_ME_CARDHOLDER	5
#define OMCI_ME_CIRCUIT_PACK	6
#define OMCI_ME_SW_IMAGE	7
#define OMCI_ME_PPTP_ETH_UNI	11	/* THE HGU gate: the OLT's
					 * gpon_ont_sync_capability counts these */
/* ★★ THE WAN SERVICE SPINE (G.988 clause 9.3): the classes through which an
 * OLT says WHERE a GEM port goes.  268 names the GEM, 266 interworks it, 47
 * hangs it on a bridge port and 45 is the bridge; 272 carries the GEM payload
 * ceiling and 50/52 are the bridge port's read-back views.  Until these were
 * modelled a Create of any of them was ACKed and stored opaquely, so an OLT
 * that expresses the service here provisioned us successfully and got a data
 * path built from uci instead -- which works only because this bench's OLT
 * also accepts untagged IPoE.
 * Attribute widths and access below are EXTRACTED from the board's own stock
 * plugins, not read off a spec from memory:
 *   dev/re-tools/venv/bin/python3 dev/OMCI-simulate/me_attr_table.py \
 *       --so <stock rootfs>/lib/omci/mib_<Name>.so                          */
#define OMCI_ME_MAC_BRIDGE_SVC	45	/* mib_MacBriServProf */
#define OMCI_ME_MAC_BRIDGE_PORT	47	/* mib_MacBriPortCfgData */
#define OMCI_ME_MAC_BRIDGE_FILTER 49	/* mib_MacBridgePortFilterTable -- the
					 * per-bridge-port MAC filter table.
					 * ⚠ 49 IS THE FILTER TABLE and 52 is
					 * PM history; the numbers are the
					 * measurement and the names are not */
#define OMCI_ME_MAC_BRIDGE_TABLE 50	/* mib_MacBriPortBriTblData */
#define OMCI_ME_MAC_BRIDGE_PM	52	/* mib_MacBridgePortPmMonitorHistoryData
					 * -- COUNTERS.  ⚠ class 49 is the filter
					 * table; 52 is PM history, and getting
					 * that backwards turns the cheapest row
					 * in the gap list into a datapath repair */
/* The VLAN / CLASSIFICATION half of the same service model.  Widths and access
 * extracted the same way, from the same plugins, and MEASURED IDENTICAL on both
 * Luna dies (X111W RTL9602C 3.18 and G24W RTL9603CVD 4.4) — the one difference
 * in all seven is ME 171 #8's DataType, which is a per-BUILD type vocabulary
 * and not a per-die attribute layout.  Semantics: gpon_omci_vlan.h. */
#define OMCI_ME_VLAN_TAG_OP	78	/* mib_VlanTagOpCfgData -- the pre-171
					 * single-tag operation ME */
#define OMCI_ME_PREASSIGN_FILTER 79	/* mib_MacBridgePortFilterPreassign --
					 * the per-protocol drop matrix */
#define OMCI_ME_VLAN_TAG_FILTER	84	/* mib_VlanTagFilterData -- the
					 * per-bridge-port VLAN admit list */
#define OMCI_ME_PBIT_MAPPER	130	/* mib_Map8021pServProf -- the OTHER way
					 * a GEM reaches a bridge port */
#define OMCI_ME_EXT_VLAN	171	/* mib_ExtVlanTagOperCfgData -- THE
					 * SUBSCRIBER VLAN.  Its rows ride inside
					 * the ME 47 frame in stock; only the
					 * DSCP half gets a command of its own */
#define OMCI_ME_OLT_G		131
#define OMCI_ME_IP_HOST		134
#define OMCI_ME_PPTP_POTS_UNI	53
#define OMCI_ME_LCT_UNI		83
#define OMCI_ME_POWER_SHED	133
#define OMCI_ME_VOIP_CONFIG	138
#define OMCI_ME_VOIP_LINE_STATUS 141
#define OMCI_ME_TR069_SERVER	340
#define OMCI_ME_ONU_G		256
#define OMCI_ME_ONU2_G		257
#define OMCI_ME_TCONT		262
#define OMCI_ME_ANI_G		263
#define OMCI_ME_UNI_G		264
#define OMCI_ME_GEM_IW_TP	266	/* mib_GemIwTp -- THE LINK: G.988 joins
					 * the GEM CTP below to a bridge port
					 * THROUGH this ME */
#define OMCI_ME_GEM_CTP		268	/* GEM Port Network CTP -- the ME that
					 * NAMES the WAN data GEM Port-ID */
#define OMCI_ME_GAL_ETH_PROF	272	/* mib_GalEthProf -- 266's companion */
#define OMCI_ME_PRIORITY_QUEUE	277
#define OMCI_ME_TRAFFIC_SCHED	278
#define OMCI_ME_GEM_TRAFFIC_DESC 280	/* mib_GemTrafficDescriptor -- CIR/PIR */
#define OMCI_ME_MCAST_GEM_IW_TP	281	/* mib_MultiGemIwTp -- the DS multicast
					 * path.  It NAMES the multicast GEM,
					 * which both our shells hardcode to
					 * GPON_MCAST_GEM_PORT today */
#define OMCI_ME_VEIP		329
#define OMCI_ME_CTC_LOID_AUTH	65530	/* 0xFFFA — CTC extension the OLT audits */

/* THE MIB-DATA-SYNC POISON SEED IS PROTOCOL POLICY, AND */
#define OMCI_MDS_POISON_SEED	7

/* THE WALK: WHAT TO DO WHEN THE SEED IS NOT ENOUGH. */
#define OMCI_MDS_WALK_STEP	37	/* coprime with 255: enumerates 1..255 */
#define OMCI_MDS_ADAPT_READS	12	/* reads with no provisioning before a step */

/* Step the reported MIB-Data-Sync when the OLT reads without provisioning.
 * Call ONLY from the GET arm: a timer-driven walk would step on a link the OLT
 * is not even reading. */
void omci_mds_walk(struct omci_onu *o);

/* Does this DS frame's AAL5-BE MIC verify?  A frame shorter than OMCI_LEN
 * cannot carry one and is therefore NOT ok: unverifiable is not acceptable. */
bool omci_mic_ok(const u8 *msg, unsigned int len);

/* COLD init: @o need not have been initialised, and this reads nothing out of
 * it before the memset.  A shell declares its UNI inventory straight after. */
void omci_onu_init(struct omci_onu *o, const u8 sn[8], u8 mds_seed);

/* Re-init a LIVE model for a new identity, carrying */
void omci_onu_reinit(struct omci_onu *o, const u8 sn[8], u8 mds_seed);

/* Declare which instances of ME 11 and ME 264 this */
bool omci_onu_declare_unis(struct omci_onu *o,
			   const u16 *pptp_inst, const u8 *pptp_type, u8 pptp_n,
			   const u16 *unig_inst, const u8 *unig_mgmt_cap,
			   u8 unig_n);

/* What ME 11 #1/#2 answer for an instance nobody typed: the value this model
 * hardcoded for every UNI before the declaration existed, so a board that
 * declares no types behaves exactly as it did. */
#define OMCI_UNI_TYPE_DEFAULT	47
#define OMCI_UNI_MAX_FRAME_DEFAULT	1518
/* The largest frame the NIC's 2048-octet RX buffer carries whole. */
#define OMCI_UNI_MAX_FRAME_LIMIT	2000
#define OMCI_UNI_PAUSE_DEFAULT	0xffff

/* What a board's declaration turned out to be. */
enum omci_uni_decl {
	OMCI_UNI_DECL_OK,	/* installed */
	OMCI_UNI_DECL_ABSENT,	/* nothing declared: the default is kept */
	OMCI_UNI_DECL_BAD,	/* declared and malformed, or it does not fit:
				 * NOTHING was changed, the accepted admin
				 * states and pending obligations included */
};

/* Decode a board's panel out of the RAW BYTES of */
enum omci_uni_decl omci_onu_declare_unis_be(struct omci_onu *o,
					    const void *pptp_be, int pptp_len,
					    const void *type, int type_len,
					    const void *unig_be, int unig_len,
					    const void *cap, int cap_len,
					    const char **why);

/* Declare the board's equipment slots.  Refused, with nothing changed, on a
 * zero or duplicate instance, on queues the model cannot number (downstream:
 * blocks of 8, one per Ethernet UNI then the VEIP; upstream: 8 per T-CONT),
 * or on rows that do not fit. */
bool omci_onu_declare_slots(struct omci_onu *o, const struct omci_slot *slot,
			    u8 n);

/* The same out of a DT blob of big-endian 16-bit sextuples
 * <instance type ports priority-queues tcont-buffers schedulers>; an empty
 * blob declares a board with no circuit pack at all. */
enum omci_uni_decl omci_onu_declare_slots_be(struct omci_onu *o,
					     const void *be, int len,
					     const char **why);

/* Declare the board's ME 134 instances: @mac[i] non-zero means that one
 * reports the WAN MAC.  Refused, nothing changed, on a zero-length, oversized
 * or duplicate list, a flag list of another length, or rows that do not fit. */
enum omci_uni_decl omci_onu_declare_ip_hosts_be(struct omci_onu *o,
						const void *inst_be, int inst_len,
						const void *mac, int mac_len,
						const char **why);

/* Declare the board's capacity and queues: @cap is <T-CONTs ONU2-G-#6
 * schedulers GEM-ports> as big-endian 16-bit cells, @dsq the downstream
 * queue ports in block order.  Either may be absent (len < 0).  Refused, with
 * nothing changed, on a count the model cannot hold or rows that do not fit. */
enum omci_uni_decl omci_onu_declare_queues_be(struct omci_onu *o,
					      const void *cap, int cap_len,
					      const void *dsq, int dsq_len,
					      const char **why);

/* Declare how many upstream priority queues each T-CONT holds, one byte per
 * declared T-CONT, each 1..8 (stock V2801RGW: 4 4 2 1 ... 2 4 4).  Absent
 * (len < 0), every T-CONT holds 8.  Refused, nothing changed, on a list whose
 * length is not the T-CONT count or a count outside 1..8. */
enum omci_uni_decl omci_onu_declare_usq_be(struct omci_onu *o, const void *per,
					   int len, const char **why);

/* Declare stock's ME 65529 #2..#5 (CTC spec version, ONU type, TX power supply
 * control, customized ONU type), four bytes.  Absent (len < 0), the core serves
 * 0 1 2 1.  Refused, nothing changed, on any other length. */
enum omci_uni_decl omci_onu_declare_ctc_be(struct omci_onu *o, const void *be,
					   int len, const char **why);

/* Declare the board's ME 53 PPTP POTS UNI instances (big-endian 16-bit list).
 * Refused, nothing changed, on an empty, oversized, zero or duplicate list. */
enum omci_uni_decl omci_onu_declare_pots_be(struct omci_onu *o, const void *be,
					    int len, const char **why);

/* Declare the further single MEs the board's stock reports, as big-endian
 * 16-bit <class instance> pairs.  Refused, nothing changed, on an odd, empty
 * or oversized list, a class this model does not carry, or rows that do not
 * fit. */
enum omci_uni_decl omci_onu_declare_extra_me_be(struct omci_onu *o,
						const void *be, int len,
						const char **why);

/* THIS UNIT's T-CONT, priority-queue and scheduler counts, as its own stock
 * declares them: "<T-CONTs>,<priority queues>,<schedulers>" in decimal.  Two
 * units of one board may run stocks that differ here (X111W: M225-260515
 * 12,96,12 and M225-260618 16,128,16), so it is the unit's fact, handed in
 * the way its serial is, and it overrides the board's onu-capacity counts and
 * the pack holding the T-CONT buffers.  @o NULL only validates.  Refused,
 * nothing changed, on a malformed string or rows that do not fit. */
enum omci_uni_decl omci_onu_declare_unit_capacity(struct omci_onu *o,
						  const char *s, const char **why);

/* How a shell hands the core one property of its board description: its
 * bytes, or NULL with *len = -1 when the board does not declare it. */
typedef const void *(*omci_prop_fn)(void *ctx, const char *name, int *len);

/* Declare everything of the board's equipment beside its UNIs: circuit-packs,
 * ip-host-instances/ip-host-wan-mac, pots-uni-instances, onu-capacity,
 * downstream-queue-ports, upstream-queues-per-tcont, ctc-onu-capability and
 * extra-me-instances.  Each is applied on its own; -> the first property
 * refused (its reason in @why), or NULL when none was. */
const char *omci_onu_declare_equipment(struct omci_onu *o, omci_prop_fn prop,
				       void *ctx, const char **why);

/* What a board's /omci-uni panel turned out to be; the shell says it out loud. */
struct omci_panel_decl {
	const char *unis_why;	/* the UNIs were refused: the single-UNI default stays */
	const char *part;	/* the first equipment property refused, or NULL */
	const char *part_why;
	int ports_len;		/* ethernet-uni-ports bytes, -1 when absent */
	int unis;		/* Ethernet UNIs declared; ports apply only when equal */
};

/* The whole panel through @prop: the UNIs, the switch port of each Ethernet UNI
 * (into @port, *@port_n of them, only when one per UNI -- else the administrative
 * state is modelled and never applied) and omci_onu_declare_equipment().  Here
 * @prop answers the LENGTH the board states: -1 absent, 0 present and empty, even
 * with a NULL value -- an empty list declares no UNI, it does not restore one. */
void omci_onu_declare_panel(struct omci_onu *o, omci_prop_fn prop, void *ctx,
			    u8 *port, unsigned int port_cap, u8 *port_n,
			    struct omci_panel_decl *d);

/* The WAN netdev's MAC, which ME 134 reports; the shell keeps it current. */
void omci_onu_set_wan_mac(struct omci_onu *o, const u8 mac[6]);

/* The inventory slot holding @inst, or NULL when this board has no such
 * instance.  Answers on the FULL u16. */
int omci_uni_slot(const struct omci_uni_inv *inv, u16 inst);

/* Take the set of slots whose administrative state */
u8 omci_uni_take_changed(struct omci_uni_inv *inv);

/* Put a drained obligation BACK, because the family could */
void omci_uni_mark_changed(struct omci_uni_inv *inv, u8 slot);

/* DRAINING THE OBLIGATION IS G.988 WORK, AND IT */
enum omci_uni_apply_rc {
	/* the port took the administrative state: the obligation is discharged */
	OMCI_UNI_APPLIED = 0,
	/* THIS BOARD HAS NO PORT for that slot — permanent.  The obligation is
	 * retained (a corrected port list must still apply what the OLT Set)
	 * and it earns NO retry timer: spinning on it would never converge. */
	OMCI_UNI_NO_PORT,
	/* the backend refused and may not next time: retained AND retried. */
	OMCI_UNI_TRANSIENT,
};

/* struct omci_uni_apply_ops - the two doors the drain */
struct omci_uni_apply_ops {
	enum omci_uni_apply_rc (*apply)(void *sh, u8 slot, bool locked);
	void (*rearm)(void *sh, u8 slot);
};

/* omci_uni_apply_run() - drive one drained changed-mask */
bool omci_uni_apply_run(const struct omci_uni_apply_ops *ops, void *sh,
			u8 changed, u8 n, const u8 *admin);

/* The ME-model API the MESSAGE layer calls. */
struct omci_me_inst *omci_store_find(struct omci_onu *o, u16 class_id, u16 inst);
bool omci_store_has_class(struct omci_onu *o, u16 class_id);
struct omci_me_inst *omci_store_nth(struct omci_onu *o, u16 idx);
bool omci_store_put(struct omci_onu *o, u16 class_id, u16 inst,
		    const u8 *body, int blen);
void omci_store_merge(struct omci_me_inst *e, const u8 *val, int vlen);
bool omci_store_create(struct omci_onu *o, u16 class_id, u16 inst,
		       const u8 *body, unsigned int blen);
bool omci_me_mutable(u16 class_id);
bool omci_get_checks_inst(u16 class_id);
/* octets of DENSE attribute body this class's descriptor rows describe, 0 when
 * its Create body is kept opaque */
u8 omci_me_dense_len(u16 class_id);
void omci_me_reset_values(struct omci_onu *o);
u8 omci_me_set(struct omci_onu *o, u16 class_id, u16 inst, u16 mask,
	       const u8 *values, unsigned int len, u16 *unsupported,
	       u16 *failed);
void omci_store_del(struct omci_onu *o, u16 class_id, u16 inst);

/* the descriptor-table engine: emit @mask's attributes into [v..end) in
 * descriptor order, reporting what was emitted and what this ME models */
u8 omci_me_fill(struct omci_onu *o, u16 class_id, u16 inst, u16 mask,
		u8 *v, const u8 *end, u16 *rmask_out, u16 *known_out);

/* does the model carry this class at all (descriptor row or vendor range)? */
bool omci_class_modelled(u16 class_id);
bool omci_class_described(u16 class_id);

/* a vendor-reserved (class, inst) this board does not upload?  Its stock
 * exposes only the vendor MEs it uploads, and answers any other with result 0
 * and every mask zero. */
bool omci_vendor_absent(const struct omci_onu *o, u16 class_id, u16 inst);

/* is (class, inst) a MIB instance this ONU holds? */
bool omci_inst_exists(struct omci_onu *o, u16 class_id, u16 inst);
bool omci_mib_has_class(struct omci_onu *o, u16 class_id);

/* THE WAN SERVICE SPINE */
#define OMCI_SVC_GEM_CTP	0x01	/* an ME 268 carries this Port-ID */
#define OMCI_SVC_IW_TP		0x02	/* an ME 266 interworks that CTP */
#define OMCI_SVC_BRIDGE_PORT	0x04	/* an ME 47 points at that ME 266 */
#define OMCI_SVC_BRIDGE		0x08	/* and its ME 45 bridge EXISTS */
#define OMCI_SVC_GAL		0x10	/* the ME 266's GAL profile exists */
#define OMCI_SVC_PBIT_MAPPER	0x20	/* ...reached THROUGH an ME 130, not
					 * directly: a different service model,
					 * and the caller is entitled to know */

struct omci_service_path {
	u16	gem_port;	/* what was asked for */
	u16	gem_ctp;	/* ME 268 instance carrying it */
	u16	iw_tp;		/* ME 266 instance */
	u16	iw_option;	/* ME 266 #2, carried as DATA */
	u16	gal_prof;	/* ME 266 #7 */
	u16	gal_payload;	/* ME 272 #1 */
	u16	bridge_port;	/* ME 47 instance */
	u16	tp_type;	/* ME 47 #3, carried as DATA — never branched on:
				 * no TP-type coding has been measured here */
	u16	bridge;		/* ME 47 #1 -> the ME 45 instance */
	u16	mapper;		/* the ME 130 instance, when the mapper route was
				 * taken; 0 on the direct one */
	u8	pbit;		/* which P-bit of that mapper named the ME 266 */
	u8	have;		/* OMCI_SVC_* — which links resolved */
};

u8 omci_service_resolve(struct omci_onu *o, u16 gem_port,
			struct omci_service_path *p);

/* The bridge this GEM's service model lands on, or 0 when the chain does not
 * reach one.  The flat form, for callers that cannot take the struct. */
u16 omci_service_bridge_of_gem(struct omci_onu *o, u16 gem_port);

/* WHICH ME 268 IS THE WAN DATA GEM */
enum omci_dgem {
	OMCI_DGEM_YES = 0,	/* adopt: bidirectional, and nobody else's */
	OMCI_DGEM_RUNT,		/* Create body too short to carry attr 1..3 */
	OMCI_DGEM_ZERO,		/* Port-ID 0 is not a provisioned port */
	OMCI_DGEM_IS_OMCC,	/* the management GEM -- adopting it as the
				 * data GEM points the WAN at the OMCC */
	OMCI_DGEM_IS_MCAST,	/* the broadcast GEM (this OLT Creates it FIRST,
				 * so a first-match rule picks it by default) */
	OMCI_DGEM_NOT_BIDIR,	/* a uni-directional CTP: G.988 direction != 3 */
};

/* Classify ONE stored ME 268 Set-by-Create body. */
enum omci_dgem omci_dgem_classify(const u8 *body, u8 blen,
				  u16 omcc_gem, u16 mcast_gem, u16 *port_id);

/* One-line name for a verdict, for logs and test failure messages. */
const char *omci_dgem_name(enum omci_dgem v);

/* Copied committed state, including explicit absence and unresolved pointers.
 * alloc_id is raw and meaningful only when alloc_known: this layer does not
 * decide whether an allocation is usable by a hardware family. */
struct omci_data_binding {
	bool gem_present;
	bool alloc_known;
	u8 direction;
	u16 gem_inst;
	u16 gem_port;
	u16 tcont_inst;
	u16 alloc_id;
};

/* Caller holds the same serialization as input/reset for this entire copy.
 * Selection remains the first eligible GEM, even if its T-CONT is unresolved. */
void omci_data_binding_snapshot(const struct omci_onu *o, u16 omcc_gem,
				u16 mcast_gem, struct omci_data_binding *binding);

/* Walk the provisioned store and return the WAN data GEM Port-ID, if the OLT
 * has created one.  -> false when it has not, which is the NORMAL state before
 * provisioning and must never be read as a failure. */
bool omci_data_gem_port(struct omci_onu *o, u16 omcc_gem, u16 mcast_gem,
			u16 *port_id);

/* THE DATA-PATH SNOOP'S OTHER TWO DECISIONS */
enum omci_tcont_verdict {
	OMCI_TCONT_NONE = 0,	/* nothing actionable: not a Create/Set, runt
				 * body, attr 1 absent from the Set mask,
				 * alloc 0, or the value already latched */
	OMCI_TCONT_ALLOC,	/* a NEW data alloc-id -- *@alloc holds it */
	OMCI_TCONT_DEALLOC,	/* the G.988 0xffff detach of the latched
				 * instance: the teardown half of the message */
};

/* Decide from @mt (the raw msg-type octet; masked here) and @body/@blen (the
 * wire from octet 8).  A Set carries {mask[0:1], alloc[2:3] when the attr-1 bit
 * is set}; a Create's SBC body has the alloc first.  @cur_alloc / @cur_inst are
 * the caller's latched shadow (0 = none).  Pure, safe from any context. */
enum omci_tcont_verdict omci_tcont_snoop(u8 mt, const u8 *body,
					 unsigned int blen, u16 inst,
					 u16 cur_alloc, u16 cur_inst,
					 u16 *alloc);

/* Does this ME 268 Delete tear down the latched data GEM?  A Delete carries
 * only the class and the instance, which is exactly why the Create had to latch
 * @cur_inst.  @cur_inst 0 means "never latched" and matches any instance — the
 * shell's pre-existing permissive fallback, kept bit-for-bit. */
bool omci_dgem_delete(u8 mt, u16 inst, u16 cur_gem, u16 cur_inst);

#endif /* GPON_OMCI_ME_H */
