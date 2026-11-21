// SPDX-License-Identifier: GPL-2.0-only
/*
 * OMCI OLT profile helpers
 *
 * Userspace (omcid) owns OLT detection, profile definitions and quirk policy.
 * The kernel accepts any non-zero u8 profile ID and opaque u32 quirk mask so
 * new OLTs and quirks can be added from userspace without kernel changes.
 */

#include <linux/export.h>
#include <linux/kernel.h>

#include "internal.h"

struct omci_profile_name_entry {
	u8 profile;
	const char *name;
};

static const struct omci_profile_name_entry omci_profile_names[] = {
	{ OMCI_OLT_PROFILE_GENERIC,	"generic" },
	{ OMCI_OLT_PROFILE_AUTO,	"auto" },
	{ OMCI_OLT_PROFILE_NOKIA_ALCL,	"nokia-alcl" },
	{ OMCI_OLT_PROFILE_DASAN,	"dasan" },
	{ OMCI_OLT_PROFILE_HUAWEI,	"huawei" },
	{ OMCI_OLT_PROFILE_FIBERHOME,	"fiberhome" },
	{ OMCI_OLT_PROFILE_ZTE,		"zte" },
	{ OMCI_OLT_PROFILE_GENEXIS,	"genexis" },
};

bool omci_profile_valid(u8 profile)
{
	return profile != OMCI_OLT_PROFILE_UNSPEC;
}

bool omci_profile_forceable(u8 profile)
{
	return profile != OMCI_OLT_PROFILE_AUTO;
}

const char *omci_olt_profile_name(u8 profile)
{
	unsigned int i;

	if (profile == OMCI_OLT_PROFILE_UNSPEC)
		return "unspecified";
	for (i = 0; i < ARRAY_SIZE(omci_profile_names); i++)
		if (omci_profile_names[i].profile == profile)
			return omci_profile_names[i].name;

	return "custom";
}
EXPORT_SYMBOL_GPL(omci_olt_profile_name);
