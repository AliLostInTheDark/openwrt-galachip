// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Crash records on the reserved "crashlog" partition, read back after any reset:
 * a boot record per boot, a clean record per orderly reboot, the kernel log tail on
 * panic. A boot record left without either is an unclean end (power cut or watchdog).
 * The region is written only when it is the free tail of a partition holding THIS
 * OpenWrt release's kernel: never behind a vendor kernel, however erased.
 */
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/utsname.h>
#include <linux/vmalloc.h>
#include "onu_crashlog_core.h"

#define KEEP_RECORDS	6
#define MIN_FREE	(16 * OCL_PAGE)

static char partition[32] = "crashlog";
module_param_string(partition, partition, sizeof(partition), 0444);
static char image[32] = "k0";
module_param_string(image, image, sizeof(image), 0444);
static char rev[64];
module_param_string(rev, rev, sizeof(rev), 0644);
MODULE_PARM_DESC(rev, "the image revision, set from userspace once known");

static struct {
	struct mtd_info *mtd;
	u8 *mirror;
	size_t size, end;
	u32 seq, boot_seq;
	bool owned;
	struct mutex lock;
} ocl = { .lock = __MUTEX_INITIALIZER(ocl.lock) };

static struct mtd_info *ocl_img, *ocl_region;
static u8 rec_buf[16 * OCL_PAGE];
static char text_buf[OCL_MAX_TEXT];

static size_t build_text(char *b, size_t n, const char *event)
{
	const char *model = NULL;

	of_property_read_string(of_root, "model", &model);
	return scnprintf(b, n, "build=%s %s\nrev=%s\nmodel=%s\n%s", init_utsname()->release,
			 init_utsname()->version, rev[0] ? rev : "?", model ?: "?", event);
}

static int append(u8 kind, const char *text, size_t len, bool in_panic)
{
	struct ocl_rec r = { .seq = ocl.seq + 1, .kind = kind, .len = min_t(size_t, len, OCL_MAX_TEXT),
			     .text = (const u8 *)text, .uptime_s = ktime_get_boottime_seconds() };
	size_t n, done = 0;
	int ret;

	r.boot_seq = kind == OCL_BOOT ? r.seq : ocl.boot_seq;
	n = ocl_encode(rec_buf, sizeof(rec_buf), &r);
	if (!ocl.owned || !n || ocl.end + n > ocl.size)
		return -ENOSPC;
	ret = in_panic ? mtd_panic_write(ocl.mtd, ocl.end, n, &done, rec_buf)
		       : mtd_write(ocl.mtd, ocl.end, n, &done, rec_buf);
	if (ret || done != n)
		return ret ?: -EIO;
	memcpy(ocl.mirror + ocl.end, rec_buf, n);
	ocl.end += n;
	ocl.seq = r.seq;
	ocl.boot_seq = r.boot_seq;
	return 0;
}

static void ocl_dump(struct kmsg_dumper *dumper, struct kmsg_dump_detail *detail)
{
	struct kmsg_dump_iter iter;
	size_t head, got = 0;

	if (detail->reason != KMSG_DUMP_PANIC)
		return;
	head = build_text(text_buf, sizeof(text_buf), "event=panic\n");
	kmsg_dump_rewind(&iter);
	kmsg_dump_get_buffer(&iter, true, text_buf + head, sizeof(text_buf) - head, &got);
	append(OCL_PANIC, text_buf, head + got, true);
}

static struct kmsg_dumper dumper = { .dump = ocl_dump, .max_reason = KMSG_DUMP_PANIC };

static int ocl_reboot(struct notifier_block *nb, unsigned long code, void *unused)
{
	const char *ev = code == SYS_POWER_OFF ? "event=poweroff\n" :
			 code == SYS_HALT ? "event=halt\n" : "event=reboot\n";

	mutex_lock(&ocl.lock);
	append(OCL_CLEAN, text_buf, build_text(text_buf, sizeof(text_buf), ev), false);
	mutex_unlock(&ocl.lock);
	return NOTIFY_DONE;
}

static struct notifier_block reboot_nb = { .notifier_call = ocl_reboot };

static bool all_erased(const u8 *p, size_t n)
{
	while (n--)
		if (*p++ != 0xff)
			return false;
	return true;
}

/* The free tail of an OpenWrt kernel partition is ours to claim; a vendor image's is not. */
static bool tail_of_our_image(struct mtd_info *img, struct mtd_info *mtd)
{
	u8 hdr[64];
	size_t got = 0;

	return !mtd_read(img, 0, sizeof(hdr), &got, hdr) && got == sizeof(hdr) &&
	       ocl_behind_our_kernel(hdr, init_utsname()->release, mtd_get_master_ofs(img, 0),
				     mtd_get_master_ofs(mtd, 0));
}

static int erase_all(void)
{
	struct erase_info ei = { .addr = 0, .len = ocl.size };
	int ret = mtd_erase(ocl.mtd, &ei);

	if (!ret)
		memset(ocl.mirror, 0xff, ocl.size);
	return ret;
}

/* Keep the last KEEP_RECORDS non-routine records, erase, write them back in order. */
static int compact(size_t scanned)
{
	size_t keep[KEEP_RECORDS], nkeep = 0, off, i;
	struct ocl_rec r;
	u8 *copy;
	int ret;

	for (off = 0; off < scanned && ocl_parse(ocl.mirror + off, ocl.size - off, &r) == OCL_RECORD;
	     off += r.bytes) {
		if (r.kind == OCL_BOOT || r.kind == OCL_CLEAN)
			continue;
		if (nkeep == KEEP_RECORDS)
			memmove(keep, keep + 1, sizeof(keep[0]) * --nkeep);
		keep[nkeep++] = off;
	}
	copy = vmalloc(ocl.size);
	if (!copy)
		return -ENOMEM;
	memcpy(copy, ocl.mirror, ocl.size);
	ret = erase_all();
	ocl.end = 0;
	for (i = 0; !ret && i < nkeep; i++) {
		size_t n, done = 0;

		ocl_parse(copy + keep[i], ocl.size - keep[i], &r);
		n = r.bytes;
		ret = mtd_write(ocl.mtd, ocl.end, n, &done, copy + keep[i]);
		if (!ret && done == n) {
			memcpy(ocl.mirror + ocl.end, copy + keep[i], n);
			ocl.end += n;
		}
	}
	vfree(copy);
	return ret;
}

static void ocl_attach(struct mtd_info *img, struct mtd_info *mtd)
{
	struct ocl_rec r = { 0 }, last = { 0 };
	size_t got = 0, off = 0;
	bool foreign = false;
	int kind;

	ocl.mtd = mtd;
	ocl.size = mtd->size;
	ocl.mirror = vmalloc(ocl.size);
	if (!ocl.mirror || mtd_read(mtd, 0, ocl.size, &got, ocl.mirror) || got != ocl.size) {
		pr_err("onu_crashlog: %s unreadable, nothing recorded\n", mtd->name);
		return;
	}
	while (off < ocl.size) {
		kind = ocl_parse(ocl.mirror + off, ocl.size - off, &r);
		if (kind != OCL_RECORD) {
			foreign = kind == OCL_FOREIGN || !all_erased(ocl.mirror + off, ocl.size - off);
			break;
		}
		last = r;
		ocl.seq = r.seq;
		ocl.boot_seq = r.boot_seq;
		off += r.bytes;
	}
	ocl.end = off;
	ocl.owned = tail_of_our_image(img, mtd);
	if (!ocl.owned) {
		pr_warn("onu_crashlog: %s is not behind this OpenWrt's %s, nothing is written\n", mtd->name, image);
		return;
	}
	if ((foreign || ocl.size - ocl.end < MIN_FREE) && compact(off))
		pr_err("onu_crashlog: %s could not be compacted\n", mtd->name);
	if (last.kind == OCL_BOOT) {
		size_t n = scnprintf(text_buf, sizeof(text_buf),
				     "previous boot seq=%u ended without a reboot record or a panic: "
				     "power cut or watchdog\n%.*s", last.seq, (int)min_t(u16, last.len, 512),
				     (const char *)last.text);
		append(OCL_UNCLEAN, text_buf, n, false);
	}
	append(OCL_BOOT, text_buf, build_text(text_buf, sizeof(text_buf), "event=boot\n"), false);
	kmsg_dump_register(&dumper);
	register_reboot_notifier(&reboot_nb);
	pr_info("onu_crashlog: %s, %zu of %zu bytes used, boot seq %u\n", mtd->name, ocl.end,
		ocl.size, ocl.boot_seq);
}

/* Called with mtd_table_mutex held: no get_mtd_device*() here, it would deadlock the boot. */
static void ocl_add(struct mtd_info *mtd)
{
	if (!strcmp(mtd->name, image))
		ocl_img = mtd;
	else if (!strcmp(mtd->name, partition))
		ocl_region = mtd;
	if (ocl.mtd || !ocl_img || !ocl_region)
		return;
	mutex_lock(&ocl.lock);
	ocl_attach(ocl_img, ocl_region);
	mutex_unlock(&ocl.lock);
}

static void ocl_remove(struct mtd_info *mtd)
{
	if (mtd == ocl_img)
		ocl_img = NULL;
	if (mtd == ocl_region)
		ocl_region = NULL;
}

static struct mtd_notifier notifier = { .add = ocl_add, .remove = ocl_remove };

static int ocl_show(struct seq_file *m, void *v)
{
	struct ocl_rec r;
	size_t off = 0;

	mutex_lock(&ocl.lock);
	seq_printf(m, "region=%s owned=%d used=%zu size=%zu\n", partition, ocl.owned, ocl.end, ocl.size);
	while (ocl.mirror && off < ocl.end && ocl_parse(ocl.mirror + off, ocl.end - off, &r) == OCL_RECORD) {
		seq_printf(m, "--- seq=%u kind=%s boot=%u uptime=%us\n%.*s\n", r.seq, ocl_kind_name(r.kind),
			   r.boot_seq, r.uptime_s, r.len, (const char *)r.text);
		off += r.bytes;
	}
	mutex_unlock(&ocl.lock);
	return 0;
}

static int __init ocl_init(void)
{
	if (!proc_create_single("onu_crashlog", 0444, NULL, ocl_show))
		pr_err("onu_crashlog: /proc/onu_crashlog not created, the records stay unreadable\n");
	register_mtd_user(&notifier);
	return 0;
}
module_init(ocl_init);

MODULE_DESCRIPTION("ONU crash records on a reserved MTD partition");
MODULE_LICENSE("GPL");
