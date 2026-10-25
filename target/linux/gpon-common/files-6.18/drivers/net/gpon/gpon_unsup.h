/* SPDX-License-Identifier: GPL-2.0-or-later */
/* gpon_unsup.h — REPORT ANYTHING THE FIRMWARE DID NOT ... -- dev/MEASURED-gpon_unsup.h.md sec 1. */
#ifndef GPON_UNSUP_H
#define GPON_UNSUP_H

#include <linux/types.h>
#include <linux/printk.h>
#include "gpon_common.h"	/* GPON_UNSUP_RANGE, GPON_UNSUP_UNKNOWN */

/* The subsystem prefix the line opens with.  DEFINE IT BEFORE INCLUDING THIS
 * HEADER, to whatever the file's existing log lines already use.  The fallback
 * is deliberately not an #error: a shell that forgets it still reports. */
#ifndef GPON_UNSUP_SUBSYS
#define GPON_UNSUP_SUBSYS	"gpon"
#endif
/* Where the line goes: a shell with its own protocol log names it here. */
#ifndef GPON_UNSUP_LOG
#define GPON_UNSUP_LOG	pr_info
#endif

/* HOST builds only; in kernel context compiler_attributes.h ...
 * dev/MEASURED-gpon_unsup.h.md sec 2. */
#ifndef noinline
# if defined(__GNUC__)
#  define noinline	__attribute__((__noinline__))
# else
#  define noinline
# endif
#endif
#ifndef __maybe_unused
# define __maybe_unused	__attribute__((__unused__))
#endif

/* The dump ceiling in OCTETS.  16 covers every datum this facility reports (the
 * longest is a 13-octet downstream PLOAM) and bounds the stack buffer below at
 * 33 bytes, which is what keeps this usable from a timer callback.  A caller
 * passing more is CLIPPED, never truncated mid-byte. */
#define GPON_UNSUP_DUMP_MAX	16u

/* The class name as the reader spells it; out-of-enum is deliberately
 * unplaceable, so the report is carried as MALFORMED and stays visible. */
static inline const char *gpon_unsup_class_name(int cls)
{
	switch (cls) {
	case GPON_UNSUP_UNKNOWN:
		return "unknown";
	case GPON_UNSUP_RANGE:
		return "range";
	default:
		return "unclassified";
	}
}

/* Emit one report, or suppress it.  @n is the caller's PER-SITE cumulative
 * counter: incremented on EVERY occurrence and printed on the powers of two, so
 * the printed `n=` is the true count.  Not __printf-checked on purpose — the
 * format is fixed here and there is exactly one pr_info. */
static noinline __maybe_unused void gpon_unsup_emit(const char *subsys,
					    const char *kind,
					    int cls, u32 val,
					    const char *want, const u8 *data,
					    unsigned int len, unsigned int *n)
{
	static const char hexd[] = "0123456789abcdef";
	char hex[2u * GPON_UNSUP_DUMP_MAX + 1u];
	unsigned int i, c;
	c = ++(*n);
	if (c & (c - 1u))		/* not a power of two -> suppressed */
		return;
	if (!data)
		len = 0;
	if (len > GPON_UNSUP_DUMP_MAX)
		len = GPON_UNSUP_DUMP_MAX;
	for (i = 0; i < len; i++) {
		hex[2u * i]      = hexd[(data[i] >> 4) & 0xfu];
		hex[2u * i + 1u] = hexd[data[i] & 0xfu];
	}
	hex[2u * len] = '\0';
	GPON_UNSUP_LOG("%s: UNSUP kind=%s class=%s val=0x%x want=%s n=%u d=%s\n",
		subsys, kind, gpon_unsup_class_name(cls), val,
		want ? want : "-", c, hex);
}

/* THE ONE THING A SHELL CALLS: ... -- dev/MEASURED-gpon_unsup.h.md sec 3. */
#define gpon_unsup_report(kind, cls, val, want, data, len)		\
	do {								\
		static unsigned int _gpon_unsup_n;			\
									\
		gpon_unsup_emit(GPON_UNSUP_SUBSYS, (kind), (int)(cls),	\
				(u32)(val), (want), (const u8 *)(data),	\
				(unsigned int)(len), &_gpon_unsup_n);	\
	} while (0)

#endif /* GPON_UNSUP_H */
