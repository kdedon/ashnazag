/*
 * bdos3.c -- the CP/M 3 function set for CP/M-68K, over host files.
 *
 * Trap #2 lands in cpm_bdos3().  Files are host files (hostfs.c);
 * an FCB holds only its name and position, so nothing is lost when a
 * program copies, reuses or never closes one.  A file appears as one
 * directory entry per 128 KB (16 KB blocks, EXM 7, 16-bit block
 * numbers), the last record byte count in S1.  A drive byte of '?'
 * makes search return every entry of every user, for the caller to
 * filter, as the CCP does when it looks for a command.
 *
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include "bdos3.h"

#define	VERSION		0x2031
#define	BLKREC		128		/* records a 16 KB block */
#define	ENTREC		1024		/* records a directory entry */
#define	DSM		65535L
#define	ALVLEN		(DSM / 8 + 1)

int b3_retcode;
unsigned long b3_dma;

static int dflt, user, errmode, mltio = 1, curfx;
static unsigned rodsk, logdsk;
static unsigned long tpalo, tpahi, tpahi0, t2entry, dpb, alv;
static unsigned long curfcb;

/* search state: every match, made by search first */
static struct match {
	struct hfile	h;
	int		ent;
} *found;
static int nfound, nextfound, maxfound;

unsigned long
b3_get16(a)
	unsigned long a;
{
	unsigned char *p = (unsigned char *)B3MEM(a);

	return (unsigned long)p[0] << 8 | p[1];
}

unsigned long
b3_get32(a)
	unsigned long a;
{
	return b3_get16(a) << 16 | b3_get16(a + 2);
}

void
b3_put16(a, v)
	unsigned long a, v;
{
	unsigned char *p = (unsigned char *)B3MEM(a);

	p[0] = v >> 8;
	p[1] = v;
}

void
b3_put32(a, v)
	unsigned long a, v;
{
	b3_put16(a, v >> 16);
	b3_put16(a + 2, v);
}

#define	FB(f, i)	(((unsigned char *)B3MEM(f))[i])

/*
 * tpalo..tpahi is the TPA; area holds the DPB and allocation vector
 * and a default DMA buffer (16 + ALVLEN + 128 bytes); t2 is the BDOS's
 * trap entry.
 */
void
b3_init(t2, lo, hi, area)
	unsigned long t2, lo, hi, area;
{
	t2entry = t2;
	tpalo = lo;
	tpahi = tpahi0 = hi;
	dpb = area;
	alv = area + 16;
	b3_dma = alv + ALVLEN;
	logdsk = hf_drives();
}

/* ---- errors ---- */

static char *errmsg[] = {
	"", "Disk I/O", "Read/Only Disk", "Read/Only File", "Invalid Drive",
	"", "", "Password Error", "File Exists", "? in Filename"
};

static void
msg(s)
	char *s;
{
	while (*s)
		b3_conout3(*s++);
}

/* extended error: shown and fatal by default; the value for modes FE, FF */
static long
berr(code, drv)
	int code, drv;
{
	char b[80];
	int i;

	if (errmode != 0xff) {
		sprintf(b, "\r\nCP/M Error On %c: %s\r\nBDOS Function = %d", 'A' + drv,
		    errmsg[code], curfx);
		msg(b);
		if (curfcb) {
			msg(" File = ");
			for (i = 1; i < 12; i++) {
				if (i == 9)
					b3_conout3('.');
				b3_conout3(FB(curfcb, i) & 0x7f);
			}
		}
		msg("\r\n");
		if (errmode != 0xfe) {
			b3_retcode = RC_BDOS;
			b3_wboot();
		}
	}
	return (long)code << 8 | 0xff;
}

/* ---- FCBs ---- */

static int
fcbdrv(f)
	unsigned long f;
{
	int d = FB(f, 0);

	return d == 0 || d == '?' ? dflt : d - 1;
}

static void
fcbname(f, n)
	unsigned long f;
	char *n;
{
	int i;

	for (i = 0; i < 11; i++)
		n[i] = FB(f, i + 1) & 0x7f;
}

static int
wild(n)
	char *n;
{
	return memchr(n, '?', 11) != 0;
}

static long
recs(h)
	struct hfile *h;
{
	long r = (h->size + RECLEN - 1) / RECLEN;

	return r > MAXREC ? MAXREC : r;
}

/* records of logical extent x present in the file */
static int
extrecs(h, x)
	struct hfile *h;
	long x;
{
	long r = recs(h) - x * RECLEN;

	return r <= 0 ? 0 : r > RECLEN ? RECLEN : (int)r;
}

/* records in directory entry e */
static long
entrecs(h, e)
	struct hfile *h;
	long e;
{
	long r = recs(h) - e * ENTREC;

	return r <= 0 ? 0 : r > ENTREC ? ENTREC : r;
}

static long
fcbext(f)
	unsigned long f;
{
	return (long)(FB(f, 14) & 0x3f) * 32 + (FB(f, 12) & 0x1f);
}

static void
setext(f, h, x)
	unsigned long f;
	struct hfile *h;
	long x;
{
	FB(f, 12) = x & 0x1f;
	FB(f, 14) = x >> 5 & 0x3f;
	FB(f, 15) = extrecs(h, x);
}

/* attributes into the FCB, block map made up so it is not empty */
static void
fcbattr(f, h)
	unsigned long f;
	struct hfile *h;
{
	int i;

	FB(f, 9) = (FB(f, 9) & 0x7f) | (h->ro ? 0x80 : 0);
	FB(f, 10) = (FB(f, 10) & 0x7f) | (h->sys ? 0x80 : 0);
	FB(f, 11) = (FB(f, 11) & 0x7f) | (h->arc ? 0x80 : 0);
	for (i = 16; i < 32; i++)
		FB(f, i) = 0;
	for (i = 0; (long)i * BLKREC < entrecs(h, fcbext(f) / 8); i++)
		b3_put16(f + 16 + 2 * i, (unsigned long)i + 1);
}

/* the FCB's file; errors as the function returns them */
static long
lookup(f, h, wr)
	unsigned long f;
	struct hfile *h;
	int wr;
{
	char n[11];
	int d = fcbdrv(f);

	if (!(logdsk & 1 << d))
		return berr(4, d);
	fcbname(f, n);
	if (wild(n))
		return 0xff;
	if (hf_find(d, user, n, h) < 0)
		return 0xff;
	if (wr && (rodsk & 1 << d))
		return berr(2, d);
	if (wr && h->ro)
		return berr(3, d);
	return 0;
}

/* ---- file functions ---- */

static long
fopen3(f)
	unsigned long f;
{
	struct hfile h;
	long r;
	int lrbc = FB(f, 32) == 0xff;

	if ((r = lookup(f, &h, 0)) != 0)
		return r;
	if (hf_open(fcbdrv(f), &h, 0) < 0)
		return errno == EACCES ? berr(7, fcbdrv(f)) : 0xff;
	if (fcbext(f) * RECLEN > recs(&h))
		return 0xff;
	FB(f, 13) = h.size % RECLEN;
	setext(f, &h, fcbext(f));
	fcbattr(f, &h);
	if (lrbc)
		FB(f, 32) = h.size % RECLEN;
	return 0;
}

static long
fclose3(f)
	unsigned long f;
{
	struct hfile h;

	return lookup(f, &h, 0);
}

static long
fmake(f)
	unsigned long f;
{
	struct hfile h;
	char n[11];
	int d = fcbdrv(f);

	if (!(logdsk & 1 << d))
		return berr(4, d);
	if (rodsk & 1 << d)
		return berr(2, d);
	fcbname(f, n);
	if (wild(n))
		return berr(9, d);
	if (hf_find(d, user, n, &h) == 0)
		return berr(8, d);
	if (hf_create(d, user, n, &h) < 0)
		return errno == EACCES ? berr(7, d) : 0xff;
	if ((FB(f, 6) & 0x80) && (FB(b3_dma, 8) & (XP_READ | XP_WRITE | XP_DELETE)))
		hf_protect(d, &h, FB(b3_dma, 8));
	FB(f, 13) = 0;
	setext(f, &h, fcbext(f));
	fcbattr(f, &h);
	return 0;
}

struct del {
	char	n[11];
	int	drv, go, hits, err;
};

/* counts matches and R/O ones; with go set, removes them */
static int
delone(h, arg)
	struct hfile *h;
	char *arg;
{
	struct del *p = (struct del *)arg;
	int i;

	for (i = 0; i < 11; i++)
		if (p->n[i] != '?' && p->n[i] != h->name[i])
			return 0;
	p->hits++;
	if (h->ro)
		p->err = 3;
	else if (p->go && hf_unlink(p->drv, h) < 0 && errno != ENOENT)
		p->err = 7;
	return 0;
}

static long
fdelete(f)
	unsigned long f;
{
	struct del del;
	int d = fcbdrv(f);

	if (!(logdsk & 1 << d))
		return berr(4, d);
	if (rodsk & 1 << d)
		return berr(2, d);
	if (FB(f, 5) & 0x80)		/* f5': passwords only, which are the mode */
		return 0;
	memset((char *)&del, 0, sizeof del);
	fcbname(f, del.n);
	del.drv = d;
	hf_scan(d, user, delone, (char *)&del);
	if (del.err)			/* one R/O file: none go */
		return berr(del.err, d);
	if (del.hits == 0)
		return 0xff;
	del.go = 1;
	hf_scan(d, user, delone, (char *)&del);
	return del.err ? berr(del.err, d) : 0;
}

static long
frename(f)
	unsigned long f;
{
	struct hfile h, h2;
	char n[11];
	long r;
	int d = fcbdrv(f);

	if ((r = lookup(f, &h, 1)) != 0)
		return r;
	fcbname(f + 16, n);
	if (wild(n))
		return berr(9, d);
	if (hf_find(d, user, n, &h2) == 0)
		return berr(8, d);
	if (hf_rename(d, &h, n) < 0)
		return berr(7, d);
	return 0;
}

static long
fattr(f)
	unsigned long f;
{
	struct hfile h;
	long r, len;
	int d = fcbdrv(f), lrbc;

	if ((r = lookup(f, &h, 0)) != 0)
		return r;
	if (rodsk & 1 << d)
		return berr(2, d);
	if (FB(f, 6) & 0x80) {		/* f6': last record byte count in cr */
		lrbc = FB(f, 32);
		len = recs(&h) * RECLEN;
		if (lrbc && len && lrbc < RECLEN && hf_truncate(d, &h, len - RECLEN + lrbc) < 0)
			return berr(3, d);
	}
	if (hf_setattr(d, &h, FB(f, 9) >> 7, FB(f, 10) >> 7, FB(f, 11) >> 7) < 0)
		return berr(3, d);
	return 0;
}

/* one record at rec; 0, 1 past the end, 4 past the extent, or an error */
static long
rw1(f, h, rec, wr, dma)
	unsigned long f, dma;
	struct hfile *h;
	long rec;
	int wr;
{
	char buf[RECLEN];
	long n, off = rec * RECLEN;
	int fd, d = fcbdrv(f);

	if (rec >= MAXREC)
		return 6;
	if ((fd = hf_open(d, h, wr)) < 0)
		return errno == EACCES ? berr(wr ? 3 : 7, d) : 1;
	if (wr) {
		if (lseek(fd, off, 0) != off || write(fd, B3MEM(dma), RECLEN) != RECLEN)
			return 2;		/* disk full */
		if (off + RECLEN > h->size)
			h->size = off + RECLEN;
		return 0;
	}
	if (off >= h->size)
		return rec / RECLEN == (recs(h) - 1) / RECLEN && recs(h) ? 1 : 4;
	if (lseek(fd, off, 0) != off || (n = read(fd, buf, RECLEN)) <= 0)
		return 1;
	if (n < RECLEN)
		memset(buf + n, 0x1a, RECLEN - n);
	memcpy(B3MEM(dma), buf, RECLEN);
	return 0;
}

/* functions 20, 21: sequential, mltio records */
static long
seqrw(f, wr)
	unsigned long f;
	int wr;
{
	struct hfile h;
	long r, x, rec;
	unsigned long dma = b3_dma;
	int i, cr;

	if ((r = lookup(f, &h, wr)) != 0)
		return r;
	for (i = 0; i < mltio; i++, dma += RECLEN) {
		x = fcbext(f);
		cr = FB(f, 32) & 0xff;
		if (cr > RECLEN)
			cr = RECLEN;
		rec = x * RECLEN + cr;
		if ((r = rw1(f, &h, rec, wr, dma)) != 0)
			return r == 4 ? 1 : r;
		if (cr == RECLEN) {
			setext(f, &h, x + 1);
			cr = 0;
		}
		FB(f, 32) = cr + 1;
		FB(f, 15) = extrecs(&h, fcbext(f));
	}
	return 0;
}

static long
ranrec(f)
	unsigned long f;
{
	return FB(f, 33) | (long)FB(f, 34) << 8 | (long)FB(f, 35) << 16;
}

static void
setran(f, r)
	unsigned long f;
	long r;
{
	FB(f, 33) = r;
	FB(f, 34) = r >> 8;
	FB(f, 35) = r >> 16;
}

/* functions 33, 34, 40: random; the position follows, cr not advanced */
static long
ranrw(f, wr)
	unsigned long f;
	int wr;
{
	struct hfile h;
	long r, rec = ranrec(f);
	unsigned long dma = b3_dma;
	int i;

	if (rec >= MAXREC)
		return 6;
	if ((r = lookup(f, &h, wr)) != 0)
		return r;
	for (i = 0; i < mltio; i++, dma += RECLEN) {
		setext(f, &h, (rec + i) / RECLEN);
		FB(f, 32) = (rec + i) % RECLEN;
		if ((r = rw1(f, &h, rec + i, wr, dma)) != 0)
			return r;
	}
	FB(f, 15) = extrecs(&h, fcbext(f));
	return 0;
}

static long
fsize(f)
	unsigned long f;
{
	struct hfile h;
	long r;

	if ((r = lookup(f, &h, 0)) != 0)
		return r;
	setran(f, recs(&h));
	return 0;
}

static long
ftrunc(f)
	unsigned long f;
{
	struct hfile h;
	long r, rec = ranrec(f);

	if ((r = lookup(f, &h, 1)) != 0)
		return r;
	if (rec >= recs(&h))
		return 0xff;
	if (hf_truncate(fcbdrv(f), &h, (rec + 1) * RECLEN) < 0)
		return berr(3, fcbdrv(f));
	return 0;
}

static long
stamps(f)
	unsigned long f;
{
	struct hfile h;
	long r;

	if ((r = lookup(f, &h, 0)) != 0)
		return r;
	FB(f, 12) = h.pwmode;
	hf_stamp(h.atime, (unsigned char *)B3MEM(f + 24));
	hf_stamp(h.mtime, (unsigned char *)B3MEM(f + 28));
	return 0;
}

static long
wrxfcb(f)
	unsigned long f;
{
	struct hfile h;
	long r;

	if ((r = lookup(f, &h, 0)) != 0)
		return r;
	if (hf_protect(fcbdrv(f), &h, FB(f, 12)) < 0)
		return berr(7, fcbdrv(f));
	return 0;
}

/* ---- search ---- */

static int
addmatch(h, arg)
	struct hfile *h;
	char *arg;
{
	unsigned long f = *(unsigned long *)arg;
	long nent = (recs(h) + ENTREC - 1) / ENTREC, e;
	int i, c, all = FB(f, 0) == '?';

	for (i = 0; i < 11 && !all; i++)
		if ((c = FB(f, i + 1) & 0x7f) != '?' && c != h->name[i])
			return 0;
	if (nent == 0)
		nent = 1;
	for (e = 0; e < nent; e++) {
		if (!all && FB(f, 12) != '?' && fcbext(f) / 8 != e)
			continue;
		if (nfound == maxfound) {
			maxfound = maxfound ? 2 * maxfound : 64;
			if ((found = realloc(found, maxfound * sizeof *found)) == 0) {
				maxfound = nfound = 0;
				return 1;
			}
		}
		found[nfound].h = *h;
		found[nfound++].ent = e;
	}
	return 0;
}

/* the next match as a directory record: the entry, two empty ones, its SFCB */
static long
searchnext()
{
	unsigned char *p = (unsigned char *)B3MEM(b3_dma);
	struct match *m;
	struct hfile *h;
	long r, er, lx;
	int i;

	if (nextfound >= nfound)
		return 0xff;
	m = &found[nextfound++];
	h = &m->h;
	r = recs(h);
	er = r - (long)m->ent * ENTREC;
	if (er > ENTREC)
		er = ENTREC;
	lx = (long)m->ent * 8 + (er ? (er - 1) / RECLEN : 0);
	memset(p, 0, RECLEN);
	memset(p + 32, 0xe5, 64);
	p[0] = h->user;
	memcpy(p + 1, h->name, 11);
	p[9] |= h->ro ? 0x80 : 0;
	p[10] |= h->sys ? 0x80 : 0;
	p[11] |= h->arc ? 0x80 : 0;
	p[12] = lx & 0x1f;
	p[13] = (r - 1) / ENTREC == m->ent ? h->size % RECLEN : 0;
	p[14] = lx >> 5;
	p[15] = er ? er - (lx - (long)m->ent * 8) * RECLEN : 0;
	for (i = 0; (long)i * BLKREC < er; i++) {
		p[16 + 2 * i] = (i + 1) >> 8;
		p[17 + 2 * i] = i + 1;
	}
	p[96] = 0x21;
	hf_stamp(h->atime, p + 97);
	hf_stamp(h->mtime, p + 101);
	p[105] = h->pwmode;
	return 0;
}

static long
search(f)
	unsigned long f;
{
	unsigned long ff = f;
	int d = fcbdrv(f), u;

	nfound = nextfound = 0;
	if (!(logdsk & 1 << d))
		return berr(4, d);
	if (FB(f, 0) == '?') {
		for (u = 0; u < NUSER; u++)
			hf_scan(d, u, addmatch, (char *)&ff);
	} else
		hf_scan(d, user, addmatch, (char *)&ff);
	return searchnext();
}

/* ---- disk parameters ---- */

static unsigned long
dparams(d)
	int d;
{
	long free = hf_free(d) / BLKREC, used, i;

	b3_put16(dpb, 128L);
	FB(dpb, 2) = 7;
	FB(dpb, 3) = BLKREC - 1;
	FB(dpb, 4) = 7;
	FB(dpb, 5) = 0;
	b3_put16(dpb + 6, DSM);
	b3_put16(dpb + 8, 1023L);
	b3_put16(dpb + 10, 0xc000L);
	b3_put16(dpb + 12, 0x8000L);
	b3_put16(dpb + 14, 0L);
	used = DSM + 1 - (free > DSM - 1 ? DSM - 1 : free);
	memset(B3MEM(alv), 0, ALVLEN);
	for (i = 0; i < used / 8; i++)
		FB(alv, i) = 0xff;
	for (i = used & ~7L; i < used; i++)
		FB(alv, i / 8) |= 0x80 >> (i & 7);
	return dpb;
}

/* ---- program load (function 59) ---- */

static long
pgmld(lpb)
	unsigned long lpb;
{
	unsigned long f = b3_get32(lpb), lo = b3_get32(lpb + 4), hi = b3_get32(lpb + 8);
	unsigned long ts, ds, bs, ss, tst, dst, bst, tl, dl, bl, bp, img, w, v, delta;
	unsigned char *b;
	struct hfile h;
	long n, hl, i, nw;
	int fd, rel, t;

	if (lookup(f, &h, 0) != 0 || (fd = hf_open(fcbdrv(f), &h, 0)) < 0)
		return 2;
	if (hi > tpahi || lo < tpalo || hi <= lo + 256 || h.size < 28 || (b = malloc(h.size)) == 0)
		return 1;
	if (lseek(fd, 0L, 0) != 0 || read(fd, (char *)b, h.size) != h.size) {
		free(b);
		return 2;
	}
	w = b[0] << 8 | b[1];
	hl = w == 0x601b ? 36 : 28;
#define	L(o)	((unsigned long)b[o] << 24 | (unsigned long)b[(o)+1] << 16 | b[(o)+2] << 8 | b[(o)+3])
	ts = L(2), ds = L(6), bs = L(10), ss = L(14), tst = L(22);
	rel = (b[26] << 8 | b[27]) == 0;
	dst = w == 0x601b ? L(28) : tst + ts;
	bst = w == 0x601b ? L(32) : dst + ds;
	if ((w != 0x601a && w != 0x601b) || hl + ts + ds + ss > h.size ||
	    (rel && hl + ts + ds + ss + ts + ds > h.size)) {
		free(b);
		return 1;
	}
	bp = lo;
	if (rel) {
		tl = lo + 256;
		dl = tl + ts;
		bl = dl + ds;
	} else {
		tl = tst, dl = dst, bl = bst;
	}
	if (tl < lo + 256 || tl + ts > hi || dl < lo + 256 || dl + ds > hi || bl < lo + 256 || bl + bs > hi - 64) {
		free(b);
		return 1;
	}
	memcpy(B3MEM(tl), b + hl, ts);
	memcpy(B3MEM(dl), b + hl + ts, ds);
	memset(B3MEM(bl), 0, bs);
	if (rel) {
		img = hl + ts + ds + ss;	/* relocation words */
		nw = (ts + ds) / 2;
		for (i = 0; i < nw; i++) {
			t = b[img + 2 * i + 1] & 7;
			v = i * 2 < (long)ts ? tl + i * 2 : dl + i * 2 - ts;
			if (t == 5 && i + 1 < nw) {
				t = b[img + 2 * i + 3] & 7;
				delta = t == 1 ? dl - dst : t == 2 ? tl - tst : t == 3 ? bl - bst : 0;
				if (t == 4) {
					free(b);
					return 3;
				}
				b3_put32(v, b3_get32(v) + delta);
				i++;
			} else if (t >= 1 && t <= 3) {
				delta = t == 1 ? dl - dst : t == 2 ? tl - tst : bl - bst;
				b3_put16(v, b3_get16(v) + delta);
			} else if (t == 4) {
				free(b);
				return 3;
			}
		}
	}
	free(b);
	memset(B3MEM(bp), 0, 0x38);
	b3_put32(bp, lo);
	b3_put32(bp + 4, hi);
	b3_put32(bp + 8, tl);
	b3_put32(bp + 12, ts);
	b3_put32(bp + 16, dl);
	b3_put32(bp + 20, ds);
	b3_put32(bp + 24, bl);
	b3_put32(bp + 28, bs);
	b3_put32(bp + 32, hi - (bl + bs));
	FB(bp, 36) = fcbdrv(f);
	n = (hi - 4) & ~1L;
	b3_put32((unsigned long)n, bp);
	b3_put32(lpb + 12, bp);
	b3_put32(lpb + 16, (unsigned long)n);
	return 0;
}

/* ---- resident system extensions (function 60) ----
 * A module's header: 6 serial, entry.l @6, 'RS' @10, next.l @12,
 * prev.l @16, warm-boot removal flag @20, name @22, len.l @38.  Trap #2
 * enters the newest module; a module passes a call on by jumping to
 * its next, which for the oldest is the BDOS.
 */

#define	NRSX	16
static unsigned long rsx[NRSX];		/* modules, oldest first */
static int nrsx;

static void
rsxlink()
{
	b3_put32(34 * 4L, nrsx ? b3_get32(rsx[nrsx - 1] + 6) : t2entry);
	tpahi = nrsx ? rsx[nrsx - 1] : tpahi0;
}

static long
rsxfn(pb)
	unsigned long pb;
{
	unsigned long org, len, src;

	switch (FB(pb, 0)) {
	case 126:
		return nrsx ? rsx[nrsx - 1] : 0;
	case 127:
		org = b3_get32(pb + 2);
		len = b3_get32(pb + 6);
		src = b3_get32(pb + 10);
		if (nrsx == NRSX || len < 42 || org & 1 || org < tpalo + 256 || org + len > tpahi)
			return 0xf2;
		if (b3_get16(src + 10) != 0x5253)
			return 0xf3;
		memmove(B3MEM(org), B3MEM(src), len);
		b3_put32(org + 12, nrsx ? b3_get32(rsx[nrsx - 1] + 6) : t2entry);
		b3_put32(org + 16, 0L);
		if (nrsx)
			b3_put32(rsx[nrsx - 1] + 16, org);
		rsx[nrsx++] = org;
		rsxlink();
		return 0;
	}
	return 0xff;
}

/* warm boot drops flagged modules from the top of the chain */
static void
rsxwarm()
{
	while (nrsx && FB(rsx[nrsx - 1], 20) == 0xff)
		nrsx--;
	if (nrsx)
		b3_put32(rsx[nrsx - 1] + 16, 0L);
	rsxlink();
}

/* ---- system control block (function 49) ---- */

static unsigned char scb[100];

static long
scbfn(pb)
	unsigned long pb;
{
	time_t t = time((time_t *)0);
	struct tm *tm;
	int off = FB(pb, 0), set = FB(pb, 1);

	if (off >= 99)
		return 0xffff;
	scb[0x05] = 0x31;
	scb[0x10] = b3_retcode;
	scb[0x11] = b3_retcode >> 8;
	scb[0x1a] = b3_conwidth - 1;
	scb[0x1b] = b3_column;
	scb[0x1c] = b3_conpage;
	scb[0x1d] = b3_conline;
	scb[0x2c] = b3_pagemode;
	scb[0x33] = b3_conmode;
	scb[0x34] = b3_conmode >> 8;
	scb[0x37] = b3_delim;
	scb[0x3c] = b3_dma;
	scb[0x3d] = b3_dma >> 8;
	scb[0x3e] = dflt;
	scb[0x44] = user;
	scb[0x4a] = mltio;
	scb[0x4b] = errmode;
	scb[0x57] = 0x80;
	hf_stamp((long)t, scb + 0x58);
	tm = localtime(&t);
	scb[0x5c] = (tm->tm_sec / 10) << 4 | tm->tm_sec % 10;
	if (set != 0xff && set != 0xfe)
		return scb[off] | scb[off + 1] << 8;
	scb[off] = FB(pb, 2);
	if (set == 0xfe)
		scb[off + 1] = FB(pb, 3);
	b3_retcode = scb[0x10] | scb[0x11] << 8;
	b3_conwidth = scb[0x1a] + 1;
	b3_column = scb[0x1b];
	b3_conpage = scb[0x1c];
	b3_conline = scb[0x1d];
	b3_pagemode = scb[0x2c];
	b3_conmode = scb[0x33] | scb[0x34] << 8;
	b3_delim = scb[0x37];
	if ((logdsk & 1 << (scb[0x3e] & 15)))
		dflt = scb[0x3e] & 15;
	user = scb[0x44] & 15;
	mltio = scb[0x4a] ? scb[0x4a] : 1;
	errmode = scb[0x4b];
	return 0;
}

/* ---- function 152 ---- */

static int
pdelim(c)
	int c;
{
	return c == 0 || strchr("\r\t .,:;[]=<>|", c) != 0;
}

static int
pupper(c)
	int c;
{
	return c < 'a' ? c : c <= 'z' ? c & 0x5f : c & 0x7f;
}

/* one field from *pos into fcb[at..at+room); 0 stops it */
static int
pfield(pos, fcb, at, room, stars, bad)
	unsigned long *pos;
	unsigned char *fcb;
	int at, room, stars, *bad;
{
	int c, n = 0;

	for (;;) {
		c = FB(*pos, 0);
		if (pdelim(c))
			return n;
		c = pupper(c);
		(*pos)++;
		if (c < ' ' || n == room) {
			*bad = 1;
			return n;
		}
		if (stars && c == '*') {
			while (n < room)
				fcb[at + n++] = '?';
			continue;
		}
		fcb[at + n++] = c;
	}
}

static long
parsefn(pb)
	unsigned long pb;
{
	unsigned char fcb[36];
	unsigned long pos = b3_get32(pb), q;
	int c, bad = 0;

	memset(fcb, 0, sizeof fcb);
	memset(fcb + 1, ' ', 11);
	memset(fcb + 16, ' ', 8);
	while ((c = FB(pos, 0)) == ' ' || c == '\t')
		pos++;
	if (c && FB(pos, 1) == ':') {
		if (pdelim(c))
			goto done;
		c = pupper(c);
		if (c < 'A' || c > 'P') {
			bad = 1;
			goto done;
		}
		fcb[0] = c - 'A' + 1;
		pos += 2;
	}
	if (pdelim(FB(pos, 0)))
		goto done;
	pfield(&pos, fcb, 1, 8, 1, &bad);
	if (!bad && FB(pos, 0) == '.') {
		pos++;
		pfield(&pos, fcb, 9, 3, 1, &bad);
	}
	if (!bad && FB(pos, 0) == ';') {
		pos++;
		fcb[26] = pfield(&pos, fcb, 16, 8, 0, &bad);
	}
done:
	memcpy(B3MEM(b3_get32(pb + 4)), fcb, sizeof fcb);
	if (bad)
		return 0xffff;
	for (q = pos; (c = FB(q, 0)) == ' ' || c == '\t'; q++)
		;
	if (pdelim(c))
		return c == 0 || c == '\r' ? 0 : (long)q;
	return (long)pos;
}

/* ---- the dispatcher ---- */

static void
warm()
{
	hf_flush();
	rsxwarm();
	b3_wboot();
}

/* d1 the parameter; regs the caller's saved d1-d7/a0-a6, then its frame */
long
cpm_bdos3(fn, d1, regs)
	long fn;
	unsigned long d1, regs;
{
	unsigned long a;
	time_t t;
	struct tm *tm;
	long r;
	int d, i;

	fn &= 0xffff;
	curfx = fn;
	curfcb = 0;
	switch (fn) {
	case 0:
		warm();
	case 1:
		return b3_conin3();
	case 2:
		b3_cookdout((int)d1, 0);
		return 0;
	case 3:
		return b3_bios(7L, 0L, 0L) & 0xff;
	case 4:
		b3_bios(6L, d1 & 0xff, 0L);
		return 0;
	case 5:
		b3_list((int)(d1 & 0xff));
		return 0;
	case 6:
		return b3_rawio((int)d1);
	case 7:
		return b3_bios(19L, 0L, 0L) & 0xff;
	case 8:
		b3_bios(20L, d1 & 0xff, 0L);
		return 0;
	case 9:
		b3_prtline(d1);
		return 0;
	case 10:
		b3_readline(d1 ? d1 : b3_dma);
		return 0;
	case 11:
		return b3_constat() ? 0xff : 0;
	case 12:
		return VERSION;
	case 13:
		hf_flush();
		logdsk = hf_drives();
		rodsk = 0;
		dflt = 0;
		return 0;
	case 14:
		d = d1 & 0xff;
		if (d >= NDRV || !(logdsk & 1 << d))
			return berr(4, d < NDRV ? d : 0);
		dflt = d;
		return 0;
	case 25:
		return dflt;
	case 26:
		b3_dma = d1;
		return 0;
	case 24:
		return logdsk;
	case 27:
		dparams(dflt);
		return alv;
	case 28:
		rodsk |= 1 << dflt;
		return 0;
	case 29:
		return rodsk;
	case 31:
		return dparams(dflt);
	case 32:
		if ((d1 & 0xff) == 0xff)
			return user;
		user = d1 & 15;
		return 0;
	case 37:
		rodsk &= ~d1;
		return 0;
	case 38:
	case 39:
	case 42:
	case 43:
	case 48:
	case 98:
		hf_flush();
		return 0;
	case 41:
		return 0xff;
	case 44:
		if ((d1 & 0xff) == 0 || (d1 & 0xff) > 128)
			return 0xff;
		mltio = d1 & 0xff;
		return 0;
	case 45:
		errmode = d1 & 0xff;
		return 0;
	case 46:
		d = d1 & 0xff;
		if (d >= NDRV || !(logdsk & 1 << d))
			return berr(4, d < NDRV ? d : 0);
		b3_put32(b3_dma, (unsigned long)hf_free(d));
		return 0;
	case 47:
		b3_chain();
		warm();
	case 49:
		return scbfn(d1);
	case 50:
		if (b3_get16(d1) == 1)
			warm();
		return b3_bios((long)b3_get16(d1), b3_get32(d1 + 2), b3_get32(d1 + 6));
	case 59:
		return pgmld(d1);
	case 60:
		return rsxfn(d1);
	case 61:
		i = b3_get16(d1);
		if (i > 255)
			return 0xff;
		a = b3_get32(i * 4L);
		if (i != 34 && i != 35)
			b3_put32(i * 4L, b3_get32(d1 + 2));
		b3_put32(d1 + 6, a);
		return 0;
	case 62:
		b3_put16(regs + 56, b3_get16(regs + 56) | 0x2000);
		return 0;
	case 63:
		if (b3_get16(d1) & 1) {
			a = b3_get32(d1 + 6);
			if (a > tpahi0 || b3_get32(d1 + 2) < tpalo)
				return 0xff;
			tpahi = a;
		}
		b3_put32(d1 + 2, tpalo);
		b3_put32(d1 + 6, tpahi);
		return 0;
	case 100:
		return 0;		/* the label is the directory's name */
	case 101:
		return 0xe1;		/* label, access and update stamps, protection */
	case 104:
		return 0;		/* the host's clock is not ours to set */
	case 105:
		t = time((time_t *)0);
		hf_stamp((long)t, (unsigned char *)B3MEM(d1));
		tm = localtime(&t);
		return (tm->tm_sec / 10) << 4 | tm->tm_sec % 10;
	case 106:
		return 0;
	case 107:
		memcpy(B3MEM(d1), "\0\0\0\0\0\1", 6);
		return 0;
	case 108:
		if ((d1 & 0xffff) == 0xffff)
			return b3_retcode;
		b3_retcode = d1 & 0xffff;
		return 0;
	case 109:
		if ((d1 & 0xffff) == 0xffff)
			return b3_conmode;
		b3_conmode = d1 & 0xffff;
		return 0;
	case 110:
		if ((d1 & 0xffff) == 0xffff)
			return b3_delim;
		b3_delim = d1 & 0xff;
		return 0;
	case 111:
	case 112:
		b3_prtblk(d1, fn == 111);
		return 0;
	case 152:
		return parsefn(d1);
	}
	curfcb = d1;
	switch (fn) {
	case 15:
		return fopen3(d1);
	case 16:
		return fclose3(d1);
	case 17:
		return search(d1);
	case 18:
		return searchnext();
	case 19:
		return fdelete(d1);
	case 20:
		return seqrw(d1, 0);
	case 21:
		return seqrw(d1, 1);
	case 22:
		return fmake(d1);
	case 23:
		return frename(d1);
	case 30:
		return fattr(d1);
	case 33:
		return ranrw(d1, 0);
	case 34:
	case 40:
		return ranrw(d1, 1);
	case 35:
		return fsize(d1);
	case 36:
		r = fcbext(d1) * RECLEN + (FB(d1, 32) & 0xff);
		setran(d1, r);
		return 0;
	case 99:
		return ftrunc(d1);
	case 102:
		return stamps(d1);
	case 103:
		return wrxfcb(d1);
	}
	return 0xffff;
}
