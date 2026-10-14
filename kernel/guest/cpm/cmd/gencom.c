/*
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * gencom.c - Bind Resident System Extensions to a program.
 *
 *   GENCOM prog.68K mod.RSX [mod.RSX ...]  bind or replace modules
 *   GENCOM prog.68K                         restore the plain program
 *
 * The loader relocates each module below the chain before it loads the
 * program; one already resident under the same name is not loaded again.
 *
 * Container layout:
 *   records 0-1  256-byte header: magic 0xEE05, module count, the
 *                program's first record
 *   offset 16    16-byte descriptors: record, byte length (long),
 *                non-banked flag, reserved byte, eight-byte name
 *   records 2..  record-padded .RSX files in attachment order
 *   final records the original program
 *
 * A non-banked module is marked temporary.  Rebinding replaces an equal
 * module name in place; new names append.  The output is built in
 * TEMP.$$$ before it replaces the program.
 */

#include "cpm.h"

#define	GCMAGIC		0xEE05
#define	GCHDRLEN	256
#define	GCHDRRECS	2
#define	DESCOFF		16
#define	DESCLEN		16
#define	MAXRSX		15

/* a .RSX file: its 28-byte header, then the module's */
#define	F_HDR		28
#define	H_MAGIC		(F_HDR + 10)
#define	H_WARMFLG	(F_HDR + 20)
#define	H_NBANK		(F_HDR + 21)
#define	H_NAME		(F_HDR + 22)

char	rec[SECLEN];
char	hdr[GCHDRLEN];
char	pre[SECLEN];		/* a plain program's first record */
int	havepre;

struct fcb	pfcb;		/* the program / container */
struct fcb	tfcb;		/* TEMP.$$$ */
struct fcb	mfcb;		/* the .RSX file being read */
char		rnfcb[36];

/* the modules in attach order: `from' is 0 for one already in the
   container, k for one from argv[k] */
int		nrec[MAXRSX];
unsigned	mnbank[MAXRSX], mwarm[MAXRSX];
char		mname[MAXRSX][8];
int		mfrom[MAXRSX];
int		oldrec[MAXRSX];
int		nmod;
char		tempnm[] = "TEMP.$$$";

VOID putname(p)
register char *p;
{
	register int	i;

	for (i = 0; i < 8; i++)
		conout(p[i]);
}

unsigned wordat(p, off)
char *p;
int off;
{
	return ((unsigned)(((p[off] & 0xff) << 8) | (p[off + 1] & 0xff)));
}

VOID setword(p, off, v)
char *p;
int off;
unsigned v;
{
	p[off] = (char)(v >> 8);
	p[off + 1] = (char) v;
}

VOID die(msg)
char *msg;
{
	if (msg[0] != 0)
		cputs("gencom: ");
	cputs(msg);
	cputs("\r\n");
	__bdos(BDOS_DELETE, (long) &tfcb);
	__bdos(BDOS_WBOOT, 0L);
}

/* after the original is gone and before TEMP.$$$ takes its name */
VOID dielast(msg)
char *msg;
{
	cputs("gencom: ");
	cputs(msg);
	cputs("\r\n");
	cputs("gencom: the new program is in TEMP.$$$ and the old one is\r\n");
	cputs("        gone; rename TEMP.$$$ by hand.\r\n");
	__bdos(BDOS_WBOOT, 0L);
}

int getrec(f)
struct fcb *f;
{
	setdma(rec);
	return (__bdos(BDOS_READSEQ, (long) f) == 0);
}

VOID putrec()
{
	setdma(rec);
	if (__bdos(BDOS_WRITESEQ, (long) &tfcb) != 0)
		die("disk write");
}

VOID copyrecs(f, n)
struct fcb *f;
register int n;
{
	while (--n >= 0) {
		if (!getrec(f))
			die("the file ends inside a module");
		putrec();
	}
}

VOID skiprecs(f, n)
struct fcb *f;
register int n;
{
	while (--n >= 0)
		if (!getrec(f))
			die("the file ends inside a module");
}

/* rec[] holds a module's first record */
VOID normalise(i)
int i;
{
	if ((rec[H_NBANK] & 0xff) != 0)
		rec[H_WARMFLG] = (char) 0xff;
	mnbank[i] = (unsigned)(rec[H_NBANK] & 0xff);
	mwarm[i] = (unsigned)(rec[H_WARMFLG] & 0xff);
}

/* one .RSX file into the module list */
VOID readmod(name, i, argi)
char *name;
int i;
int argi;
{
	register int	k, n;

	mkfcb(name, &mfcb);
	if ((__bdos(BDOS_OPEN, (long) &mfcb) & 0xff) == 255) {
		cputs("gencom: no such RSX file: ");
		cputs(name);
		die("");
	}
	if (!getrec(&mfcb))
		die("an RSX file is empty");
	if (wordat(rec, 0) != 0x601A || wordat(rec, H_MAGIC) != 0x5253)
		die("that is not an RSX");
	normalise(i);
	for (k = 0; k < 8; k++)
		mname[i][k] = rec[H_NAME + k];
	for (n = 1; getrec(&mfcb); n++)
		;
	__bdos(BDOS_CLOSE, (long) &mfcb);
	setdma(_base->buff);
	nrec[i] = n;
	mfrom[i] = argi;
}

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i, k;
	int		oldn, rc, argi;
	unsigned	off;
	long		len;

	if (argc < 2) {
		cputs("usage: GENCOM prog.68K [mod.RSX ...]\r\n");
		return (1);
	}
	mkfcb(tempnm, &tfcb);
	__bdos(BDOS_DELETE, (long) &tfcb);
	mkfcb(argv[1], &pfcb);
	if (pfcb.ftype[0] == ' ') {
		pfcb.ftype[0] = '6';
		pfcb.ftype[1] = '8';
		pfcb.ftype[2] = 'K';
	}
	if ((__bdos(BDOS_OPEN, (long) &pfcb) & 0xff) == 255)
		die("no such program file");
	if (!getrec(&pfcb))
		die("the program file is empty");
	oldn = 0;
	if (wordat(rec, 0) == GCMAGIC) {
		for (i = 0; i < SECLEN; i++)
			hdr[i] = rec[i];
		if (!getrec(&pfcb))
			die("the container header is truncated");
		for (i = 0; i < SECLEN; i++)
			hdr[SECLEN + i] = rec[i];
		oldn = (int) wordat(hdr, 2);
		if (oldn < 0 || oldn > MAXRSX)
			die("the container header names an impossible number "
			    "of modules");
		for (i = 0; i < oldn; i++) {
			off = (unsigned)(DESCOFF + i * DESCLEN);
			len = (long) wordat(hdr, (int)(off + 2)) << 16 | wordat(hdr, (int)(off + 4));
			nrec[i] = (int)((len + SECLEN - 1) / SECLEN);
			mnbank[i] = (unsigned)(hdr[off + 6] & 0xff);
			for (k = 0; k < 8; k++)
				mname[i][k] = hdr[off + 8 + k];
			mwarm[i] = 0;
			mfrom[i] = 0;
			oldrec[i] = nrec[i];
		}
	} else if (wordat(rec, 0) != 0x601A && wordat(rec, 0) != 0x601B)
		die("that is not a program file");
	else {
		for (i = 0; i < SECLEN; i++)
			pre[i] = rec[i];
		havepre = 1;
	}
	/* no module named: the plain program comes back */
	nmod = (argc > 2) ? oldn : 0;
	for (argi = 2; argi < argc; argi++) {
		if (nmod >= MAXRSX)
			die("there are not enough available RSX slots");
		readmod(argv[argi], nmod, argi);
		for (i = 0; i < nmod; i++) {
			for (k = 0; k < 8; k++)
				if (mname[i][k] != mname[nmod][k])
					break;
			if (k < 8)
				continue;
			if (mfrom[i] != 0) {
				cputs("gencom: ");
				putname(mname[i]);
				die(" is named twice on one command line");
			}
			cputs("gencom: replacing ");
			putname(mname[i]);
			cputs(" already in the header\r\n");
			nrec[i] = nrec[nmod];
			mnbank[i] = mnbank[nmod];
			mwarm[i] = mwarm[nmod];
			mfrom[i] = argi;
			mfrom[nmod] = -1;
			break;
		}
		if (mfrom[nmod] != -1)
			nmod++;
	}
	if (nmod == 0 && oldn == 0)
		die("no header or RSXs to strip");
	for (i = 0; i < GCHDRLEN; i++)
		hdr[i] = 0;
	if (nmod > 0) {
		setword(hdr, 0, GCMAGIC);
		setword(hdr, 2, (unsigned) nmod);
		off = GCHDRRECS;
		for (i = 0; i < nmod; i++) {
			k = (int)(DESCOFF + i * DESCLEN);
			len = (long) nrec[i] * SECLEN;
			setword(hdr, k, off);
			setword(hdr, k + 2, (unsigned)(len >> 16));
			setword(hdr, k + 4, (unsigned) len);
			hdr[k + 6] = (char) mnbank[i];
			for (rc = 0; rc < 8; rc++)
				hdr[k + 8 + rc] = mname[i][rc];
			off += (unsigned) nrec[i];
		}
		setword(hdr, 4, off);
	}
	if ((__bdos(BDOS_MAKE, (long) &tfcb) & 0xff) == 255)
		die("no directory space for TEMP.$$$");
	if (nmod > 0) {
		for (i = 0; i < SECLEN; i++)
			rec[i] = hdr[i];
		putrec();
		for (i = 0; i < SECLEN; i++)
			rec[i] = hdr[SECLEN + i];
		putrec();
	}
	/* modules kept are copied through; replaced ones are skipped */
	for (i = 0; i < nmod; i++) {
		if (mfrom[i] == 0) {
			if (!getrec(&pfcb))
				die("the file ends inside a module");
			normalise(i);
			putrec();
			copyrecs(&pfcb, oldrec[i] - 1);
			continue;
		}
		if (i < oldn)
			skiprecs(&pfcb, oldrec[i]);
		mkfcb(argv[mfrom[i]], &mfcb);
		if ((__bdos(BDOS_OPEN, (long) &mfcb) & 0xff) == 255)
			die("an RSX file went away mid-run");
		for (k = 0; k < nrec[i]; k++) {
			if (!getrec(&mfcb))
				die("an RSX file got shorter");
			if (k == 0)
				normalise(i);
			putrec();
		}
		__bdos(BDOS_CLOSE, (long) &mfcb);
		setdma(_base->buff);
	}
	for (i = nmod; i < oldn; i++)
		skiprecs(&pfcb, oldrec[i]);
	if (havepre) {
		for (i = 0; i < SECLEN; i++)
			rec[i] = pre[i];
		putrec();
	}
	while (getrec(&pfcb))
		putrec();
	if ((__bdos(BDOS_CLOSE, (long) &tfcb) & 0xff) == 255)
		die("cannot close TEMP.$$$; the program file is untouched");
	__bdos(BDOS_CLOSE, (long) &pfcb);
	setdma(_base->buff);
	if ((__bdos(BDOS_DELETE, (long) &pfcb) & 0xff) == 255)
		die("cannot replace the program file");
	for (i = 0; i < 16; i++) {
		rnfcb[i] = ((char *) &tfcb)[i];
		rnfcb[16 + i] = ((char *) &pfcb)[i];
	}
	if ((__bdos(BDOS_RENAME, (long) rnfcb) & 0xff) == 255)
		dielast("cannot rename TEMP.$$$ over the program file");
	for (i = 0; i < nmod; i++) {
		cputs("gencom: ");
		putname(mname[i]);
		cputs(mwarm[i] ? " temporary\r\n" : " resident\r\n");
	}
	cputs("GENCOM completed.\r\n");
	return (0);
}
