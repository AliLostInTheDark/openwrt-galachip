// SPDX-License-Identifier: GPL-2.0-only
/*
 * OMCI OLT interoperability profiles
 *
 * omcid detects the OLT and applies the profile's policy. The kernel keeps
 * the table to validate configured profiles and the state omcid reports.
 */

#include <linux/export.h>
#include <linux/kernel.h>

#include "internal.h"

struct omci_profile_desc {
	u8 profile;
	u32 quirks;
	const char *name;
};

static const struct omci_profile_desc omci_profiles[] = {
	{
		.profile = OMCI_OLT_PROFILE_GENERIC,
		.name = "generic",
	}, {
		.profile = OMCI_OLT_PROFILE_AUTO,
		.name = "auto",
	}, {
		.profile = OMCI_OLT_PROFILE_NOKIA_ALCL,
		.quirks = OMCI_OLT_QUIRK_SANITIZE_VERSION |
			  OMCI_OLT_QUIRK_VENDOR_SPECIFIC_MES,
		.name = "nokia-alcl",
	}, {
		.profile = OMCI_OLT_PROFILE_DASAN,
		.quirks = OMCI_OLT_QUIRK_ALLOW_SET_CREATE |
			  OMCI_OLT_QUIRK_FAKE_UNSUPPORTED_SUCCESS |
			  OMCI_OLT_QUIRK_IGNORE_UNSUPPORTED_UNI |
			  OMCI_OLT_QUIRK_DASAN_MULTICAST_ANI,
		.name = "dasan",
	}, {
		.profile = OMCI_OLT_PROFILE_HUAWEI,
		.quirks = OMCI_OLT_QUIRK_VENDOR_SPECIFIC_MES,
		.name = "huawei",
	}, {
		.profile = OMCI_OLT_PROFILE_FIBERHOME,
		.quirks = OMCI_OLT_QUIRK_ALLOW_SET_CREATE |
			  OMCI_OLT_QUIRK_FULL_UNI_ENTITY_ID,
		.name = "fiberhome",
	}, {
		.profile = OMCI_OLT_PROFILE_ZTE,
		.quirks = OMCI_OLT_QUIRK_ALLOW_SET_CREATE |
			  OMCI_OLT_QUIRK_ZTE_VLAN_TAG_MODE,
		.name = "zte",
	},
};

static const struct omci_profile_desc *omci_profile_find(u8 profile)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(omci_profiles); i++)
		if (omci_profiles[i].profile == profile)
			return &omci_profiles[i];

	return NULL;
}

bool omci_profile_valid(u8 profile)
{
	return !!omci_profile_find(profile);
}

bool omci_profile_forceable(u8 profile)
{
	return profile == OMCI_OLT_PROFILE_UNSPEC ||
	       (profile != OMCI_OLT_PROFILE_AUTO && omci_profile_valid(profile));
}

u32 omci_profile_quirks(u8 profile)
{
	const struct omci_profile_desc *desc = omci_profile_find(profile);

	return desc ? desc->quirks : 0;
}

const char *omci_olt_profile_name(u8 profile)
{
	const struct omci_profile_desc *desc = omci_profile_find(profile);

	return desc ? desc->name : "unspecified";
}
EXPORT_SYMBOL_GPL(omci_olt_profile_name);
