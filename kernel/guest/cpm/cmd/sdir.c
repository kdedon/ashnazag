/*
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * sdir.c - SDIR, the CP/M 3 directory utility, for the C900.
 *
 *	SDIR [options] [afn ...]
 *
 * The CCP supplies the plain built-in DIR; this transient adds sized,
 * attributed, and dated listings. [DATE] requires a stamping-enabled label
 * and SFCBs, which are read directly from directory entries. [SHORT] uses the
 * same collected and sorted file table as the other display modes.
 */

#include "cpm.h"

#define	BDOS_VERSION	12
#define	BDOS_SELDSK	14
#define	BDOS_LOGINVEC	24
#define	BDOS_CURDSK	25
#define	BDOS_GETDPB	31
#define	BDOS_GETUSER	32
#define	BDOS_RAWIO	6

#define	MAXFILES	300		/* file table capacity		*/
#define	MAXSPEC		10		/* v3's max$search$files	*/

#define	FORM_SHORT	0
#define	FORM_SIZE	1
#define	FORM_FULL	2

#define	E5		0xe5		/* deleted directory entry	*/
#define	LABELTYPE	0x20		/* directory label		*/
#define	SFCBTYPE	0x21		/* date/time stamp entry	*/

#define	DL_EXISTS	0x01
#define	DL_MAKEXFCB	0x10
#define	DL_UPDATE	0x20
#define	DL_ACCESS	0x40

/*
 * Function 31 does not return the address of the disk parameter block on
 * this system -- the BDOS data segment is not addressable from the TPA --
 * it copies the block to the caller (sys/bdosmain.c:319-321).  The
 * declaration below is byte-for-byte sys/bdosdef.h:112-124 so that the
 * two agree under the same compiler.
 */
struct dpb {
	unsigned	spt;		/* sectors per track		*/
	char		bsh;		/* block shift factor		*/
	char		blm;		/* block mask			*/
	char		exm;		/* extent mask			*/
	char		dpbdum;		/* fill				*/
	unsigned	dsm;		/* max block number		*/
	unsigned	drm;		/* max directory entry number	*/
	unsigned	dir_al;		/* directory allocation bits	*/
	unsigned	cks;		/* checksummed dir records	*/
	unsigned	trk_off;	/* reserved tracks		*/
};

struct finfo {
	char		name[12];	/* 11 name+type bytes, attrs set */
	unsigned	onek;		/* 1K blocks, sparse-corrected	*/
	unsigned	kbytes;		/* allocated kilobytes		*/
	long		recs;		/* 128-byte records		*/
	char		usr;
	char		hasts;
	char		hasx;		/* a password XFCB names it	*/
	char		ts[10];		/* create(4) update(4) passmode	*/
};

static struct dpb	dpb;
static struct fcb	qfcb;
static char		dbuf[SECLEN];

static struct finfo	fi[MAXFILES];
static unsigned		idx[MAXFILES];
static int		nfiles;

static char		spec[MAXSPEC][11];
static char		specdrv[MAXSPEC];
static char		specany[MAXSPEC];
static int		nspec;

static unsigned		usrvec, drvvec;
static int		curdrv, curusr;

static int		fmt = FORM_FULL;
static int		f_dir, f_sys, f_ro, f_rw, f_excl, f_xfcb, f_nonxfcb;
static int		attopt, dateopt, msgopt, ffopt, sortop = 1, nopage;
static unsigned		pagelen = 0xffff;

static unsigned		used_de;
static int		labelbyte, sfcbs;

static long		tkb, trec, tblk;
static int		curfile, curline, gline = 1, fpl, firsttime;
static int		firsttitle = 1, displayed;

static int		scbpb[2];
static char		tail[SECLEN + 2];
static int		tp;			/* scan position in tail[] */
static int		lastp;			/* tp before the last token */
static char		tok[16];		/* current token	*/
static int		toklen;

static char dpm[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };

/* ---------------- BDOS shims ---------------- */

static int scbgetb(off)
int off;
{
	char	*p;

	p = (char *) scbpb;
	p[0] = (char) off;
	p[1] = 0;
	p[2] = 0;
	p[3] = 0;
	return (__bdos(49, (long) scbpb) & 0xff);
}

static VOID die(msg)
char *msg;
{
	cputs(msg);
	__bdos(BDOS_WBOOT, 0L);
}

/* ---------------- output ---------------- */

static VOID crlf()
{
	cputs("\r\n");
}

static VOID pb()
{
	conout(' ');
}

/*
 * A decimal field of exactly `width' characters, right aligned, blank
 * padded when zsup is set and zero padded otherwise -- the shape of
 * pdecimal(v,prec,zsup) (util.plm:110-124), computed on a long so the
 * totals stay correct past 65535.
 */
static VOID pfield(v, width, zsup)
long v;
int width;
int zsup;
{
	char		d[12];
	register int	n, i;

	n = 0;
	do {
		d[n++] = (char) ('0' + (int) (v % 10L));
		v /= 10L;
	} while (v != 0L && n < 12);
	for (i = n; i < width; i++)
		conout(zsup ? ' ' : '0');
	while (n > 0)
		conout(d[--n]);
}

/* "NAME     TYP" -- 12 columns, attribute bits masked (util.plm:88-101) */
static VOID printfn(p)
register char *p;
{
	register int	i;

	for (i = 0; i < 8; i++)
		conout(p[i] & 0x7f);
	conout(' ');
	for (i = 8; i < 11; i++)
		conout(p[i] & 0x7f);
}

/* v3's break: any keystroke abandons the listing (search.plm:129-137) */
static VOID brk()
{
	if (__bdos(BDOS_CONST, 0L) != 0) {
		__bdos(BDOS_CONIN, 0L);
		__bdos(BDOS_WBOOT, 0L);
	}
}

/* newline with the page pause (disp.plm:172-187) */
static VOID crlfck()
{
	register int	c;

	if (nopage == 0 && (unsigned) gline > pagelen - 1) {
		cputs("\r\nPress RETURN to Continue ");
		curline++;
		while ((c = __bdos(BDOS_RAWIO, 0xffL) & 0xff) == 0)
			;
		if (c == 3)
			__bdos(BDOS_WBOOT, 0L);
		gline = 0;
	}
	crlf();
	gline++;
}

/* ---------------- date stamps ---------------- */

static int isleap(y)
int y;
{
	return ((y & 3) == 0);
}

static int mlen(m, y)
int m, y;
{
	if (m == 2 && isleap(y))
		return (29);
	return (dpm[m - 1]);
}

/*
 * "MM/DD/YY HH:MM" from a 4-byte CP/M 3 stamp: the date word is days
 * since 1977-12-31 stored LOW BYTE FIRST on disk, then hour and minute
 * in BCD.  14 columns, the
 * width disp.plm:216 reserves for it.
 */
static VOID putstamp(p)
register char *p;
{
	unsigned	day;
	long		n;
	int		y, m;

	day = ((unsigned) (p[1] & 0xff) << 8) | (unsigned) (p[0] & 0xff);
	if (day == 0) {
		cputs("              ");
		return;
	}
	n = (long) day;
	y = 1978;
	while (n > (isleap(y) ? 366L : 365L)) {
		n -= isleap(y) ? 366L : 365L;
		y++;
	}
	m = 1;
	while (n > (long) mlen(m, y)) {
		n -= (long) mlen(m, y);
		m++;
	}
	pfield((long) m, 2, 0);
	conout('/');
	pfield(n, 2, 0);
	conout('/');
	pfield((long) (y % 100), 2, 0);
	conout(' ');
	conout('0' + ((p[2] >> 4) & 0x0f));
	conout('0' + (p[2] & 0x0f));
	conout(':');
	conout('0' + ((p[3] >> 4) & 0x0f));
	conout('0' + (p[3] & 0x0f));
}

/* ---------------- command tail scanner ---------------- */

static VOID skipbl()
{
	while (tail[tp] == ' ' || tail[tp] == '\t')
		tp++;
}

/*
 * The next token: a run of characters that is neither white space nor
 * one of v3's option punctuation ([ ] = ( ) ,).  Returns 0 at the end of
 * the tail, otherwise the first character of the token.
 */
static int next()
{
	register int	c;

	skipbl();
	lastp = tp;
	toklen = 0;
	tok[0] = 0;
	c = tail[tp] & 0xff;
	if (c == 0)
		return (0);
	if (c == '[' || c == ']' || c == '=' || c == '(' || c == ')'
	    || c == ',') {
		tp++;
		tok[0] = (char) c;
		tok[1] = 0;
		toklen = 1;
		return (c);
	}
	while ((c = tail[tp] & 0xff) != 0 && c != ' ' && c != '\t'
	       && c != '[' && c != ']' && c != '=' && c != '('
	       && c != ')' && c != ',') {
		if (toklen < 15)
			tok[toklen++] = (char) (c >= 'a' && c <= 'z'
					        ? c - 32 : c);
		tp++;
	}
	tok[toklen] = 0;
	return (tok[0] & 0xff);
}

static VOID operr()
{
	die("ERROR: Illegal Option or Modifier.\r\n");
}

static VOID setvec(vp, n)
unsigned *vp;
int n;
{
	*vp |= (unsigned) 1 << n;
}

/* ---------------- options (main.plm:295-430) ---------------- */

static int optnum()
{
	register int	i, v;

	v = 0;
	if (toklen == 0)
		operr();
	for (i = 0; i < toklen; i++) {
		if (tok[i] < '0' || tok[i] > '9')
			return (-1);
		v = v * 10 + (tok[i] - '0');
	}
	return (v);
}

/*
 * One keyword.  v3 recognizes options by position within the token
 * (main.plm:299-361), so the same letter tests are used here: tok[0] is
 * v3's token(1), tok[1] its token(2), and so on.
 */
static VOID onekeyword()
{
	register int	v;

	switch (tok[0]) {
	case 'A':
		attopt = 1;
		return;
	case 'D':
		if (tok[1] == 'I') {			/* DIR		*/
			f_dir = 1;
			return;
		}
		if (tok[1] == 'A') {			/* DATE		*/
			fmt = FORM_FULL;
			dateopt = 1;
			return;
		}
		break;
	case 'E':					/* EXCLUDE	*/
		f_excl = 1;
		return;
	case 'F':
		if (tok[1] == 'F') {			/* FF		*/
			ffopt = 1;
			return;
		}
		if (tok[1] == 'U') {			/* FULL		*/
			fmt = FORM_FULL;
			return;
		}
		break;
	case 'G':					/* Gnn		*/
		if (toklen < 3)
			v = tok[1] - '0';
		else
			v = (tok[1] - '0') * 10 + (tok[2] - '0');
		if (v >= 0 && v <= 15) {
			setvec(&usrvec, v);
			return;
		}
		break;
	case 'M':					/* MESSAGE	*/
		msgopt = 1;
		return;
	case 'N':
		if (tok[3] == 'X') {			/* NONXFCB	*/
			f_nonxfcb = 1;
			return;
		}
		if (tok[2] == 'P') {			/* NOPAGE	*/
			nopage = 0xff;
			return;
		}
		if (tok[2] == 'S') {			/* NOSORT	*/
			sortop = 0;
			return;
		}
		break;
	case 'R':
		if (tok[1] == 'O') {
			f_ro = 1;
			return;
		}
		if (tok[1] == 'W') {
			f_rw = 1;
			return;
		}
		break;
	case 'S':
		if (tok[1] == 'Y') {			/* SYS		*/
			f_sys = 1;
			return;
		}
		if (tok[1] == 'I') {			/* SIZE		*/
			fmt = FORM_SIZE;
			return;
		}
		if (tok[1] == 'O') {			/* SORT		*/
			sortop = 1;
			return;
		}
		if (tok[1] == 'H') {			/* SHORT	*/
			fmt = FORM_SHORT;
			return;
		}
		break;
	case 'X':					/* XFCB		*/
		f_xfcb = 1;
		return;
	}
	operr();
}

/* put the last token back so the caller's scan can see it again */
static VOID unnext()
{
	tp = lastp;
}

/* one value of a LENGTH= / USER= / DRIVE= modifier list */
static VOID modvalue(want)
int want;
{
	register int	v;

	if (want == 'L') {
		if ((v = optnum()) < 5)
			operr();
		pagelen = (unsigned) v;
	} else if (want == 'U') {
		if (tok[0] == 'A' && tok[1] == 'L')
			usrvec = 0xffff;
		else if ((v = optnum()) >= 0 && v <= 15)
			setvec(&usrvec, v);
		else
			operr();
	} else {
		if (tok[0] == 'A' && tok[1] == 'L')
			drvvec = (unsigned) __bdos(BDOS_LOGINVEC, 0L);
		else if (toklen == 1 && tok[0] >= 'A' && tok[0] <= 'P')
			setvec(&drvvec, tok[0] - 'A');
		else
			operr();
	}
}

/*
 * The bracketed option group.  v3's scanner tells an option from its
 * modifier list by token type (main.plm:297,384); here LENGTH, USER and
 * DRIVE/DISK are the three keywords that take an `=' and then either one
 * value, a comma list, or a parenthesised list -- the three forms the
 * v3 help text shows (main.plm:266-271).
 */
static VOID getoptions()
{
	register int	c;
	int		want, paren;

	while ((c = next()) != 0 && c != ']') {
		if (c == ',')
			continue;
		want = 0;
		if (tok[0] == 'L')
			want = 'L';
		else if (tok[0] == 'U')
			want = 'U';
		else if (tok[0] == 'D' && tok[1] == 'R')
			want = 'D';
		else if (tok[0] == 'D' && tok[1] == 'I' && tok[2] == 'S')
			want = 'D';
		if (want == 0) {
			onekeyword();
			continue;
		}
		if (next() != '=')
			operr();
		c = next();
		paren = (c == '(');
		if (paren)
			c = next();
		for (;;) {
			if (c == 0 || c == ')' || c == ']')
				break;
			modvalue(want);
			c = next();
			if (c == ',') {
				c = next();
				continue;
			}
			if (!paren) {
				unnext();
				break;
			}
		}
		if (c == ']')
			return;
	}
}

/* ---------------- file specifications ---------------- */

/* "D:NAME.TYP", with '*' expanded to '?' (v3's scanner does this) */
static VOID getspec()
{
	register char	*p;
	register int	i, n;
	char		*name;

	if (nspec >= MAXSPEC) {
		cputs("File Spec Limit is ");
		pfield((long) MAXSPEC, 3, 1);
		crlf();
		return;
	}
	p = spec[nspec];
	for (i = 0; i < 11; i++)
		p[i] = ' ';
	name = tok;
	specdrv[nspec] = (char) 0xff;
	if (name[0] != 0 && name[1] == ':') {
		specdrv[nspec] = (char) (name[0] - 'A');
		name += 2;
	}
	for (i = 0; i < 8 && *name != 0 && *name != '.'; name++) {
		if (*name == '*') {
			while (i < 8)
				p[i++] = '?';
			break;
		}
		p[i++] = *name;
	}
	while (*name != 0 && *name != '.')
		name++;
	if (*name == '.')
		name++;
	for (i = 0; i < 3 && *name != 0; name++) {
		if (*name == '*') {
			while (i < 3)
				p[8 + i++] = '?';
			break;
		}
		p[8 + i++] = *name;
	}

	n = 1;
	for (i = 0; i < 11; i++)
		if (p[i] != '?')
			n = 0;
	specany[nspec] = (char) n;
	nspec++;
}

/* search.plm:156-168 -- 7-bit compare with '?' matching anything */
static int specmatch(e)
register char *e;
{
	register int	i, k;

	for (k = 0; k < nspec; k++) {
		if ((specdrv[k] & 0xff) != 0xff
		    && (specdrv[k] & 0xff) != curdrv)
			continue;
		if (specany[k])
			return (1);
		for (i = 0; i < 11; i++)
			if (spec[k][i] != '?'
			    && (spec[k][i] & 0x7f) != (e[i] & 0x7f))
				break;
		if (i == 11)
			return (1);
	}
	return (0);
}

/* ---------------- the directory scan ---------------- */

static struct finfo *lookup(e, u)
register char *e;
int u;
{
	register int	i, j;

	for (i = 0; i < nfiles; i++) {
		if ((fi[i].usr & 0xff) != u)
			continue;
		for (j = 0; j < 11; j++)
			if ((fi[i].name[j] & 0x7f) != (e[j] & 0x7f))
				break;
		if (j == 11)
			return (&fi[i]);
	}
	if (nfiles >= MAXFILES)
		return ((struct finfo *) 0);
	i = nfiles++;
	for (j = 0; j < 11; j++)
		fi[i].name[j] = e[j];
	fi[i].name[11] = 0;
	fi[i].usr = (char) u;
	fi[i].hasts = 0;
	fi[i].hasx = 0;
	fi[i].onek = 0;
	fi[i].kbytes = 0;
	fi[i].recs = 0L;
	return (&fi[i]);
}

/* one directory entry into the table (search.plm:277-308) */
static VOID addentry(e, u, tsp)
register char *e;
int u;
char *tsp;
{
	struct finfo		*f;
	register int		j, cnt;
	int			w, kper, rem, arc;

	if ((usrvec & ((unsigned) 1 << u)) == 0)
		return;
	if (specmatch(&e[1]) == (f_excl != 0))
		return;
	if ((f = lookup(&e[1], u)) == (struct finfo *) 0)
		return;

	arc = f->name[10];
	for (j = 0; j < 11; j++)
		f->name[j] = e[j + 1];
	f->name[10] = (char) (arc & e[11]);

	if (tsp != (char *) 0 && !f->hasts) {
		for (j = 0; j < 9; j++)
			f->ts[j] = tsp[j];
		f->hasts = 1;
	}

	w = (dpb.dsm > 255) ? 2 : 1;
	kper = ((dpb.blm & 0xff) + 1) >> 3;
	cnt = 0;
	for (j = 16; j < 32; j += w) {
		if ((e[j] & 0xff) != 0 || (w == 2 && (e[j + 1] & 0xff) != 0))
			cnt++;
	}
	if (cnt > 0) {
		rem = (128 - (e[15] & 0xff)) & (dpb.blm & 0xff);
		f->recs += (long) cnt * (long) ((dpb.blm & 0xff) + 1)
			 - (long) rem;
		f->kbytes += (unsigned) (cnt * kper);
		f->onek += (unsigned) (cnt * kper - (rem >> 3));
	}
}

static VOID getfiles()
{
	register int	rc, k;
	char		*e;

	cputs("\r\nScanning Directory...\r\n");
	nfiles = 0;
	used_de = 0;
	labelbyte = 0;
	setdma(dbuf);
	qfcb.drvcode = '?';
	rc = __bdos(BDOS_SFIRST, (long) &qfcb) & 0xff;
	sfcbs = ((dbuf[96] & 0xff) == SFCBTYPE);
	while (rc != 0xff) {
		k = rc & 3;
		e = &dbuf[k << 5];
		if ((e[0] & 0xff) != E5) {
			used_de++;
			if ((e[0] & 0xff) == LABELTYPE)
				labelbyte = e[12] & 0xff;
			else if ((e[0] & 0xff) < 0x10)
				addentry(e, e[0] & 0x0f,
					 sfcbs ? &dbuf[97 + k * 10]
					       : (char *) 0);
		}
		brk();
		rc = __bdos(BDOS_SNEXT, 0L) & 0xff;
	}
}

/*
 * [XFCB] and [NONXFCB]: select on whether a file has a password XFCB.
 *
 * A second directory pass, because an XFCB (type 10h+user) can sit
 * anywhere in the directory relative to the file it names -- the first
 * pass would have to remember every XFCB it walked past, and this
 * remembers none.  It runs only when one of the two options was given,
 * so an ordinary SDIR reads the directory exactly once, as before.
 *
 * The XFCB's eleven name bytes are the file's, so the comparison is the
 * sort key's comparison with the attribute bits masked off (an XFCB's
 * name bytes carry none, and a file's may).
 */
static int samename(a, b)
register char *a;
register char *b;
{
	register int	i;

	for (i = 0; i < 11; i++)
		if (((a[i] ^ b[i]) & 0x7f) != 0)
			return (0);
	return (1);
}

static VOID markxfcbs()
{
	register int	rc, k, i;
	char		*e;

	setdma(dbuf);
	qfcb.drvcode = '?';
	rc = __bdos(BDOS_SFIRST, (long) &qfcb) & 0xff;
	while (rc != 0xff) {
		k = rc & 3;
		e = &dbuf[k << 5];
		if (((e[0] & 0xff) & 0xf0) == 0x10)
			for (i = 0; i < nfiles; i++)
				if (fi[i].usr == (char) (e[0] & 0x0f)
				    && samename(fi[i].name, &e[1]))
					fi[i].hasx = 1;
		brk();
		rc = __bdos(BDOS_SNEXT, 0L) & 0xff;
	}
}

static VOID xfcbfilter()
{
	register int	i, j;
	int		n;
	char		*p, *q;

	markxfcbs();
	n = 0;
	for (i = 0; i < nfiles; i++) {
		if (f_xfcb ? !fi[i].hasx : fi[i].hasx)
			continue;
		if (n != i) {
			p = (char *) &fi[n];
			q = (char *) &fi[i];
			for (j = 0; j < (int) sizeof (struct finfo); j++)
				p[j] = q[j];
		}
		n++;
	}
	nfiles = n;
}

/* sort.plm:35-45 -- name and type only, attribute bits masked off */
static int lessthan(a, b)
register char *a;
register char *b;
{
	register int	i;

	for (i = 0; i < 11; i++)
		if ((a[i] & 0x7f) != (b[i] & 0x7f))
			return ((a[i] & 0x7f) < (b[i] & 0x7f));
	return (0);
}

static VOID sortfiles()
{
	register int	i, j;
	unsigned	t;

	for (i = 0; i < nfiles; i++)
		idx[i] = (unsigned) i;
	if (nfiles < 2 || !sortop)
		return;
	cputs("\r\nSorting  Directory...\r\n");
	for (i = 1; i < nfiles; i++) {		/* stable insertion sort */
		t = idx[i];
		for (j = i; j > 0
		     && lessthan(fi[t].name, fi[idx[j - 1]].name); j--)
			idx[j] = idx[j - 1];
		idx[j] = t;
	}
}

/* ---------------- display ---------------- */

static char hdr[] = "    Name     Bytes   Recs   Attributes ";
static char bars[] = "------------ ------ ------ ------------";
static char hdrpu[] = "  Prot      Update    ";
static char xbars[] = " ------ --------------  --------------";

static VOID title()
{
	if (ffopt)
		conout(12);
	else if (!firsttitle)
		crlfck();
	cputs("Directory For Drive ");
	conout('A' + curdrv);
	conout(':');
	cputs("  User ");
	pfield((long) curusr, 2, 1);
	crlfck();
	curline = 2;
	firsttitle = 0;
}

/* disp.plm:365-378 -- RO/RW and DIR/SYS filtering */
static int rightattr(p)
register char *p;
{
	if ((p[8] & 0x80) ? !f_ro : !f_rw)
		return (0);
	if ((p[9] & 0x80) ? !f_sys : !f_dir)
		return (0);
	return (1);
}

static VOID shortdisp(p)
char *p;
{
	if (curfile % fpl == 0) {
		if (curline % pagelen == 0 && firsttime == 0) {
			crlfck();
			title();
			crlfck();
		} else
			crlfck();
		curline++;
		conout('A' + curdrv);
	} else
		pb();
	cputs(": ");
	printfn(p);
	brk();
	curfile++;
	firsttime++;
}

static VOID addtotals(f)
register struct finfo *f;
{
	tkb += (long) f->kbytes;
	trec += f->recs;
	tblk += (long) f->onek;
}

/* disp.plm:212-268 -- name, size, records, then the attribute columns */
static VOID fileinfo(f)
register struct finfo *f;
{
	printfn(f->name);
	pb();
	pfield((long) f->kbytes, 5, 1);
	conout('k');
	pb();
	pfield(f->recs, 6, 1);
	pb();
	cputs((f->name[9] & 0x80) ? "Sys" : "Dir");
	pb();
	cputs((f->name[8] & 0x80) ? "RO" : "RW");
	pb();
	if (!attopt)
		cputs((f->name[10] & 0x80) ? "Arcv " : "     ");
	else {
		conout((f->name[10] & 0x80) ? 'A' : ' ');
		conout((f->name[0] & 0x80) ? '1' : ' ');
		conout((f->name[1] & 0x80) ? '2' : ' ');
		conout((f->name[2] & 0x80) ? '3' : ' ');
		conout((f->name[3] & 0x80) ? '4' : ' ');
	}
}

/* disp.plm:270-300, driven by the SFCB rather than an XFCB */
static VOID stampinfo(f)
register struct finfo *f;
{
	register int	pm;

	if (!f->hasts)
		return;
	pb();
	pm = f->ts[8] & 0xff;
	if (pm & 0x80)
		cputs("Read  ");
	else if (pm & 0x40)
		cputs("Write ");
	else if (pm & 0x20)
		cputs("Delete");
	else
		cputs("None  ");
	pb();
	putstamp(&f->ts[4]);		/* update */
	pb();
	pb();
	putstamp(&f->ts[0]);		/* create or access */
}

static int wanted(f)
register struct finfo *f;
{
	if ((f->usr & 0xff) != curusr)
		return (0);
	return (rightattr(f->name));
}

static VOID sizedisplay()
{
	register int	i;
	register struct finfo *f;

	fpl = (fmt == FORM_SIZE) ? 3 : 4;
	for (i = 0; i < nfiles; i++) {
		f = &fi[idx[i]];
		if (!wanted(f))
			continue;
		addtotals(f);
		shortdisp(f->name);
		if (fmt == FORM_SIZE) {
			pfield((long) f->kbytes, 5, 1);
			cputs("k");
		}
	}
}

static VOID fullheads(withts)
int withts;
{
	crlfck();
	title();
	crlfck();
	cputs(hdr);
	if (withts) {
		cputs(hdrpu);
		cputs((labelbyte & DL_ACCESS) ? "      Access    "
					      : "      Create    ");
		crlfck();
		cputs(bars);
		cputs(xbars);
	} else {
		pb();
		cputs(hdr);
		crlfck();
		cputs(bars);
		pb();
		cputs(bars);
	}
	crlfck();
	curline += 4;
	firsttime++;
}

static VOID fulldisplay(withts)
int withts;
{
	register int	i;
	register struct finfo *f;

	fpl = withts ? 1 : 2;
	firsttime = 0;
	for (i = 0; i < nfiles; i++) {
		f = &fi[idx[i]];
		if (!wanted(f))
			continue;
		if (curfile % fpl == 0) {
			if (curline % pagelen == 0) {
				if (nopage == 0 || firsttime == 0)
					fullheads(withts);
				else {
					crlfck();
					curline++;
				}
			} else {
				crlfck();
				curline++;
			}
		} else
			pb();
		fileinfo(f);
		if (withts)
			stampinfo(f);
		curfile++;
		addtotals(f);
		brk();
	}
}

static VOID display()
{
	curline = 0;
	curfile = 0;
	tkb = 0L;
	trec = 0L;
	tblk = 0L;

	if (fmt == FORM_SHORT || fmt == FORM_SIZE)
		sizedisplay();
	else if (dateopt) {
		if ((labelbyte & DL_EXISTS)
		    && (labelbyte & (DL_ACCESS | DL_UPDATE | DL_MAKEXFCB))
		    && sfcbs)
			fulldisplay(1);
		else
			die("ERROR: Date and Time Stamping Inactive.\r\n");
	} else if ((labelbyte & DL_EXISTS) && sfcbs)
		fulldisplay(1);
	else
		fulldisplay(0);

	if (fmt != FORM_SHORT && curfile > 0) {
		if (curline + 4 > (int) pagelen && ffopt) {
			conout(13);
			conout(12);
		} else {
			crlfck();
			crlfck();
		}
		cputs("Total Bytes     = ");
		pfield(tkb, 6, 1);
		conout('k');
		cputs("  Total Records = ");
		pfield(trec, 7, 1);
		cputs("  Files Found = ");
		pfield((long) curfile, 4, 1);
		cputs("\r\nTotal 1k Blocks = ");
		pfield(tblk, 6, 1);
		cputs("   Used/Max Dir Entries For Drive ");
		conout('A' + curdrv);
		conout(':');
		pb();
		pfield((long) used_de, 4, 1);
		conout('/');
		pfield((long) dpb.drm + 1L, 4, 1);
	}

	if (curfile == 0) {
		if (msgopt) {
			crlfck();
			title();
			cputs("No File\r\n");
		}
	} else {
		displayed = 1;
		if (!ffopt)
			crlfck();
	}
}

/* ---------------- defaults and main ---------------- */

static VOID setdefaults()
{
	register int	i, v;

	if (!(f_dir || f_sys))
		f_dir = f_sys = 1;
	if (!(f_ro || f_rw))
		f_ro = f_rw = 1;

	if (f_xfcb && f_nonxfcb)	/* both is neither, as DIR+SYS is */
		f_xfcb = f_nonxfcb = 0;

	if (nspec == 0) {
		specany[0] = 1;
		specdrv[0] = (char) 0xff;
		nspec = 1;
	}
	if (drvvec == 0) {
		for (i = 0; i < nspec; i++) {
			if ((specdrv[i] & 0xff) == 0xff)
				specdrv[i] = (char) curdrv;
			setvec(&drvvec, specdrv[i] & 0xff);
		}
	} else {
		for (i = 0; i < nspec; i++)
			if ((specdrv[i] & 0xff) != 0xff
			    && (specdrv[i] & 0xff) != curdrv)
				die("ERROR: Illegal Global/Local Drive Spec Mixing.\r\n");
	}
	if (usrvec == 0)
		setvec(&usrvec, __bdos(BDOS_GETUSER, 0xffL) & 0xff);

	if (!ffopt && pagelen == 0xffff) {
		v = scbgetb(0x1c);		/* SCB console page length */
		pagelen = (v < 5) ? 24 : (unsigned) v;
	}
}

int main(argc, argv)
int argc;
char *argv[];
{
	register int	c, i, n;
	unsigned	saveu;

	n = _base->buff[0] & 0x7f;
	for (i = 0; i < n; i++)
		tail[i] = _base->buff[i + 1];
	tail[n] = 0;

	curdrv = __bdos(BDOS_CURDSK, 0L) & 0xff;
	nopage = scbgetb(0x2c);

	tp = 0;
	while ((c = next()) != 0) {
		if (c == '[')
			getoptions();
		else if (c == ']' || c == '=' || c == '(' || c == ')'
			 || c == ',')
			die("ERROR: Illegal command tail.\r\n");
		else
			getspec();
	}
	setdefaults();

	saveu = usrvec;
	for (i = 0; i < 16; i++) {
		if ((drvvec & ((unsigned) 1 << i)) == 0)
			continue;
		curdrv = i;
		__bdos(BDOS_SELDSK, (long) i);
		__bdos(BDOS_GETDPB, (long) &dpb);
		usrvec = saveu;
		getfiles();
		if (f_xfcb || f_nonxfcb)
			xfcbfilter();
		sortfiles();
		for (n = 0; n < 16; n++) {
			if ((usrvec & ((unsigned) 1 << n)) == 0)
				continue;
			curusr = n;
			display();
		}
	}

	if (!displayed && !msgopt)
		cputs("No File\r\n");
	return (0);
}
