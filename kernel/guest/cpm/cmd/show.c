/*
 * Portions Copyright (c) 2026 Kevin Dedon.
 * SPDX-License-Identifier: MIT
 */
/*
 * show.c - SHOW, the CP/M 3 drive/space/user reporter, for the C900.
 *
 *	SHOW			free space on every logged-in drive
 *	SHOW d:			free space on one drive
 *	SHOW [SPACE]		the same, explicitly
 *	SHOW d:[DRIVE]		the drive's characteristics
 *	SHOW [USERS]		active users and their file counts
 *	SHOW [DIR]		directory-entry usage
 *	SHOW [LABEL]		the directory label
 *
 * Free space comes from function 46 as a four-byte native record count.
 * Function 31 copies the DPB to the caller; this DPB has no physical-record
 * fields. Directory labels are read directly from type-20h entries.
 */

#include "cpm.h"

#define	BDOS_SELDSK	14
#define	BDOS_LOGINVEC	24
#define	BDOS_CURDSK	25
#define	BDOS_RODSKVEC	29
#define	BDOS_GETDPB	31
#define	BDOS_GETUSER	32
#define	BDOS_FREESP	46
#define	BDOS_RAWIO	6

#define	E5		0xe5
#define	LABELTYPE	0x20
#define	SFCBTYPE	0x21

#define	DL_EXISTS	0x01
#define	DL_MAKEXFCB	0x10
#define	DL_UPDATE	0x20
#define	DL_ACCESS	0x40
#define	DL_PASSWORD	0x80

#define	O_SPACE		0
#define	O_DIR		1
#define	O_DRIVE		2
#define	O_LABEL		3
#define	O_USER		4
#define	NOPT		5

struct dpb {				/* sys/bdosdef.h:112-124	*/
	unsigned	spt;
	char		bsh;
	char		blm;
	char		exm;
	char		dpbdum;
	unsigned	dsm;
	unsigned	drm;
	unsigned	dir_al;
	unsigned	cks;
	unsigned	trk_off;
};

static struct dpb	dpb;
static struct fcb	qfcb;
static char		dirbuf[SECLEN];

static char	optmap[16][NOPT];
static char	drives[16];
static int	cdisk, usercode, drive;
static int	allflag, onceonly, pageon, linepage, lineout;
static int	donedrive[16];
static int	usrseen[16];
static unsigned	usrused[16];
static unsigned	freedir, nsfcb;

static int	scbpb[2];
static char	tail[SECLEN + 2];
static int	tp;
static char	tok[20];
static int	toklen;

static char *optname[] = {
	"SPACE", "DIRECTORY", "DRIVES", "LABEL", "USERS", "PAGE", "NOPAGE"
};
#define	NKEY	7

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

/* ---------------- output ---------------- */

static VOID crlf2()
{
	cputs("\r\n");
}

/* the paging newline (show.plm:274-292) */
static VOID nl()
{
	register int	c;

	if (pageon) {
		lineout++;
		if (lineout + 2 > linepage) {
			crlf2();
			crlf2();
			cputs("Press RETURN to continue.");
			while (__bdos(BDOS_CONST, 0L) == 0)
				;
			c = __bdos(BDOS_CONIN, 0L) & 0xff;
			if (c == 3)
				__bdos(BDOS_WBOOT, 0L);
			lineout = 1;
			crlf2();
		}
	}
	crlf2();
}

/* show.plm:296-303 -- start a new line, then the text */
static VOID pr(s)
char *s;
{
	nl();
	cputs(s);
}

static VOID eprint(s)
char *s;
{
	pr("ERROR: ");
	cputs(s);
}

static VOID showdrive()
{
	conout('A' + cdisk);
	cputs(": ");
}

/*
 * v3's p3byte/printbcd (show.plm:926-994): seven digit positions with a
 * comma after the seventh and the fourth, leading zeros (and the commas
 * that precede only zeros) blanked -- nine columns, always.
 */
static VOID p3(v)
long v;
{
	char		val[7];
	register int	i;
	int		zsup;

	for (i = 0; i < 7; i++) {
		val[i] = (char) (v % 10L);
		v /= 10L;
	}
	zsup = 1;
	for (i = 6; i >= 0; i--) {
		if (val[i] == 0 && zsup && i != 0)
			conout(' ');
		else {
			conout('0' + val[i]);
			zsup = 0;
		}
		if (i == 6 || i == 3) {
			if (val[i] == 0 && zsup && i != 0)
				conout(' ');
			else {
				conout(',');
				zsup = 0;
			}
		}
	}
}

/* pdecimal(v,prec,true): right aligned, blank padded (show.plm:878-901) */
static VOID pdec(v, width)
long v;
int width;
{
	char		d[12];
	register int	n, i;

	n = 0;
	do {
		d[n++] = (char) ('0' + (int) (v % 10L));
		v /= 10L;
	} while (v != 0L && n < 12);
	for (i = n; i < width; i++)
		conout(' ');
	while (n > 0)
		conout(d[--n]);
}

/* a two-digit zero-padded field, the shape of v3's emit$slant */
static VOID pdec2(v)
long v;
{
	conout('0' + (int) ((v / 10L) % 10L));
	conout('0' + (int) (v % 10L));
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

/* "MM/DD/YY HH:MM", the 14 columns display$ts writes (show.plm:812-824) */
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
	pdec2((long) m);
	conout('/');
	pdec2(n);
	conout('/');
	pdec2((long) (y % 100));
	conout(' ');
	conout('0' + ((p[2] >> 4) & 0x0f));
	conout('0' + (p[2] & 0x0f));
	conout(':');
	conout('0' + ((p[3] >> 4) & 0x0f));
	conout('0' + (p[3] & 0x0f));
}

/* ---------------- disk access ---------------- */

static VOID seldisk(d)
int d;
{
	cdisk = d;
	__bdos(BDOS_SELDSK, (long) d);
	__bdos(BDOS_GETDPB, (long) &dpb);
}

/*
 * Walk the directory.  relog nonzero just forces the drive to be logged
 * in (show.plm:1399-1412); otherwise the type-20h label entry is
 * returned, or 0 if this drive has none.
 */
static char *readlbl(relog)
int relog;
{
	register int	rc;
	register char	*e;

	setdma(dirbuf);
	qfcb.drvcode = '?';
	rc = __bdos(BDOS_SFIRST, (long) &qfcb) & 0xff;
	if (relog)
		return ((char *) 0);
	while (rc != 0xff) {
		e = &dirbuf[(rc & 3) << 5];
		if ((e[0] & 0xff) == LABELTYPE)
			return (e);
		rc = __bdos(BDOS_SNEXT, 0L) & 0xff;
	}
	return ((char *) 0);
}

/* ---------------- reports ---------------- */

/* free kilobytes on the current drive (show.plm:1174-1188): function 46
   leaves a long record count at the DMA address */
static VOID prcount()
{
	long	recs;

	recs = 0;
	setdma((char *) &recs);
	__bdos(BDOS_FREESP, (long) cdisk);
	p3(recs >> 3);
	conout('k');
}

static VOID stat(ro)
int ro;
{
	nl();
	showdrive();
	conout('R');
	conout(ro ? 'O' : 'W');
	cputs(", Space: ");
	prcount();
}

static VOID prstatus()
{
	unsigned	login, rodisk;
	register int	d;
	int		save;

	if (onceonly)
		return;
	save = cdisk;
	login = (unsigned) __bdos(BDOS_LOGINVEC, 0L);
	rodisk = (unsigned) __bdos(BDOS_RODSKVEC, 0L);
	d = 0;
	while (login != 0) {
		if (login & 1) {
			if (!allflag) {
				if (d == save)
					stat(rodisk & 1);
			} else {
				seldisk(d);
				stat(rodisk & 1);
			}
		}
		login >>= 1;
		rodisk >>= 1;
		d++;
	}
	if (allflag)
		onceonly = 1;
	nl();
}

/* show.plm:1087-1145 */
static VOID drivestatus()
{
	long	cap;

	pr("        ");
	showdrive();
	cputs("Drive Characteristics");

	cap = ((long) dpb.dsm + 1L) << (dpb.bsh & 0xff);
	nl();
	p3(cap);
	cputs(": 128 Byte Record Capacity");
	nl();
	p3(cap >> 3);
	cputs(": Kilobyte Drive  Capacity");
	nl();
	p3((long) dpb.drm + 1L);
	cputs(": 32 Byte  Directory Entries");
	nl();
	p3((long) dpb.cks << 2);
	cputs(": Checked  Directory Entries");
	nl();
	p3(((long) (dpb.exm & 0xff) + 1L) * 128L);
	cputs(": Records / Directory Entry");
	nl();
	p3(1L << (dpb.bsh & 0xff));
	cputs(": Records / Block");
	nl();
	p3((long) dpb.spt);
	cputs(": Records / Track");
	nl();
	p3((long) dpb.trk_off);
	cputs(": Reserved  Tracks");
	nl();
	cputs("           (Bytes / Physical Record: this BDOS's disk");
	nl();
	cputs("            parameter block has no psh field to report)");
	nl();
}

/* show.plm:1268-1313 */
static VOID getusrfiles()
{
	register int	rc, u;
	register char	*e;
	unsigned	nfcbs;

	for (u = 0; u < 16; u++) {
		usrseen[u] = 0;
		usrused[u] = 0;
	}
	nsfcb = 0;
	nfcbs = 0;

	setdma(dirbuf);
	qfcb.drvcode = '?';
	rc = __bdos(BDOS_SFIRST, (long) &qfcb) & 0xff;
	while (rc != 0xff) {
		e = &dirbuf[(rc & 3) << 5];
		u = e[0] & 0xff;
		if (u != E5) {
			if (u != SFCBTYPE) {
				nfcbs++;
				usrseen[u & 0x0f] = 1;
				if (u <= 15
				    && (e[12] & 0xff) <= (dpb.exm & 0xff)
				    && (e[14] & 0xff) == 0)
					usrused[u & 0x0f]++;
			} else
				nsfcb++;
		}
		rc = __bdos(BDOS_SNEXT, 0L) & 0xff;
	}
	donedrive[cdisk] = 1;
	if (nsfcb > 0)			/* the search stops at the high */
		nsfcb = (dpb.drm + 1) >> 2;	/* water mark (show.plm:1310) */
	freedir = (dpb.drm + 1) - nsfcb - nfcbs;
}

static VOID prdir()
{
	nl();
	nl();
	showdrive();
	if (nsfcb > 0) {
		cputs("Number of time/date directory entries: ");
		pdec((long) nsfcb, 4);
		nl();
		showdrive();
	}
	cputs("Number of free directory entries:      ");
	pdec((long) freedir, 4);
	nl();
}

static VOID userstatus()
{
	register int	i;

	nl();
	showdrive();
	cputs("Active User :");
	pdec((long) usercode, 4);
	nl();
	showdrive();
	cputs("Active Files:");
	if (!donedrive[cdisk])
		getusrfiles();
	for (i = 0; i < 16; i++)
		if (usrseen[i])
			pdec((long) i, 4);
	nl();
	showdrive();
	cputs("# of files  :");
	for (i = 0; i < 16; i++)
		if (usrseen[i])
			pdec((long) usrused[i], 4);
	prdir();
}

static VOID directory()
{
	if (!donedrive[cdisk])
		getusrfiles();
	prdir();
}

/* show.plm:1417-1470 */
static VOID labelstatus()
{
	register char	*e;
	register int	k;
	int		lbl;

	e = readlbl(0);
	if (e == (char *) 0) {
		eprint("No directory label exists on drive ");
		conout('A' + cdisk);
		nl();
		return;
	}
	lbl = e[12] & 0xff;

	pr("Label for drive ");
	showdrive();
	nl();
	pr("Directory     Passwds  Stamp   Stamp");
	pr("Label         Reqd     ");
	cputs((lbl & DL_ACCESS) ? "Access" : "Create");
	cputs("  Update  Label Created   Label Updated");
	pr("------------  -------  ------  ------  --------------  --------------");
	nl();

	for (k = 1; k <= 11; k++) {
		if (k == 9)
			conout('.');
		conout(e[k] & 0x7f);
	}
	cputs((lbl & DL_PASSWORD) ? "    on   " : "    off  ");
	if ((lbl & DL_ACCESS) || (lbl & DL_MAKEXFCB))
		cputs("   on   ");
	else
		cputs("   off  ");
	cputs((lbl & DL_UPDATE) ? "   on " : "   off");
	cputs("    ");
	putstamp(&e[24]);
	cputs("  ");
	putstamp(&e[28]);
	nl();
}

static VOID dooption(i)
int i;
{
	if (optmap[i][O_SPACE])
		prstatus();
	if (optmap[i][O_LABEL])
		labelstatus();
	if (optmap[i][O_DRIVE])
		drivestatus();
	if (optmap[i][O_USER])
		userstatus();
	if (optmap[i][O_DIR])
		directory();
}

/* ---------------- command tail ---------------- */

static VOID skipbl()
{
	while (tail[tp] == ' ' || tail[tp] == '\t' || tail[tp] == ',')
		tp++;
}

static int next()
{
	register int	c;

	skipbl();
	toklen = 0;
	tok[0] = 0;
	c = tail[tp] & 0xff;
	if (c == 0)
		return (0);
	if (c == '[' || c == ']' || c == ':') {
		tp++;
		tok[0] = (char) c;
		tok[1] = 0;
		toklen = 1;
		return (c);
	}
	while ((c = tail[tp] & 0xff) != 0 && c != ' ' && c != '\t'
	       && c != ',' && c != '[' && c != ']' && c != ':') {
		if (toklen < 19)
			tok[toklen++] = (char) (c >= 'a' && c <= 'z'
						? c - 32 : c);
		tp++;
	}
	tok[toklen] = 0;
	return (tok[0] & 0xff);
}

/*
 * The unique-prefix match of show.plm:1491-1705: the input must be a
 * prefix of exactly one keyword.  Returns 1..NKEY, or 0 for no match or
 * an ambiguous one.
 */
static int keyword()
{
	register int	i, j;
	int		hit;

	hit = 0;
	for (i = 0; i < NKEY; i++) {
		for (j = 0; j < toklen; j++)
			if (optname[i][j] == 0 || optname[i][j] != tok[j])
				break;
		if (j == toklen) {
			if (hit != 0)
				return (0);	/* ambiguous */
			hit = i + 1;
		}
	}
	return (hit);
}

static VOID setdefdrive()
{
	if (drive == 0xff) {
		drive = cdisk;
		drives[drive] = (char) drive;
	}
}

static VOID parser()
{
	register int	c, k;

	drive = 0xff;
	c = next();
	if (c == 0) {				/* bare SHOW		*/
		setdefdrive();
		optmap[drive][O_SPACE] = 1;
		allflag = 1;
		return;
	}
	while (c != 0) {
		if (c == '[') {
			setdefdrive();
			if (optmap[drive][O_SPACE] == (char) 0xff)
				optmap[drive][O_SPACE] = 0;
			c = next();
			if (c == ']' || c == 0) {	/* SHOW []	*/
				optmap[drive][O_SPACE] = 1;
				c = (c == 0) ? 0 : next();
				continue;
			}
			while (c != 0 && c != ']') {
				if ((k = keyword()) == 0) {
					eprint("Unrecognized Option.");
					pr("OPTION: ");
					cputs(tok);
					nl();
					__bdos(BDOS_WBOOT, 0L);
				}
				if (k == 6)
					pageon = 1;
				else if (k == 7)
					pageon = 0;
				else
					optmap[drive][k - 1] = 1;
				c = next();
			}
			if (c == ']')
				c = next();
			drive = 0xff;
			continue;
		}
		if (toklen == 1 && tok[0] >= 'A' && tok[0] <= 'P') {
			drive = tok[0] - 'A';
			drives[drive] = (char) drive;
			optmap[drive][O_SPACE] = (char) 0xff;
			c = next();
			if (c == ':')
				c = next();
			continue;
		}
		eprint("Unrecognized drive.");
		pr("DRIVE: ");
		cputs(tok);
		nl();
		__bdos(BDOS_WBOOT, 0L);
	}
}

int main(argc, argv)
int argc;
char *argv[];
{
	register int	i, n;

	n = _base->buff[0] & 0x7f;
	for (i = 0; i < n; i++)
		tail[i] = _base->buff[i + 1];
	tail[n] = 0;

	cdisk = __bdos(BDOS_CURDSK, 0L) & 0xff;
	usercode = __bdos(BDOS_GETUSER, 0xffL) & 0xff;
	for (i = 0; i < 16; i++)
		drives[i] = (char) 0xff;

	pageon = (scbgetb(0x2c) == 0);
	linepage = scbgetb(0x1c);
	if (linepage < 5)		/* GENCPM owns this byte and has	*/
		linepage = 24;		/*  not been run: use v3's default */

	tp = 0;
	parser();

	for (i = 0; i < 16; i++) {
		if ((drives[i] & 0xff) == 0xff)
			continue;
		seldisk(drives[i] & 0xff);
		readlbl(1);		/* force the drive logged in	*/
		if (optmap[i][O_SPACE] == (char) 0xff)
			optmap[i][O_SPACE] = 1;
		dooption(i);
	}
	crlf2();
	return (0);
}
