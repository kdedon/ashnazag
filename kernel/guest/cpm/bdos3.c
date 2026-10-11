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
#include <poll.h>
#include "bdos3.h"

#define	VERSION		0x2031
#define	BLKREC		128		/* records a 16 KB block */
#define	ENTREC		1024		/* records a directory entry */
#define	DSM		65535L
#define	ALVLEN		(DSM / 8 + 1)

int b3_retcode;
unsigned long b3_dma, b3_dma0;

static int dflt, user, errmode, mltio = 1, curfx;
static unsigned rodsk, logdsk;
static int sysdrv = -1;
static unsigned long tpalo, tpahi, tpahi0, t2entry, dpb, alv;
static unsigned long curfcb;

/* search state: every match, made by search first */
static struct match {
	struct hfile	h;
	int		ent, drv;
} *found;
static int scandrv;		/* the drive addmatch() is given files of */
static int nfound, nextfound, maxfound;

/* made-up block numbers: a file keeps one run of its own until a disk reset */
static struct blkrun {
	char	key[13];	/* drive, user, name */
	long	base, n;
} *blkruns;
static int nblkruns, maxblkruns;
static long blktop = 2;		/* 0 and 1 hold the directory */

static long
blkbase(h, d, n)
	struct hfile *h;
	int d;
	long n;
{
	struct blkrun *b;
	char key[13];
	int i;

	key[0] = d;
	key[1] = h->user;
	memcpy(key + 2, h->name, 11);
	for (i = 0; i < nblkruns; i++)
		if (memcmp(blkruns[i].key, key, 13) == 0 && blkruns[i].n >= n)
			return blkruns[i].base;
	if (n < 1)
		n = 1;
	if (blktop + n > DSM) {
		nblkruns = 0;
		blktop = 2;
	}
	if (nblkruns == maxblkruns) {
		if ((b = realloc(blkruns, (maxblkruns + 256) * sizeof *blkruns)) == 0)
			return blktop;
		blkruns = b;
		maxblkruns += 256;
	}
	b = &blkruns[nblkruns++];
	memcpy(b->key, key, 13);
	b->base = blktop;
	b->n = n;
	blktop += n;
	return b->base;
}

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
	b3_relog();
}

/* drives as the drive table has them now */
void
b3_relog()
{
	int d, fl;

	hf_flush();
	logdsk = hf_drives();
	rodsk = hf_rodrives();
	nblkruns = 0;
	blktop = 2;
	sysdrv = -1;
	for (d = 0; d < NDRV; d++)
		if (*hf_drive(d, &fl) && (fl & HF_SYS) && (logdsk & 1 << d))
			sysdrv = d;
}

/* a program or submit file, looked for on the system drive too */
static int
systype(n)
	char *n;
{
	return memcmp(n + 8, "68K", 3) == 0 || memcmp(n + 8, "SUB", 3) == 0 || shimtype(n);
}

/* a CP/M-80 or CP/M-86 program, which runs as a 68K one through the shim */
int
shimtype(n)
	char *n;
{
	return memcmp(n + 8, "COM", 3) == 0 ? SHIM_80 : memcmp(n + 8, "CMD", 3) == 0 ? SHIM_86 : 0;
}

/* X.68K asked for, X.COM or X.CMD there */
static int
shimfind(d, u, n, h)
	int d, u;
	char *n;
	struct hfile *h;
{
	char m[11];

	if (memcmp(n + 8, "68K", 3))
		return -1;
	memcpy(m, n, 8);
	memcpy(m + 8, "COM", 3);
	if (hf_find(d, u, m, h) == 0)
		return 0;
	memcpy(m + 8, "CMD", 3);
	return hf_find(d, u, m, h);
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
		return berr(9, d);
	if ((FB(f, 8) & 0x80) && user) {
		/* opened through the user-0 fallback: read-only */
		if (hf_find(d, 0, n, h) < 0 || !h->sys)
			return 0xff;
		return wr ? berr(3, d) : 0;
	}
	if (hf_find(d, user, n, h) < 0) {
		if (wr)
			return 0xff;
		if (user && hf_find(d, 0, n, h) == 0 && h->sys) {
			FB(f, 8) |= 0x80;
			return 0;
		}
		if (shimfind(d, user, n, h) == 0)
			return 0;
		if (sysdrv < 0 || d == sysdrv || !systype(n) ||
		    (hf_find(sysdrv, 0, n, h) < 0 && shimfind(sysdrv, 0, n, h) < 0))
			return 0xff;
		FB(f, 0) = sysdrv + 1;	/* later calls go straight there */
		return 0;
	}
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

	FB(f, 8) &= 0x7f;
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
	long r;

	if ((r = lookup(f, &h, 0)) == 0 && *b3_nlocks)
		b3_hlock(fcbdrv(f), &h, 0L, 0L, LK_CLOSE);
	return r;
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
		if (*b3_nlocks && b3_hlock(d, h, rec, 1L, LK_WRITE) == 8)
			return 8;
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
		if ((r = rw1(f, &h, rec, wr, dma)) != 0) {
			r = r == 4 ? 1 : r;
			return r < 0x100 ? r | (long)i << 8 : r;	/* records done in the high byte */
		}
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
	return (long)FB(f, 33) << 16 | (long)FB(f, 34) << 8 | FB(f, 35);
}

static void
setran(f, r)
	unsigned long f;
	long r;
{
	FB(f, 33) = r >> 16;
	FB(f, 34) = r >> 8;
	FB(f, 35) = r;
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
			return r < 0x100 ? r | (long)i << 8 : r;
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
	char n[11];
	long r;

	fcbname(f, n);
	if (wild(n))
		return berr(9, fcbdrv(f));
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

/*
 * functions 42, 43: mltio records from the random record, which must
 * have been written; 8 when another process holds one.
 */
static long
reclock(f, lock)
	unsigned long f;
	int lock;
{
	struct hfile h;
	long r, rec = ranrec(f);
	int d = fcbdrv(f);

	if ((r = lookup(f, &h, 0)) != 0)
		return r;
	if (rec + mltio > recs(&h))
		return 1;
	return b3_hlock(d, &h, rec, (long)mltio, lock ? LK_LOCK : LK_UNLOCK);
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

	/* the CCP looks a command up with a blank type and takes only .68K */
	if (all && shimtype(h->name) && memcmp(B3MEM(f + 9), "   ", 3) == 0) {
		for (i = 0; i < 8 && (FB(f, i + 1) & 0x7f) == h->name[i]; i++)
			;
		if (i == 8)
			memcpy(h->name + 8, "68K", 3);
	}
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
		found[nfound].drv = scandrv;
		found[nfound++].ent = e;
	}
	return 0;
}

/* the CCP's command lookup: system drive programs, shown in the current user */
static int
addsys(h, arg)
	struct hfile *h;
	char *arg;
{
	unsigned long f = *(unsigned long *)arg;
	int i, c;

	if (!systype(h->name))
		return 0;
	for (i = 0; i < 8; i++)
		if ((c = FB(f, i + 1) & 0x7f) != '?' && c != h->name[i])
			return 0;
	h->user = user;
	return addmatch(h, arg);
}

/* the next match as a directory record: the entry, two empty ones, its SFCB */
static long
searchnext()
{
	unsigned char *p = (unsigned char *)B3MEM(b3_dma);
	struct match *m;
	struct hfile *h;
	long r, er, lx, b;
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
	b = blkbase(h, m->drv, (r + BLKREC - 1) / BLKREC) + (long)m->ent * 8;
	for (i = 0; (long)i * BLKREC < er; i++) {
		p[16 + 2 * i] = (b + i) >> 8;
		p[17 + 2 * i] = b + i;
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
	scandrv = d;
	if (FB(f, 0) == '?') {
		for (u = 0; u < NUSER; u++)
			hf_scan(d, u, addmatch, (char *)&ff);
		scandrv = sysdrv;
		if (sysdrv >= 0 && d != sysdrv)
			hf_scan(sysdrv, 0, addsys, (char *)&ff);
	} else
		hf_scan(d, user, addmatch, (char *)&ff);
	return searchnext();
}

/* ---- disk parameters ---- */

/* every file's run, so the allocation vector below covers them */
static int
addrun(h, arg)
	struct hfile *h;
	char *arg;
{
	blkbase(h, *(int *)arg, (recs(h) + BLKREC - 1) / BLKREC);
	return 0;
}

static unsigned long
dparams(d)
	int d;
{
	long free = hf_free(d) / BLKREC, used, i;
	int u;

	for (u = 0; u < NUSER; u++)
		hf_scan(d, u, addrun, (char *)&d);

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
	if (used < blktop)
		used = blktop;
	memset(B3MEM(alv), 0, ALVLEN);
	for (i = 0; i < used / 8; i++)
		FB(alv, i) = 0xff;
	for (i = used & ~7L; i < used; i++)
		FB(alv, i / 8) |= 0x80 >> (i & 7);
	return dpb;
}

/* ---- program load (function 59) ---- */

/* move.l 4(sp),d1; move.w #B3_SHIM,d0; trap #3; moveq #0,d0; trap #2 */
static unsigned char shimprg[] = {
	0x60, 0x1a, 0, 0, 0, 14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0x22, 0x2f, 0, 4, 0x30, 0x3c, 0, B3_SHIM, 0x4e, 0x43, 0x70, 0, 0x4e, 0x42,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0	/* no relocations */
};
int b3_shim, b3_shimdrv;
struct hfile b3_shimh;

/*
 * Relocate text (ts bytes at tl) and data (ds at dl) by the relocation
 * words r: dt, dd and db are how far text, data and bss moved.
 */
static int
reloc(r, ts, ds, dt, dd, db, tl, dl)
	unsigned char *r;
	unsigned long ts, ds, dt, dd, db, tl, dl;
{
	unsigned long v, delta;
	long i, nw = (ts + ds) / 2;
	int t;

	for (i = 0; i < nw; i++) {
		t = r[2 * i + 1] & 7;
		v = i * 2 < (long)ts ? tl + i * 2 : dl + i * 2 - ts;
		if (t == 5 && i + 1 < nw) {
			t = r[2 * i + 3] & 7;
			if (t == 4)
				return 3;
			delta = t == 1 ? dd : t == 2 ? dt : t == 3 ? db : 0;
			b3_put32(v, b3_get32(v) + delta);
			i++;
		} else if (t >= 1 && t <= 3) {
			delta = t == 1 ? dd : t == 2 ? dt : db;
			b3_put16(v, b3_get16(v) + delta);
		} else if (t == 4)
			return 3;
	}
	return 0;
}

/*
 * An image of size bytes at b, loaded with its base page at lo and its
 * stack below hi; drv goes in the base page.  0, or 1 (no room or not a
 * program) or 3 (bad relocation).
 */
long
b3_ldimg(b, size, lo, hi, drv, bpp, spp)
	unsigned char *b;
	long size;
	unsigned long lo, hi, *bpp, *spp;
	int drv;
{
	unsigned long ts, ds, bs, ss, tst, dst, bst, tl, dl, bl, bp, w;
	long n, hl;
	int rel;

	if (lo == tpalo && hi > tpahi)
		hi = tpahi;
	if (size < 28 || hi <= lo + 256)
		return 1;
	w = b[0] << 8 | b[1];
	hl = w == 0x601b ? 36 : 28;
#define	L(o)	((unsigned long)b[o] << 24 | (unsigned long)b[(o)+1] << 16 | b[(o)+2] << 8 | b[(o)+3])
	ts = L(2), ds = L(6), bs = L(10), ss = L(14), tst = L(22);
	rel = (b[26] << 8 | b[27]) == 0;
	dst = w == 0x601b ? L(28) : tst + ts;
	bst = w == 0x601b ? L(32) : dst + ds;
	if ((w != 0x601a && w != 0x601b) || hl + ts + ds + ss > size ||
	    (rel && hl + ts + ds + ss + ts + ds > size))
		return 1;
	bp = lo;
	if (rel) {
		tl = lo + 256;
		dl = tl + ts;
		bl = dl + ds;
	} else {
		tl = tst, dl = dst, bl = bst;
	}
	if (tl < lo + 256 || tl + ts > hi || dl < lo + 256 || dl + ds > hi || bl < lo + 256 || bl + bs > hi - 64)
		return 1;
	memcpy(B3MEM(tl), b + hl, ts);
	memcpy(B3MEM(dl), b + hl + ts, ds);
	memset(B3MEM(bl), 0, bs);
	if (rel && reloc(b + hl + ts + ds + ss, ts, ds, tl - tst, dl - dst, bl - bst, tl, dl) != 0)
		return 3;
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
	FB(bp, 36) = drv;
	n = (hi - 4) & ~1L;
	b3_put32((unsigned long)n, bp);
	*bpp = bp;
	*spp = n;
	return 0;
}

static long rsxload();
static int rsxhave();

/*
 * GENCOM's file: a 256-byte header (0xEE05, the module count, the
 * program's record), 16-byte descriptors from offset 16 (record.w,
 * length.l, 0, 0, name[8]), the modules, then the program.  The
 * modules attach first, the first one highest, unless the program
 * goes to another process's memory.
 */
static long
gcload(b, size, lo, hi, drv, bpp, spp)
	unsigned char *b;
	long size;
	unsigned long lo, hi, *bpp, *spp;
	int drv;
{
	unsigned char *d;
	unsigned long off, len;
	int i, n;

	if (size < 256 || (b[0] << 8 | b[1]) != 0xee05)
		return b3_ldimg(b, size, lo, hi, drv, bpp, spp);
	n = b[2] << 8 | b[3];
	off = (b[4] << 8 | b[5]) * 128L;
	if (n > 15 || off < 256 || off >= size)
		return 1;
	for (i = 0; i < n && lo == tpalo; i++) {
		d = b + 16 + 16 * i;
		len = (unsigned long)d[2] << 24 | (unsigned long)d[3] << 16 | d[4] << 8 | d[5];
		d = b + (d[0] << 8 | d[1]) * 128L;
		if (d < b + 256 || d + len > b + off || len < 28 + 42)
			return 1;
		if (!rsxhave(d) && rsxload(d, (long)len, tpahi) < 256)
			return 1;
	}
	return b3_ldimg(b + off, size - off, lo, hi, drv, bpp, spp);
}

/* the FCB's program at lo..hi */
long
b3_ldfile(f, lo, hi, bpp, spp)
	unsigned long f, lo, hi, *bpp, *spp;
{
	unsigned char *b;
	struct hfile h;
	long r;
	int fd;

	if (lookup(f, &h, 0) != 0)
		return 2;
	if ((b3_shim = shimtype(h.name)) != 0) {
		/* the shim runs it: load a program that calls it */
		b3_shimh = h;
		b3_shimdrv = fcbdrv(f);
		return b3_ldimg(shimprg, (long)sizeof shimprg, lo, hi, fcbdrv(f), bpp, spp);
	}
	if ((fd = hf_open(fcbdrv(f), &h, 0)) < 0)
		return 2;
	if (h.size < 28 || (b = malloc(h.size)) == 0)
		return 1;
	if (lseek(fd, 0L, 0) != 0 || read(fd, (char *)b, h.size) != h.size) {
		free(b);
		return 2;
	}
	r = gcload(b, h.size, lo, hi, fcbdrv(f), bpp, spp);
	free(b);
	return r;
}

static long
pgmld(lpb)
	unsigned long lpb;
{
	unsigned long lo = b3_get32(lpb + 4), hi = b3_get32(lpb + 8), bp, sp;
	long r;

	if (hi > tpahi || lo < tpalo)
		return 1;
	if ((r = b3_ldfile(b3_get32(lpb), lo, hi, &bp, &sp)) != 0)
		return r;
	b3_put32(lpb + 12, bp);
	b3_put32(lpb + 16, sp);
	return 0;
}

/* ---- resident system extensions (function 60, GENCOM) ----
 * A module is a CP/M-68K program whose text starts with this header:
 *	0 serial[6]	6 entry.l	10 'RS'		12 next.l, where calls go on
 *	16 prev.l, the module below	20 FF: remove at warm boot	21 nbank
 *	22 name[8]	30 FF on the topmost	32 up.l, the module above	38 len.l
 * Trap #2 enters the newest, lowest module; the oldest passes calls to
 * the BDOS.
 */

#define	NRSX	16
static unsigned long rsx[NRSX];		/* modules, oldest first */
static int nrsx;

/* rewrite every module's links and lower the TPA to the chain */
static void
rsxlink()
{
	unsigned long m, up;
	int i;

	for (i = 0; i < nrsx; i++) {
		m = rsx[i];
		up = i ? rsx[i - 1] : 0;
		b3_put32(m + 12, up ? b3_get32(up + 6) : t2entry);
		b3_put32(m + 16, i + 1 < nrsx ? rsx[i + 1] : 0L);
		FB(m, 30) = up ? 0 : 0xff;
		b3_put32(m + 32, up);
	}
	b3_put32(34 * 4L, nrsx ? b3_get32(rsx[nrsx - 1] + 6) : t2entry);
	tpahi = nrsx ? rsx[nrsx - 1] : tpahi0;
}

static void
rsxadd(org, len)
	unsigned long org, len;
{
	memcpy(B3MEM(org), "\0\0\0\0\0\1", 6);
	b3_put32(org + 38, len);
	rsx[nrsx++] = org;
	rsxlink();
}

/*
 * Module b (a CP/M-68K program of size bytes) relocated to the highest
 * 256-byte boundary that fits it under top: its address, or 0xf1 (not
 * a program), 0xf2 (no room), 0xf3 (no header).
 */
static long
rsxload(b, size, top)
	unsigned char *b;
	long size;
	unsigned long top;
{
	unsigned long ts, ds, bs, ss, tst, org, need;

	if (size < 28 + 42 || (b[0] << 8 | b[1]) != 0x601a || b[26] || b[27])
		return 0xf1;
#define	L(o)	((unsigned long)b[o] << 24 | (unsigned long)b[(o)+1] << 16 | b[(o)+2] << 8 | b[(o)+3])
	ts = L(2), ds = L(6), bs = L(10), ss = L(14), tst = L(22);
#undef L
	if (ts < 42 || 28 + ss + 2 * (ts + ds) > size)
		return 0xf1;
	if ((b[28 + 10] << 8 | b[28 + 11]) != 0x5253)
		return 0xf3;
	need = ts + ds + bs;
	if (top > tpahi)
		top = tpahi;
	org = (top - need) & ~0xffL;
	if (nrsx == NRSX || top < need || org < tpalo + 256)
		return 0xf2;
	memcpy(B3MEM(org), b + 28, ts + ds);
	memset(B3MEM(org + ts + ds), 0, bs);
	if (reloc(b + 28 + ts + ds + ss, ts, ds, org - tst, org + ts - (tst + ts),
	    org + ts + ds - (tst + ts + ds), org, org + ts) != 0)
		return 0xf1;
	rsxadd(org, need);
	return org;
}

/* a module named like the one at b is resident */
static int
rsxhave(b)
	unsigned char *b;
{
	int i;

	for (i = 0; i < nrsx; i++)
		if (memcmp(B3MEM(rsx[i] + 22), b + 28 + 22, 8) == 0)
			return 1;
	return 0;
}

/*
 * pb: sub-function.b, 0, org.l, len.l, src.l.  127 attaches: org 0
 * relocates the program file at src below the caller's stack, otherwise
 * the module at src is copied to org as it is.  126 answers the newest.
 */
static long
rsxfn(pb, usp)
	unsigned long pb, usp;
{
	unsigned long org, len, src;

	switch (FB(pb, 0)) {
	case 126:
		return nrsx ? rsx[nrsx - 1] : 0;
	case 127:
		org = b3_get32(pb + 2);
		len = b3_get32(pb + 6);
		src = b3_get32(pb + 10);
		if (org == 0)
			return rsxload((unsigned char *)B3MEM(src), (long)len, usp - 1024);
		if (nrsx == NRSX || len < 42 || org & 1 || org < tpalo + 256 || org + len > tpahi)
			return 0xf2;
		if (b3_get16(src + 10) != 0x5253)
			return 0xf3;
		memmove(B3MEM(org), B3MEM(src), len);
		rsxadd(org, len);
		return 0;
	}
	return 0xff;
}

/* a process's own TPA, no modules */
void
b3_settpa(lo, hi)
	unsigned long lo, hi;
{
	tpalo = lo;
	tpahi0 = hi;
	nrsx = 0;
	rsxlink();
}

/* warm boot drops the flagged modules; the TPA ends at the lowest left */
static void
rsxwarm()
{
	int i, n;

	for (i = n = 0; i < nrsx; i++)
		if (FB(rsx[i], 20) == 0)
			rsx[n++] = rsx[i];
	nrsx = n;
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
	scb[0x43] = curfx;
	scb[0x44] = user;
	scb[0x4a] = mltio;
	scb[0x4b] = errmode;
	scb[0x57] = 0x80;
	scb[0x62] = tpahi;
	scb[0x63] = tpahi >> 8;
	hf_stamp((long)t, scb + 0x58);
	tm = localtime(&t);
	scb[0x5c] = (tm->tm_sec / 10) << 4 | tm->tm_sec % 10;
	if (set != 0xff && set != 0xfe)	/* @CRDMA reads as the whole address */
		return off == 0x3c ? (long)b3_dma : scb[off] | scb[off + 1] << 8;
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
		b3_conflush();
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
		b3_conflush();
		return b3_constat() ? 0xff : 0;
	case 12:
		return VERSION;
	case 141:
		/* delay: d1 ticks of 1/60 s */
		poll((struct pollfd *)0, 0L, (int)((d1 & 0xffff) * 50 / 3));
		return 0;
	case 13:
		b3_relog();
		dflt = 0;
		if (b3_dma0)
			b3_dma = b3_dma0;
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
		a = dparams(dflt);
		if (d1)
			memmove(B3MEM(d1), B3MEM(a), 16);
		return a;
	case 32:
		if ((d1 & 0xff) == 0xff)
			return user;
		user = d1 & 15;
		return 0;
	case 37:
		rodsk &= ~d1 | hf_rodrives();
		return 0;
	case 38:
	case 39:
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
		if ((d1 & 0xff) == 0xff)
			scb[0x17] |= 0x40;	/* the chained command keeps drive and user */
		b3_chain();
		warm();
	case 143:
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
		return rsxfn(d1, regs + 64);
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
	if (fn >= 128)
		return b3_xdos(fn, d1);
	if ((fn >= 51 && fn <= 97) || (fn >= 113 && fn <= 127))
		return 0xff;
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
	case 42:
	case 43:
		return reclock(d1, fn == 42);
	case 102:
		return stamps(d1);
	case 103:
		return wrxfcb(d1);
	}
	return 0xffff;
}
