// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * rtk_factory - clean-room per-board identity reader for the RTL9602C ONU.
 *
 * The vendor stores this unit's identity as Realtek config-XML files inside the
 * JFFS2 "config" flash partition. This tool parses the JFFS2 image directly from
 * the raw (read-only) MTD - no kernel JFFS2 and no writable flash needed -
 * reconstructs the CURRENT version of the relevant file, and prints one
 * <Value Name="KEY" Value="..."/> value. One identical firmware image can thus
 * self-provision an entire fleet; the factory partition is only ever read.
 *
 *   HW (lastgood_hs.xml): ELAN_MAC_ADDR WLAN_MAC_ADDR GPON_SN OUI
 *                         PON_VENDOR_ID GPON_ONU_MODEL MAC_KEY HW_HWVER
 *                         HW_WLAN0_* (the WiFi cal: TX power, THER, XCAP, REG_DOMAIN)
 *   SW (lastgood.xml):    LOID LOID_PASSWD GPON_PLOAM_PASSWD OMCI_OLT_MODE
 *                         OMCI_SW_VER1 OMCI_SW_VER2 OMCI_VENDOR_PRODUCT_CODE
 * The *_bak and *_mp_hs2 (manufacturing-default) variants are never read.
 *
 * JFFS2 facts (big-endian on this MIPS target): node header = {u16 magic 0x1985,
 * u16 nodetype, u32 totlen, u32 hdr_crc}. ACCURATE bit 0x2000 set = live, clear =
 * obsolete. DIRENT (nodetype&0xff==1): ino@20, ver@16, nsize@28, name@40.
 * INODE (nodetype&0xff==2): ino@12, ver@16, off@44, csize@48, dsize@52,
 * compr@56 (0=none, 6=zlib), data@68.
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <zlib.h>

#define PART_MAX (512 * 1024)
#define FILE_MAX (256 * 1024)

static uint8_t  partbuf[PART_MAX];
static uint8_t  filebuf[FILE_MAX];
/* ★★★ WHICH BYTES A LIVE NODE ACTUALLY WROTE (2026-09-12). Reconstruction used
 * max(off + dsize) as the length -- an EXTENT, not COVERAGE -- and the two
 * differ in exactly the cases that matter. MEASURED on synthetic images whose
 * every CRC is valid:
 *   v1 = 1024 "A" · v2 = metadata isize 0 · v3 = 512 "B" at offset 512,
 *   isize 1024  ->  returned A512 + B512: the bytes v2 DISCARDED came back,
 *   because filebuf still held them and nothing recorded that they were no
 *   longer live. A lone node at offset 512 with isize 1024 returned 512 STALE
 *   bytes followed by B.
 * ⇒ record what was written, drop it on every chronological shrink, and refuse
 *   a current content that is not fully covered. Holes are NOT guessed at: this
 *   stays fail-closed and does not become a filesystem. */
static uint8_t  covered[FILE_MAX];

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

struct vnode { uint32_t ver, pos; };
static struct vnode vn[16384];

static int cmp_ver(const void *a, const void *b)
{
	uint32_t x = ((const struct vnode *)a)->ver, y = ((const struct vnode *)b)->ver;
	return (x > y) - (x < y);
}

/* ★★★ THE CONVENTION IS LINUX'S, NOT ZLIB'S DEFAULT, AND THE FIRST DRAFT OF
 * THIS FUNCTION HAD IT WRONG. jffs2 computes crc32 with SEED 0 in Linux terms,
 * which in zlib terms is crc32(0xffffffff, ...) ^ 0xffffffff -- NOT crc32(0,...).
 * VERIFIED against this unit's own bytes, two independent nodes of
 * G24W mtd3-config.bin:
 *
 *   inode @0x1b53c  hdr stored 0x306517db   crc32(0,..)=0x5547c8b2  Linux=0x306517db
 *   inode @0x2e938  hdr stored 0xa4ef223e   crc32(0,..)=0xc1cdfd57  Linux=0xa4ef223e
 *
 * ⚠ THE DANGER WAS NOT THE BUG, IT WAS THE REPAIR THAT WOULD HAVE FOLLOWED: a
 *   wrong convention rejects the known-good unit outright, and the tempting fix
 *   is to build a fixture that agrees with the code -- a self-consistent test
 *   of the wrong thing. The REAL image is the oracle, always. */
static uint32_t j_crc(const uint8_t *p, size_t len)
{
	return (uint32_t)(crc32(0xffffffffU, p, (uInt)len) ^ 0xffffffffU);
}

/* ★★★ ONE AUTHORITATIVE ROOT LOOKUP, used by BOTH selection and reading
 * (2026-09-12). The first cut of the family probe was a SECOND, weaker
 * directory parser: header CRC only, first match wins, no node/name CRC, no
 * version order, and ino==0 tombstones counted as present. Two parsers of one
 * on-disk structure is how they come to disagree -- and the weaker one was the
 * gatekeeper, so its mistakes decided which file the strong one then read.
 *
 * -> latest live ino for `name` under root, 0 if absent/deleted, -1 on a CRC or
 *    bounds failure. `*saw` is set when ANY jffs2 node was seen at all, which is
 *    a statement about the FORMAT and deliberately independent of the name.
 */
static long jffs2_root_ino(const uint8_t *d, uint32_t n, const char *name,
			   int *saw)
{
	size_t nl = strlen(name);
	uint32_t best = 0, i;
	long ino = 0;

	for (i = 0; i + 12 <= n; ) {
		uint32_t tot;
		uint16_t nt;

		if (be16(d + i) != 0x1985) { i += 4; continue; }
		nt = be16(d + i + 2);
		tot = be32(d + i + 4);
		/* A malformed newer dirent may delete or replace an older file. */
		if ((nt & 0x2000) && (nt & 0xff) == 1 &&
		    (tot < 40 || (uint64_t)i + tot > n))
			return -1;
		if (tot < 12 || (uint64_t)i + tot > n) { i += 4; continue; }
		if (saw)
			*saw = 1;
		if ((nt & 0x2000) && (nt & 0xff) == 1 && tot >= 40) {
			uint8_t nsize = d[i + 28];
			uint32_t ver = be32(d + i + 16);

			if (j_crc(d + i, 8) != be32(d + i + 8))
				return -1;
			if (j_crc(d + i, 32) != be32(d + i + 32))
				return -1;
			if ((uint64_t)40 + nsize > tot)
				return -1;
			if (j_crc(d + i + 40, nsize) != be32(d + i + 36))
				return -1;
			if (be32(d + i + 12) == 1 &&           /* root only */
			    nsize == nl && !memcmp(d + i + 40, name, nsize) &&
			    ver >= best) {
				best = ver;
				/* ★ ino == 0 IS A TOMBSTONE: the newest dirent for a name may
				 * DELETE it, and taking "a dirent exists" as "the file exists"
				 * resurrects a removed file. */
				ino = (long)be32(d + i + 20);
			}
		}
		i += (tot + 3) & ~3u;
	}
	return ino;
}


/* Reconstruct current contents of `name` into filebuf.
 * -> length (0 is a VALID answer: a latest node may truncate to empty), or -1
 *    when the image cannot be reconstructed FAITHFULLY.
 *
 * ★★★ WHAT THIS USED TO DO, AND WHY IT COULD NOT VALIDATE ANYTHING (repaired
 * 2026-09-12, after the CURRENT compiled function was executed under Unicorn
 * against four fixtures):
 *
 *   valid 1024-byte file                      -> 1024            (right)
 *   newer metadata node with isize = 0        -> the OLD 1024     (wrong)
 *   deliberate data-CRC mismatch              -> the CORRUPT 1024 (wrong)
 *   newest fragment, good CRC, invalid zlib   -> the OLD 1024     (wrong)
 *
 * ⇒ an "exactly 1024 bytes came back" check could not tell fresh from stale nor
 *   sound from corrupt, and the GN25L95 calibration contract requires exactly
 *   1024 VALIDATED bytes before a single laser register is written. Programming
 *   a laser from silently stale calibration is the failure this prevents.
 *
 * ★ THE FOUR REPAIRS: header/node/data CRCs are verified; `isize` of the
 *   highest version decides the length, so a truncation SHRINKS the file; every
 *   read is bounded inside its own node and inside the image; and a node we
 *   cannot decode is an ERROR, never a silent skip back to older bytes.
 *
 * ⚠ AN EMPTY FILE IS NOT CORRUPTION. A valid latest node with isize = 0 returns
 *   0, faithfully -- it is the CONSUMER that says "0 is not 1024 bytes of
 *   calibration". Forcing this generic reader to call every empty file corrupt
 *   would put a policy of one caller into a routine several callers share.
 */
/* JFFS2's rtime: (byte, repeat) pairs; a repeat copies from where that byte last
 * began. -> 0, or -1 when the stream does not expand to exactly `dsize` bytes.
 * jffs2_garbage_collect rewrites a file's tail this way, which is how this unit's
 * rtkbosa_k.bin came to end in a 4-byte rtime node (G24W mtd3, 2026-10-14). */
static int rtime_expand(uint8_t *out, uint32_t dsize, const uint8_t *in, uint32_t csize)
{
	uint32_t last[256] = { 0 }, o = 0, i = 0;

	while (o < dsize) {
		uint32_t from, rep;
		uint8_t v;

		if (i + 2 > csize)
			return -1;
		v = in[i++];
		rep = in[i++];
		out[o++] = v;
		from = last[v];
		last[v] = o;
		if (rep > dsize - o)
			return -1;
		while (rep--)
			out[o++] = out[from++];
	}
	return i == csize ? 0 : -1;
}

static long jffs2_read_file(const uint8_t *d, uint32_t n, const char *name)
{
	size_t nl = strlen(name);
	uint32_t ino = 0, best = 0, i;
	int nv = 0;
	int saw_jffs2 = 0;      /* any node with the jffs2 magic at all */
	long ino_l;

	/* ★ ONE AUTHORITY, ACTUALLY CALLED. This function used to carry its own copy
	 * of the directory scan beside jffs2_root_ino() -- the helper existed and
	 * nothing here called it, so "one parser" was true of the file and false of
	 * the code. Two scans of one structure disagree eventually, and the reader
	 * would have diverged from the selection that chose the filename for it. */
	ino_l = jffs2_root_ino(d, n, name, &saw_jffs2);
	if (ino_l < 0)
		return -1;
	ino = (uint32_t)ino_l;
	(void)nl; (void)best;

	for (i = 0; i + 12 <= n; ) {                       /* pass 2: collect INODE nodes */
		uint32_t tot;
		uint16_t nt;
		if (be16(d + i) != 0x1985) { i += 4; continue; }
		nt = be16(d + i + 2);
		tot = be32(d + i + 4);
		/* INVARIANT: a live node of OUR inode is never skipped silently --
		 * neither one whose declared totlen runs past the image nor one too
		 * short to be an inode. Skipping is right for another inode's node;
		 * for ours it loses the newest version and the previous contents are
		 * returned as current. The ino field is at +12, so the bound test must
		 * come first and the malformed test BEFORE the generic `continue`. */
		if ((nt & 0x2000) && (nt & 0xff) == 2 && i + 16 <= n &&
		    be32(d + i + 12) == ino &&
		    (tot < 68 || (uint64_t)i + tot > n))
			return -1;
		if (tot < 12 || (uint64_t)i + tot > n) { i += 4; continue; }
		if ((nt & 0x2000) && (nt & 0xff) == 2 && tot >= 68 &&
		    be32(d + i + 12) == ino &&
		    nv < (int)(sizeof vn / sizeof vn[0])) {
			vn[nv].ver = be32(d + i + 16);
			vn[nv].pos = i;
			nv++;
		}
		i += (tot + 3) & ~3u;
	}
	qsort(vn, nv, sizeof vn[0], cmp_ver);
	memset(covered, 0, sizeof covered);

	long flen = 0;
	uint32_t isize = 0;
	long c;              /* from the HIGHEST version that carries one */
	int have_isize = 0;
	/* ★★★ WIDTH IS PART OF THE CHECK ON A 32-BIT TARGET (2026-09-12). This was a
	 * `long` filled from be32(): on MIPS32 a valid-CRC inode declaring isize
	 * >= 0x80000000 lands NEGATIVE, `if (isize >= 0)` is skipped, and the
	 * authoritative-size check silently does not happen -- the very check that
	 * stops a confident short read. A 64-bit host wrapper cannot see it, because
	 * there `long` is wide enough to hold the value. Keep it unsigned and bound
	 * it against FILE_MAX before any signed conversion. */
	int k;
	for (k = 0; k < nv; k++) {
		const uint8_t *p = d + vn[k].pos;
		uint32_t tot  = be32(p + 4);
		uint32_t off  = be32(p + 44), csize = be32(p + 48), dsize = be32(p + 52);
		uint8_t compr = p[56];
		const uint8_t *src = p + 68;

		/* header CRC covers the first 8 bytes; node CRC the first 64 */
		if (j_crc(p, 8) != be32(p + 8))
			return -1;
		/* ★ 60, NOT 64: data_crc occupies [60..63] and node_crc [64..67], so the
		 * node CRC covers everything BEFORE data_crc. Confirmed on both nodes
		 * above -- over 60 matches, over 64 does not. */
		if (j_crc(p, 60) != be32(p + 64))
			return -1;
		/* the payload must lie inside its own node AND inside the image */
		if ((uint64_t)68 + csize > tot ||
		    (uint64_t)(src - d) + csize > n)
			return -1;
		if (j_crc(src, csize) != be32(p + 60))
			return -1;
		if ((uint64_t)off + dsize > FILE_MAX)
			return -1;

		isize = be32(p + 28);         /* ascending version order: last wins */
		have_isize = 1;
		if (isize > (uint32_t)FILE_MAX)
			return -1;            /* unrepresentable here; never silently clip */
		/* ★ A SHRINK INVALIDATES what lies beyond it, for every later node too.
		 * Without this the buffer keeps older bytes and they are handed back as
		 * current content. */
		if ((long)isize < flen) {
			memset(covered + isize, 0, (size_t)(flen - (long)isize));
			flen = (long)isize;
		}
		if (compr == 0) {                              /* none */
			/* ★ WAS `if (dsize > csize) dsize = csize;` -- a SHORT raw node
			 * quietly delivered fewer bytes than it claimed and the tail stayed
			 * whatever an older fragment had left there. Inconsistent is an
			 * error, not something to round down. */
			if (dsize != csize)
				return -1;
			memcpy(filebuf + off, src, dsize);
		} else if (compr == 6) {                       /* zlib */
			uLongf dl = dsize;
			if (uncompress(filebuf + off, &dl, src, csize) != Z_OK)
				return -1;            /* ERROR, not "keep the older bytes" */
			/* ★ AND IT MUST EXPAND TO EXACTLY WHAT IT ADVERTISES. A stream that
			 * inflates to 512 while declaring dsize 1024 -- every stored CRC
			 * valid -- used to return 1024 bytes made of 512 NEW and 512 OLD.
			 * Measured: that blend hashes 24d095b1ab349f84c1b20c98e1ea957e5...
			 * and would have been programmed into a laser as "this unit's
			 * calibration". */
			if (dl != dsize)
				return -1;
		} else if (compr == 2) {                       /* rtime */
			if (rtime_expand(filebuf + off, dsize, src, csize))
				return -1;
		} else {
			return -1;                    /* lzo and the rest: we cannot reconstruct */
		}
		memset(covered + off, 1, dsize);
		if ((long)(off + dsize) > flen)
			flen = off + dsize;
	}
	/* ★★ THE LATEST isize IS AUTHORITATIVE, IN BOTH DIRECTIONS. Shrinking alone
	 *   was still wrong: a newest metadata node declaring isize 2048 over an old
	 *   1024 returned 1024 -- a CONFIDENT SHORT READ, which for calibration is
	 *   the worst answer of all because the length check passes.
	 * ⇒ if the reconstruction does not cover the declared size, the image is
	 *   incomplete for this file and we say so. Legal holes are not guessed at
	 *   here; a caller that needs them can ask for them explicitly. */
	if (have_isize) {
		if ((long)isize > flen)
			return -1;            /* declared longer than we can reconstruct */
		flen = (long)isize;
	}
	/* ★ EVERY byte of the current content must have been WRITTEN by a live
	 * node. An extent that reaches the declared size while leaving a gap in the
	 * middle is not the file -- it is the file plus whatever was in the buffer. */
	for (c = 0; c < flen; c++)
		if (!covered[c])
			return -1;
	return flen;
}

/* Legacy raw MIB fallback: recover XML from zlib streams when no JFFS2
 * family exists. The current G24W and X111W dumps use JFFS2, with different
 * authoritative filenames; neither may fall through after a corrupt read. */
static const char *xml_value(const uint8_t *buf, long len, const char *key);

static const char *mib_value(const uint8_t *d, long n, const char *key)
{
	static char out[256];
	const char *hit = NULL;
	long i;

	for (i = 0; i + 4 <= n; i++) {
		uLongf dl;
		const char *v;

		if (d[i] != 0x78)                     /* zlib CMF: deflate, 32K win */
			continue;
		if (((unsigned)(d[i] << 8) | d[i + 1]) % 31)
			continue;                     /* FCHECK invalid: not a header */
		dl = FILE_MAX;
		if (uncompress(filebuf, &dl, d + i, (uLong)(n - i)) != Z_OK)
			continue;
		if (dl <= 64)                         /* oracle's own noise floor */
			continue;
		v = xml_value(filebuf, (long)dl, key);
		if (v) {
			snprintf(out, sizeof out, "%s", v);
			hit = out;                    /* last stream wins, as the oracle */
		}
	}
	return hit;
}

/*
 * The Realtek RTOS ("OSK") storage of the VSOL V2802RH / V2801RD, measured on a
 * full dump of the unit's 4 MiB NOR:
 *
 *   MIB    : be32 clen, be32 sum, then clen bytes of LZSS (4 KiB window, 3..18
 *            byte matches, LSB-first flag bits) expanding to
 *            <Config_Information_File_8671> XML of <V N="KEY" V="value"/>.
 *            sum is the 32-bit total of the compressed bytes as be16 words.
 *   OSK1FS : "-OSK1FS-", be32 len, be32 sum, then len bytes of entries
 *            { char name[28]; be32 size; u8 data[size]; } each starting on a
 *            16-byte boundary. sum is the body's be16 total folded to 16 bits
 *            with end-around carry. Both checksums match the stock boot log.
 */
#define OSK_MAGIC	"-OSK1FS-"
#define OSK_NAME_LEN	28

static uint32_t sum_be16(const uint8_t *p, uint32_t n)
{
	uint32_t s = 0, i;

	for (i = 0; i + 1 < n; i += 2)
		s += be16(p + i);
	if (n & 1)
		s += (uint32_t)p[n - 1] << 8;
	return s;
}

static uint32_t fold16(uint32_t s)
{
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return s;
}

static long lzss_expand(uint8_t *out, long max, const uint8_t *in, uint32_t n)
{
	uint8_t win[4096];
	unsigned int r = sizeof win - 18, flags = 0, k;
	uint32_t i = 0;
	long o = 0;

	memset(win, ' ', sizeof win);
	while (i < n) {
		flags >>= 1;
		if (!(flags & 0x100)) {
			flags = in[i++] | 0xff00;
			if (i >= n)
				break;
		}
		if (flags & 1) {
			if (o >= max)
				return -1;
			out[o++] = win[r] = in[i++];
			r = (r + 1) & (sizeof win - 1);
			continue;
		}
		if (i + 2 > n)
			return -1;
		unsigned int pos = in[i] | ((in[i + 1] & 0xf0) << 4);
		unsigned int len = (in[i + 1] & 0x0f) + 3;

		i += 2;
		for (k = 0; k < len; k++) {
			if (o >= max)
				return -1;
			out[o++] = win[r] = win[(pos + k) & (sizeof win - 1)];
			r = (r + 1) & (sizeof win - 1);
		}
	}
	return o;
}

/* -> the expanded XML length in filebuf, or -1 when d is not a valid OSK MIB. */
static long osk_mib(const uint8_t *d, uint32_t n)
{
	uint32_t clen;
	long len;

	if (n < 8)
		return -1;
	clen = be32(d);
	if (!clen || clen > n - 8 || sum_be16(d + 8, clen) != be32(d + 4))
		return -1;
	len = lzss_expand(filebuf, FILE_MAX, d + 8, clen);
	if (len < 0 || !memmem(filebuf, len, "<Config_Information_File", 24))
		return -1;
	return len;
}

static const char *osk_value(const uint8_t *buf, long len, const char *key)
{
	static char out[256];
	char pat[96];
	int pl = snprintf(pat, sizeof pat, "<V N=\"%s\" V=\"", key);
	const uint8_t *v = memmem(buf, len, pat, pl), *e;

	if (!v)
		return NULL;
	v += pl;
	e = memchr(v, '"', buf + len - v);
	if (!e || e - v >= (long)sizeof out)
		return NULL;
	memcpy(out, v, e - v);
	out[e - v] = 0;
	return out;
}

/*
 * One file from the OSK1FS containers in d (each on a 4 KiB boundary).
 * -> its size, copied to filebuf; -1 when absent, or when a container fails its
 * checksum (a file it might hold is never guessed at).
 */
static long osk1fs_read_file(const uint8_t *d, uint32_t n, const char *name)
{
	uint32_t o;
	int corrupt = 0;

	for (o = 0; o + 16 <= n; o += 0x1000) {
		const uint8_t *body = d + o + 16;
		uint32_t len, p;

		if (memcmp(d + o, OSK_MAGIC, 8))
			continue;
		len = be32(d + o + 8);
		if (len > n - o - 16 || fold16(sum_be16(body, len)) != be32(d + o + 12)) {
			corrupt = 1;
			continue;
		}
		for (p = 0; p + OSK_NAME_LEN + 4 <= len; ) {
			const uint8_t *ent = body + p;
			uint32_t size = be32(ent + OSK_NAME_LEN);

			if (size > len - p - OSK_NAME_LEN - 4 || size > FILE_MAX)
				break;
			if (strnlen((const char *)ent, OSK_NAME_LEN) == strlen(name) &&
			    !memcmp(ent, name, strlen(name))) {
				memcpy(filebuf, ent + OSK_NAME_LEN + 4, size);
				return size;
			}
			p = (p + OSK_NAME_LEN + 4 + size + 15) & ~15u;
		}
	}
	if (corrupt)
		fprintf(stderr, "rtk_factory: an OSK1FS container failed its checksum\n");
	return -1;
}

static const char *chain_value(const uint8_t *buf, long len, const char *key);

/* Extract Value of <... Name="key" ... Value="val" ...> from a buffer; a key
 * "TABLE.idx.field" names one row of a vendor chain (chain_value). */
static const char *xml_value(const uint8_t *buf, long len, const char *key)
{
	static char out[256];
	char pat[96];
	int pl = snprintf(pat, sizeof pat, "Name=\"%s\"", key);
	long i;
	if (strchr(key, '.'))
		return chain_value(buf, len, key);
	for (i = 0; i + pl <= len; i++) {
		if (memcmp(buf + i, pat, pl))
			continue;
		const uint8_t *v = (const uint8_t *)memmem(buf + i, len - i, "Value=\"", 7);
		if (!v)
			return NULL;
		v += 7;
		const uint8_t *e = (const uint8_t *)memchr(v, '"', buf + len - v);
		if (!e || e - v >= (long)sizeof out)
			return NULL;
		memcpy(out, v, e - v);
		out[e - v] = 0;
		return out;
	}
	return NULL;
}

/* One field of one row of a vendor chain: the row is a <Dir Name="TABLE">
 * element whose first token is <!--index=IDX-->, and SSID and key are read
 * from the SAME row so they can never be paired across two networks. */
static const char *chain_value(const uint8_t *buf, long len, const char *key)
{
	char table[64], idx[16], field[64], dir[96], mark[32];
	const uint8_t *p = buf, *end = buf + len;
	int dl, ml;

	if (sscanf(key, "%63[^.].%15[^.].%63s", table, idx, field) != 3)
		return NULL;
	dl = snprintf(dir, sizeof dir, "<Dir Name=\"%s\">", table);
	ml = snprintf(mark, sizeof mark, "<!--index=%s-->", idx);
	while (p < end && (p = memmem(p, end - p, dir, dl))) {
		const uint8_t *body = p + dl;
		const uint8_t *close = memmem(body, end - body, "</Dir>", 6);

		if (!close)
			return NULL;
		while (body < close && (*body == ' ' || *body == '\t' ||
					*body == '\r' || *body == '\n'))
			body++;
		if (close - body >= ml && !memcmp(body, mark, ml))
			return xml_value(body + ml, close - body - ml, field);
		p = close;
	}
	return NULL;
}

/* ★★★ TWO UNITS, TWO NAMINGS, ONE READER (2026-09-12). Measured with header,
 * dirent and name CRCs on each unit's OWN partition:
 *
 *   X111W  mtd3 245760 B : lastgood.xml (ino 16, ver 3466) + lastgood_hs.xml (3)
 *   G24W   mtd3 262144 B : config.xml   (ino 392, ver 1118) + config_hs.xml (389)
 *
 * ⚠ A GLOBAL RENAME IS THE WRONG REPAIR, and I made it before this was measured:
 *   pointing every HW verb at config_hs.xml broke the X111W's hardware identity
 *   while still missing the G24W's software half. One family per unit, SELECTED,
 *   is the operator's "common code, differences only" applied literally.
 *
 * ⚠ SELECTION IS BY ROOT PRESENCE, NEVER BY CONTENT. A corrupt selected file
 *   must NOT make us try the other family -- that would answer from a different
 *   unit's naming convention rather than admitting the read failed. And *_bak is
 *   never a fallback: it is the PREVIOUS generation, not a spare copy.
 */
struct family { const char *hw, *sw; };
static const struct family FAMILIES[] = {
	{ "lastgood_hs.xml", "lastgood.xml" },
	{ "config_hs.xml",   "config.xml"   },
	{ NULL, NULL }
};

/* -> the family this partition uses, or NULL when neither naming is present. */
static const struct family *jffs2_family(const uint8_t *d, uint32_t n)
{
	const struct family *f;

	const struct family *hit = NULL;

	/* ★ EVERY family is examined, because TAKING THE FIRST COMPLETE PAIR is not
	 * a selection -- it is a preference, and it would silently pick one naming
	 * on a partition that carries both. Two complete pairs is an AMBIGUITY and
	 * the honest answer is to refuse, not to let table order decide which
	 * unit's convention an identity comes from. */
	for (f = FAMILIES; f->hw; f++) {
		long a = jffs2_root_ino(d, n, f->hw, NULL);
		long b = jffs2_root_ino(d, n, f->sw, NULL);

		if (a < 0 || b < 0)
			return NULL;            /* corrupt directory: never guess */
		if (a > 0 && b > 0) {
			if (hit)
				return NULL;    /* ambiguous: two complete namings */
			hit = f;
		}
	}
	return hit;
}

/* ★★★ THE FILENAMES WERE WRONG AND THAT IS WHY THIS READER WAS NEVER REACHED
 * (measured 2026-09-12 on this unit's own 256 KiB mtd3). The container IS jffs2;
 * its root dirents are config.xml (ino 392, ver 1118), config_bak.xml (391),
 * config_hs.xml (389), config_hs_bak.xml (388), rtkbosa_k.bin (74) -- and there
 * is NO lastgood.xml or lastgood_hs.xml anywhere in it. Asking for a file that
 * does not exist made jffs2_read_file return -1, and the caller treated that as
 * "not jffs2" and fell through to a BLIND zlib scan. So every identity answer
 * came from a scan with no CRCs, no isize and no idea which file it came from,
 * while the validated route sat unused.
 * ⚠ THE SAME CLAIM IS IN CLAUDE.md ("holds the vendor's live lastgood*.xml")
 *   and is refuted by these bytes; it is owed a correction where it is written.
 * ⚠ _hs IS THE HARDWARE HALF: config_hs.xml carries ELAN_MAC_ADDR and GPON_SN.
 *   The *_bak.xml files are the PREVIOUS generation -- current-vs-backup is a
 *   semantic this reader must keep, not a pair of interchangeable names. */
enum famrole { F_HW, F_SW };
struct map { const char *verb; enum famrole role; const char *key; int is_mac; int secret; };
static const struct map MAP[] = {
	{ "mac",           F_HW, "ELAN_MAC_ADDR",     1, 0 },
	{ "wlan_mac",      F_HW, "WLAN_MAC_ADDR",     1, 0 },
	{ "sn",            F_HW, "GPON_SN",           0, 0 },
	{ "oui",           F_HW, "OUI",               0, 0 },
	{ "vendor_id",     F_HW, "PON_VENDOR_ID",     0, 0 },
	{ "model",         F_HW, "GPON_ONU_MODEL",    0, 0 },
	{ "hw_ver",        F_HW, "HW_HWVER",          0, 0 },
	{ "lan_type",      F_HW, "LAN_TYPE",          0, 0 },
	{ "wlan_cck_a",    F_HW, "HW_WLAN0_TX_POWER_CCK_A",     0, 0 },
	{ "wlan_cck_b",    F_HW, "HW_WLAN0_TX_POWER_CCK_B",     0, 0 },
	{ "wlan_ht40_a",   F_HW, "HW_WLAN0_TX_POWER_HT40_1S_A", 0, 0 },
	{ "wlan_ht40_b",   F_HW, "HW_WLAN0_TX_POWER_HT40_1S_B", 0, 0 },
	{ "wlan_thermal",  F_HW, "HW_WLAN0_11N_THER",           0, 0 },
	{ "wlan_xcap",     F_HW, "HW_WLAN0_11N_XCAP",           0, 0 },
	{ "wlan_reg_domain", F_HW, "HW_WLAN0_REG_DOMAIN",       0, 0 },
	{ "mac_key",       F_HW, "MAC_KEY",           0, 0 },
	{ "loid",          F_SW,    "LOID",              0, 0 },
	{ "loid_passwd",   F_SW,    "LOID_PASSWD",       0, 0 },
	{ "ploam_passwd",  F_SW,    "GPON_PLOAM_PASSWD", 0, 0 },
	{ "olt_mode",      F_SW,    "OMCI_OLT_MODE",     0, 0 },
	{ "sw_ver1",       F_SW,    "OMCI_SW_VER1",      0, 0 },
	{ "sw_ver2",       F_SW,    "OMCI_SW_VER2",      0, 0 },
	{ "product_code",  F_SW,    "OMCI_VENDOR_PRODUCT_CODE", 0, 0 },
	/* The stock AP of this unit: the first row of the vendor's SSID chain,
	 * else its flat pair. A key is a secret: `all` never prints it. */
	{ "wlan_ssid",      F_SW, "WLAN_MBSSIB_TBL.0.ssid",   0, 0 },
	{ "wlan_psk",       F_SW, "WLAN_MBSSIB_TBL.0.wpaPSK", 0, 1 },
	{ "wlan_ssid_flat", F_SW, "SSID",                     0, 0 },
	{ "wlan_psk_flat",  F_SW, "WLAN_WPA_PSK",             0, 1 },
	{ NULL, F_HW, NULL, 0, 0 }
};

static int mtd_path_by_name(const char *name, char *out, size_t n);

/*
 * Read a whole partition into partbuf. -> bytes read, or -1.
 *
 * ⚠ A SHORT READ IS NOT A SHORT PARTITION. The three copies this replaces
 *   stopped on any read() <= 0 and went on with whatever they had, so an I/O
 *   error partway through left a TRUNCATED image that still parses -- and an
 *   older but still valid inode inside it is then published as the current
 *   one. Stale calibration is the worst possible output of this program, so
 *   only EINTR is retried and every other error is a failure. close() is
 *   checked too: on a flash device that is where a deferred error surfaces.
 */
static ssize_t part_read(const char *part, char *devpath, size_t dplen)
{
	ssize_t got = 0, r;
	uint8_t extra;
	int fd;

	if (part[0] == '/')
		snprintf(devpath, dplen, "%s", part);
	else if (mtd_path_by_name(part, devpath, dplen) < 0) {
		fprintf(stderr, "rtk_factory: partition '%s' not in /proc/mtd\n", part);
		return -1;
	}

	fd = open(devpath, O_RDONLY);
	if (fd < 0) { perror(devpath); return -1; }
	for (;;) {
		r = got < (ssize_t)sizeof partbuf ?
			read(fd, partbuf + got, sizeof partbuf - got) :
			read(fd, &extra, 1);
		if (r == 0)
			break;			/* the partition ends here */
		if (r < 0) {
			if (errno == EINTR)
				continue;
			perror(devpath);
			if (close(fd))
				perror(devpath);
			return -1;
		}
		if (got == (ssize_t)sizeof partbuf) {
			fprintf(stderr, "rtk_factory: %s exceeds the partition buffer - refusing a partial image\n", devpath);
			if (close(fd))
				perror(devpath);
			return -1;
		}
		got += r;
	}
	if (close(fd)) { perror(devpath); return -1; }
	return got;
}

/*
 * optical_cal: parse the rtl8290b Europa transceiver calibration blob
 * (rtl8290b.data in the config JFFS2 partition, 4096 bytes factory-written).
 *
 * Calibration byte offsets (big-endian, as laid out in the factory-written blob):
 *   1350 int32  rx_a     - RX polynomial coefficient A
 *   1354 int32  rx_b     - RX polynomial coefficient B
 *   1358 int32  rx_c     - RX polynomial coefficient C (intercept ~= launch dBm*1000)
 *   1362 uint32 rssi_v0  - RX RSSI reference ADC count at factory calibration
 *   1366 uint32 mpd0     - TX MPD reference photo-diode count
 *   1372 int32  tx_a     - TX polynomial coefficient A
 *   1376 int32  tx_b     - TX polynomial coefficient B
 *   1380 int32  tx_c     - TX polynomial coefficient C (intercept ~= RX sensitivity dBm*1000)
 *   1384 int8   temp_off - Temperature offset (°C)
 *   1386 uint8  rx_th    - RX LOS assert threshold (raw)
 *   1387 uint8  rx_deth  - RX LOS de-assert threshold (raw)
 *
 * Prints: key=value pairs, one per line.
 */
/*
 * bosa_cal: hand out THIS unit's GN25L95 calibration, verbatim.
 *
 * ⚠ NOT optical_cal. That verb decodes the RTL8290B's DDM coefficients and
 *   prints key=value lines; this one emits the 1024 raw bytes of the Semtech
 *   part's factory calibration. Two different parts, two different files, two
 *   different shapes -- and the names are close enough that conflating them
 *   would hand a laser driver a table of polynomial constants.
 *
 * Stock's own precedence (rtkbosa.sh): the PON-mode file first, then the
 * general one. Anything that is not exactly GN_CAL_LEN bytes is REFUSED: there
 * is no second source, because the numbers belong to this optical subassembly
 * and no other unit's will do.
 */
#define BOSA_CAL_LEN	1024
#define OSK_BOSA_NAME	"rtl-25l95-backup.bin"

static int remove_file(const char *path)
{
	if (unlink(path) && errno != ENOENT) {
		perror(path);
		return 1;
	}
	return 0;
}

static int emit_file(const char *out, const uint8_t *data, size_t len)
{
	char tmp[300];
	ssize_t w;
	int fd;

	snprintf(tmp, sizeof tmp, "%s.new", out);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { perror(tmp); return 1; }
	w = write(fd, data, len);
	if (close(fd) || w != (ssize_t)len) {
		fprintf(stderr, "rtk_factory: short write to %s (%ld of %zu)\n",
			tmp, (long)w, len);
		remove_file(tmp);
		return 1;
	}
	if (rename(tmp, out)) { perror(out); remove_file(tmp); return 1; }
	return 0;
}

static int do_bosa_cal(const char *part, const char *out)
{
	static const char *const GPON_NAME = "rtkbosa_gpon_k.bin";
	static const char *const GEN_NAME  = "rtkbosa_k.bin";
	static const char *const SUM_NAME  = "rtkbosa_k_checksum";
	char devpath[256], sumpath[300];
	const char *picked;
	int general = 0, saw = 0;
	ssize_t got;
	long flen;

	got = part_read(part, devpath, sizeof devpath);
	if (got < 0)
		return 1;

	/*
	 * ⚠ THE CHOICE IS PRESENCE, NOT LENGTH -- stock's rtkbosa.sh tests -f.
	 *   Falling through to the general file when the PON-mode one reads back
	 *   empty would quietly substitute a different file for the one this
	 *   board is configured to use, and hide a corrupt preferred file behind
	 *   a healthy-looking general one.
	 */
	if (jffs2_root_ino(partbuf, (uint32_t)got, GPON_NAME, &saw) > 0) {
		picked = GPON_NAME;
	} else if (jffs2_root_ino(partbuf, (uint32_t)got, GEN_NAME, &saw) > 0) {
		picked = GEN_NAME;
		general = 1;
	} else if (!saw) {
		/* The RTOS keeps the same 1024 bytes as an OSK1FS file, with no sidecar. */
		picked = OSK_BOSA_NAME;
		flen = osk1fs_read_file(partbuf, (uint32_t)got, picked);
		if (flen != BOSA_CAL_LEN) {
			fprintf(stderr, "rtk_factory: no rtkbosa calibration in %s (OSK1FS: %ld)\n",
				devpath, flen);
			return 1;
		}
		snprintf(sumpath, sizeof sumpath, "%s.md5", out);
		if (emit_file(out, filebuf, BOSA_CAL_LEN) || remove_file(sumpath))
			return 1;
		fprintf(stderr, "rtk_factory: %s -> %s (%d bytes)\n", picked, out,
			BOSA_CAL_LEN);
		return 0;
	} else {
		fprintf(stderr, "rtk_factory: no rtkbosa calibration in %s%s\n",
			devpath, saw ? "" : " (no jffs2 nodes at all)");
		return 1;
	}

	flen = jffs2_read_file(partbuf, (uint32_t)got, picked);
	if (flen != BOSA_CAL_LEN) {
		fprintf(stderr, "rtk_factory: %s reconstructs to %ld bytes, expected %d - refusing it\n",
			picked, flen, BOSA_CAL_LEN);
		return 1;
	}
	if (emit_file(out, filebuf, BOSA_CAL_LEN))
		return 1;

	/*
	 * The md5 sidecar belongs to the GENERAL file and stock's GPON branch
	 * never consults it, so it is neither read nor emitted for the PON-mode
	 * file. An unreadable sidecar is a refusal; an empty sidecar is valid,
	 * as in stock. The comparison
	 * itself is the caller's -- md5 lives in the shell, not here.
	 */
	snprintf(sumpath, sizeof sumpath, "%s.md5", out);
	if (general && jffs2_root_ino(partbuf, (uint32_t)got, SUM_NAME, &saw) > 0) {
		long sl = jffs2_read_file(partbuf, (uint32_t)got, SUM_NAME);

		/* ⚠ EMPTY IS NOT BROKEN. Stock treats an empty stored sum as "use
		 *   the file anyway" and the Elnath staging script does the same,
		 *   so refusing it here would reject a board the vendor's own
		 *   firmware accepts. A read that FAILED is the separate fact. */
		if (sl < 0) {
			fprintf(stderr, "rtk_factory: %s is present and could not be read (%ld) - refusing %s\n",
				SUM_NAME, sl, picked);
			remove_file(out);
			remove_file(sumpath);
			return 1;
		}
		if (emit_file(sumpath, filebuf, (size_t)sl)) {
			remove_file(out);
			return 1;
		}
	} else {
		/* ⚠ NO STALE SIDECAR. A previous call may have left one here; if
		 *   this call selected the PON-mode file, or the general file has
		 *   no sidecar, that older checksum belongs to different bytes and
		 *   a consumer would verify the new calibration against it. */
		if (remove_file(sumpath)) {
			remove_file(out);
			return 1;
		}
	}

	fprintf(stderr, "rtk_factory: %s -> %s (%d bytes)\n", picked, out,
		BOSA_CAL_LEN);
	return 0;
}

static int do_optical_cal(const char *part)
{
	uint8_t blob[4096];
	char devpath[256];
	ssize_t got;
	long flen = -1;

	got = part_read(part, devpath, sizeof devpath);
	if (got < 0)
		return 1;
	flen = jffs2_read_file(partbuf, (uint32_t)got, "rtl8290b.data");

	if (flen < 1388) {
		fprintf(stderr, "rtk_factory: rtl8290b.data absent or too short (%ld)\n", flen);
		return 1;
	}
	memcpy(blob, filebuf, sizeof blob < (size_t)flen ? sizeof blob : (size_t)flen);

	int32_t  rx_a    = (int32_t)be32(blob + 1350);
	int32_t  rx_b    = (int32_t)be32(blob + 1354);
	int32_t  rx_c    = (int32_t)be32(blob + 1358);
	uint32_t rssi_v0 = be32(blob + 1362);
	uint32_t mpd0    = be32(blob + 1366);
	int32_t  tx_a    = (int32_t)be32(blob + 1372);
	int32_t  tx_b    = (int32_t)be32(blob + 1376);
	int32_t  tx_c    = (int32_t)be32(blob + 1380);
	int8_t   t_off   = (int8_t)blob[1384];
	uint8_t  rx_th   = blob[1386];
	uint8_t  rx_deth = blob[1387];

	printf("rx_a=%d\n",    rx_a);
	printf("rx_b=%d\n",    rx_b);
	printf("rx_c=%d\n",    rx_c);
	printf("rssi_v0=%u\n", rssi_v0);
	printf("mpd0=%u\n",    mpd0);
	printf("tx_a=%d\n",    tx_a);
	printf("tx_b=%d\n",    tx_b);
	printf("tx_c=%d\n",    tx_c);
	printf("temp_off=%d\n",(int)t_off);
	printf("rx_los_th=%u\n",   rx_th);
	printf("rx_delos_th=%u\n", rx_deth);
	return 0;
}

/*
 * laser_cal: THIS unit's RTL8290B laser bias and modulation currents, read from
 * its own rtl8290b.data (CAL_IBIAS / CAL_IMOD, big-endian uA at 0x606 / 0x60a;
 * the bench X111W holds 9960 / 36694 uA there). Printed beside the hi-8 code of
 * the 12-bit DAC over 50 mA full scale, which luna_gpon's laser_bias / laser_mod
 * take, and the full 12-bit code (its low nibble is per unit too). A value the
 * DAC cannot express is refused: a laser is never programmed
 * from another unit's numbers or from a guess.
 */
#define CAL_IBIAS_OFF	0x606
#define CAL_IMOD_OFF	0x60a
#define DAC_FULL_UA	50000u

static unsigned int dac12(uint32_t ua)
{
	return (unsigned int)((uint64_t)ua * 4096 / DAC_FULL_UA);
}

static int do_laser_cal(const char *part)
{
	char devpath[256];
	uint32_t ibias, imod;
	ssize_t got;
	long flen;

	got = part_read(part, devpath, sizeof devpath);
	if (got < 0)
		return 1;
	flen = jffs2_read_file(partbuf, (uint32_t)got, "rtl8290b.data");
	if (flen < CAL_IMOD_OFF + 4) {
		fprintf(stderr, "rtk_factory: rtl8290b.data absent or too short (%ld)\n", flen);
		return 1;
	}
	ibias = be32(filebuf + CAL_IBIAS_OFF);
	imod = be32(filebuf + CAL_IMOD_OFF);
	if (ibias >= DAC_FULL_UA || imod >= DAC_FULL_UA || dac12(ibias) < 16 || dac12(imod) < 16) {
		fprintf(stderr, "rtk_factory: rtl8290b.data CAL_IBIAS %u / CAL_IMOD %u uA "
			"is outside the DAC's range - refusing it\n", ibias, imod);
		return 1;
	}
	printf("ibias_ua=%u\nimod_ua=%u\nlaser_bias=0x%02x\nlaser_mod=0x%02x\n"
	       "laser_bias_dac=0x%03x\nlaser_mod_dac=0x%03x\n", ibias, imod,
	       dac12(ibias) >> 4, dac12(imod) >> 4, dac12(ibias), dac12(imod));
	return 0;
}

static int mtd_path_by_name(const char *name, char *out, size_t n)
{
	FILE *f = fopen("/proc/mtd", "r");
	char line[256], nm[128];
	int idx, found = -1;
	if (!f)
		return -1;
	if (fgets(line, sizeof line, f)) {                 /* header */
		while (fgets(line, sizeof line, f))
			if (sscanf(line, "mtd%d: %*x %*x \"%127[^\"]\"", &idx, nm) == 2 &&
			    !strcmp(nm, name)) {
				snprintf(out, n, "/dev/mtd%d", idx);
				found = 0;
				break;
			}
	}
	fclose(f);
	return found;
}

/* One value as the verb prints it: a MAC as aa:bb:.., anything else verbatim. */
static void print_value(const struct map *m, const char *val, const char *prefix)
{
	int j;

	printf("%s", prefix);
	if (m->is_mac && strlen(val) == 12) {              /* 001122334455 -> 00:11:22:... */
		for (j = 0; j < 12; j += 2)
			printf("%c%c%s", val[j], val[j + 1], j < 10 ? ":" : "\n");
	} else {
		printf("%s\n", val);
	}
}

/* Every MAP verb as one `verb=value` line from ONE read of the partition: the
 * provisioning init asked ~25 times at ~0.12 s each. A key the unit lacks is
 * left out, exactly as its own verb exits 1; a refusal refuses the whole set. */
static int do_all(const char *part)
{
	static const enum famrole roles[] = { F_HW, F_SW };
	const struct family *fam;
	const struct map *m;
	char path[256], prefix[40];
	int is_jffs2 = 0;
	ssize_t got;
	long osk_len;
	unsigned int r;

	got = part_read(part, path, sizeof path);
	if (got < 0)
		return 1;
	osk_len = osk_mib(partbuf, (uint32_t)got);
	if (osk_len >= 0) {
		for (m = MAP; m->verb; m++) {
			const char *val = m->secret ? NULL : osk_value(filebuf, osk_len, m->key);

			if (!val)
				continue;
			snprintf(prefix, sizeof prefix, "%s=", m->verb);
			print_value(m, val, prefix);
		}
		return 0;
	}
	(void)jffs2_root_ino(partbuf, (uint32_t)got, "", &is_jffs2);
	fam = jffs2_family(partbuf, (uint32_t)got);
	if (!fam && is_jffs2) {
		fprintf(stderr, "rtk_factory: %s IS jffs2 but names no known current pair -- "
			"refusing the blind scan\n", path);
		return 3;
	}
	for (r = 0; r < sizeof(roles) / sizeof(roles[0]); r++) {
		long flen = -2;

		if (fam) {
			flen = jffs2_read_file(partbuf, (uint32_t)got,
					       roles[r] == F_HW ? fam->hw : fam->sw);
			if (flen == -1) {
				fprintf(stderr, "rtk_factory: %s is jffs2 but '%s' could not be "
					"read FAITHFULLY -- refusing every field\n", path,
					roles[r] == F_HW ? fam->hw : fam->sw);
				return 3;
			}
		}
		for (m = MAP; m->verb; m++) {
			const char *val;

			if (m->role != roles[r] || m->secret)
				continue;
			val = fam ? (flen >= 0 ? xml_value(filebuf, flen, m->key) : NULL)
				  : mib_value(partbuf, (long)got, m->key);
			if (!val)
				continue;
			snprintf(prefix, sizeof prefix, "%s=", m->verb);
			print_value(m, val, prefix);
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *part = "config", *verb;
	const struct family *fam;
	const struct map *m;
	char path[256];
	int a = 1;
	ssize_t got;
	long flen = -1;   /* the !fam path reaches have_val without setting it */
	const char *val;

	if (argc >= 3 && !strcmp(argv[1], "-p")) { part = argv[2]; a = 3; }
	if (a >= argc) {
		fprintf(stderr, "usage: rtk_factory [-p part] <mac|wlan_mac|sn|oui|"
			"vendor_id|model|hw_ver|mac_key|loid|loid_passwd|ploam_passwd|"
			"wlan_cck_a|wlan_cck_b|wlan_ht40_a|wlan_ht40_b|wlan_thermal|"
			"wlan_xcap|wlan_reg_domain|"
			"olt_mode|sw_ver1|sw_ver2|product_code|wlan_ssid|wlan_psk|"
			"wlan_ssid_flat|wlan_psk_flat|all|"
			"optical_cal|laser_cal|bosa_cal <outfile>>\n");
		return 2;
	}
	verb = argv[a];

	if (!strcmp(verb, "all"))
		return do_all(part);
	if (!strcmp(verb, "optical_cal"))
		return do_optical_cal(part);
	if (!strcmp(verb, "laser_cal"))
		return do_laser_cal(part);

	if (!strcmp(verb, "bosa_cal")) {
		if (a + 1 >= argc) {
			fprintf(stderr, "usage: rtk_factory [-p part] bosa_cal <outfile>\n");
			return 2;
		}
		return do_bosa_cal(part, argv[a + 1]);
	}

	for (m = MAP; m->verb && strcmp(m->verb, verb); m++)
		;
	if (!m->verb) {
		fprintf(stderr, "rtk_factory: unknown field '%s'\n", verb);
		return 2;
	}

	got = part_read(part, path, sizeof path);
	if (got < 0)
		return 1;

	flen = osk_mib(partbuf, (uint32_t)got);
	if (flen >= 0) {
		val = osk_value(filebuf, flen, m->key);
		goto have_val;
	}

	/* ★★ A JFFS2 FAILURE AND A DIFFERENT FORMAT ARE NOT THE SAME ANSWER. They
	 * used to share one `if (!val)`, so a CRC rejection, a truncated node and a
	 * genuinely non-jffs2 container all silently became a blind zlib scan --
	 * which is how a validated read gets bypassed by the very checks that were
	 * added to make it trustworthy. The scan stays as a route for a container
	 * that is positively NOT jffs2; it is no longer the place errors go. */
	/* ★★★ FORMAT AND FILENAME-FAMILY ARE TWO DIFFERENT QUESTIONS, and collapsing
	 * them reintroduced the exact bypass this repair removed -- one level up.
	 * A VALID jffs2 whose naming pair is missing, renamed or half-tombstoned is
	 * NOT a raw MIB container, and answering it with a blind zlib scan is
	 * answering from bytes nobody validated. Ask the format first; only a
	 * container with no jffs2 node at all earns the scan. */
	{
		int is_jffs2 = 0;
		(void)jffs2_root_ino(partbuf, (uint32_t)got, "", &is_jffs2);
		fam = jffs2_family(partbuf, (uint32_t)got);
		if (!fam) {
			if (is_jffs2) {
				fprintf(stderr, "rtk_factory: %s IS jffs2 but names no known "
					"current pair (lastgood*.xml / config*.xml) -- refusing "
					"the blind scan: an unknown or ambiguous naming is an "
					"error, not a different format\n", path);
				return 3;
			}
			val = mib_value(partbuf, (long)got, m->key);
			goto have_val;
		}
	}
	flen = jffs2_read_file(partbuf, (uint32_t)got,
			       m->role == F_HW ? fam->hw : fam->sw);
	if (flen == -1) {
		fprintf(stderr, "rtk_factory: %s is jffs2 but '%s' could not be read "
			"FAITHFULLY (CRC, truncation or decompression) -- refusing to "
			"fall back to a blind scan, which would answer without "
			"validating anything\n", path,
			m->role == F_HW ? fam->hw : fam->sw);
		return 3;         /* a refusal, distinct from "key not present" */
	}
	val = xml_value(filebuf, flen, m->key);
have_val:
	if (!val) {
		fprintf(stderr, "rtk_factory: %s not found in %s (jffs2 route: %s; "
			"MIB-container route: no zlib stream carries it)\n",
			m->key, path,
			flen < 0 ? "file absent" : "file present, key absent");
		return 1;
	}

	print_value(m, val, "");
	return 0;
}
