// SPDX-License-Identifier: GPL-2.0-only
/*
 * OMCI managed entity catalogue and attribute descriptors
 */

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#include "me.h"

#define OMCI_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct omci_class_name {
	__u16 class_id;
	const char *name;
};

static const struct omci_class_name omci_classes[] = {
	{ 1, "ONT" },
	{ 2, "ONU data" },
	{ 3, "PON IF line cardholder" },
	{ 4, "PON IF line card" },
	{ 5, "Cardholder" },
	{ 6, "Circuit pack" },
	{ 7, "Software image" },
	{ 8, "UNI" },
	{ 9, "TC adapter" },
	{ 10, "Physical path termination point ATM UNI" },
	{ 11, "Physical path termination point Ethernet UNI" },
	{ 12, "Physical path termination point CES UNI" },
	{ 13, "Logical N x 64 kbit/s sub-port connection termination point (CTP)" },
	{ 14, "Interworking VCC termination point" },
	{ 15, "AAL 1 profile" },
	{ 16, "AAL 5 profile" },
	{ 17, "AAL 1 protocol monitoring history data" },
	{ 18, "AAL 5 performance monitoring history data" },
	{ 19, "AAL 2 profile" },
	{ 20, "Intentionally left blank" },
	{ 21, "CES service profile" },
	{ 22, "Reserved" },
	{ 23, "CES physical interface performance monitoring history data" },
	{ 24, "Ethernet performance monitoring history data" },
	{ 25, "VP network CTP" },
	{ 26, "ATM VP cross-connection" },
	{ 27, "Priority queue" },
	{ 28, "DBR/CBR traffic descriptor" },
	{ 29, "UBR traffic descriptor" },
	{ 30, "SBR1/VBR1 traffic descriptor" },
	{ 31, "SBR2/VBR2 traffic descriptor" },
	{ 32, "SBR3/VBR3 traffic descriptor" },
	{ 33, "ABR traffic descriptor" },
	{ 34, "GFR traffic descriptor" },
	{ 35, "ABT/DT/IT traffic descriptor" },
	{ 36, "UPC disagreement monitoring history data" },
	{ 37, "Intentionally left blank" },
	{ 38, "ANI" },
	{ 39, "PON TC adapter" },
	{ 40, "PON physical path termination point" },
	{ 41, "TC adapter protocol monitoring history data" },
	{ 42, "Threshold data" },
	{ 43, "Operator specific" },
	{ 44, "Vendor specific" },
	{ 45, "MAC bridge service profile" },
	{ 46, "MAC bridge configuration data" },
	{ 47, "MAC bridge port configuration data" },
	{ 48, "MAC bridge port designation data" },
	{ 49, "MAC bridge port filter table data" },
	{ 50, "MAC bridge port bridge table data" },
	{ 51, "MAC bridge performance monitoring history data" },
	{ 52, "MAC bridge port performance monitoring history data" },
	{ 53, "Physical path termination point POTS UNI" },
	{ 54, "Voice CTP" },
	{ 55, "Voice PM history data" },
	{ 56, "AAL2 PVC profile" },
	{ 57, "AAL2 CPS protocol monitoring history data" },
	{ 58, "Voice service profile" },
	{ 59, "LES service profile" },
	{ 60, "AAL2 SSCS parameter profile 1" },
	{ 61, "AAL2 SSCS parameter profile 2" },
	{ 62, "VP performance monitoring history data" },
	{ 63, "Traffic scheduler" },
	{ 64, "T-CONT buffer" },
	{ 65, "UBR+ traffic descriptor" },
	{ 66, "AAL2 SSCS protocol monitoring history data" },
	{ 67, "IP port configuration data" },
	{ 68, "IP router service profile" },
	{ 69, "IP router configuration data" },
	{ 70, "IP router performance monitoring history data 1" },
	{ 71, "IP router performance monitoring history data 2" },
	{ 72, "ICMP performance monitoring history data 1" },
	{ 73, "ICMP performance monitoring history data 2" },
	{ 74, "IP route table" },
	{ 75, "IP static routes" },
	{ 76, "ARP service profile" },
	{ 77, "ARP configuration data" },
	{ 78, "VLAN tagging operation configuration data" },
	{ 79, "MAC bridge port filter pre-assign table" },
	{ 80, "Physical path termination point ISDN UNI" },
	{ 81, "Reserved" },
	{ 82, "Physical path termination point video UNI" },
	{ 83, "Physical path termination point LCT UNI" },
	{ 84, "VLAN tagging filter data" },
	{ 85, "ONU" },
	{ 86, "ATM VC cross-connection" },
	{ 87, "VC network CTP" },
	{ 88, "VC PM history data" },
	{ 89, "Ethernet performance monitoring history data 2" },
	{ 90, "Physical path termination point video ANI" },
	{ 91, "Physical path termination point IEEE 802.11 UNI" },
	{ 92, "IEEE 802.11 station management data 1" },
	{ 93, "IEEE 802.11 station management data 2" },
	{ 94, "IEEE 802.11 general purpose object" },
	{ 95, "IEEE 802.11 MAC and PHY operation and antenna data" },
	{ 96, "IEEE 802.11 performance monitoring history data" },
	{ 97, "IEEE 802.11 PHY FHSS DSSS IR tables" },
	{ 98, "Physical path termination point xDSL UNI part 1" },
	{ 99, "Physical path termination point xDSL UNI part 2" },
	{ 100, "xDSL line inventory and status data part 1" },
	{ 101, "xDSL line inventory and status data part 2" },
	{ 102, "xDSL channel downstream status data" },
	{ 103, "xDSL channel upstream status data" },
	{ 104, "xDSL line configuration profile part 1" },
	{ 105, "xDSL line configuration profile part 2" },
	{ 106, "xDSL line configuration profile part 3" },
	{ 107, "xDSL channel configuration profile" },
	{ 108, "xDSL subcarrier masking downstream profile" },
	{ 109, "xDSL subcarrier masking upstream profile" },
	{ 110, "xDSL PSD mask profile" },
	{ 111, "xDSL downstream radio frequency interference (RFI) bands profile" },
	{ 112, "xDSL xTU-C performance monitoring history data" },
	{ 113, "xDSL xTU-R performance monitoring history data" },
	{ 114, "xDSL xTU-C channel performance monitoring history data" },
	{ 115, "xDSL xTU-R channel performance monitoring history data" },
	{ 116, "TC adaptor performance monitoring history data xDSL" },
	{ 117, "Physical path termination point VDSL UNI (ITU-T G.993.1 VDSL1)" },
	{ 118, "VDSL VTU-O physical data" },
	{ 119, "VDSL VTU-R physical data" },
	{ 120, "VDSL channel data" },
	{ 121, "VDSL line configuration profile" },
	{ 122, "VDSL channel configuration profile" },
	{ 123, "VDSL band plan configuration profile" },
	{ 124, "VDSL VTU-O physical interface monitoring history data" },
	{ 125, "VDSL VTU-R physical interface monitoring history data" },
	{ 126, "VDSL VTU-O channel performance monitoring history data" },
	{ 127, "VDSL VTU-R channel performance monitoring history data" },
	{ 128, "Video return path service profile" },
	{ 129, "Video return path performance monitoring history data" },
	{ 130, "IEEE 802.1p mapper service profile" },
	{ 131, "OLT-G" },
	{ 132, "Multicast interworking VCC termination point" },
	{ 133, "ONU power shedding" },
	{ 134, "IP host config data" },
	{ 135, "IP host performance monitoring history data" },
	{ 136, "TCP/UDP config data" },
	{ 137, "Network address" },
	{ 138, "VoIP config data" },
	{ 139, "VoIP voice CTP" },
	{ 140, "Call control performance monitoring history data" },
	{ 141, "VoIP line status" },
	{ 142, "VoIP media profile" },
	{ 143, "RTP profile data" },
	{ 144, "RTP performance monitoring history data" },
	{ 145, "Network dial plan table" },
	{ 146, "VoIP application service profile" },
	{ 147, "VoIP feature access codes" },
	{ 148, "Authentication security method" },
	{ 149, "SIP config portal" },
	{ 150, "SIP agent config data" },
	{ 151, "SIP agent performance monitoring history data" },
	{ 152, "SIP call initiation performance monitoring history data" },
	{ 153, "SIP user data" },
	{ 154, "MGC config portal" },
	{ 155, "MGC config data" },
	{ 156, "MGC performance monitoring history data" },
	{ 157, "Large string" },
	{ 158, "ONU remote debug" },
	{ 159, "Equipment protection profile" },
	{ 160, "Equipment extension package" },
	{ 161, "Port-mapping package (legacy B-PON)" },
	{ 162, "Physical path termination point MoCA UNI" },
	{ 163, "MoCA Ethernet performance monitoring history data" },
	{ 164, "MoCA interface performance monitoring history data" },
	{ 165, "VDSL2 line configuration extensions" },
	{ 166, "xDSL line inventory and status data part 3" },
	{ 167, "xDSL line inventory and status data part 4" },
	{ 168, "VDSL2 line inventory and status data part 1" },
	{ 169, "VDSL2 line inventory and status data part 2" },
	{ 170, "VDSL2 line inventory and status data part 3" },
	{ 171, "Extended VLAN tagging operation configuration data" },
	{ 256, "ONU-G" },
	{ 257, "ONU2-G" },
	{ 258, "ONU-G (deprecated)" },
	{ 259, "ONU2-G (deprecated)" },
	{ 260, "PON IF line card-G" },
	{ 261, "PON TC adapter-G" },
	{ 262, "T-CONT" },
	{ 263, "ANI-G" },
	{ 264, "UNI-G" },
	{ 265, "ATM interworking VCC termination point" },
	{ 266, "GEM interworking termination point" },
	{ 267, "GEM port performance monitoring history data (obsolete)" },
	{ 268, "GEM port network CTP" },
	{ 269, "VP network CTP" },
	{ 270, "VC network CTP-G" },
	{ 271, "GAL TDM profile (deprecated)" },
	{ 272, "GAL Ethernet profile" },
	{ 273, "Threshold data 1" },
	{ 274, "Threshold data 2" },
	{ 275, "GAL TDM performance monitoring history data (deprecated)" },
	{ 276, "GAL Ethernet performance monitoring history data" },
	{ 277, "Priority queue" },
	{ 278, "Traffic scheduler" },
	{ 279, "Protection data" },
	{ 280, "Traffic descriptor" },
	{ 281, "Multicast GEM interworking termination point" },
	{ 282, "Pseudowire termination point" },
	{ 283, "RTP pseudowire parameters" },
	{ 284, "Pseudowire maintenance profile" },
	{ 285, "Pseudowire performance monitoring history data" },
	{ 286, "Ethernet flow termination point" },
	{ 287, "OMCI" },
	{ 288, "Managed entity" },
	{ 289, "Attribute" },
	{ 290, "Dot1X port extension package" },
	{ 291, "Dot1X configuration profile" },
	{ 292, "Dot1X performance monitoring history data" },
	{ 293, "Radius performance monitoring history data" },
	{ 294, "TU CTP" },
	{ 295, "TU performance monitoring history data" },
	{ 296, "Ethernet performance monitoring history data 3" },
	{ 297, "Port-mapping package" },
	{ 298, "Dot1 rate limiter" },
	{ 299, "Dot1ag maintenance domain" },
	{ 300, "Dot1ag maintenance association" },
	{ 301, "Dot1ag default MD level" },
	{ 302, "Dot1ag MEP" },
	{ 303, "Dot1ag MEP status" },
	{ 304, "Dot1ag MEP CCM database" },
	{ 305, "Dot1ag CFM stack" },
	{ 306, "Dot1ag chassis - management info" },
	{ 307, "Octet string" },
	{ 308, "General purpose buffer" },
	{ 309, "Multicast operations profile" },
	{ 310, "Multicast subscriber config info" },
	{ 311, "Multicast subscriber monitor" },
	{ 312, "FEC performance monitoring history data" },
	{ 313, "RE ANI-G" },
	{ 314, "Physical path termination point RE UNI" },
	{ 315, "RE upstream amplifier" },
	{ 316, "RE downstream amplifier" },
	{ 317, "RE config portal" },
	{ 318, "File transfer controller" },
	{ 319, "CES physical interface performance monitoring history data 2" },
	{ 320, "CES physical interface performance monitoring history data 3" },
	{ 321, "Ethernet frame performance monitoring history data downstream" },
	{ 322, "Ethernet frame performance monitoring history data upstream" },
	{ 323, "VDSL2 line configuration extensions 2" },
	{ 324, "xDSL impulse noise monitor performance monitoring history data" },
	{ 325, "xDSL line inventory and status data part 5" },
	{ 326, "xDSL line inventory and status data part 6" },
	{ 327, "xDSL line inventory and status data part 7" },
	{ 328, "RE common amplifier parameters" },
	{ 329, "Virtual Ethernet interface point" },
	{ 330, "Generic status portal" },
	{ 331, "ONU-E" },
	{ 332, "Enhanced security control" },
	{ 333, "MPLS pseudowire termination point" },
	{ 334, "Ethernet frame extended PM" },
	{ 335, "Simple network management protocol (SNMP) configuration data" },
	{ 336, "ONU dynamic power management control" },
	{ 337, "PW ATM configuration data" },
	{ 338, "PW ATM performance monitoring history data" },
	{ 339, "PW Ethernet configuration data" },
	{ 340, "BBF TR-069 management server" },
	{ 341, "GEM port network CTP performance monitoring history data" },
	{ 342, "TCP/UDP performance monitoring history data" },
	{ 343, "Energy consumption performance monitoring history data" },
	{ 344, "XG-PON TC performance monitoring history data" },
	{ 345, "XG-PON downstream management performance monitoring history data" },
	{ 346, "XG-PON upstream management performance monitoring history data" },
	{ 347, "IPv6 host config data" },
	{ 348, "MAC bridge port ICMPv6 process pre-assign table" },
	{ 349, "Power over Ethernet (PoE) control" },
	{ 400, "Ethernet pseudowire parameters" },
	{ 401, "Physical path termination point RS232/RS485 UNI" },
	{ 402, "RS232/RS485 port operation configuration data" },
	{ 403, "RS232/RS485 performance monitoring history data" },
	{ 404, "L2 multicast GEM interworking termination point" },
	{ 405, "ANI-E" },
	{ 406, "EPON downstream performance monitoring configuration" },
	{ 407, "SIP agent config data 2" },
	{ 408, "xDSL xTU-C performance monitoring history data part 2" },
	{ 409, "PTM performance monitoring history data xDSL" },
	{ 410, "VDSL2 line configuration extensions 3" },
	{ 411, "Vectoring line configuration extensions" },
	{ 412, "xDSL channel configuration profile part 2" },
	{ 413, "xTU data gathering configuration" },
	{ 414, "xDSL line inventory and status data part 8" },
	{ 415, "VDSL2 line inventory and status data part 4" },
	{ 416, "Vectoring line inventory and status data" },
	{ 417, "Data gathering line test, diagnostic and status" },
	{ 419, "EFM bonding group" },
	{ 420, "EFM bonding link" },
	{ 421, "EFM bonding group performance monitoring history data" },
	{ 422, "EFM bonding group performance monitoring history data part 2" },
	{ 423, "EFM bonding link performance monitoring history data" },
	{ 424, "EFM bonding port performance monitoring history data" },
	{ 425, "EFM bonding port performance monitoring history data part 2" },
	{ 426, "Ethernet frame extended PM 64 bit" },
	{ 427, "Physical path termination point xDSL UNI part 3" },
	{ 428, "FAST line configuration profile part 1" },
	{ 429, "FAST line configuration profile part 2" },
	{ 430, "FAST line configuration profile part 3" },
	{ 431, "FAST line configuration profile part 4" },
	{ 432, "FAST channel configuration profile" },
	{ 433, "FAST data path configuration profile" },
	{ 434, "FAST vectoring line configuration extensions" },
	{ 435, "FAST line inventory and status data" },
	{ 436, "FAST line inventory and status data part 2" },
	{ 437, "FAST xTU-C performance monitoring history data" },
	{ 438, "FAST xTU-R performance monitoring history data" },
	{ 439, "OpenFlow config data" },
	{ 440, "Time Status Message" },
	{ 441, "ONU3-G" },
	{ 442, "TWDM System Profile managed entity" },
	{ 443, "TWDM channel managed entity" },
	{ 444, "TWDM channel PHY/LODS performance monitoring history data" },
	{ 445, "TWDM channel XGEM performance monitoring history data" },
	{ 446, "TWDM channel PLOAM performance monitoring history data part 1" },
	{ 447, "TWDM channel PLOAM performance monitoring history data part 2" },
	{ 448, "TWDM channel PLOAM performance monitoring history data part 3" },
	{ 449, "TWDM channel tuning performance monitoring history data part 1" },
	{ 450, "TWDM channel tuning performance monitoring history data part 2" },
	{ 451, "TWDM channel tuning performance monitoring history data part 3" },
	{ 452, "TWDM channel OMCI performance monitoring history data" },
};

#define OMCI_ATTR(_bit, _offset, _len, _access) \
	{ .mask = OMCI_BIT(_bit), .offset = (_offset), .len = (_len), \
	  .access = (_access) }

#define OMCI_RW_SC	(OMCI_ATTR_ACCESS_READ | OMCI_ATTR_ACCESS_WRITE | \
			 OMCI_ATTR_ACCESS_SET_CREATE)
#define OMCI_RW		(OMCI_ATTR_ACCESS_READ | OMCI_ATTR_ACCESS_WRITE)
#define OMCI_R		OMCI_ATTR_ACCESS_READ
#define OMCI_R_SC	(OMCI_ATTR_ACCESS_READ | OMCI_ATTR_ACCESS_SET_CREATE)
#define OMCI_W		OMCI_ATTR_ACCESS_WRITE

static const struct omci_attr_desc omci_software_image_attrs[] = {
	OMCI_ATTR(15, 0, OMCI_SOFTWARE_VERSION_LEN, OMCI_R),
	OMCI_ATTR(14, 14, 1, OMCI_R),
	OMCI_ATTR(13, 15, 1, OMCI_R),
	OMCI_ATTR(12, 16, 1, OMCI_R),
};

static const struct omci_attr_desc omci_cardholder_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),
	OMCI_ATTR(14, 1, 1, OMCI_RW),
	OMCI_ATTR(13, 2, 1, OMCI_RW),
	OMCI_ATTR(12, 3, 20, OMCI_RW),
	OMCI_ATTR(11, 23, 20, OMCI_RW),
	OMCI_ATTR(10, 43, 2, OMCI_R),
};

static const struct omci_attr_desc omci_circuit_pack_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),
	OMCI_ATTR(14, 1, 1, OMCI_R),
	OMCI_ATTR(13, 2, 8, OMCI_R),
	OMCI_ATTR(12, 10, 14, OMCI_R),
	OMCI_ATTR(11, 24, 4, OMCI_R),
	OMCI_ATTR(10, 28, 1, OMCI_RW),
	OMCI_ATTR(9, 29, 1, OMCI_R),
	OMCI_ATTR(8, 30, 1, OMCI_RW),
	OMCI_ATTR(7, 31, 20, OMCI_R),
	OMCI_ATTR(6, 51, 1, OMCI_RW),
	OMCI_ATTR(5, 52, 1, OMCI_R),
	OMCI_ATTR(4, 53, 1, OMCI_R),
	OMCI_ATTR(3, 54, 1, OMCI_R),
	OMCI_ATTR(2, 55, 4, OMCI_RW),
};

static const struct omci_attr_desc omci_pptp_ethernet_uni_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),
	OMCI_ATTR(14, 1, 1, OMCI_R),
	OMCI_ATTR(13, 2, 1, OMCI_RW),
	OMCI_ATTR(12, 3, 1, OMCI_RW),
	OMCI_ATTR(11, 4, 1, OMCI_RW),
	OMCI_ATTR(10, 5, 1, OMCI_R),
	OMCI_ATTR(9, 6, 1, OMCI_R),
	OMCI_ATTR(8, 7, 2, OMCI_RW),
	OMCI_ATTR(7, 9, 1, OMCI_RW),
	OMCI_ATTR(6, 10, 2, OMCI_RW),
	OMCI_ATTR(5, 12, 1, OMCI_RW),
	OMCI_ATTR(4, 13, 1, OMCI_RW),
	OMCI_ATTR(3, 14, 1, OMCI_RW),
	OMCI_ATTR(2, 15, 1, OMCI_RW),
	OMCI_ATTR(1, 16, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_bridge_port_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),
	OMCI_ATTR(14, 2, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 3, 1, OMCI_RW_SC),
	OMCI_ATTR(12, 4, 2, OMCI_RW_SC),
	OMCI_ATTR(11, 6, 2, OMCI_RW_SC),
	OMCI_ATTR(10, 8, 2, OMCI_RW_SC),
	OMCI_ATTR(9, 10, 1, OMCI_RW_SC),
	OMCI_ATTR(8, 11, 1, OMCI_RW_SC),
	OMCI_ATTR(7, 12, 1, OMCI_RW_SC),
	OMCI_ATTR(6, 13, 6, OMCI_R),
	OMCI_ATTR(5, 19, 2, OMCI_RW),
	OMCI_ATTR(4, 21, 2, OMCI_RW),
	OMCI_ATTR(3, 23, 1, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_traffic_descriptor_attrs[] = {
	OMCI_ATTR(15, 0, 4, OMCI_RW_SC),
	OMCI_ATTR(14, 4, 4, OMCI_RW_SC),
	OMCI_ATTR(13, 8, 4, OMCI_RW_SC),
	OMCI_ATTR(12, 12, 4, OMCI_RW_SC),
	OMCI_ATTR(11, 16, 1, OMCI_RW_SC),
	OMCI_ATTR(10, 17, 1, OMCI_RW_SC),
	OMCI_ATTR(9, 18, 1, OMCI_RW_SC),
	OMCI_ATTR(8, 19, 1, OMCI_ATTR_ACCESS_READ | OMCI_ATTR_ACCESS_SET_CREATE),
};

static const struct omci_attr_desc omci_vlan_filter_attrs[] = {
	OMCI_ATTR(15, 0, 24, OMCI_RW_SC),
	OMCI_ATTR(14, 24, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 25, 1, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_8021p_mapper_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),
	OMCI_ATTR(14, 2, 2, OMCI_RW_SC),
	OMCI_ATTR(13, 4, 2, OMCI_RW_SC),
	OMCI_ATTR(12, 6, 2, OMCI_RW_SC),
	OMCI_ATTR(11, 8, 2, OMCI_RW_SC),
	OMCI_ATTR(10, 10, 2, OMCI_RW_SC),
	OMCI_ATTR(9, 12, 2, OMCI_RW_SC),
	OMCI_ATTR(8, 14, 2, OMCI_RW_SC),
	OMCI_ATTR(7, 16, 2, OMCI_RW_SC),
	OMCI_ATTR(6, 18, 1, OMCI_RW_SC),
	OMCI_ATTR(5, 19, 24, OMCI_RW),
	OMCI_ATTR(4, 43, 1, OMCI_RW_SC),
	OMCI_ATTR(3, 44, 1, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_extended_vlan_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 2, OMCI_R),
	OMCI_ATTR(13, 3, 2, OMCI_RW),
	OMCI_ATTR(12, 5, 2, OMCI_RW),
	OMCI_ATTR(11, 7, 1, OMCI_RW),
	OMCI_ATTR(10, 8, 16, OMCI_RW),
	OMCI_ATTR(9, 24, 2, OMCI_RW_SC),
	OMCI_ATTR(8, 26, 24, OMCI_RW),
};

static const struct omci_attr_desc omci_tcont_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),
	OMCI_ATTR(14, 2, 1, OMCI_R),
	OMCI_ATTR(13, 3, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_ani_g_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* SR indication */
	OMCI_ATTR(14, 1, 2, OMCI_R),	/* Total T-CONT number */
	OMCI_ATTR(13, 3, 2, OMCI_RW),	/* GEM block length */
	OMCI_ATTR(12, 5, 1, OMCI_R),	/* Piggyback DBA reporting */
	OMCI_ATTR(11, 6, 1, OMCI_R),	/* Deprecated */
	OMCI_ATTR(10, 7, 1, OMCI_RW),	/* SF threshold */
	OMCI_ATTR(9, 8, 1, OMCI_RW),	/* SD threshold */
	OMCI_ATTR(8, 9, 1, OMCI_RW),	/* ARC */
	OMCI_ATTR(7, 10, 1, OMCI_RW),	/* ARC interval */
	OMCI_ATTR(6, 11, 2, OMCI_R),	/* Optical signal level */
	OMCI_ATTR(5, 13, 1, OMCI_RW),	/* Lower optical threshold */
	OMCI_ATTR(4, 14, 1, OMCI_RW),	/* Upper optical threshold */
	OMCI_ATTR(3, 15, 2, OMCI_R),	/* ONU response time */
	OMCI_ATTR(2, 17, 2, OMCI_R),	/* Transmit optical level */
	OMCI_ATTR(1, 19, 1, OMCI_RW),	/* Lower transmit power threshold */
	OMCI_ATTR(0, 20, 1, OMCI_RW),	/* Upper transmit power threshold */
};

static const struct omci_attr_desc omci_uni_g_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),
	OMCI_ATTR(14, 2, 1, OMCI_RW),
	OMCI_ATTR(13, 3, 1, OMCI_R),
	OMCI_ATTR(12, 4, 2, OMCI_RW),
};

static const struct omci_attr_desc omci_gem_iwtp_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),
	OMCI_ATTR(14, 2, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 3, 2, OMCI_RW_SC),
	OMCI_ATTR(12, 5, 2, OMCI_RW_SC),
	OMCI_ATTR(11, 7, 1, OMCI_R),
	OMCI_ATTR(10, 8, 1, OMCI_R),
	OMCI_ATTR(9, 9, 2, OMCI_RW_SC),
	OMCI_ATTR(8, 11, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_gem_port_ctp_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),
	OMCI_ATTR(14, 2, 2, OMCI_RW_SC),
	OMCI_ATTR(13, 4, 1, OMCI_RW_SC),
	OMCI_ATTR(12, 5, 2, OMCI_RW_SC),
	OMCI_ATTR(11, 7, 2, OMCI_RW_SC),
	OMCI_ATTR(10, 9, 1, OMCI_R),
	OMCI_ATTR(9, 10, 2, OMCI_RW_SC),
	OMCI_ATTR(8, 12, 1, OMCI_R),
	OMCI_ATTR(7, 13, 2, OMCI_RW_SC),
	OMCI_ATTR(6, 15, 2, OMCI_RW),
};

/* G.988 9.2.9 FEC performance monitoring history data */
static const struct omci_attr_desc omci_fec_pm_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),		/* interval end time */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* threshold data 1/2 id */
	OMCI_ATTR(13, 3, 4, OMCI_R),		/* corrected bytes */
	OMCI_ATTR(12, 7, 4, OMCI_R),		/* corrected code words */
	OMCI_ATTR(11, 11, 4, OMCI_R),		/* uncorrectable code words */
	OMCI_ATTR(10, 15, 4, OMCI_R),		/* total code words */
	OMCI_ATTR(9, 19, 2, OMCI_R),		/* FEC seconds */
};

static const struct omci_attr_desc omci_veip_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_R),
	OMCI_ATTR(13, 2, 25, OMCI_RW),
	OMCI_ATTR(12, 27, 2, OMCI_RW),
	OMCI_ATTR(11, 29, 2, OMCI_R),
};

/* Huawei interoperability MEs: attribute numbering and lengths as Huawei OLTs use them */
static const struct omci_attr_desc omci_huawei_350_attrs[] = {
	OMCI_ATTR(15, 0, 4, OMCI_RW),
	OMCI_ATTR(14, 4, 2, OMCI_RW),
	OMCI_ATTR(13, 6, 2, OMCI_RW),
	OMCI_ATTR(12, 8, 2, OMCI_RW),
	OMCI_ATTR(11, 10, 2, OMCI_RW),
	OMCI_ATTR(10, 12, 2, OMCI_RW),
	OMCI_ATTR(9, 14, 1, OMCI_RW),
	OMCI_ATTR(8, 15, 4, OMCI_RW),
	OMCI_ATTR(7, 19, 3, OMCI_RW),
	OMCI_ATTR(6, 22, 1, OMCI_RW),
	OMCI_ATTR(5, 23, 1, OMCI_RW),
	OMCI_ATTR(4, 24, 1, OMCI_RW),
	OMCI_ATTR(3, 25, 1, OMCI_RW),
	OMCI_ATTR(2, 26, 4, OMCI_RW),
	OMCI_ATTR(1, 30, 6, OMCI_RW),
	OMCI_ATTR(0, 36, 2, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_351_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_RW),
	OMCI_ATTR(13, 2, 1, OMCI_RW),
	OMCI_ATTR(12, 3, 1, OMCI_RW),
	OMCI_ATTR(11, 4, 1, OMCI_RW),
	OMCI_ATTR(10, 5, 1, OMCI_RW),
	OMCI_ATTR(9, 6, 1, OMCI_RW),
	OMCI_ATTR(8, 7, 1, OMCI_RW),
	OMCI_ATTR(7, 8, 1, OMCI_RW),
	OMCI_ATTR(6, 9, 1, OMCI_RW),
	OMCI_ATTR(5, 10, 1, OMCI_RW),
	OMCI_ATTR(4, 11, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_352_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),
	OMCI_ATTR(14, 2, 2, OMCI_RW_SC),
	OMCI_ATTR(13, 4, 2, OMCI_RW_SC),
	OMCI_ATTR(12, 6, 1, OMCI_RW_SC),
	OMCI_ATTR(11, 7, 4, OMCI_RW_SC),
	OMCI_ATTR(10, 11, 4, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_huawei_353_attrs[] = {
	OMCI_ATTR(15, 0, 14, OMCI_R),
	OMCI_ATTR(14, 14, 1, OMCI_R),
	OMCI_ATTR(13, 15, 1, OMCI_R),
	OMCI_ATTR(12, 16, 1, OMCI_R),
	OMCI_ATTR(11, 17, 4, OMCI_R),
};

static const struct omci_attr_desc omci_huawei_370_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),
	OMCI_ATTR(14, 2, 4, OMCI_RW),
	OMCI_ATTR(13, 6, 4, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_373_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 25, OMCI_RW),
	OMCI_ATTR(13, 26, 25, OMCI_RW),
	OMCI_ATTR(12, 51, 25, OMCI_RW),
	OMCI_ATTR(11, 76, 25, OMCI_RW),
	OMCI_ATTR(10, 101, 25, OMCI_RW),
	OMCI_ATTR(9, 126, 25, OMCI_RW),
	OMCI_ATTR(8, 151, 25, OMCI_RW),
	OMCI_ATTR(7, 176, 25, OMCI_RW),
	OMCI_ATTR(6, 201, 25, OMCI_RW),
	OMCI_ATTR(5, 226, 25, OMCI_RW),
	OMCI_ATTR(4, 251, 25, OMCI_RW),
	OMCI_ATTR(3, 276, 25, OMCI_RW),
	OMCI_ATTR(2, 301, 25, OMCI_RW),
	OMCI_ATTR(1, 326, 25, OMCI_RW),
	OMCI_ATTR(0, 351, 25, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_65398_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_RW),
	OMCI_ATTR(13, 2, 1, OMCI_RW),
	OMCI_ATTR(12, 3, 1, OMCI_RW),
	OMCI_ATTR(11, 4, 1, OMCI_RW),
	OMCI_ATTR(10, 5, 1, OMCI_RW),
	OMCI_ATTR(9, 6, 1, OMCI_RW),
	OMCI_ATTR(8, 7, 2, OMCI_RW),
	OMCI_ATTR(7, 9, 2, OMCI_RW),
	OMCI_ATTR(6, 11, 2, OMCI_RW),
	OMCI_ATTR(5, 13, 1, OMCI_RW),
	OMCI_ATTR(4, 14, 1, OMCI_RW),
	OMCI_ATTR(3, 15, 1, OMCI_RW),
	OMCI_ATTR(2, 16, 25, OMCI_RW),
	OMCI_ATTR(1, 41, 1, OMCI_RW),
	OMCI_ATTR(0, 42, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_65414_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),
	OMCI_ATTR(14, 1, 1, OMCI_R),
	OMCI_ATTR(13, 2, 1, OMCI_R),
	OMCI_ATTR(12, 3, 1, OMCI_R),
	OMCI_ATTR(11, 4, 1, OMCI_R),
	OMCI_ATTR(10, 5, 16, OMCI_R),
	OMCI_ATTR(9, 21, 2, OMCI_R),
	OMCI_ATTR(8, 23, 2, OMCI_R),
	OMCI_ATTR(7, 25, 4, OMCI_R),
	OMCI_ATTR(6, 29, 4, OMCI_R),
	OMCI_ATTR(5, 33, 2, OMCI_R),
	OMCI_ATTR(4, 35, 2, OMCI_R),
	OMCI_ATTR(3, 37, 8, OMCI_R),
};

static const struct omci_attr_desc omci_huawei_65415_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_RW),
	OMCI_ATTR(13, 2, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_65417_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),
	OMCI_ATTR(14, 2, 25, OMCI_RW),
	OMCI_ATTR(13, 27, 2, OMCI_RW),
	OMCI_ATTR(12, 29, 2, OMCI_RW),
	OMCI_ATTR(11, 31, 25, OMCI_RW),
	OMCI_ATTR(10, 56, 2, OMCI_RW),
	OMCI_ATTR(9, 58, 25, OMCI_RW),
	OMCI_ATTR(8, 83, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_65423_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),
	OMCI_ATTR(14, 1, 1, OMCI_R),
	OMCI_ATTR(13, 2, 1, OMCI_R),
	OMCI_ATTR(12, 3, 1, OMCI_R),
	OMCI_ATTR(11, 4, 4, OMCI_R),
	OMCI_ATTR(10, 8, 8, OMCI_R),
	OMCI_ATTR(9, 16, 1, OMCI_R),
	OMCI_ATTR(8, 17, 25, OMCI_R),
	OMCI_ATTR(7, 42, 6, OMCI_R),
	OMCI_ATTR(6, 48, 4, OMCI_R),
	OMCI_ATTR(5, 52, 1, OMCI_R),
	OMCI_ATTR(4, 53, 1, OMCI_R),
	OMCI_ATTR(3, 54, 1, OMCI_R),
	OMCI_ATTR(2, 55, 1, OMCI_R),
	OMCI_ATTR(1, 56, 1, OMCI_R),
	OMCI_ATTR(0, 57, 2, OMCI_R),
};

static const struct omci_attr_desc omci_huawei_65425_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 4, OMCI_RW),
	OMCI_ATTR(13, 5, 4, OMCI_RW),
	OMCI_ATTR(12, 9, 4, OMCI_RW),
	OMCI_ATTR(11, 13, 4, OMCI_RW),
	OMCI_ATTR(10, 17, 2, OMCI_RW),
	OMCI_ATTR(9, 19, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_65426_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),
	OMCI_ATTR(14, 2, 2, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_65427_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),
	OMCI_ATTR(14, 2, 24, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_65430_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),
	OMCI_ATTR(14, 1, 1, OMCI_R),
	OMCI_ATTR(13, 2, 1, OMCI_R),
	OMCI_ATTR(12, 3, 1, OMCI_R),
	OMCI_ATTR(11, 4, 1, OMCI_R),
	OMCI_ATTR(10, 5, 1, OMCI_R),
	OMCI_ATTR(9, 6, 1, OMCI_R),
	OMCI_ATTR(8, 7, 1, OMCI_R),
	OMCI_ATTR(7, 8, 1, OMCI_R),
	OMCI_ATTR(6, 9, 1, OMCI_R),
	OMCI_ATTR(5, 10, 1, OMCI_R),
	OMCI_ATTR(4, 11, 1, OMCI_R),
	OMCI_ATTR(3, 12, 1, OMCI_R),
	OMCI_ATTR(2, 13, 1, OMCI_R),
	OMCI_ATTR(1, 14, 1, OMCI_R),
	OMCI_ATTR(0, 15, 1, OMCI_R),
};

static const struct omci_attr_desc omci_huawei_65431_attrs[] = {
	OMCI_ATTR(15, 0, 4, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_65444_attrs[] = {
	OMCI_ATTR(15, 0, 8, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_65447_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_65450_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_RW),
	OMCI_ATTR(13, 2, 1, OMCI_RW),
	OMCI_ATTR(12, 3, 1, OMCI_RW),
	OMCI_ATTR(11, 4, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_huawei_65451_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 2, 2, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_huawei_65453_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 2, 12, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_huawei_65454_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),
	OMCI_ATTR(14, 2, 2, OMCI_RW),
	OMCI_ATTR(13, 4, 2, OMCI_RW),
	OMCI_ATTR(12, 6, 4, OMCI_RW),
	OMCI_ATTR(11, 10, 8, OMCI_RW),
	OMCI_ATTR(10, 18, 6, OMCI_RW),
	OMCI_ATTR(9, 24, 6, OMCI_RW),
};

static const struct omci_attr_desc omci_zte_242_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_zte_243_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 8, OMCI_RW_SC),
	OMCI_ATTR(13, 9, 8, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_zte_247_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),
	OMCI_ATTR(14, 2, 2, OMCI_RW),
	OMCI_ATTR(13, 4, 14, OMCI_R),
	OMCI_ATTR(12, 18, 2, OMCI_RW),
	OMCI_ATTR(11, 20, 2, OMCI_RW),
	OMCI_ATTR(10, 22, 2, OMCI_RW),
	OMCI_ATTR(9, 24, 2, OMCI_RW),
	OMCI_ATTR(8, 26, 2, OMCI_RW),
	OMCI_ATTR(7, 28, 2, OMCI_RW),
	OMCI_ATTR(6, 30, 2, OMCI_RW),
	OMCI_ATTR(5, 32, 2, OMCI_RW),
	OMCI_ATTR(4, 34, 2, OMCI_RW),
	OMCI_ATTR(3, 36, 2, OMCI_RW),
	OMCI_ATTR(2, 38, 2, OMCI_RW),
	OMCI_ATTR(1, 40, 2, OMCI_RW),
	OMCI_ATTR(0, 42, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_zte_65294_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),
	OMCI_ATTR(14, 2, 4, OMCI_RW),
	OMCI_ATTR(13, 6, 4, OMCI_RW),
	OMCI_ATTR(12, 10, 4, OMCI_RW),
	OMCI_ATTR(11, 14, 1, OMCI_RW),
	OMCI_ATTR(10, 15, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_zte_65305_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 2, 1, OMCI_RW_SC),
	OMCI_ATTR(12, 3, 2, OMCI_RW_SC),
	OMCI_ATTR(11, 5, 2, OMCI_RW_SC),
	OMCI_ATTR(10, 7, 4, OMCI_RW_SC),
	OMCI_ATTR(9, 11, 4, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_zte_65314_attrs[] = {
	OMCI_ATTR(15, 0, 6, OMCI_R),
	OMCI_ATTR(14, 6, 2, OMCI_RW),
	OMCI_ATTR(13, 8, 1, OMCI_RW),
	OMCI_ATTR(12, 9, 1, OMCI_RW),
	OMCI_ATTR(11, 10, 2, OMCI_RW),
	OMCI_ATTR(10, 12, 2, OMCI_R),
	OMCI_ATTR(9, 14, 16, OMCI_RW),
	OMCI_ATTR(8, 30, 16, OMCI_R),
};

static const struct omci_attr_desc omci_zte_65315_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_RW),
	OMCI_ATTR(13, 2, 1, OMCI_RW),
	OMCI_ATTR(12, 3, 2, OMCI_RW),
	OMCI_ATTR(11, 5, 2, OMCI_RW),
	OMCI_ATTR(10, 7, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_fh_65321_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_R),
	OMCI_ATTR(14, 2, 2, OMCI_R),
};

static const struct omci_attr_desc omci_fh_65322_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 16, OMCI_RW_SC),
	OMCI_ATTR(13, 17, 16, OMCI_RW),
	OMCI_ATTR(12, 33, 16, OMCI_RW),
	OMCI_ATTR(11, 49, 16, OMCI_RW),
	OMCI_ATTR(10, 65, 1, OMCI_RW_SC),
	OMCI_ATTR(9, 66, 1, OMCI_RW_SC),
	OMCI_ATTR(8, 67, 2, OMCI_RW_SC),
	OMCI_ATTR(7, 69, 2, OMCI_RW_SC),
	OMCI_ATTR(6, 71, 1, OMCI_RW_SC),
	OMCI_ATTR(5, 72, 1, OMCI_RW_SC),
	OMCI_ATTR(4, 73, 1, OMCI_RW),
	OMCI_ATTR(3, 74, 1, OMCI_R),
	OMCI_ATTR(2, 75, 2, OMCI_RW_SC),
	OMCI_ATTR(1, 77, 10, OMCI_RW),
};

static const struct omci_attr_desc omci_fh_65323_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 16, OMCI_RW_SC),
	OMCI_ATTR(13, 17, 16, OMCI_RW),
	OMCI_ATTR(12, 33, 16, OMCI_RW_SC),
	OMCI_ATTR(11, 49, 16, OMCI_RW),
	OMCI_ATTR(10, 65, 16, OMCI_RW),
	OMCI_ATTR(9, 81, 16, OMCI_RW),
	OMCI_ATTR(8, 97, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_fh_65324_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 1, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_fh_65326_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_RW),
	OMCI_ATTR(13, 2, 1, OMCI_RW),
	OMCI_ATTR(12, 3, 1, OMCI_RW),
	OMCI_ATTR(11, 4, 1, OMCI_RW),
	OMCI_ATTR(10, 5, 16, OMCI_RW),
	OMCI_ATTR(9, 21, 16, OMCI_RW),
	OMCI_ATTR(8, 37, 16, OMCI_RW),
	OMCI_ATTR(7, 53, 16, OMCI_RW),
	OMCI_ATTR(6, 69, 4, OMCI_RW),
	OMCI_ATTR(5, 73, 2, OMCI_RW),
	OMCI_ATTR(4, 75, 16, OMCI_RW),
	OMCI_ATTR(3, 91, 16, OMCI_RW),
	OMCI_ATTR(2, 107, 1, OMCI_RW),
	OMCI_ATTR(1, 108, 2, OMCI_R),
};

static const struct omci_attr_desc omci_fh_65327_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 16, OMCI_RW),
	OMCI_ATTR(13, 17, 16, OMCI_RW),
	OMCI_ATTR(12, 33, 16, OMCI_RW),
	OMCI_ATTR(11, 49, 16, OMCI_RW),
	OMCI_ATTR(10, 65, 16, OMCI_RW),
	OMCI_ATTR(9, 81, 16, OMCI_RW),
	OMCI_ATTR(8, 97, 16, OMCI_RW),
	OMCI_ATTR(7, 113, 16, OMCI_RW),
	OMCI_ATTR(6, 129, 1, OMCI_RW),
	OMCI_ATTR(5, 130, 1, OMCI_RW),
	OMCI_ATTR(4, 131, 1, OMCI_RW),
	OMCI_ATTR(3, 132, 1, OMCI_RW),
	OMCI_ATTR(2, 133, 2, OMCI_RW),
	OMCI_ATTR(1, 135, 2, OMCI_R),
	OMCI_ATTR(0, 137, 2, OMCI_RW),
};

static const struct omci_attr_desc omci_fh_65328_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),
	OMCI_ATTR(14, 2, 16, OMCI_RW),
	OMCI_ATTR(13, 18, 16, OMCI_RW),
	OMCI_ATTR(12, 34, 16, OMCI_RW),
	OMCI_ATTR(11, 50, 16, OMCI_RW),
	OMCI_ATTR(10, 66, 16, OMCI_RW),
	OMCI_ATTR(9, 82, 16, OMCI_RW),
	OMCI_ATTR(8, 98, 16, OMCI_RW),
	OMCI_ATTR(7, 114, 16, OMCI_RW),
	OMCI_ATTR(6, 130, 2, OMCI_RW),
	OMCI_ATTR(5, 132, 16, OMCI_RW),
	OMCI_ATTR(4, 148, 16, OMCI_RW),
	OMCI_ATTR(3, 164, 4, OMCI_RW),
	OMCI_ATTR(2, 168, 2, OMCI_RW),
};

static const struct omci_attr_desc omci_fh_65329_attrs[] = {
	OMCI_ATTR(15, 0, 4, OMCI_RW_SC),
	OMCI_ATTR(14, 4, 4, OMCI_RW_SC),
	OMCI_ATTR(13, 8, 4, OMCI_RW_SC),
	OMCI_ATTR(12, 12, 4, OMCI_RW_SC),
	OMCI_ATTR(11, 16, 4, OMCI_RW_SC),
	OMCI_ATTR(10, 20, 19, OMCI_RW),
	OMCI_ATTR(9, 39, 19, OMCI_RW),
	OMCI_ATTR(8, 58, 19, OMCI_RW),
};

static const struct omci_attr_desc omci_fh_65333_attrs[] = {
	OMCI_ATTR(15, 0, 4, OMCI_RW),
	OMCI_ATTR(14, 4, 4, OMCI_RW),
};

static const struct omci_attr_desc omci_fh_65338_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 2, 2, OMCI_RW_SC),
	OMCI_ATTR(12, 4, 2, OMCI_RW_SC),
	OMCI_ATTR(11, 6, 1, OMCI_RW),
	OMCI_ATTR(10, 7, 2, OMCI_RW),
	OMCI_ATTR(9, 9, 2, OMCI_RW),
	OMCI_ATTR(8, 11, 2, OMCI_RW),
};

static const struct omci_attr_desc omci_fh_65363_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 12, OMCI_R),
	OMCI_ATTR(13, 13, 2, OMCI_R),
	OMCI_ATTR(12, 15, 2, OMCI_R),
	OMCI_ATTR(11, 17, 2, OMCI_R),
};

static const struct omci_attr_desc omci_fh_65411_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_R),
	OMCI_ATTR(14, 2, 1, OMCI_R),
	OMCI_ATTR(13, 3, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_ctc_250_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 2, 1, OMCI_RW_SC),
	OMCI_ATTR(12, 3, 2, OMCI_RW_SC),
	OMCI_ATTR(11, 5, 25, OMCI_RW),
	OMCI_ATTR(10, 30, 25, OMCI_RW),
	OMCI_ATTR(9, 55, 1, OMCI_R),
	OMCI_ATTR(8, 56, 4, OMCI_R),
	OMCI_ATTR(7, 60, 2, OMCI_RW),
	OMCI_ATTR(6, 62, 2, OMCI_RW),
};

static const struct omci_attr_desc omci_ctc_65282_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 2, 24, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_ctc_65320_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),
	OMCI_ATTR(14, 1, 16, OMCI_R),
	OMCI_ATTR(13, 17, 3, OMCI_R),
	OMCI_ATTR(12, 20, 16, OMCI_R),
	OMCI_ATTR(11, 36, 4, OMCI_R),
	OMCI_ATTR(10, 40, 16, OMCI_R),
	OMCI_ATTR(9, 56, 8, OMCI_R),
	OMCI_ATTR(8, 64, 1, OMCI_R),
};

static const struct omci_attr_desc omci_ctc_65321_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),
	OMCI_ATTR(13, 3, 2, OMCI_RW_SC),
	OMCI_ATTR(12, 5, 2, OMCI_RW),
	OMCI_ATTR(11, 7, 2, OMCI_RW_SC),
	OMCI_ATTR(10, 9, 2, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_ctc_65380_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 2, 2, OMCI_RW_SC),
	OMCI_ATTR(12, 4, 1, OMCI_RW_SC),
	OMCI_ATTR(11, 5, 1, OMCI_RW_SC),
	OMCI_ATTR(10, 6, 1, OMCI_RW_SC),
	OMCI_ATTR(9, 7, 2, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_ctc_65381_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 24, OMCI_RW_SC),
	OMCI_ATTR(13, 25, 24, OMCI_RW_SC),
	OMCI_ATTR(12, 49, 1, OMCI_RW_SC),
	OMCI_ATTR(11, 50, 1, OMCI_RW_SC),
	OMCI_ATTR(10, 51, 1, OMCI_RW_SC),
	OMCI_ATTR(9, 52, 4, OMCI_RW_SC),
	OMCI_ATTR(8, 56, 4, OMCI_RW_SC),
	OMCI_ATTR(7, 60, 4, OMCI_RW_SC),
	OMCI_ATTR(6, 64, 4, OMCI_RW_SC),
	OMCI_ATTR(5, 68, 4, OMCI_RW_SC),
	OMCI_ATTR(4, 72, 1, OMCI_RW_SC),
	OMCI_ATTR(3, 73, 1, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_ctc_65385_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 2, 24, OMCI_RW_SC),
	OMCI_ATTR(12, 26, 1, OMCI_RW_SC),
	OMCI_ATTR(11, 27, 1, OMCI_RW_SC),
	OMCI_ATTR(10, 28, 2, OMCI_RW_SC),
	OMCI_ATTR(9, 30, 2, OMCI_RW_SC),
	OMCI_ATTR(8, 32, 2, OMCI_RW_SC),
	OMCI_ATTR(7, 34, 1, OMCI_RW_SC),
	OMCI_ATTR(6, 35, 1, OMCI_RW_SC),
	OMCI_ATTR(5, 36, 1, OMCI_RW_SC),
	OMCI_ATTR(4, 37, 1, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_ctc_65386_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 24, OMCI_RW_SC),
	OMCI_ATTR(13, 25, 1, OMCI_RW_SC),
	OMCI_ATTR(12, 26, 1, OMCI_RW_SC),
	OMCI_ATTR(11, 27, 1, OMCI_RW_SC),
	OMCI_ATTR(10, 28, 26, OMCI_RW_SC),
	OMCI_ATTR(9, 54, 26, OMCI_RW_SC),
	OMCI_ATTR(8, 80, 26, OMCI_RW_SC),
	OMCI_ATTR(7, 106, 26, OMCI_RW_SC),
	OMCI_ATTR(6, 132, 1, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_ctc_65523_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_ctc_65528_attrs[] = {
	OMCI_ATTR(15, 0, 4, OMCI_R),
	OMCI_ATTR(14, 4, 2, OMCI_RW),
	OMCI_ATTR(13, 6, 2, OMCI_RW),
	OMCI_ATTR(12, 8, 2, OMCI_RW),
	OMCI_ATTR(11, 10, 2, OMCI_RW),
	OMCI_ATTR(10, 12, 0, OMCI_RW),
};

static const struct omci_attr_desc omci_ctc_65529_attrs[] = {
	OMCI_ATTR(15, 0, 4, OMCI_R),
	OMCI_ATTR(14, 4, 1, OMCI_R),
	OMCI_ATTR(13, 5, 1, OMCI_R),
	OMCI_ATTR(12, 6, 1, OMCI_R),
	OMCI_ATTR(11, 7, 1, OMCI_R),
};

static const struct omci_attr_desc omci_ctc_65530_attrs[] = {
	OMCI_ATTR(15, 0, 4, OMCI_R),
	OMCI_ATTR(14, 4, 24, OMCI_R),
	OMCI_ATTR(13, 28, 12, OMCI_R),
	OMCI_ATTR(12, 40, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_ctc_65531_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),
	OMCI_ATTR(14, 1, 1, OMCI_RW_SC),
	OMCI_ATTR(13, 2, 1, OMCI_RW_SC),
	OMCI_ATTR(12, 3, 2, OMCI_RW_SC),
	OMCI_ATTR(11, 5, 1, OMCI_RW_SC),
	OMCI_ATTR(10, 6, 4, OMCI_RW_SC),
	OMCI_ATTR(9, 10, 0, OMCI_RW),
	OMCI_ATTR(8, 10, 0, OMCI_RW),
	OMCI_ATTR(7, 10, 0, OMCI_R),
	OMCI_ATTR(6, 10, 1, OMCI_RW_SC),
	OMCI_ATTR(5, 11, 16, OMCI_RW),
	OMCI_ATTR(4, 27, 4, OMCI_RW_SC),
	OMCI_ATTR(3, 31, 4, OMCI_RW_SC),
	OMCI_ATTR(2, 35, 4, OMCI_RW),
	OMCI_ATTR(1, 39, 1, OMCI_RW),
	OMCI_ATTR(0, 40, 3, OMCI_RW_SC),
};

static const struct omci_attr_desc omci_nokia_65295_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),
	OMCI_ATTR(14, 1, 2, OMCI_R),
	OMCI_ATTR(13, 3, 1, OMCI_R),
	OMCI_ATTR(12, 4, 2, OMCI_R),
	OMCI_ATTR(11, 6, 1, OMCI_R),
	OMCI_ATTR(10, 7, 2, OMCI_R),
	OMCI_ATTR(9, 9, 1, OMCI_R),
	OMCI_ATTR(8, 10, 2, OMCI_R),
	OMCI_ATTR(7, 12, 1, OMCI_R),
	OMCI_ATTR(6, 13, 2, OMCI_R),
};

static const struct omci_attr_desc omci_nokia_65296_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_RW),
	OMCI_ATTR(13, 2, 4, OMCI_R),
	OMCI_ATTR(12, 6, 1, OMCI_RW),
	OMCI_ATTR(11, 7, 1, OMCI_RW),
	OMCI_ATTR(10, 8, 1, OMCI_RW),
	OMCI_ATTR(9, 9, 4, OMCI_R),
	OMCI_ATTR(8, 13, 1, OMCI_R),
	OMCI_ATTR(7, 14, 1, OMCI_RW),
	OMCI_ATTR(6, 15, 12, OMCI_R),
	OMCI_ATTR(5, 27, 4, OMCI_R),
	OMCI_ATTR(4, 31, 25, OMCI_R),
};

static const struct omci_attr_desc omci_nokia_65297_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 1, OMCI_RW),
	OMCI_ATTR(13, 2, 2, OMCI_RW),
	OMCI_ATTR(12, 4, 1, OMCI_RW),
	OMCI_ATTR(11, 5, 1, OMCI_RW),
	OMCI_ATTR(10, 6, 1, OMCI_RW),
	OMCI_ATTR(9, 7, 1, OMCI_RW),
};

static const struct omci_attr_desc omci_ctc_65408_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_W),
};

static const struct omci_attr_desc omci_ctc_65527_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),
	OMCI_ATTR(14, 2, 2, OMCI_RW),
	OMCI_ATTR(13, 4, 8, OMCI_RW),
	OMCI_ATTR(12, 12, 4, OMCI_RW),
	OMCI_ATTR(11, 16, 8, OMCI_RW),
};

/* Genexis Ethernet PM history data 4: two flags, then ten counters */
static const struct omci_attr_desc omci_genexis_255_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),
	OMCI_ATTR(13, 3, 1, OMCI_RW_SC),
	OMCI_ATTR(12, 4, 4, OMCI_R),
	OMCI_ATTR(11, 8, 4, OMCI_R),
	OMCI_ATTR(10, 12, 4, OMCI_R),
	OMCI_ATTR(9, 16, 4, OMCI_R),
	OMCI_ATTR(8, 20, 4, OMCI_R),
	OMCI_ATTR(7, 24, 4, OMCI_R),
	OMCI_ATTR(6, 28, 4, OMCI_R),
	OMCI_ATTR(5, 32, 4, OMCI_R),
	OMCI_ATTR(4, 36, 4, OMCI_R),
	OMCI_ATTR(3, 40, 4, OMCI_R),
};

static const struct omci_attr_desc omci_nokia_65305_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),
	OMCI_ATTR(14, 1, 6, OMCI_RW),
};

static const struct omci_attr_desc omci_nokia_65306_attrs[] = {
	OMCI_ATTR(15, 0, 19, OMCI_RW),
};

static const struct omci_attr_desc omci_omci_attrs[] = {
	OMCI_ATTR(15, 0, 4, OMCI_R),
	OMCI_ATTR(14, 4, 4, OMCI_R),
};

static const struct omci_attr_desc omci_managed_entity_attrs[] = {
	OMCI_ATTR(15, 0, 25, OMCI_R),
	OMCI_ATTR(14, 25, 4, OMCI_R),
	OMCI_ATTR(13, 29, 1, OMCI_R),
	OMCI_ATTR(12, 30, 4, OMCI_R),
	OMCI_ATTR(11, 34, 4, OMCI_R),
	OMCI_ATTR(10, 38, 4, OMCI_R),
	OMCI_ATTR(9, 42, 4, OMCI_R),
	OMCI_ATTR(8, 46, 1, OMCI_R),
};

static const struct omci_attr_desc omci_attribute_attrs[] = {
	OMCI_ATTR(15, 0, 25, OMCI_R),
	OMCI_ATTR(14, 25, 2, OMCI_R),
	OMCI_ATTR(13, 27, 1, OMCI_R),
	OMCI_ATTR(12, 28, 1, OMCI_R),
	OMCI_ATTR(11, 29, 4, OMCI_R),
	OMCI_ATTR(10, 33, 4, OMCI_R),
	OMCI_ATTR(9, 37, 4, OMCI_R),
	OMCI_ATTR(8, 41, 4, OMCI_R),
	OMCI_ATTR(7, 45, 1, OMCI_R),
};

/* G.988 class 24, Ethernet performance monitoring history data */
static const struct omci_attr_desc omci_ethernet_pm_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Interval end time */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* Threshold data 1/2 ID */
	OMCI_ATTR(13, 3, 4, OMCI_R),	/* FCS errors */
	OMCI_ATTR(12, 7, 4, OMCI_R),	/* Excessive collision counter */
	OMCI_ATTR(11, 11, 4, OMCI_R),	/* Late collision counter */
	OMCI_ATTR(10, 15, 4, OMCI_R),	/* Frames too long */
	OMCI_ATTR(9, 19, 4, OMCI_R),	/* Buffer overflows on receive */
	OMCI_ATTR(8, 23, 4, OMCI_R),	/* Buffer overflows on transmit */
	OMCI_ATTR(7, 27, 4, OMCI_R),	/* Single collision frame counter */
	OMCI_ATTR(6, 31, 4, OMCI_R),	/* Multiple collisions frame counter */
	OMCI_ATTR(5, 35, 4, OMCI_R),	/* SQE counter */
	OMCI_ATTR(4, 39, 4, OMCI_R),	/* Deferred transmission counter */
	OMCI_ATTR(3, 43, 4, OMCI_R),	/* Internal MAC transmit error counter */
	OMCI_ATTR(2, 47, 4, OMCI_R),	/* Carrier sense error counter */
	OMCI_ATTR(1, 51, 4, OMCI_R),	/* Alignment error counter */
	OMCI_ATTR(0, 55, 4, OMCI_R),	/* Internal MAC receive error counter */
};

/* G.988 class 49, MAC bridge port filter table data */
static const struct omci_attr_desc omci_mac_bridge_port_filter_table_attrs[] = {
	OMCI_ATTR(15, 0, 8, OMCI_RW),	/* MAC filter table */
};

/* G.988 class 50, MAC bridge port bridge table data */
static const struct omci_attr_desc omci_mac_bridge_port_bridge_table_attrs[] = {
	OMCI_ATTR(15, 0, 8, OMCI_R),	/* Bridge table */
};

/* G.988 class 51, MAC bridge performance monitoring history data */
static const struct omci_attr_desc omci_mac_bridge_pm_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Interval end time */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* Threshold data 1/2 ID */
	OMCI_ATTR(13, 3, 4, OMCI_R),	/* Bridge learning entry discard count */
};

/* G.988 class 52, MAC bridge port performance monitoring history data */
static const struct omci_attr_desc omci_mac_bridge_port_pm_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Interval end time */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* Threshold data 1/2 ID */
	OMCI_ATTR(13, 3, 4, OMCI_R),	/* Forwarded frame counter */
	OMCI_ATTR(12, 7, 4, OMCI_R),	/* Delay exceeded discard counter */
	OMCI_ATTR(11, 11, 4, OMCI_R),	/* MTU exceeded discard counter */
	OMCI_ATTR(10, 15, 4, OMCI_R),	/* Received frame counter */
	OMCI_ATTR(9, 19, 4, OMCI_R),	/* Received and discarded counter */
};

/* G.988 class 53, Physical path termination point POTS UNI */
static const struct omci_attr_desc omci_pptp_pots_uni_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),	/* Administrative state */
	OMCI_ATTR(14, 1, 2, OMCI_RW),	/* Interworking TP pointer */
	OMCI_ATTR(13, 3, 1, OMCI_RW),	/* ARC */
	OMCI_ATTR(12, 4, 1, OMCI_RW),	/* ARC interval */
	OMCI_ATTR(11, 5, 1, OMCI_RW),	/* Impedance */
	OMCI_ATTR(10, 6, 1, OMCI_RW),	/* Transmission path */
	OMCI_ATTR(9, 7, 1, OMCI_RW),	/* Rx gain */
	OMCI_ATTR(8, 8, 1, OMCI_RW),	/* Tx gain */
	OMCI_ATTR(7, 9, 1, OMCI_R),	/* Operational state */
	OMCI_ATTR(6, 10, 1, OMCI_R),	/* Hook state */
	OMCI_ATTR(5, 11, 2, OMCI_RW),	/* POTS holdover time */
	OMCI_ATTR(4, 13, 1, OMCI_RW),	/* Nominal feed voltage */
	OMCI_ATTR(3, 14, 1, OMCI_RW),	/* Loss of softswitch */
};

/* G.988 class 58, Voice service profile */
static const struct omci_attr_desc omci_voice_service_profile_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),	/* Announcement type */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* Jitter target */
	OMCI_ATTR(13, 3, 2, OMCI_RW_SC),	/* Jitter buffer max */
	OMCI_ATTR(12, 5, 1, OMCI_RW_SC),	/* Echo cancel ind */
	OMCI_ATTR(11, 6, 2, OMCI_RW_SC),	/* PSTN protocol variant */
	OMCI_ATTR(10, 8, 2, OMCI_RW_SC),	/* DTMF digit levels */
	OMCI_ATTR(9, 10, 2, OMCI_RW_SC),	/* DTMF digit duration */
	OMCI_ATTR(8, 12, 2, OMCI_RW_SC),	/* Hook flash minimum time */
	OMCI_ATTR(7, 14, 2, OMCI_RW_SC),	/* Hook flash maximum time */
	OMCI_ATTR(6, 16, 20, OMCI_RW),	/* Tone pattern table */
	OMCI_ATTR(5, 36, 7, OMCI_RW),	/* Tone event table */
	OMCI_ATTR(4, 43, 5, OMCI_RW),	/* Ringing pattern table */
	OMCI_ATTR(3, 48, 7, OMCI_RW),	/* Ringing event table */
	OMCI_ATTR(2, 55, 2, OMCI_RW_SC),	/* Network specific extensions pointer */
};

/* G.988 class 79, MAC bridge port filter preassign table */
static const struct omci_attr_desc omci_mac_bridge_port_filter_preassign_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),	/* IPv4 multicast filtering */
	OMCI_ATTR(14, 1, 1, OMCI_RW),	/* IPv6 multicast filtering */
	OMCI_ATTR(13, 2, 1, OMCI_RW),	/* IPv4 broadcast filtering */
	OMCI_ATTR(12, 3, 1, OMCI_RW),	/* RARP filtering */
	OMCI_ATTR(11, 4, 1, OMCI_RW),	/* IPX filtering */
	OMCI_ATTR(10, 5, 1, OMCI_RW),	/* NetBEUI filtering */
	OMCI_ATTR(9, 6, 1, OMCI_RW),	/* AppleTalk filtering */
	OMCI_ATTR(8, 7, 1, OMCI_RW),	/* Bridge management information filtering */
	OMCI_ATTR(7, 8, 1, OMCI_RW),	/* ARP filtering */
	OMCI_ATTR(6, 9, 1, OMCI_RW),	/* PPPoE broadcast filtering */
};

/* G.988 class 89, Ethernet performance monitoring history data 2 */
static const struct omci_attr_desc omci_ethernet_pm_2_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Interval end time */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* Threshold data 1/2 ID */
	OMCI_ATTR(13, 3, 4, OMCI_R),	/* PPPoE filtered frame counter */
};

/* G.988 class 133, ONU power shedding */
static const struct omci_attr_desc omci_onu_power_shedding_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW),	/* Restore power timer reset interval */
	OMCI_ATTR(14, 2, 2, OMCI_RW),	/* Data class shedding interval */
	OMCI_ATTR(13, 4, 2, OMCI_RW),	/* Voice class shedding interval */
	OMCI_ATTR(12, 6, 2, OMCI_RW),	/* Video overlay class shedding interval */
	OMCI_ATTR(11, 8, 2, OMCI_RW),	/* Video return class shedding interval */
	OMCI_ATTR(10, 10, 2, OMCI_RW),	/* DSL class shedding interval */
	OMCI_ATTR(9, 12, 2, OMCI_RW),	/* ATM class shedding interval */
	OMCI_ATTR(8, 14, 2, OMCI_RW),	/* CES class shedding interval */
	OMCI_ATTR(7, 16, 2, OMCI_RW),	/* Frame class shedding interval */
	OMCI_ATTR(6, 18, 2, OMCI_RW),	/* SDH-SONET class shedding interval */
	OMCI_ATTR(5, 20, 2, OMCI_R),	/* Shedding status */
};

/* G.988 class 134, IP host config data */
static const struct omci_attr_desc omci_ip_host_config_data_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),	/* IP options */
	OMCI_ATTR(14, 1, 6, OMCI_R),	/* MAC address */
	OMCI_ATTR(13, 7, 25, OMCI_RW),	/* ONU identifier */
	OMCI_ATTR(12, 32, 4, OMCI_RW),	/* IP address */
	OMCI_ATTR(11, 36, 4, OMCI_RW),	/* Mask */
	OMCI_ATTR(10, 40, 4, OMCI_RW),	/* Gateway */
	OMCI_ATTR(9, 44, 4, OMCI_RW),	/* Primary DNS */
	OMCI_ATTR(8, 48, 4, OMCI_RW),	/* Secondary DNS */
	OMCI_ATTR(7, 52, 4, OMCI_R),	/* Current address */
	OMCI_ATTR(6, 56, 4, OMCI_R),	/* Current mask */
	OMCI_ATTR(5, 60, 4, OMCI_R),	/* Current gateway */
	OMCI_ATTR(4, 64, 4, OMCI_R),	/* Current primary DNS */
	OMCI_ATTR(3, 68, 4, OMCI_R),	/* Current secondary DNS */
	OMCI_ATTR(2, 72, 25, OMCI_R),	/* Domain name */
	OMCI_ATTR(1, 97, 25, OMCI_R),	/* Host name */
	OMCI_ATTR(0, 122, 2, OMCI_RW),	/* Relay agent options */
};

/* G.988 class 136, TCP/UDP config data */
static const struct omci_attr_desc omci_tcp_udp_config_data_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),	/* Port ID */
	OMCI_ATTR(14, 2, 1, OMCI_RW_SC),	/* Protocol */
	OMCI_ATTR(13, 3, 1, OMCI_RW_SC),	/* TOS/diffserv field */
	OMCI_ATTR(12, 4, 2, OMCI_RW_SC),	/* IP host pointer */
};

/* G.988 class 137, Network address */
static const struct omci_attr_desc omci_network_address_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),	/* Security pointer */
	OMCI_ATTR(14, 2, 2, OMCI_RW_SC),	/* Address pointer */
};

/* G.988 class 138, VoIP config data */
static const struct omci_attr_desc omci_voip_config_data_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Available signalling protocols */
	OMCI_ATTR(14, 1, 1, OMCI_RW),	/* Signalling protocol used */
	OMCI_ATTR(13, 2, 4, OMCI_R),	/* Available VoIP configuration methods */
	OMCI_ATTR(12, 6, 1, OMCI_RW),	/* VoIP configuration method used */
	OMCI_ATTR(11, 7, 2, OMCI_RW),	/* VoIP configuration address pointer */
	OMCI_ATTR(10, 9, 1, OMCI_R),	/* VoIP configuration state */
	OMCI_ATTR(9, 10, 1, OMCI_W),	/* Retrieve profile */
	OMCI_ATTR(8, 11, 25, OMCI_R),	/* Profile version */
};

/* G.988 class 139, VoIP voice CTP */
static const struct omci_attr_desc omci_voip_voice_ctp_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),	/* User protocol pointer */
	OMCI_ATTR(14, 2, 2, OMCI_RW_SC),	/* PPTP pointer */
	OMCI_ATTR(13, 4, 2, OMCI_RW_SC),	/* VoIP media profile pointer */
	OMCI_ATTR(12, 6, 1, OMCI_RW_SC),	/* Signalling code */
};

/* G.988 class 140, Call control performance monitoring history data */
static const struct omci_attr_desc omci_call_control_pm_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Interval end time */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* Threshold data 1/2 ID */
	OMCI_ATTR(13, 3, 4, OMCI_R),	/* Call setup failures */
	OMCI_ATTR(12, 7, 4, OMCI_R),	/* Call setup timer */
	OMCI_ATTR(11, 11, 4, OMCI_R),	/* Call terminate failures */
	OMCI_ATTR(10, 15, 4, OMCI_R),	/* Analog port releases */
	OMCI_ATTR(9, 19, 4, OMCI_R),	/* Analog port off-hook timer */
};

/* G.988 class 141, VoIP line status */
static const struct omci_attr_desc omci_voip_line_status_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_R),	/* VoIP codec used */
	OMCI_ATTR(14, 2, 1, OMCI_R),	/* VoIP voice server status */
	OMCI_ATTR(13, 3, 1, OMCI_R),	/* VoIP port session type */
	OMCI_ATTR(12, 4, 2, OMCI_R),	/* VoIP call 1 packet period */
	OMCI_ATTR(11, 6, 2, OMCI_R),	/* VoIP call 2 packet period */
	OMCI_ATTR(10, 8, 25, OMCI_R),	/* VoIP call 1 dest addr */
	OMCI_ATTR(9, 33, 25, OMCI_R),	/* VoIP call 2 dest addr */
	OMCI_ATTR(8, 58, 1, OMCI_R),	/* VoIP line state */
	OMCI_ATTR(7, 59, 1, OMCI_R),	/* Emergency call status */
};

/* G.988 class 142, VoIP media profile */
static const struct omci_attr_desc omci_voip_media_profile_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),	/* Fax mode */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* Voice service profile pointer */
	OMCI_ATTR(13, 3, 1, OMCI_RW_SC),	/* Codec selection (1st order) */
	OMCI_ATTR(12, 4, 1, OMCI_RW_SC),	/* Packet period selection (1st order) */
	OMCI_ATTR(11, 5, 1, OMCI_RW_SC),	/* Silence suppression (1st order) */
	OMCI_ATTR(10, 6, 1, OMCI_RW_SC),	/* Codec selection (2nd order) */
	OMCI_ATTR(9, 7, 1, OMCI_RW_SC),	/* Packet period selection (2nd order) */
	OMCI_ATTR(8, 8, 1, OMCI_RW_SC),	/* Silence suppression (2nd order) */
	OMCI_ATTR(7, 9, 1, OMCI_RW_SC),	/* Codec selection (3rd order) */
	OMCI_ATTR(6, 10, 1, OMCI_RW_SC),	/* Packet period selection (3rd order) */
	OMCI_ATTR(5, 11, 1, OMCI_RW_SC),	/* Silence suppression (3rd order) */
	OMCI_ATTR(4, 12, 1, OMCI_RW_SC),	/* Codec selection (4th order) */
	OMCI_ATTR(3, 13, 1, OMCI_RW_SC),	/* Packet period selection (4th order) */
	OMCI_ATTR(2, 14, 1, OMCI_RW_SC),	/* Silence suppression (4th order) */
	OMCI_ATTR(1, 15, 1, OMCI_RW_SC),	/* OOB DTMF */
	OMCI_ATTR(0, 16, 2, OMCI_RW_SC),	/* RTP profile pointer */
};

/* G.988 class 143, RTP profile data */
static const struct omci_attr_desc omci_rtp_profile_data_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),	/* Local port min */
	OMCI_ATTR(14, 2, 2, OMCI_RW_SC),	/* Local port max */
	OMCI_ATTR(13, 4, 1, OMCI_RW_SC),	/* DSCP mark */
	OMCI_ATTR(12, 5, 1, OMCI_RW_SC),	/* Piggyback events */
	OMCI_ATTR(11, 6, 1, OMCI_RW_SC),	/* Tone events */
	OMCI_ATTR(10, 7, 1, OMCI_RW_SC),	/* DTMF events */
	OMCI_ATTR(9, 8, 1, OMCI_RW_SC),	/* CAS events */
	OMCI_ATTR(8, 9, 2, OMCI_RW),	/* IP host config pointer */
};

/* G.988 class 144, RTP performance monitoring history data */
static const struct omci_attr_desc omci_rtp_pm_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Interval end time */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* Threshold data 1/2 ID */
	OMCI_ATTR(13, 3, 4, OMCI_R),	/* RTP errors */
	OMCI_ATTR(12, 7, 4, OMCI_R),	/* Packet loss */
	OMCI_ATTR(11, 11, 4, OMCI_R),	/* Maximum jitter */
	OMCI_ATTR(10, 15, 4, OMCI_R),	/* Maximum time between RTCP packets */
	OMCI_ATTR(9, 19, 4, OMCI_R),	/* Buffer underflows */
	OMCI_ATTR(8, 23, 4, OMCI_R),	/* Buffer overflows */
};

/* G.988 class 145, Network dial plan table */
static const struct omci_attr_desc omci_network_dial_plan_table_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_R),	/* Dial plan number */
	OMCI_ATTR(14, 2, 2, OMCI_R_SC),	/* Dial plan table max size */
	OMCI_ATTR(13, 4, 2, OMCI_RW_SC),	/* Critical dial timeout */
	OMCI_ATTR(12, 6, 2, OMCI_RW_SC),	/* Partial dial timeout */
	OMCI_ATTR(11, 8, 1, OMCI_RW_SC),	/* Dial plan format */
	OMCI_ATTR(10, 9, 30, OMCI_RW),	/* Dial plan table */
};

/* G.988 class 146, VoIP application service profile */
static const struct omci_attr_desc omci_voip_app_service_profile_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),	/* CID features */
	OMCI_ATTR(14, 1, 1, OMCI_RW_SC),	/* Call waiting features */
	OMCI_ATTR(13, 2, 2, OMCI_RW_SC),	/* Call progress or transfer features */
	OMCI_ATTR(12, 4, 2, OMCI_RW_SC),	/* Call presentation features */
	OMCI_ATTR(11, 6, 1, OMCI_RW_SC),	/* Direct connect feature */
	OMCI_ATTR(10, 7, 2, OMCI_RW_SC),	/* Direct connect URI pointer */
	OMCI_ATTR(9, 9, 2, OMCI_RW_SC),	/* Bridged line agent URI pointer */
	OMCI_ATTR(8, 11, 2, OMCI_RW_SC),	/* Conference factory URI pointer */
	OMCI_ATTR(7, 13, 2, OMCI_RW),	/* Dial tone feature delay/warmline timer */
};

/* G.988 class 147, VoIP feature access codes */
static const struct omci_attr_desc omci_voip_feature_access_codes_attrs[] = {
	OMCI_ATTR(15, 0, 5, OMCI_RW),	/* Cancel call waiting */
	OMCI_ATTR(14, 5, 5, OMCI_RW),	/* Call hold */
	OMCI_ATTR(13, 10, 5, OMCI_RW),	/* Call park */
	OMCI_ATTR(12, 15, 5, OMCI_RW),	/* Caller ID activate */
	OMCI_ATTR(11, 20, 5, OMCI_RW),	/* Caller ID deactivate */
	OMCI_ATTR(10, 25, 5, OMCI_RW),	/* Do not disturb activation */
	OMCI_ATTR(9, 30, 5, OMCI_RW),	/* Do not disturb deactivation */
	OMCI_ATTR(8, 35, 5, OMCI_RW),	/* Do not disturb PIN change */
	OMCI_ATTR(7, 40, 5, OMCI_RW),	/* Emergency service number */
	OMCI_ATTR(6, 45, 5, OMCI_RW),	/* Intercom service */
	OMCI_ATTR(5, 50, 5, OMCI_RW),	/* Unattended/blind call transfer */
	OMCI_ATTR(4, 55, 5, OMCI_RW),	/* Attended call transfer */
};

/* G.988 class 148, Authentication security method */
static const struct omci_attr_desc omci_auth_method_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),	/* Validation scheme */
	OMCI_ATTR(14, 1, 25, OMCI_RW_SC),	/* Username 1 */
	OMCI_ATTR(13, 26, 25, OMCI_RW),	/* Password */
	OMCI_ATTR(12, 51, 25, OMCI_RW),	/* Realm */
	OMCI_ATTR(11, 76, 14, OMCI_RW),	/* Username 2 */
};

/* G.988 class 150, SIP agent config data */
static const struct omci_attr_desc omci_sip_agent_config_data_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),	/* Proxy server address pointer */
	OMCI_ATTR(14, 2, 2, OMCI_RW_SC),	/* Outbound proxy address pointer */
	OMCI_ATTR(13, 4, 4, OMCI_RW_SC),	/* Primary SIP DNS */
	OMCI_ATTR(12, 8, 4, OMCI_RW_SC),	/* Secondary SIP DNS */
	OMCI_ATTR(11, 12, 2, OMCI_RW),	/* TCP/UDP pointer */
	OMCI_ATTR(10, 14, 4, OMCI_RW),	/* SIP reg exp time */
	OMCI_ATTR(9, 18, 4, OMCI_RW),	/* SIP rereg head start time */
	OMCI_ATTR(8, 22, 2, OMCI_RW_SC),	/* Host part URI */
	OMCI_ATTR(7, 24, 1, OMCI_R),	/* SIP status */
	OMCI_ATTR(6, 25, 2, OMCI_RW_SC),	/* SIP registrar */
	OMCI_ATTR(5, 27, 4, OMCI_RW_SC),	/* Softswitch */
	OMCI_ATTR(4, 31, 5, OMCI_RW),	/* SIP response table */
	OMCI_ATTR(3, 36, 1, OMCI_RW_SC),	/* SIP option transmit control */
	OMCI_ATTR(2, 37, 1, OMCI_R),	/* SIP URI format */
	OMCI_ATTR(1, 38, 2, OMCI_RW_SC),	/* Redundant SIP agent pointer */
};

/* G.988 class 151, SIP agent performance monitoring history data */
static const struct omci_attr_desc omci_sip_agent_pm_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Interval end time */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* Threshold data 1/2 ID */
	OMCI_ATTR(13, 3, 4, OMCI_R),	/* Transactions */
	OMCI_ATTR(12, 7, 4, OMCI_R),	/* Rx invite reqs */
	OMCI_ATTR(11, 11, 4, OMCI_R),	/* Rx invite retrans */
	OMCI_ATTR(10, 15, 4, OMCI_R),	/* Rx noninvite reqs */
	OMCI_ATTR(9, 19, 4, OMCI_R),	/* Rx noninvite retrans */
	OMCI_ATTR(8, 23, 4, OMCI_R),	/* Rx response */
	OMCI_ATTR(7, 27, 4, OMCI_R),	/* Rx response retransmissions */
	OMCI_ATTR(6, 31, 4, OMCI_R),	/* Tx invite reqs */
	OMCI_ATTR(5, 35, 4, OMCI_R),	/* Tx invite retrans */
	OMCI_ATTR(4, 39, 4, OMCI_R),	/* Tx noninvite reqs */
	OMCI_ATTR(3, 43, 4, OMCI_R),	/* Tx noninvite retrans */
	OMCI_ATTR(2, 47, 4, OMCI_R),	/* Tx response */
	OMCI_ATTR(1, 51, 4, OMCI_R),	/* Tx response retransmissions */
};

/* G.988 class 152, SIP call initiation performance monitoring history data */
static const struct omci_attr_desc omci_sip_call_init_pm_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Interval end time */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* Threshold data 1/2 ID */
	OMCI_ATTR(13, 3, 4, OMCI_R),	/* Failed to connect counter */
	OMCI_ATTR(12, 7, 4, OMCI_R),	/* Failed to validate counter */
	OMCI_ATTR(11, 11, 4, OMCI_R),	/* Timeout counter */
	OMCI_ATTR(10, 15, 4, OMCI_R),	/* Failure received counter */
	OMCI_ATTR(9, 19, 4, OMCI_R),	/* Failed to authenticate counter */
};

/* G.988 class 153, SIP user data */
static const struct omci_attr_desc omci_sip_user_data_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),	/* SIP agent pointer */
	OMCI_ATTR(14, 2, 2, OMCI_RW_SC),	/* User part AOR */
	OMCI_ATTR(13, 4, 25, OMCI_RW),	/* SIP display name */
	OMCI_ATTR(12, 29, 2, OMCI_RW_SC),	/* Username/password */
	OMCI_ATTR(11, 31, 2, OMCI_RW_SC),	/* Voicemail server SIP URI */
	OMCI_ATTR(10, 33, 4, OMCI_RW_SC),	/* Voicemail subscription expiration time */
	OMCI_ATTR(9, 37, 2, OMCI_RW_SC),	/* Network dial plan pointer */
	OMCI_ATTR(8, 39, 2, OMCI_RW_SC),	/* Application services profile pointer */
	OMCI_ATTR(7, 41, 2, OMCI_RW_SC),	/* Feature code pointer */
	OMCI_ATTR(6, 43, 2, OMCI_RW_SC),	/* PPTP pointer */
	OMCI_ATTR(5, 45, 1, OMCI_RW),	/* Release timer */
	OMCI_ATTR(4, 46, 1, OMCI_RW),	/* ROH timer */
};

/* G.988 class 157, Large string */
static const struct omci_attr_desc omci_large_string_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),	/* Number of parts */
	OMCI_ATTR(14, 1, 25, OMCI_RW),	/* Part 1 */
	OMCI_ATTR(13, 26, 25, OMCI_RW),	/* Part 2 */
	OMCI_ATTR(12, 51, 25, OMCI_RW),	/* Part 3 */
	OMCI_ATTR(11, 76, 25, OMCI_RW),	/* Part 4 */
	OMCI_ATTR(10, 101, 25, OMCI_RW),	/* Part 5 */
	OMCI_ATTR(9, 126, 25, OMCI_RW),	/* Part 6 */
	OMCI_ATTR(8, 151, 25, OMCI_RW),	/* Part 7 */
	OMCI_ATTR(7, 176, 25, OMCI_RW),	/* Part 8 */
	OMCI_ATTR(6, 201, 25, OMCI_RW),	/* Part 9 */
	OMCI_ATTR(5, 226, 25, OMCI_RW),	/* Part 10 */
	OMCI_ATTR(4, 251, 25, OMCI_RW),	/* Part 11 */
	OMCI_ATTR(3, 276, 25, OMCI_RW),	/* Part 12 */
	OMCI_ATTR(2, 301, 25, OMCI_RW),	/* Part 13 */
	OMCI_ATTR(1, 326, 25, OMCI_RW),	/* Part 14 */
	OMCI_ATTR(0, 351, 25, OMCI_RW),	/* Part 15 */
};

/* G.988 class 158, ONU remote debug */
static const struct omci_attr_desc omci_onu_remote_debug_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Command format */
	OMCI_ATTR(14, 1, 25, OMCI_W),	/* Command */
	OMCI_ATTR(13, 26, 25, OMCI_R),	/* Reply table */
};

/* G.988 class 273, Threshold data 1 */
static const struct omci_attr_desc omci_threshold_data_1_attrs[] = {
	OMCI_ATTR(15, 0, 4, OMCI_RW_SC),	/* Threshold value 1 */
	OMCI_ATTR(14, 4, 4, OMCI_RW_SC),	/* Threshold value 2 */
	OMCI_ATTR(13, 8, 4, OMCI_RW_SC),	/* Threshold value 3 */
	OMCI_ATTR(12, 12, 4, OMCI_RW_SC),	/* Threshold value 4 */
	OMCI_ATTR(11, 16, 4, OMCI_RW_SC),	/* Threshold value 5 */
	OMCI_ATTR(10, 20, 4, OMCI_RW_SC),	/* Threshold value 6 */
	OMCI_ATTR(9, 24, 4, OMCI_RW_SC),	/* Threshold value 7 */
};

/* G.988 class 274, Threshold data 2 */
static const struct omci_attr_desc omci_threshold_data_2_attrs[] = {
	OMCI_ATTR(15, 0, 4, OMCI_RW_SC),	/* Threshold value 8 */
	OMCI_ATTR(14, 4, 4, OMCI_RW_SC),	/* Threshold value 9 */
	OMCI_ATTR(13, 8, 4, OMCI_RW_SC),	/* Threshold value 10 */
	OMCI_ATTR(12, 12, 4, OMCI_RW_SC),	/* Threshold value 11 */
	OMCI_ATTR(11, 16, 4, OMCI_RW_SC),	/* Threshold value 12 */
	OMCI_ATTR(10, 20, 4, OMCI_RW_SC),	/* Threshold value 13 */
	OMCI_ATTR(9, 24, 4, OMCI_RW_SC),	/* Threshold value 14 */
};

/* G.988 class 276, GAL Ethernet performance monitoring history data */
static const struct omci_attr_desc omci_gal_ethernet_pm_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Interval end time */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* Threshold data 1/2 ID */
	OMCI_ATTR(13, 3, 4, OMCI_R),	/* Discarded frames */
};

/* G.988 class 296, Ethernet performance monitoring history data 3 */
static const struct omci_attr_desc omci_ethernet_pm_3_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Interval end time */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* Threshold data 1/2 ID */
	OMCI_ATTR(13, 3, 4, OMCI_R),	/* Drop events */
	OMCI_ATTR(12, 7, 4, OMCI_R),	/* Octets */
	OMCI_ATTR(11, 11, 4, OMCI_R),	/* Packets */
	OMCI_ATTR(10, 15, 4, OMCI_R),	/* Broadcast packets */
	OMCI_ATTR(9, 19, 4, OMCI_R),	/* Multicast packets */
	OMCI_ATTR(8, 23, 4, OMCI_R),	/* Undersize packets */
	OMCI_ATTR(7, 27, 4, OMCI_R),	/* Fragments */
	OMCI_ATTR(6, 31, 4, OMCI_R),	/* Jabbers */
	OMCI_ATTR(5, 35, 4, OMCI_R),	/* Packets 64 octets */
	OMCI_ATTR(4, 39, 4, OMCI_R),	/* Packets 65 to 127 octets */
	OMCI_ATTR(3, 43, 4, OMCI_R),	/* Packets 128 to 255 octets */
	OMCI_ATTR(2, 47, 4, OMCI_R),	/* Packets 256 to 511 octets */
	OMCI_ATTR(1, 51, 4, OMCI_R),	/* Packets 512 to 1023 octets */
	OMCI_ATTR(0, 55, 4, OMCI_R),	/* Packets 1024 to 1518 octets */
};

/* G.988 class 298, Dot1 rate limiter */
static const struct omci_attr_desc omci_dot1_rate_limiter_attrs[] = {
	OMCI_ATTR(15, 0, 2, OMCI_RW_SC),	/* Parent ME pointer */
	OMCI_ATTR(14, 2, 1, OMCI_RW_SC),	/* TP type */
	OMCI_ATTR(13, 3, 2, OMCI_RW_SC),	/* Upstream unicast flood rate pointer */
	OMCI_ATTR(12, 5, 2, OMCI_RW_SC),	/* Upstream broadcast rate pointer */
	OMCI_ATTR(11, 7, 2, OMCI_RW_SC),	/* Upstream multicast payload rate pointer */
	OMCI_ATTR(10, 9, 2, OMCI_RW),	/* Downstream unicast flood rate pointer */
	OMCI_ATTR(9, 11, 2, OMCI_RW),	/* Downstream broadcast rate pointer */
	OMCI_ATTR(8, 13, 2, OMCI_RW),	/* Downstream multicast payload rate pointer */
};

/* G.988 class 311, Multicast subscriber monitor */
static const struct omci_attr_desc omci_multicast_subscriber_monitor_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW_SC),	/* ME type */
	OMCI_ATTR(14, 1, 4, OMCI_R),	/* Current multicast bandwidth */
	OMCI_ATTR(13, 5, 4, OMCI_R),	/* Join messages counter */
	OMCI_ATTR(12, 9, 4, OMCI_R),	/* Bandwidth exceeded counter */
	OMCI_ATTR(11, 13, 24, OMCI_R),	/* IPv4 active group list table */
	OMCI_ATTR(10, 37, 58, OMCI_R),	/* IPv6 active group list table */
};

/* G.988 class 336, ONU dynamic power management control */
static const struct omci_attr_desc omci_onu_dynamic_power_mgmt_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Power reduction management capability */
	OMCI_ATTR(14, 1, 1, OMCI_RW),	/* Power reduction management mode */
	OMCI_ATTR(13, 2, 2, OMCI_R),	/* Itransinit */
	OMCI_ATTR(12, 4, 2, OMCI_R),	/* Itxinit */
	OMCI_ATTR(11, 6, 4, OMCI_RW),	/* Maximum sleep interval */
	OMCI_ATTR(10, 10, 4, OMCI_RW),	/* Maximum receiver-off interval */
	OMCI_ATTR(9, 14, 4, OMCI_RW),	/* Minimum aware interval */
	OMCI_ATTR(8, 18, 2, OMCI_RW),	/* Minimum active held interval */
	OMCI_ATTR(7, 20, 8, OMCI_RW),	/* Maximum sleep interval extension */
	OMCI_ATTR(6, 28, 1, OMCI_R),	/* Ethernet passive optical network capability extension */
	OMCI_ATTR(5, 29, 1, OMCI_RW),	/* Ethernet passive optical network setup extension */
	OMCI_ATTR(4, 30, 4, OMCI_RW),	/* Missing consecutive bursts threshold */
};

/* G.988 class 340, TR-069 management server */
static const struct omci_attr_desc omci_tr069_management_server_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_RW),	/* Administrative state */
	OMCI_ATTR(14, 1, 2, OMCI_RW),	/* ACS network address */
	OMCI_ATTR(13, 3, 2, OMCI_RW),	/* Associated tag */
};

/* G.988 class 341, GEM port network CTP performance monitoring history data */
static const struct omci_attr_desc omci_gem_port_ctp_pm_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Interval end time */
	OMCI_ATTR(14, 1, 2, OMCI_RW_SC),	/* Threshold data 1/2 ID */
	OMCI_ATTR(13, 3, 4, OMCI_R),	/* Transmitted GEM frames */
	OMCI_ATTR(12, 7, 4, OMCI_R),	/* Received GEM frames */
	OMCI_ATTR(11, 11, 8, OMCI_R),	/* Received payload bytes */
	OMCI_ATTR(10, 19, 8, OMCI_R),	/* Transmitted payload bytes */
	OMCI_ATTR(9, 27, 4, OMCI_R),	/* Encryption key errors */
};

/* G.988 class 334, Ethernet frame extended PM */
static const struct omci_attr_desc omci_ethernet_frame_ext_pm_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Interval end time */
	OMCI_ATTR(14, 1, 16, OMCI_RW_SC),	/* Control block */
	OMCI_ATTR(13, 17, 4, OMCI_R),	/* Drop events */
	OMCI_ATTR(12, 21, 4, OMCI_R),	/* Octets */
	OMCI_ATTR(11, 25, 4, OMCI_R),	/* Frames */
	OMCI_ATTR(10, 29, 4, OMCI_R),	/* Broadcast frames */
	OMCI_ATTR(9, 33, 4, OMCI_R),	/* Multicast frames */
	OMCI_ATTR(8, 37, 4, OMCI_R),	/* CRC errored frames */
	OMCI_ATTR(7, 41, 4, OMCI_R),	/* Undersize frames */
	OMCI_ATTR(6, 45, 4, OMCI_R),	/* Oversize frames */
	OMCI_ATTR(5, 49, 4, OMCI_R),	/* Frames 64 octets */
	OMCI_ATTR(4, 53, 4, OMCI_R),	/* Frames 65 to 127 octets */
	OMCI_ATTR(3, 57, 4, OMCI_R),	/* Frames 128 to 255 octets */
	OMCI_ATTR(2, 61, 4, OMCI_R),	/* Frames 256 to 511 octets */
	OMCI_ATTR(1, 65, 4, OMCI_R),	/* Frames 512 to 1023 octets */
	OMCI_ATTR(0, 69, 4, OMCI_R),	/* Frames 1024 to 1518 octets */
};

/* G.988 class 425, Ethernet frame extended PM 64-bit */
static const struct omci_attr_desc omci_ethernet_frame_ext_pm_64_attrs[] = {
	OMCI_ATTR(15, 0, 1, OMCI_R),	/* Interval end time */
	OMCI_ATTR(14, 1, 16, OMCI_RW_SC),	/* Control block */
	OMCI_ATTR(13, 17, 8, OMCI_R),	/* Drop events */
	OMCI_ATTR(12, 25, 8, OMCI_R),	/* Octets */
	OMCI_ATTR(11, 33, 8, OMCI_R),	/* Frames */
	OMCI_ATTR(10, 41, 8, OMCI_R),	/* Broadcast frames */
	OMCI_ATTR(9, 49, 8, OMCI_R),	/* Multicast frames */
	OMCI_ATTR(8, 57, 8, OMCI_R),	/* CRC errored frames */
	OMCI_ATTR(7, 65, 8, OMCI_R),	/* Undersize frames */
	OMCI_ATTR(6, 73, 8, OMCI_R),	/* Oversize frames */
	OMCI_ATTR(5, 81, 8, OMCI_R),	/* Frames 64 octets */
	OMCI_ATTR(4, 89, 8, OMCI_R),	/* Frames 65 to 127 octets */
	OMCI_ATTR(3, 97, 8, OMCI_R),	/* Frames 128 to 255 octets */
	OMCI_ATTR(2, 105, 8, OMCI_R),	/* Frames 256 to 511 octets */
	OMCI_ATTR(1, 113, 8, OMCI_R),	/* Frames 512 to 1023 octets */
	OMCI_ATTR(0, 121, 8, OMCI_R),	/* Frames 1024 to 1518 octets */
};

#define STANDARD_ACTIONS (OMCI_ME_ACTION_GET | OMCI_ME_ACTION_SET)
#define OLT_CREATED_ACTIONS (OMCI_ME_ACTION_CREATE | OMCI_ME_ACTION_DELETE | \
			     OMCI_ME_ACTION_GET | OMCI_ME_ACTION_SET)
#define PM_ACTIONS (OLT_CREATED_ACTIONS | OMCI_ME_ACTION_GET_CURRENT_DATA)
#define STANDARD_DESC(_class, _name, _actions, _flags, _mask, _upload, \
		      _len, _category, _support, _attrs) \
	{ .class_id = (_class), .name = (_name), \
	  .profile_mask = OMCI_PROFILE_STANDARD_MASK, .actions = (_actions), \
	  .flags = (_flags), .valid_attr_mask = (_mask), \
	  .mib_upload_mask = (_upload), .data_len = (_len), \
	  .category = (_category), .support = (_support), \
	  .attrs = (_attrs), .num_attrs = OMCI_ARRAY_SIZE(_attrs) }
#define VENDOR_DESC(_class, _name, _profile, _actions, _flags, _mask, \
		    _upload, _len, _category, _support, _attrs) \
	{ .class_id = (_class), .name = (_name), \
	  .profile_mask = OMCI_PROFILE_BIT(_profile), .actions = (_actions), \
	  .flags = (_flags) | OMCI_ME_F_VENDOR, .valid_attr_mask = (_mask), \
	  .mib_upload_mask = (_upload), .data_len = (_len), \
	  .category = (_category), .support = (_support), \
	  .attrs = (_attrs), .num_attrs = OMCI_ARRAY_SIZE(_attrs) }
#define VENDOR_MASK_DESC(_class, _name, _pmask, _actions, _flags, _mask, \
			 _upload, _len, _category, _support, _attrs) \
	{ .class_id = (_class), .name = (_name), \
	  .profile_mask = (_pmask), .actions = (_actions), \
	  .flags = (_flags) | OMCI_ME_F_VENDOR, .valid_attr_mask = (_mask), \
	  .mib_upload_mask = (_upload), .data_len = (_len), \
	  .category = (_category), .support = (_support), \
	  .attrs = (_attrs), .num_attrs = OMCI_ARRAY_SIZE(_attrs) }

static const struct omci_me_desc omci_me_descs[] = {
	STANDARD_DESC(OMCI_CLASS_SOFTWARE_IMAGE, "Software image",
		      OMCI_ME_ACTION_GET, OMCI_ME_F_ONU_CREATED,
		      OMCI_GENMASK(15, 12), OMCI_GENMASK(15, 12), 17,
		      OMCI_CLASS_CATEGORY_EQUIPMENT, OMCI_CLASS_SUPPORT_NATIVE,
		      omci_software_image_attrs),
	STANDARD_DESC(OMCI_CLASS_CARDHOLDER, "Cardholder", STANDARD_ACTIONS,
		      OMCI_ME_F_ONU_CREATED, OMCI_GENMASK(15, 10), OMCI_GENMASK(15, 10),
		      45, OMCI_CLASS_CATEGORY_EQUIPMENT,
		      OMCI_CLASS_SUPPORT_NATIVE, omci_cardholder_attrs),
	STANDARD_DESC(OMCI_CLASS_CIRCUIT_PACK, "Circuit pack", STANDARD_ACTIONS,
		      OMCI_ME_F_ONU_CREATED, OMCI_GENMASK(15, 2), OMCI_GENMASK(15, 2),
		      59, OMCI_CLASS_CATEGORY_EQUIPMENT,
		      OMCI_CLASS_SUPPORT_NATIVE, omci_circuit_pack_attrs),
	STANDARD_DESC(OMCI_CLASS_PPTP_ETHERNET_UNI,
		      "Physical path termination point Ethernet UNI",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED | OMCI_ME_F_DATAPATH,
		      OMCI_GENMASK(15, 1), OMCI_GENMASK(15, 1), 17,
		      OMCI_CLASS_CATEGORY_UNI, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_pptp_ethernet_uni_attrs),
	STANDARD_DESC(OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA,
		      "MAC bridge port configuration data", OLT_CREATED_ACTIONS,
		      OMCI_ME_F_DATAPATH, OMCI_GENMASK(15, 3), OMCI_GENMASK(15, 3), 24,
		      OMCI_CLASS_CATEGORY_LAYER2, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_bridge_port_attrs),
	STANDARD_DESC(OMCI_CLASS_VLAN_TAGGING_FILTER_DATA,
		      "VLAN tagging filter data", OLT_CREATED_ACTIONS,
		      OMCI_ME_F_DATAPATH, OMCI_GENMASK(15, 13), OMCI_GENMASK(15, 13), 26,
		      OMCI_CLASS_CATEGORY_LAYER2, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_vlan_filter_attrs),
	STANDARD_DESC(OMCI_CLASS_8021P_MAPPER,
		      "802.1p mapper service profile", OLT_CREATED_ACTIONS,
		      OMCI_ME_F_DATAPATH, OMCI_GENMASK(15, 3), OMCI_GENMASK(15, 3), 45,
		      OMCI_CLASS_CATEGORY_LAYER2, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_8021p_mapper_attrs),
	STANDARD_DESC(OMCI_CLASS_EXTENDED_VLAN,
		      "Extended VLAN tagging operation configuration data",
		      OLT_CREATED_ACTIONS | OMCI_ME_ACTION_SET_TABLE,
		      /* Bit 10, the rule table, is not part of an MIB upload. */
		      OMCI_ME_F_DATAPATH, OMCI_GENMASK(15, 8),
		      OMCI_GENMASK(15, 8) & ~OMCI_BIT(10), 50,
		      OMCI_CLASS_CATEGORY_LAYER2, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_extended_vlan_attrs),
	STANDARD_DESC(OMCI_CLASS_TCONT, "T-CONT", STANDARD_ACTIONS,
		      OMCI_ME_F_ONU_CREATED | OMCI_ME_F_DATAPATH,
		      OMCI_GENMASK(15, 13), OMCI_GENMASK(15, 13), 4,
		      OMCI_CLASS_CATEGORY_ANI, OMCI_CLASS_SUPPORT_NATIVE,
		      omci_tcont_attrs),
	STANDARD_DESC(OMCI_CLASS_ANI_G, "ANI-G",
		      STANDARD_ACTIONS | OMCI_ME_ACTION_TEST,
		      OMCI_ME_F_ONU_CREATED, OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 21,
		      OMCI_CLASS_CATEGORY_ANI, OMCI_CLASS_SUPPORT_NATIVE,
		      omci_ani_g_attrs),
	STANDARD_DESC(OMCI_CLASS_UNI_G, "UNI-G", STANDARD_ACTIONS,
		      OMCI_ME_F_ONU_CREATED, OMCI_GENMASK(15, 12), OMCI_GENMASK(15, 12),
		      6, OMCI_CLASS_CATEGORY_UNI, OMCI_CLASS_SUPPORT_NATIVE,
		      omci_uni_g_attrs),
	STANDARD_DESC(OMCI_CLASS_GEM_IWTP,
		      "GEM interworking termination point", OLT_CREATED_ACTIONS,
		      OMCI_ME_F_DATAPATH, OMCI_GENMASK(15, 8), OMCI_GENMASK(15, 8), 12,
		      OMCI_CLASS_CATEGORY_ANI, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_gem_iwtp_attrs),
	STANDARD_DESC(OMCI_CLASS_GEM_PORT_CTP, "GEM port network CTP",
		      OLT_CREATED_ACTIONS, OMCI_ME_F_DATAPATH,
		      OMCI_GENMASK(15, 6), OMCI_GENMASK(15, 6), 17,
		      OMCI_CLASS_CATEGORY_ANI, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_gem_port_ctp_attrs),
	STANDARD_DESC(OMCI_CLASS_TRAFFIC_DESCRIPTOR, "Traffic descriptor",
		      OLT_CREATED_ACTIONS, OMCI_ME_F_DATAPATH,
		      OMCI_GENMASK(15, 8), OMCI_GENMASK(15, 8), 20,
		      OMCI_CLASS_CATEGORY_ANI, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_traffic_descriptor_attrs),
	STANDARD_DESC(OMCI_CLASS_FEC_PM,
		      "FEC performance monitoring history data",
		      OLT_CREATED_ACTIONS | OMCI_ME_ACTION_GET_CURRENT_DATA, 0,
		      OMCI_GENMASK(15, 9), OMCI_BIT(14), 21,
		      OMCI_CLASS_CATEGORY_ANI, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_fec_pm_attrs),
	STANDARD_DESC(OMCI_CLASS_OMCI, "OMCI",
		      OMCI_ME_ACTION_GET | OMCI_ME_ACTION_GET_NEXT,
		      OMCI_ME_F_ONU_CREATED | OMCI_ME_F_NO_MIB_UPLOAD,
		      OMCI_GENMASK(15, 14), 0, 8,
		      OMCI_CLASS_CATEGORY_MANAGEMENT, OMCI_CLASS_SUPPORT_NATIVE,
		      omci_omci_attrs),
	STANDARD_DESC(OMCI_CLASS_MANAGED_ENTITY, "Managed entity",
		      OMCI_ME_ACTION_GET | OMCI_ME_ACTION_GET_NEXT,
		      OMCI_ME_F_ONU_CREATED | OMCI_ME_F_NO_MIB_UPLOAD,
		      OMCI_GENMASK(15, 8), 0, 47,
		      OMCI_CLASS_CATEGORY_MANAGEMENT, OMCI_CLASS_SUPPORT_NATIVE,
		      omci_managed_entity_attrs),
	STANDARD_DESC(OMCI_CLASS_ATTRIBUTE, "Attribute",
		      OMCI_ME_ACTION_GET | OMCI_ME_ACTION_GET_NEXT,
		      OMCI_ME_F_ONU_CREATED | OMCI_ME_F_NO_MIB_UPLOAD,
		      OMCI_GENMASK(15, 7), 0, 46,
		      OMCI_CLASS_CATEGORY_MANAGEMENT, OMCI_CLASS_SUPPORT_NATIVE,
		      omci_attribute_attrs),
	STANDARD_DESC(OMCI_CLASS_VEIP, "Virtual Ethernet interface point",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED | OMCI_ME_F_DATAPATH,
		      OMCI_GENMASK(15, 11), OMCI_GENMASK(15, 11), 31,
		      OMCI_CLASS_CATEGORY_UNI, OMCI_CLASS_SUPPORT_PROVISIONED,
		      omci_veip_attrs),

	STANDARD_DESC(OMCI_CLASS_ETHERNET_PM, "Ethernet performance monitoring history data",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 59,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_ethernet_pm_attrs),
	STANDARD_DESC(OMCI_CLASS_MAC_BRIDGE_PORT_FILTER_TABLE, "MAC bridge port filter table data",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		      OMCI_GENMASK(15, 15), 0x0000, 8,
		      OMCI_CLASS_CATEGORY_LAYER2, OMCI_CLASS_SUPPORT_PARSED,
		      omci_mac_bridge_port_filter_table_attrs),
	STANDARD_DESC(OMCI_CLASS_MAC_BRIDGE_PORT_BRIDGE_TABLE, "MAC bridge port bridge table data",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		      OMCI_GENMASK(15, 15), 0x0000, 8,
		      OMCI_CLASS_CATEGORY_LAYER2, OMCI_CLASS_SUPPORT_PARSED,
		      omci_mac_bridge_port_bridge_table_attrs),
	STANDARD_DESC(OMCI_CLASS_MAC_BRIDGE_PM, "MAC bridge performance monitoring history data",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 13), OMCI_GENMASK(15, 13), 7,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_mac_bridge_pm_attrs),
	STANDARD_DESC(OMCI_CLASS_MAC_BRIDGE_PORT_PM, "MAC bridge port performance monitoring history data",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 9), OMCI_GENMASK(15, 9), 23,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_mac_bridge_port_pm_attrs),
	STANDARD_DESC(OMCI_CLASS_PPTP_POTS_UNI, "Physical path termination point POTS UNI",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		      OMCI_GENMASK(15, 3), OMCI_GENMASK(15, 3), 15,
		      OMCI_CLASS_CATEGORY_VOICE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_pptp_pots_uni_attrs),
	STANDARD_DESC(OMCI_CLASS_VOICE_SERVICE_PROFILE, "Voice service profile",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 2), 0xff84, 57,
		      OMCI_CLASS_CATEGORY_VOICE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_voice_service_profile_attrs),
	STANDARD_DESC(OMCI_CLASS_MAC_BRIDGE_PORT_FILTER_PREASSIGN, "MAC bridge port filter preassign table",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		      OMCI_GENMASK(15, 6), OMCI_GENMASK(15, 6), 10,
		      OMCI_CLASS_CATEGORY_LAYER2, OMCI_CLASS_SUPPORT_PARSED,
		      omci_mac_bridge_port_filter_preassign_attrs),
	STANDARD_DESC(OMCI_CLASS_ETHERNET_PM_2, "Ethernet performance monitoring history data 2",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 13), OMCI_GENMASK(15, 13), 7,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_ethernet_pm_2_attrs),
	STANDARD_DESC(OMCI_CLASS_ONU_POWER_SHEDDING, "ONU power shedding",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		      OMCI_GENMASK(15, 5), OMCI_GENMASK(15, 5), 22,
		      OMCI_CLASS_CATEGORY_EQUIPMENT, OMCI_CLASS_SUPPORT_PARSED,
		      omci_onu_power_shedding_attrs),
	STANDARD_DESC(OMCI_CLASS_IP_HOST_CONFIG_DATA, "IP host config data",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		      OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 124,
		      OMCI_CLASS_CATEGORY_LAYER3, OMCI_CLASS_SUPPORT_PARSED,
		      omci_ip_host_config_data_attrs),
	STANDARD_DESC(OMCI_CLASS_TCP_UDP_CONFIG_DATA, "TCP/UDP config data",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 12), OMCI_GENMASK(15, 12), 6,
		      OMCI_CLASS_CATEGORY_LAYER3, OMCI_CLASS_SUPPORT_PARSED,
		      omci_tcp_udp_config_data_attrs),
	STANDARD_DESC(OMCI_CLASS_NETWORK_ADDRESS, "Network address",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 14), OMCI_GENMASK(15, 14), 4,
		      OMCI_CLASS_CATEGORY_MANAGEMENT, OMCI_CLASS_SUPPORT_PARSED,
		      omci_network_address_attrs),
	STANDARD_DESC(OMCI_CLASS_VOIP_CONFIG_DATA, "VoIP config data",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		      OMCI_GENMASK(15, 8), OMCI_GENMASK(15, 8), 36,
		      OMCI_CLASS_CATEGORY_VOICE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_voip_config_data_attrs),
	STANDARD_DESC(OMCI_CLASS_VOIP_VOICE_CTP, "VoIP voice CTP",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 12), OMCI_GENMASK(15, 12), 7,
		      OMCI_CLASS_CATEGORY_VOICE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_voip_voice_ctp_attrs),
	STANDARD_DESC(OMCI_CLASS_CALL_CONTROL_PM, "Call control performance monitoring history data",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 9), OMCI_GENMASK(15, 9), 23,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_call_control_pm_attrs),
	STANDARD_DESC(OMCI_CLASS_VOIP_LINE_STATUS, "VoIP line status",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		      OMCI_GENMASK(15, 7), OMCI_GENMASK(15, 7), 60,
		      OMCI_CLASS_CATEGORY_VOICE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_voip_line_status_attrs),
	STANDARD_DESC(OMCI_CLASS_VOIP_MEDIA_PROFILE, "VoIP media profile",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 18,
		      OMCI_CLASS_CATEGORY_VOICE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_voip_media_profile_attrs),
	STANDARD_DESC(OMCI_CLASS_RTP_PROFILE_DATA, "RTP profile data",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 8), OMCI_GENMASK(15, 8), 11,
		      OMCI_CLASS_CATEGORY_VOICE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_rtp_profile_data_attrs),
	STANDARD_DESC(OMCI_CLASS_RTP_PM, "RTP performance monitoring history data",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 8), OMCI_GENMASK(15, 8), 27,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_rtp_pm_attrs),
	STANDARD_DESC(OMCI_CLASS_NETWORK_DIAL_PLAN_TABLE, "Network dial plan table",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 10), 0xf800, 39,
		      OMCI_CLASS_CATEGORY_VOICE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_network_dial_plan_table_attrs),
	STANDARD_DESC(OMCI_CLASS_VOIP_APP_SERVICE_PROFILE, "VoIP application service profile",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 7), OMCI_GENMASK(15, 7), 15,
		      OMCI_CLASS_CATEGORY_VOICE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_voip_app_service_profile_attrs),
	STANDARD_DESC(OMCI_CLASS_VOIP_FEATURE_ACCESS_CODES, "VoIP feature access codes",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 4), OMCI_GENMASK(15, 4), 60,
		      OMCI_CLASS_CATEGORY_VOICE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_voip_feature_access_codes_attrs),
	STANDARD_DESC(OMCI_CLASS_AUTH_METHOD, "Authentication security method",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 11), OMCI_GENMASK(15, 11), 90,
		      OMCI_CLASS_CATEGORY_SECURITY, OMCI_CLASS_SUPPORT_PARSED,
		      omci_auth_method_attrs),
	STANDARD_DESC(OMCI_CLASS_SIP_AGENT_CONFIG_DATA, "SIP agent config data",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 1), 0xffee, 40,
		      OMCI_CLASS_CATEGORY_VOICE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_sip_agent_config_data_attrs),
	STANDARD_DESC(OMCI_CLASS_SIP_AGENT_PM, "SIP agent performance monitoring history data",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 1), OMCI_GENMASK(15, 1), 55,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_sip_agent_pm_attrs),
	STANDARD_DESC(OMCI_CLASS_SIP_CALL_INIT_PM, "SIP call initiation performance monitoring history data",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 9), OMCI_GENMASK(15, 9), 23,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_sip_call_init_pm_attrs),
	STANDARD_DESC(OMCI_CLASS_SIP_USER_DATA, "SIP user data",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 4), OMCI_GENMASK(15, 4), 47,
		      OMCI_CLASS_CATEGORY_VOICE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_sip_user_data_attrs),
	STANDARD_DESC(OMCI_CLASS_LARGE_STRING, "Large string",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 376,
		      OMCI_CLASS_CATEGORY_MANAGEMENT, OMCI_CLASS_SUPPORT_PARSED,
		      omci_large_string_attrs),
	STANDARD_DESC(OMCI_CLASS_ONU_REMOTE_DEBUG, "ONU remote debug",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		      OMCI_GENMASK(15, 13), 0xc000, 51,
		      OMCI_CLASS_CATEGORY_EQUIPMENT, OMCI_CLASS_SUPPORT_PARSED,
		      omci_onu_remote_debug_attrs),
	STANDARD_DESC(OMCI_CLASS_THRESHOLD_DATA_1, "Threshold data 1",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 9), OMCI_GENMASK(15, 9), 28,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_threshold_data_1_attrs),
	STANDARD_DESC(OMCI_CLASS_THRESHOLD_DATA_2, "Threshold data 2",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 9), OMCI_GENMASK(15, 9), 28,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_threshold_data_2_attrs),
	STANDARD_DESC(OMCI_CLASS_GAL_ETHERNET_PM, "GAL Ethernet performance monitoring history data",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 13), OMCI_GENMASK(15, 13), 7,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_gal_ethernet_pm_attrs),
	STANDARD_DESC(OMCI_CLASS_ETHERNET_PM_3, "Ethernet performance monitoring history data 3",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 59,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_ethernet_pm_3_attrs),
	STANDARD_DESC(OMCI_CLASS_DOT1_RATE_LIMITER, "Dot1 rate limiter",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 8), OMCI_GENMASK(15, 8), 15,
		      OMCI_CLASS_CATEGORY_LAYER2, OMCI_CLASS_SUPPORT_PARSED,
		      omci_dot1_rate_limiter_attrs),
	STANDARD_DESC(OMCI_CLASS_MULTICAST_SUBSCRIBER_MONITOR, "Multicast subscriber monitor",
		      OLT_CREATED_ACTIONS, 0,
		      OMCI_GENMASK(15, 10), 0xf000, 95,
		      OMCI_CLASS_CATEGORY_MULTICAST, OMCI_CLASS_SUPPORT_PARSED,
		      omci_multicast_subscriber_monitor_attrs),
	STANDARD_DESC(OMCI_CLASS_ONU_DYNAMIC_POWER_MGMT, "ONU dynamic power management control",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		      OMCI_GENMASK(15, 4), OMCI_GENMASK(15, 4), 34,
		      OMCI_CLASS_CATEGORY_EQUIPMENT, OMCI_CLASS_SUPPORT_PARSED,
		      omci_onu_dynamic_power_mgmt_attrs),
	STANDARD_DESC(OMCI_CLASS_TR069_MANAGEMENT_SERVER, "TR-069 management server",
		      STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		      OMCI_GENMASK(15, 13), OMCI_GENMASK(15, 13), 5,
		      OMCI_CLASS_CATEGORY_LAYER3, OMCI_CLASS_SUPPORT_PARSED,
		      omci_tr069_management_server_attrs),
	STANDARD_DESC(OMCI_CLASS_GEM_PORT_CTP_PM, "GEM port network CTP performance monitoring history data",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 9), OMCI_GENMASK(15, 9), 31,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_gem_port_ctp_pm_attrs),
	STANDARD_DESC(OMCI_CLASS_ETHERNET_FRAME_EXT_PM, "Ethernet frame extended PM",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 73,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_ethernet_frame_ext_pm_attrs),
	STANDARD_DESC(OMCI_CLASS_ETHERNET_FRAME_EXT_PM_64, "Ethernet frame extended PM 64-bit",
		      PM_ACTIONS, 0,
		      OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 129,
		      OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_PARSED,
		      omci_ethernet_frame_ext_pm_64_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_FLOW_MAPPING,
		    "Huawei ONU management", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 38,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_350_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_351,
		    "Huawei extended configuration 351", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 4), OMCI_GENMASK(15, 4), 12,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_351_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_VLAN_MAPPING,
		    "Huawei VLAN mapping configuration", OMCI_OLT_PROFILE_HUAWEI,
		    OLT_CREATED_ACTIONS, OMCI_ME_F_NO_MIB_UPLOAD | OMCI_ME_F_DATAPATH,
		    OMCI_GENMASK(15, 10), 0, 15, OMCI_CLASS_CATEGORY_VENDOR,
		    OMCI_CLASS_SUPPORT_SHADOW, omci_huawei_352_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_SW_IMAGE_EXT,
		    "Huawei VoIP image", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 11), OMCI_GENMASK(15, 11), 21,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_NATIVE,
		    omci_huawei_353_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_MULTICAST_370,
		    "Huawei UNI QoS", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED | OMCI_ME_F_DATAPATH,
		    OMCI_GENMASK(15, 13), OMCI_GENMASK(15, 13), 10,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_370_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_MULTICAST_373,
		    "Huawei product identification", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 376,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_373_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_EXT_ONT_G2,
		    "Huawei extended ONT-G2", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 43,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65398_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_MULTICAST_65414,
		    "Huawei optical data", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 3), OMCI_GENMASK(15, 3), 45,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65414_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_EXT_WAN_INFO,
		    "Huawei extended WAN info", OMCI_OLT_PROFILE_HUAWEI,
		    OLT_CREATED_ACTIONS, 0,
		    OMCI_GENMASK(15, 13), OMCI_GENMASK(15, 13), 3,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65415_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_EXT_PPP,
		    "Huawei extended PPP", OMCI_OLT_PROFILE_HUAWEI,
		    OLT_CREATED_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 8), OMCI_GENMASK(15, 8), 84,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65417_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_EXT_ONT3_ABILITY,
		    "Huawei extended ONT3 ability", OMCI_OLT_PROFILE_HUAWEI,
		    OMCI_ME_ACTION_GET, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 59,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65423_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_MULTICAST_65425,
		    "Huawei extended CPU", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 9), OMCI_GENMASK(15, 9), 20,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65425_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_MAINTENANCE,
		    "Huawei maintenance", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 14), OMCI_GENMASK(15, 14), 4,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65426_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_CAPABILITY,
		    "Huawei ME 65427", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 14), OMCI_GENMASK(15, 14), 26,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65427_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_EXT_TYPE,
		    "Huawei extended type", OMCI_OLT_PROFILE_HUAWEI,
		    OMCI_ME_ACTION_GET, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 16,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65430_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_EXT_OLT,
		    "Huawei extended OLT", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_BIT(15), OMCI_BIT(15), 4,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65431_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_EXT_80211,
		    "Huawei extended 802.11", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_BIT(15), OMCI_BIT(15), 8,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65444_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_EXT_MAC,
		    "Huawei extended MAC", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_BIT(15), OMCI_BIT(15), 1,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65447_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_EXT_80211_IF,
		    "Huawei extended 802.11 interface", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 11), OMCI_GENMASK(15, 11), 5,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65450_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_EXT_WAN_CFG,
		    "Huawei extended WAN config", OMCI_OLT_PROFILE_HUAWEI,
		    OLT_CREATED_ACTIONS, 0,
		    OMCI_GENMASK(15, 13), OMCI_GENMASK(15, 13), 4,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65451_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_EXT_WAN_DATA3,
		    "Huawei extended WAN data 3", OMCI_OLT_PROFILE_HUAWEI,
		    OLT_CREATED_ACTIONS, 0,
		    OMCI_GENMASK(15, 13), OMCI_GENMASK(15, 13), 14,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65453_attrs),
	VENDOR_DESC(OMCI_CLASS_HUAWEI_EXT_80211_MGMT,
		    "Huawei extended 802.11 management", OMCI_OLT_PROFILE_HUAWEI,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 9), OMCI_GENMASK(15, 9), 30,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_huawei_65454_attrs),

	VENDOR_DESC(OMCI_CLASS_ZTE_PPTP_SCHED,
		    "ZTE PPTP schedule pointer", OMCI_OLT_PROFILE_ZTE,
		    OLT_CREATED_ACTIONS, 0,
		    OMCI_GENMASK(15, 14), OMCI_GENMASK(15, 14), 3,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_zte_242_attrs),
	VENDOR_DESC(OMCI_CLASS_ZTE_UNI_SCHED,
		    "ZTE UNI schedule configuration", OMCI_OLT_PROFILE_ZTE,
		    OLT_CREATED_ACTIONS, 0,
		    OMCI_GENMASK(15, 13), OMCI_GENMASK(15, 13), 17,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_zte_243_attrs),
	VENDOR_DESC(OMCI_CLASS_ZTE_EXT_ONU_G,
		    "ZTE extended ONU-G", OMCI_OLT_PROFILE_ZTE,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 43,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_zte_247_attrs),
	VENDOR_DESC(OMCI_CLASS_ZTE_SNTP,
		    "ZTE SNTP configuration", OMCI_OLT_PROFILE_ZTE,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 10), OMCI_GENMASK(15, 10), 16,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_zte_65294_attrs),
	VENDOR_DESC(OMCI_CLASS_ZTE_IP_FILTER,
		    "ZTE IP flow filter", OMCI_OLT_PROFILE_ZTE,
		    OLT_CREATED_ACTIONS, 0,
		    OMCI_GENMASK(15, 9), OMCI_GENMASK(15, 9), 15,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_zte_65305_attrs),
	VENDOR_DESC(OMCI_CLASS_GENEXIS_IP_FILTER,
		    "Genexis IP flow filter", OMCI_OLT_PROFILE_GENEXIS,
		    OLT_CREATED_ACTIONS, 0,
		    OMCI_GENMASK(15, 9), OMCI_GENMASK(15, 9), 15,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_zte_65305_attrs),
	VENDOR_DESC(OMCI_CLASS_GENEXIS_ETH_PM4,
		    "Genexis Ethernet PM history data 4", OMCI_OLT_PROFILE_GENEXIS,
		    PM_ACTIONS, 0,
		    OMCI_GENMASK(15, 3), OMCI_GENMASK(15, 3), 44,
		    OMCI_CLASS_CATEGORY_PERFORMANCE, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_genexis_255_attrs),
	VENDOR_DESC(OMCI_CLASS_ZTE_SSID_CONTROL,
		    "ZTE SSID control", OMCI_OLT_PROFILE_ZTE,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 8), OMCI_GENMASK(15, 8), 46,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_zte_65314_attrs),
	VENDOR_DESC(OMCI_CLASS_ZTE_SSID_AUTH,
		    "ZTE SSID authentication and encryption", OMCI_OLT_PROFILE_ZTE,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 10), OMCI_GENMASK(15, 10), 8,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_zte_65315_attrs),

	VENDOR_DESC(OMCI_CLASS_FH_WAN_PROFILE,
		    "FiberHome WAN profile", OMCI_OLT_PROFILE_FIBERHOME,
		    OMCI_ME_ACTION_GET, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 14), OMCI_GENMASK(15, 14), 4,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_fh_65321_attrs),
	VENDOR_DESC(OMCI_CLASS_FH_WAN_CONFIG,
		    "FiberHome WAN configuration", OMCI_OLT_PROFILE_FIBERHOME,
		    OLT_CREATED_ACTIONS, 0,
		    OMCI_GENMASK(15, 1), OMCI_GENMASK(15, 1), 87,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_fh_65322_attrs),
	VENDOR_DESC(OMCI_CLASS_FH_PPPOE_CONFIG,
		    "FiberHome PPPoE configuration", OMCI_OLT_PROFILE_FIBERHOME,
		    OLT_CREATED_ACTIONS, 0,
		    OMCI_GENMASK(15, 8), OMCI_GENMASK(15, 8), 98,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_fh_65323_attrs),
	VENDOR_DESC(OMCI_CLASS_FH_WAN_PORT_BIND,
		    "FiberHome WAN port binding", OMCI_OLT_PROFILE_FIBERHOME,
		    OLT_CREATED_ACTIONS, 0,
		    OMCI_GENMASK(15, 14), OMCI_GENMASK(15, 14), 2,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_fh_65324_attrs),
	VENDOR_DESC(OMCI_CLASS_FH_WIFI_STA_1,
		    "FiberHome WiFi station management 1", OMCI_OLT_PROFILE_FIBERHOME,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 1), OMCI_GENMASK(15, 1), 110,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_fh_65326_attrs),
	VENDOR_DESC(OMCI_CLASS_FH_WIFI_STA_2,
		    "FiberHome WiFi station management 2", OMCI_OLT_PROFILE_FIBERHOME,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 0), OMCI_GENMASK(15, 0), 139,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_fh_65327_attrs),
	VENDOR_DESC(OMCI_CLASS_FH_WIFI_STA_3,
		    "FiberHome WiFi station management 3", OMCI_OLT_PROFILE_FIBERHOME,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 2), OMCI_GENMASK(15, 2), 170,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_fh_65328_attrs),
	VENDOR_DESC(OMCI_CLASS_FH_WAN_IP_HOST,
		    "FiberHome WAN IP host configuration", OMCI_OLT_PROFILE_FIBERHOME,
		    OLT_CREATED_ACTIONS, 0,
		    OMCI_GENMASK(15, 8), OMCI_GENMASK(15, 8), 77,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_fh_65329_attrs),
	VENDOR_DESC(OMCI_CLASS_FH_PON_SWITCHPORT,
		    "FiberHome PON switchport rate limit", OMCI_OLT_PROFILE_FIBERHOME,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 14), OMCI_GENMASK(15, 14), 8,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_fh_65333_attrs),
	VENDOR_DESC(OMCI_CLASS_FH_WAN_EXT_VLAN,
		    "FiberHome WAN extended VLAN", OMCI_OLT_PROFILE_FIBERHOME,
		    OLT_CREATED_ACTIONS, 0,
		    OMCI_GENMASK(15, 8), OMCI_GENMASK(15, 8), 13,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_fh_65338_attrs),
	VENDOR_DESC(OMCI_CLASS_FH_OMCI_MODE,
		    "FiberHome OMCI mode", OMCI_OLT_PROFILE_FIBERHOME,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 11), OMCI_GENMASK(15, 11), 19,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_fh_65363_attrs),
	VENDOR_DESC(OMCI_CLASS_FH_WIFI_STATUS,
		    "FiberHome WiFi status", OMCI_OLT_PROFILE_FIBERHOME,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 13), OMCI_GENMASK(15, 13), 4,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_fh_65411_attrs),

	VENDOR_MASK_DESC(OMCI_CLASS_CTC_PPPOE_WAN,
			 "CTC PPPoE WAN configuration", OMCI_PROFILE_STANDARD_MASK,
			 OLT_CREATED_ACTIONS, 0,
			 OMCI_GENMASK(15, 6), OMCI_GENMASK(15, 6), 64,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_250_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_VLAN_TRANS,
			 "CTC extended VLAN translation", OMCI_PROFILE_STANDARD_MASK,
			 OLT_CREATED_ACTIONS | OMCI_ME_ACTION_GET_NEXT, 0,
			 OMCI_GENMASK(15, 13), OMCI_GENMASK(15, 13), 26,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65282_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_OPTICAL_INFO,
			 "CTC optical transceiver info", OMCI_PROFILE_STANDARD_MASK,
			 OMCI_ME_ACTION_GET, OMCI_ME_F_ONU_CREATED,
			 OMCI_GENMASK(15, 8), OMCI_GENMASK(15, 8), 65,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65320_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_WAN_SERVICE,
			 "CTC WAN service binding",
			 OMCI_PROFILE_STANDARD_MASK & ~OMCI_PROFILE_BIT(OMCI_OLT_PROFILE_FIBERHOME),
			 OLT_CREATED_ACTIONS, 0,
			 OMCI_GENMASK(15, 10), OMCI_GENMASK(15, 10), 11,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65321_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_WAN_CONN,
			 "CTC WAN connection configuration", OMCI_PROFILE_STANDARD_MASK,
			 OLT_CREATED_ACTIONS, 0,
			 OMCI_GENMASK(15, 9), OMCI_GENMASK(15, 9), 9,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65380_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_WAN_PROFILE,
			 "CTC WAN IP/PPPoE profile", OMCI_PROFILE_STANDARD_MASK,
			 OLT_CREATED_ACTIONS, 0,
			 OMCI_GENMASK(15, 3), OMCI_GENMASK(15, 3), 74,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65381_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_WIFI_CFG,
			 "CTC WiFi interface configuration", OMCI_PROFILE_STANDARD_MASK,
			 OLT_CREATED_ACTIONS, 0,
			 OMCI_GENMASK(15, 4), OMCI_GENMASK(15, 4), 38,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65385_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_WIFI_SEC,
			 "CTC WiFi security configuration", OMCI_PROFILE_STANDARD_MASK,
			 OLT_CREATED_ACTIONS, 0,
			 OMCI_GENMASK(15, 6), OMCI_GENMASK(15, 6), 133,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65386_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_LOOP_CTRL,
			 "CTC loop detection control", OMCI_PROFILE_STANDARD_MASK,
			 STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
			 OMCI_GENMASK(15, 14), OMCI_GENMASK(15, 14), 2,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65523_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_LOOP_DETECT,
			 "CTC loop detection", OMCI_PROFILE_STANDARD_MASK,
			 STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
			 OMCI_GENMASK(15, 10), OMCI_GENMASK(15, 11), 12,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65528_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_ONU_CAP,
			 "CTC ONU capability", OMCI_PROFILE_STANDARD_MASK,
			 STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
			 OMCI_GENMASK(15, 11), OMCI_GENMASK(15, 11), 8,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65529_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_EXT_ONU_G,
			 "CTC extended ONU-G",
			 OMCI_PROFILE_STANDARD_MASK & ~OMCI_PROFILE_BIT(OMCI_OLT_PROFILE_HUAWEI),
			 OMCI_ME_ACTION_SET, OMCI_ME_F_ONU_CREATED,
			 OMCI_BIT(15), 0, 1,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65408_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_OLT_LOCATION,
			 "CTC OLT location configuration data", OMCI_PROFILE_STANDARD_MASK,
			 STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
			 OMCI_GENMASK(15, 11), OMCI_GENMASK(15, 11), 24,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65527_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_LOID_AUTH,
			 "CTC LOID authentication", OMCI_PROFILE_STANDARD_MASK,
			 STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
			 OMCI_GENMASK(15, 12), OMCI_GENMASK(15, 12), 41,
			 OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65530_attrs),
	VENDOR_MASK_DESC(OMCI_CLASS_CTC_EXT_MCAST,
			 "CTC extended multicast operations profile",
			 OMCI_PROFILE_STANDARD_MASK,
			 OLT_CREATED_ACTIONS | OMCI_ME_ACTION_GET_NEXT |
			 OMCI_ME_ACTION_SET_TABLE, 0,
			 OMCI_GENMASK(15, 0),
			 OMCI_GENMASK(15, 10) | OMCI_GENMASK(6, 0), 43,
			 OMCI_CLASS_CATEGORY_MULTICAST, OMCI_CLASS_SUPPORT_SHADOW,
			 omci_ctc_65531_attrs),

	VENDOR_DESC(OMCI_CLASS_NOKIA_OPTICAL_SUPERVISION,
		    "Nokia ONT optical supervision status",
		    OMCI_OLT_PROFILE_NOKIA_ALCL, OMCI_ME_ACTION_GET,
		    OMCI_ME_F_ONU_CREATED, OMCI_GENMASK(15, 6), OMCI_GENMASK(15, 6), 15,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_NATIVE,
		    omci_nokia_65295_attrs),
	VENDOR_DESC(OMCI_CLASS_NOKIA_ONT_GENERIC_V2,
		    "Nokia ONT generic V2", OMCI_OLT_PROFILE_NOKIA_ALCL,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 4), OMCI_GENMASK(15, 4), 56,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_NATIVE,
		    omci_nokia_65296_attrs),
	VENDOR_DESC(OMCI_CLASS_NOKIA_UNI_SUPPLEMENTAL_V2,
		    "Nokia UNI supplemental 1 V2", OMCI_OLT_PROFILE_NOKIA_ALCL,
		    STANDARD_ACTIONS, OMCI_ME_F_ONU_CREATED,
		    OMCI_GENMASK(15, 9), OMCI_GENMASK(15, 9), 8,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_nokia_65297_attrs),
	VENDOR_DESC(OMCI_CLASS_NOKIA_VLAN_DS_SUPPLEMENTAL,
		    "Nokia VLAN downstream supplemental",
		    OMCI_OLT_PROFILE_NOKIA_ALCL, OLT_CREATED_ACTIONS,
		    OMCI_ME_F_DATAPATH, OMCI_GENMASK(15, 14), OMCI_GENMASK(15, 14), 7,
		    OMCI_CLASS_CATEGORY_VENDOR, OMCI_CLASS_SUPPORT_SHADOW,
		    omci_nokia_65305_attrs),
	VENDOR_DESC(OMCI_CLASS_NOKIA_VLAN_US_POLICER,
		    "Nokia upstream VLAN policer", OMCI_OLT_PROFILE_NOKIA_ALCL,
		    OLT_CREATED_ACTIONS, OMCI_ME_F_DATAPATH,
		    OMCI_BIT(15), OMCI_BIT(15), 19, OMCI_CLASS_CATEGORY_VENDOR,
		    OMCI_CLASS_SUPPORT_SHADOW, omci_nokia_65306_attrs),
};

static bool omci_class_name_contains(const char *name, const char *needle)
{
	return strstr(name, needle);
}

static __u8 omci_class_category(__u16 class_id, const char *name)
{
	if ((class_id >= 240 && class_id <= 255) ||
	    (class_id >= 350 && class_id <= 399) || class_id >= 65280)
		return OMCI_CLASS_CATEGORY_VENDOR;
	if ((class_id >= 172 && class_id <= 239) ||
	    (class_id >= 453 && class_id <= 65279) ||
	    omci_class_name_contains(name, "Reserved") ||
	    omci_class_name_contains(name, "Intentionally left blank"))
		return OMCI_CLASS_CATEGORY_RESERVED;
	if (omci_class_name_contains(name, "performance monitoring") ||
	    omci_class_name_contains(name, "PM history") ||
	    omci_class_name_contains(name, "monitoring history"))
		return OMCI_CLASS_CATEGORY_PERFORMANCE;
	if (omci_class_name_contains(name, "xDSL") ||
	    omci_class_name_contains(name, "VDSL") ||
	    omci_class_name_contains(name, "FAST") ||
	    omci_class_name_contains(name, "EFM bonding"))
		return OMCI_CLASS_CATEGORY_XDSL;
	if (omci_class_name_contains(name, "VoIP") ||
	    omci_class_name_contains(name, "SIP") ||
	    omci_class_name_contains(name, "MGC") ||
	    omci_class_name_contains(name, "RTP") ||
	    omci_class_name_contains(name, "Voice") ||
	    omci_class_name_contains(name, "POTS"))
		return OMCI_CLASS_CATEGORY_VOICE;
	if (omci_class_name_contains(name, "Multicast"))
		return OMCI_CLASS_CATEGORY_MULTICAST;
	if (omci_class_name_contains(name, "security") ||
	    omci_class_name_contains(name, "Authentication") ||
	    omci_class_name_contains(name, "Dot1X") ||
	    omci_class_name_contains(name, "Radius"))
		return OMCI_CLASS_CATEGORY_SECURITY;
	if (omci_class_name_contains(name, "IP ") ||
	    omci_class_name_contains(name, "TCP/UDP") ||
	    omci_class_name_contains(name, "ARP") ||
	    omci_class_name_contains(name, "ICMP") ||
	    omci_class_name_contains(name, "TR-069") ||
	    omci_class_name_contains(name, "SNMP") ||
	    omci_class_name_contains(name, "Network address"))
		return OMCI_CLASS_CATEGORY_LAYER3;
	if (omci_class_name_contains(name, "VLAN") ||
	    omci_class_name_contains(name, "MAC bridge") ||
	    omci_class_name_contains(name, "Ethernet") ||
	    omci_class_name_contains(name, "802.1p") ||
	    omci_class_name_contains(name, "Dot1ag") ||
	    omci_class_name_contains(name, "OpenFlow"))
		return OMCI_CLASS_CATEGORY_LAYER2;
	if (omci_class_name_contains(name, "UNI"))
		return OMCI_CLASS_CATEGORY_UNI;
	if (omci_class_name_contains(name, "ANI") ||
	    omci_class_name_contains(name, "GEM") ||
	    omci_class_name_contains(name, "T-CONT") ||
	    omci_class_name_contains(name, "PON") ||
	    omci_class_name_contains(name, "Traffic scheduler") ||
	    omci_class_name_contains(name, "Priority queue"))
		return OMCI_CLASS_CATEGORY_ANI;
	if (class_id <= 7 || class_id == 133 || class_id == 159 ||
	    class_id == 160 || class_id == 256 || class_id == 257 ||
	    class_id == 297 || class_id == 331 || class_id == 336 ||
	    class_id == 441)
		return OMCI_CLASS_CATEGORY_EQUIPMENT;
	if (class_id == 137 || class_id == 157 || class_id == 158 ||
	    class_id == 287 || class_id == 288 || class_id == 289 ||
	    class_id == 307 || class_id == 308 || class_id == 318 ||
	    class_id == 330 || class_id == 440)
		return OMCI_CLASS_CATEGORY_MANAGEMENT;
	if (class_id < 256)
		return OMCI_CLASS_CATEGORY_LEGACY;

	return OMCI_CLASS_CATEGORY_OTHER;
}

static __u32 omci_class_flags(__u16 class_id, const char *name)
{
	__u32 flags = OMCI_CLASS_F_STANDARD;
	__u8 category = omci_class_category(class_id, name);

	if (omci_class_name_contains(name, "deprecated") ||
	    omci_class_name_contains(name, "obsolete"))
		flags |= OMCI_CLASS_F_DEPRECATED;
	if (category == OMCI_CLASS_CATEGORY_RESERVED)
		flags |= OMCI_CLASS_F_RESERVED;
	if (category == OMCI_CLASS_CATEGORY_VENDOR)
		flags |= OMCI_CLASS_F_VENDOR_SPECIFIC;
	if (category == OMCI_CLASS_CATEGORY_PERFORMANCE)
		flags |= OMCI_CLASS_F_PERFORMANCE;
	if (omci_class_name_contains(name, "table") || class_id == 84 ||
	    class_id == 145 || class_id == 171)
		flags |= OMCI_CLASS_F_TABLE;

	return flags;
}

const struct omci_me_desc *omci_me_lookup_profile(__u8 profile, __u16 class_id)
{
	unsigned int i;

	if (profile == OMCI_OLT_PROFILE_UNSPEC ||
	    profile == OMCI_OLT_PROFILE_AUTO)
		profile = OMCI_OLT_PROFILE_GENERIC;

	if (profile > OMCI_OLT_PROFILE_GENEXIS)
		return NULL;

	for (i = 0; i < OMCI_ARRAY_SIZE(omci_me_descs); i++) {
		const struct omci_me_desc *desc = &omci_me_descs[i];

		if (desc->class_id == class_id &&
		    (desc->profile_mask & OMCI_PROFILE_BIT(profile)))
			return desc;
	}

	return NULL;
}

bool omci_me_active_profile(__u8 profile, __u16 class_id)
{
	const struct omci_me_desc *desc;

	if (profile > OMCI_OLT_PROFILE_GENEXIS)
		return false;
	desc = omci_me_lookup_profile(profile, class_id);
	if (desc)
		return true;

	/* Standard catalogue entries remain available as shadow objects. */
	return class_id < 240 || (class_id >= 256 && class_id < 350) ||
	       (class_id >= 400 && class_id <= 452);
}

bool omci_me_action_allowed_profile(__u8 profile, __u16 class_id,
				    __u8 message_type)
{
	const struct omci_me_desc *desc = omci_me_lookup_profile(profile, class_id);

	if (!desc)
		return omci_me_active_profile(profile, class_id);
	if (message_type >= 32)
		return false;

	return !!(desc->actions & OMCI_BIT(message_type));
}

static const struct omci_attr_desc *
omci_me_find_attr(const struct omci_me_desc *desc, __u16 mask)
{
	unsigned int i;

	for (i = 0; i < desc->num_attrs; i++)
		if (desc->attrs[i].mask == mask)
			return &desc->attrs[i];

	return NULL;
}

int omci_me_decode_create_data(const struct omci_me_desc *desc,
			       __u8 *object_data, __u16 *object_mask,
			       const __u8 *data, size_t len)
{
	unsigned int i;
	__u16 mask = 0;

	if (!desc)
		return 0;
	if (!object_data || !object_mask || (!data && len))
		return -EINVAL;
	for (i = 0; i < desc->num_attrs; i++) {
		const struct omci_attr_desc *attr = &desc->attrs[i];

		if (!(attr->access & OMCI_ATTR_ACCESS_SET_CREATE))
			continue;
		if (attr->offset + attr->len > OMCI_MAX_ATTR_DATA ||
		    len < attr->len)
			return -EINVAL;
		memcpy(object_data + attr->offset, data, attr->len);
		data += attr->len;
		len -= attr->len;
		mask |= attr->mask;
	}
	*object_mask |= mask;

	return 0;
}

int omci_me_decode_set_data(const struct omci_me_desc *desc,
			    __u8 *object_data, __u16 *object_mask, __u16 mask,
			    const __u8 *data, size_t len)
{
	unsigned int bit;

	if (!desc)
		return 0;
	if (!object_data || !object_mask || (!data && len))
		return -EINVAL;
	if (mask & ~desc->valid_attr_mask)
		return -EINVAL;

	for (bit = 0; bit < 16; bit++) {
		const struct omci_attr_desc *attr;
		__u16 attr_mask = OMCI_BIT(15 - bit);

		if (!(mask & attr_mask))
			continue;
		attr = omci_me_find_attr(desc, attr_mask);
		if (!attr || !(attr->access & OMCI_ATTR_ACCESS_WRITE))
			return -EACCES;
		if (attr->offset + attr->len > OMCI_MAX_ATTR_DATA ||
		    len < attr->len)
			return -EINVAL;
		/* the part count of the product identification names at most 15 parts */
		if (desc->class_id == OMCI_CLASS_HUAWEI_MULTICAST_373 &&
		    !attr->offset && data[0] > 15)
			return -EINVAL;
		memcpy(object_data + attr->offset, data, attr->len);
		data += attr->len;
		len -= attr->len;
	}
	*object_mask |= mask;

	return 0;
}

int omci_me_encode_data(const struct omci_me_desc *desc,
			const __u8 *object_data,
			__u16 mask, __u8 *data, size_t len,
			__u16 *encoded_mask, size_t *encoded_len)
{
	size_t used = 0;
	unsigned int bit;
	__u16 encoded = 0;

	if (!desc)
		return -ENOENT;
	if (!object_data || (!data && len))
		return -EINVAL;
	mask &= desc->valid_attr_mask;
	for (bit = 0; bit < 16; bit++) {
		const struct omci_attr_desc *attr;
		__u16 attr_mask = OMCI_BIT(15 - bit);

		if (!(mask & attr_mask))
			continue;
		attr = omci_me_find_attr(desc, attr_mask);
		if (!attr || !(attr->access & OMCI_ATTR_ACCESS_READ))
			continue;
		if (attr->offset + attr->len > OMCI_MAX_ATTR_DATA)
			return -EINVAL;
		if (len - used < attr->len)
			break;
		memcpy(data + used, object_data + attr->offset, attr->len);
		used += attr->len;
		encoded |= attr_mask;
	}
	if (encoded_mask)
		*encoded_mask = encoded;
	if (encoded_len)
		*encoded_len = used;

	return encoded ? 0 : -ENOSPC;
}

unsigned int omci_me_attr_count_profile(__u8 profile)
{
	unsigned int count = 0;
	unsigned int i;

	for (i = 0; i < OMCI_ARRAY_SIZE(omci_me_descs); i++) {
		const struct omci_me_desc *desc = &omci_me_descs[i];

		if (omci_me_lookup_profile(profile, desc->class_id) != desc)
			continue;
		count += desc->num_attrs;
	}

	return count;
}

int omci_me_attr_id_profile(__u8 profile, __u16 class_id,
			    unsigned int attr_index, __u16 *attr_id)
{
	unsigned int next_id = 1;
	unsigned int i;

	if (!attr_id)
		return -EINVAL;

	for (i = 0; i < OMCI_ARRAY_SIZE(omci_me_descs); i++) {
		const struct omci_me_desc *desc = &omci_me_descs[i];

		if (omci_me_lookup_profile(profile, desc->class_id) != desc)
			continue;
		if (desc->class_id == class_id) {
			if (attr_index >= desc->num_attrs)
				return -ENOENT;
			if (next_id + attr_index > 0xffffU)
				return -EOVERFLOW;
			*attr_id = next_id + attr_index;
			return 0;
		}
		if (next_id + desc->num_attrs > 0xffffU)
			return -EOVERFLOW;
		next_id += desc->num_attrs;
	}

	return -ENOENT;
}

int omci_me_attr_get_profile(__u8 profile, __u16 attr_id,
			     __u16 *class_id, unsigned int *attr_index,
			     const struct omci_me_desc **me_desc,
			     const struct omci_attr_desc **me_attr)
{
	unsigned int next_id = 1;
	unsigned int i;

	if (!attr_id)
		return -EINVAL;

	for (i = 0; i < OMCI_ARRAY_SIZE(omci_me_descs); i++) {
		const struct omci_me_desc *desc = &omci_me_descs[i];
		unsigned int index;

		if (omci_me_lookup_profile(profile, desc->class_id) != desc)
			continue;
		if (attr_id < next_id || attr_id >= next_id + desc->num_attrs) {
			next_id += desc->num_attrs;
			continue;
		}

		index = attr_id - next_id;
		if (class_id)
			*class_id = desc->class_id;
		if (attr_index)
			*attr_index = index;
		if (me_desc)
			*me_desc = desc;
		if (me_attr)
			*me_attr = &desc->attrs[index];
		return 0;
	}

	return -ENOENT;
}

const char *omci_me_class_name(__u16 class_id)
{
	unsigned int i;

	for (i = 0; i < OMCI_ARRAY_SIZE(omci_me_descs); i++)
		if (omci_me_descs[i].class_id == class_id)
			return omci_me_descs[i].name;
	for (i = 0; i < OMCI_ARRAY_SIZE(omci_classes); i++)
		if (omci_classes[i].class_id == class_id)
			return omci_classes[i].name;

	if (class_id >= 172 && class_id <= 239)
		return "Reserved for future managed entities";
	if (class_id >= 240 && class_id <= 255)
		return "Reserved for vendor-specific managed entities";
	if (class_id >= 350 && class_id <= 399)
		return "Vendor-specific managed entity";
	if (class_id >= 453 && class_id <= 65279)
		return "Reserved for future standardization";
	if (class_id >= 65280)
		return "Vendor-specific managed entity";

	return "Unassigned managed entity";
}

int omci_me_class_get_profile(__u8 profile, __u16 class_id,
			      struct omci_me_class *class)
{
	const struct omci_me_desc *desc;
	const char *name;

	if (!class)
		return -EINVAL;
	if (!omci_me_active_profile(profile, class_id))
		return -ENOENT;

	desc = omci_me_lookup_profile(profile, class_id);
	name = omci_me_class_name(class_id);
	class->class_id = class_id;
	class->category = desc ? desc->category :
			  omci_class_category(class_id, name);
	class->support = desc ? desc->support : OMCI_CLASS_SUPPORT_SHADOW;
	class->flags = omci_class_flags(class_id, name);
	if (desc) {
		if (desc->flags & OMCI_ME_F_VENDOR)
			class->flags |= OMCI_CLASS_F_VENDOR_SPECIFIC;
		if (desc->flags & OMCI_ME_F_DATAPATH)
			class->flags |= OMCI_CLASS_F_DATAPATH;
	}
	class->name = name;

	return 0;
}

int omci_me_class_next_profile(__u8 profile, __u32 index,
			       struct omci_me_class *class, __u32 *next_index)
{
	__u32 position = 0;
	unsigned int i;

	if (!class || !next_index)
		return -EINVAL;

	for (i = 0; i < OMCI_ARRAY_SIZE(omci_classes); i++) {
		if (!omci_me_active_profile(profile, omci_classes[i].class_id))
			continue;
		if (position++ < index)
			continue;
		omci_me_class_get_profile(profile, omci_classes[i].class_id, class);
		*next_index = index + 1;
		return 0;
	}
	for (i = 0; i < OMCI_ARRAY_SIZE(omci_me_descs); i++) {
		__u16 class_id = omci_me_descs[i].class_id;
		unsigned int j;
		bool catalogued = false;

		for (j = 0; j < OMCI_ARRAY_SIZE(omci_classes); j++)
			if (omci_classes[j].class_id == class_id) {
				catalogued = true;
				break;
			}
		if (catalogued || omci_me_lookup_profile(profile, class_id) != &omci_me_descs[i])
			continue;
		if (position++ < index)
			continue;
		omci_me_class_get_profile(profile, class_id, class);
		*next_index = index + 1;
		return 0;
	}

	return -ENOENT;
}
