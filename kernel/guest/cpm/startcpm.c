/*
 * startcpm -- run Concurrent CP/M-68K as Unix processes.
 *
 *	startcpm [-e env [-b base]] [-d env] [-m MB] [-D X=path] ...
 *	startcpm [-e env] -c N		join the running environment as console N
 *	startcpm -d env | -l		set the default environment, list them
 *
 * Memory is mapped at 0 (16 MB by default) and the process becomes a
 * lone guest: CP/M runs natively, its supervisor state virtual, its
 * traps through its own vector table.  Trap #2 is our CP/M 3 BDOS,
 * trap #3 our BIOS.  CCP.68K, the command processor, is loaded again
 * at every warm boot.  Each CP/M process is a Unix process; all of them
 * share memory from XPRIV up, so a pointer means the same in each.
 * Console 0 is the caller's terminal; consoles 1-3 are sessions that
 * wait for "startcpm -c N" to join from another terminal.  Drives:
 *	A:	~/CPM (environment ccpm, else ~/CPM/env; -b base starts a new one as
 *		a copy of base's), user areas as subdirectories
 *	H:	home
 *	M:	~/.cpm/tmp (with -e, ~/.cpm/tmp/env), private, emptied at start
 *	O:	/
 *	P:	DRI's programs, read-only; programs not found elsewhere run from here
 * Lines X=path in ~/CPM/DRIVES, then -D, add or replace drives; MOUNT
 * (on P:) changes them in a session.  EXIT ends the session.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <pwd.h>
#include <dirent.h>
#include <termio.h>
#include "../include/cpmio.h"
#include "bdos3.h"
#include "xdos.h"
#include "cpmshim.h"
#include "../include/envroot.h"
#include "../include/envmem.h"

#define	MEMMB	16		/* memory at 0, MB */
#define	TABLES	0x400L		/* BIOS tables, up to XPRIV */
#define	CCP	"/cpm/sys/CCP.68K"
#define	DIST	"/cpm/dist"
#define	CONS	4		/* consoles */
#define	QUIT	0x7f		/* BIOS function: end the session, d1 the status */
#define	MOUNT	0x7e		/* BIOS function: MOUNT, d1 its base page */
#define	EXC	0x7c		/* BIOS function: an exception, d1 its vector */
#define	SVLEN	0x600		/* the CCP's state; it starts with magic.w, then: */
#define	SV_PEND	2		/* .w: a program to run at the warm boot */
#define	SV_FCB	4		/* its FCB, base page FCBs 1 and 2, tail length, tail */
#define	SV_FCB1	40
#define	SV_FCB2	76
#define	SV_TLEN	112
#define	SV_TAIL	113

extern void cpm_t2(), cpm_t3(), cpm_wboot(), cpm_enter();

char cpm_stk[0x8000];		/* the BIOS's stack */

#define	root	b3_root
char *b3_mem;
static unsigned long mem, tables = TABLES, tablim = XPRIV, mrt, retstub, excstub;
static int iobyte, lst = -1;
static int tty, peeked = -1, eof, cin, cout = 1, isroot = 1, session = 1;
static int contty, curcon, homecon, homein, homeout = 1, hometty, con0in, con0out = 1, con0tty;
static struct termio tio;
static char tmpdir[1100];
static unsigned char *ccpimg, sv[SVLEN];
static long ccplen;
int b3_conshared;
static void
die(what)
	char *what;
{
	fprintf(stderr, "startcpm: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void
put16(a, v)
	unsigned long a, v;
{
	unsigned char *p = (unsigned char *)a;

	p[0] = v >> 8;
	p[1] = v;
}

static void
put32(a, v)
	unsigned long a, v;
{
	put16(a, v >> 16);
	put16(a + 2, v);
}

static unsigned long
get16(a)
	unsigned long a;
{
	unsigned char *p = (unsigned char *)a;

	return p[0] << 8 | p[1];
}

static unsigned long
get32(a)
	unsigned long a;
{
	return get16(a) << 16 | get16(a + 2);
}

/* zeroed space for BIOS tables */
static unsigned long
galloc(n)
	unsigned long n;
{
	unsigned long a = tables;

	if ((tables = (a + n + 3) & ~3L) > tablim) {
		fprintf(stderr, "startcpm: no room for the BIOS tables\n");
		exit(1);
	}
	memset((char *)a, 0, n);
	return a;
}

static char obuf[512];
static int nobuf;
static void conlost(), quit();

static void
flush()
{
	int n = nobuf;

	nobuf = 0;
	if (n && write(cout, obuf, n) < 0 && errno == EPIPE) {
		if (curcon != homecon)
			b3_xdos(148L, (unsigned long)homecon);
		else
			quit(0);
	}
}

static void
restore()
{
	if (tty)
		ioctl(0, TCSETAW, &tio);
	tty = 0;
}


/* the session ends; a joined console's session waits for the next */
static void
quit(st)
	int st;
{
	flush();
	if (!isroot) {
		if (session)
			conlost();
		xexit(st);
	}
	restore();
	xkillall();
	exit(st);
}

static void
onsig(sig)
	int sig;
{
	if (!isroot)
		b3_exit(128 + sig);
	quit(128 + sig);
}

void
b3_exit(st)
	int st;
{
	flush();
	if (isroot) {
		restore();
		xkillall();
		exit(st);
	}
	_exit(st);
}

void
b3_conflush()
{
	flush();
}

static void
rawtty()
{
	struct termio t;

	if (ioctl(0, TCGETA, &tio) < 0)
		return;
	t = tio;
	t.c_iflag &= ~(ICRNL | INLCR | IGNCR | IXON | ISTRIP | BRKINT);
	t.c_lflag &= ~(ICANON | ECHO | ISIG);
	t.c_oflag &= ~OPOST;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (ioctl(0, TCSETAW, &t) == 0)
		tty = contty = hometty = 1;
}

/* ---- drives ---- */

long cpm_bios();
static char *home;

static int
samecase(a, b)
	char *a, *b;
{
	for (; *a && (*a | 32) == (*b | 32); a++, b++)
		;
	return *a == 0 && *b == 0;
}

static void
cputs(s)
	char *s;
{
	for (; *s; s++) {
		if (*s == '\n')
			cpm_bios(4L, (long)'\r', 0L);
		cpm_bios(4L, (long)*s, 0L);
	}
}

/* ~ and relative names start at home */
static int
fullpath(p, out)
	char *p, *out;
{
	if (strlen(home) + strlen(p) + 2 > 1024)
		return -1;
	if (*p == '/')
		strcpy(out, p);
	else if (*p == '~' && (p[1] == 0 || p[1] == '/'))
		sprintf(out, "%s%s", home, p + 1);
	else
		sprintf(out, "%s/%s", home, p);
	return 0;
}

/* "X=path": 0, or -1 when it is not one; an empty path removes X: */
static int
setmap(spec)
	char *spec;
{
	char path[1024];
	int d = *spec;

	d = d >= 'a' && d <= 'z' ? d - 'a' : d - 'A';
	if (d < 0 || d >= NDRV || spec[1] != '=')
		return -1;
	if (spec[2] == 0) {
		hf_setdrive(d, "", 0);
		return 0;
	}
	if (fullpath(spec + 2, path) < 0)
		return -1;
	hf_setdrive(d, path, 0);
	return 0;
}

static void
tmppath(env, path)
	char *env, *path;
{
	sprintf(path, "%s/.cpm/tmp", home);
	if (env)
		sprintf(path + strlen(path), "/%s", env);
}

/* M:, private and empty */
static void
tmpdrive(env, path)
	char *env, *path;
{
	char f[1400];
	struct dirent *de;
	struct stat sb;
	DIR *dp;

	sprintf(path, "%s/.cpm", home);
	mkdir(path, 0700);
	strcat(path, "/tmp");
	mkdir(path, 0700);
	tmppath(env, path);
	mkdir(path, 0700);
	if (lstat(path, &sb) < 0 || (sb.st_mode & S_IFMT) != S_IFDIR || sb.st_uid != getuid() ||
	    chmod(path, 0700) < 0) {
		path[0] = 0;
		return;
	}
	if ((dp = opendir(path)) == 0)
		return;
	while ((de = readdir(dp)) != 0) {
		sprintf(f, "%s/%s", path, de->d_name);
		if (lstat(f, &sb) == 0 && (sb.st_mode & S_IFMT) != S_IFDIR)
			unlink(f);
	}
	closedir(dp);
}

static void
drives(env, nd, dv)
	char *env, **dv;
	int nd;
{
	char path[1100], line[1200];
	FILE *fp;
	int i, n = 0;

	hf_setdrive(0, root, HF_AREAS);
	hf_setdrive('H' - 'A', home, 0);
	tmpdrive(env, path);
	strcpy(tmpdir, path);
	hf_setdrive('M' - 'A', path, 0);
	hf_setdrive('O' - 'A', "/", 0);
	hf_setdrive('P' - 'A', DIST, HF_RO | HF_SYS);
	sprintf(path, "%s/CPM/DRIVES", home);
	if ((fp = fopen(path, "r")) != 0) {
		while (fgets(line, sizeof line, fp) != 0) {
			n++;
			line[strcspn(line, "\r\n")] = 0;
			if (line[0] && line[0] != '#' && setmap(line) < 0)
				fprintf(stderr, "startcpm: %s line %d: not X=path\n", path, n);
		}
		fclose(fp);
	}
	for (i = 0; i < nd; i++)
		if (setmap(dv[i]) < 0)
			fprintf(stderr, "startcpm: -D %s: not X=path\n", dv[i]);
}

/* the CCP gives the tail in upper case: each missing name is matched ignoring case */
static int
anycase(p)
	char *p;
{
	char *q, *e, c;
	struct dirent *de;
	struct stat sb;
	DIR *dp;
	int found;

	if (stat(p, &sb) == 0)
		return 0;
	for (q = p + 1; *q; q = *e ? e + 1 : e) {
		e = strchr(q, '/');
		if (e == 0)
			e = q + strlen(q);
		c = *e;
		*e = 0;
		if (stat(p, &sb) < 0) {
			q[-1] = 0;
			dp = opendir(q == p + 1 ? "/" : p);
			q[-1] = '/';
			found = 0;
			while (dp && !found && (de = readdir(dp)) != 0)
				if (samecase(de->d_name, q)) {
					strcpy(q, de->d_name);
					found = 1;
				}
			if (dp)
				closedir(dp);
			if (!found) {
				*e = c;
				return -1;
			}
		}
		*e = c;
	}
	return 0;
}

/* MOUNT X: path, or MOUNT alone to list the drives */
static void
mount(bp)
	unsigned long bp;
{
	char tail[130], path[1100], line[1200], *p;
	struct stat sb;
	int d, fl, n;

	if (bp < TABLES || bp > mem - 0x100)
		return;
	n = *(unsigned char *)(bp + 0x80);
	if (n > 127)
		n = 127;
	memcpy(tail, (char *)bp + 0x81, n);
	tail[n] = 0;
	for (p = tail; *p == ' '; p++)
		;
	if (*p == 0) {
		for (d = 0; d < NDRV; d++)
			if (*(p = hf_drive(d, &fl))) {
				sprintf(line, "%c: %s%s\n", 'A' + d, p, fl & HF_RO ? " (R/O)" : "");
				cputs(line);
			}
		return;
	}
	d = *p - 'A';
	if (d < 0 || d >= NDRV || p[1] != ':' || p[2] != ' ') {
		cputs("usage: MOUNT [X: path]\n");
		return;
	}
	for (p += 2; *p == ' '; p++)
		;
	p[strcspn(p, " ")] = 0;
	if (fullpath(p, path) < 0 || anycase(path) < 0 || stat(path, &sb) < 0 ||
	    (sb.st_mode & S_IFMT) != S_IFDIR) {
		sprintf(line, "MOUNT: no directory %s\n", p);
		cputs(line);
		return;
	}
	hf_setdrive(d, path, 0);
	b3_relog();
	sprintf(line, "%c: %s\n", 'A' + d, path);
	cputs(line);
}

/* ---- the BIOS ---- */

static int
conin()
{
	struct pollfd pf;
	unsigned char c;
	int n;

	flush();
	xconwait();
	if (peeked >= 0) {
		c = peeked;
		peeked = -1;
	} else {
		/* a process that attached the console after we waited reads first */
		pf.fd = cin;
		pf.events = POLLIN;
		while (poll(&pf, 1L, -1) < 0 || xconbusy())
			xconwait();
		do
			n = read(cin, (char *)&c, 1);
		while (n < 0 && errno == EINTR);
		if (n <= 0 && curcon != homecon) {
			b3_xdos(148L, (unsigned long)homecon);
			return conin();
		}
		if (n <= 0)
			quit(0);
	}
	if (c == 0x7f)
		return 8;
	return !contty && c == '\n' ? '\r' : c;
}

/* fl: show what was written first, as a program asking would expect */
static int
constat(fl)
	int fl;
{
	struct pollfd pf;
	unsigned char c;

	if (fl)
		flush();
	if (xconbusy())
		return 0;
	if (peeked >= 0 || eof)
		return 0xff;
	pf.fd = cin;
	pf.events = POLLIN;
	if (poll(&pf, 1L, 0) <= 0)
		return 0;
	if (read(cin, (char *)&c, 1) == 1)
		peeked = c;
	else
		eof = 1;	/* conin ends the session */
	return 0xff;
}

/* an exception CP/M has no handler for ends the program */
static void
exc(v, regs)
	long v;
	unsigned long regs;
{
	char b[80];

	sprintf(b, "\nException %ld at %lx\n", v, get32(regs + 56 + 8 + 2));
	cputs(b);
	b3_wboot();
}

long
cpm_bios(fn, d1, d2, regs)
	long fn, d1, d2;
	unsigned long regs;
{
	unsigned char c;
	long old;

	switch (fn & 0xffff) {
	case 2:
		return constat(1);
	case 3:
		return conin();
	case 4:
		if (nobuf == sizeof obuf)
			flush();
		obuf[nobuf++] = d1;
		if (b3_conshared && (d1 == '\n' || nobuf > 80))
			flush();
		return 0;
	case 5:
		if (lst < 0) {
			char path[1100];

			sprintf(path, "%s/lst.txt", root);
			lst = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
		}
		c = d1;
		if (lst >= 0)
			write(lst, (char *)&c, 1);
		return 0;
	case 7:
		return 0x1a;		/* no reader */
	case 13:
	case 14:
		return 1;		/* no disks below the BDOS */
	case 15:
		return 0xff;
	case 16:
		d1 &= 0xffff;
		if (d2 && (d2 >= mem || d2 + 2 * d1 > mem - 2))
			return d1;
		return d2 ? get16(d2 + 2 * d1) : d1;
	case 18:
		return mrt;
	case 19:
		return iobyte;
	case 20:
		iobyte = d1 & 0xff;
		return 0;
	case 22:
		d1 &= 0xffff;
		if (d1 > 255)
			return 0;
		old = get32(d1 * 4);
		if (d1 != 34 && d1 != 35)	/* traps #2 and #3 stay ours */
			put32(d1 * 4, d2);
		return old;
	case MOUNT:
		mount((unsigned long)d1);
		return 0;
	case B3_SHIM:
		return cpm_shim((unsigned long)d1);
	case EXC:
		exc(d1, regs);
	case QUIT:
		quit((int)(d1 & 0xff));
	}
	return 0;
}

/* the BDOS's view of the BIOS */
int
b3_conin()
{
	return conin();
}

int
b3_const()
{
	return constat(0);
}

void
b3_conout(c)
	int c;
{
	cpm_bios(4L, (long)c, 0L);
}

void
b3_list(c)
	int c;
{
	cpm_bios(5L, (long)c, 0L);
}

long
b3_bios(fn, d1, d2)
	long fn, d1, d2;
{
	return cpm_bios(fn, d1, d2, 0L);
}


/* ---- programs ---- */

/* 150, 151: the CCP's state out to its buffer, or back */
long
b3_ccpsv(a, put)
	unsigned long a;
	int put;
{
	if (put)
		memcpy((char *)sv, B3MEM(a), SVLEN);
	else
		memcpy(B3MEM(a), (char *)sv, SVLEN);
	return 0;
}

static void
setmrt(lo, hi)
	unsigned long lo, hi;
{
	put32(mrt + 2, lo);
	put32(mrt + 6, hi - lo);
}

/* a blank FCB, or one from the next word of the tail */
static char *
tailfcb(p, f)
	char *p;
	unsigned long f;
{
	int i, n;

	memset((char *)f, 0, 36);
	memset((char *)f + 1, ' ', 11);
	while (*p == ' ')
		p++;
	if (*p && p[1] == ':') {
		*(char *)f = (*p & 0x5f) - 'A' + 1;
		p += 2;
	}
	for (i = 0, n = 8; *p && *p != ' '; p++)
		if (*p == '.')
			i = 8, n = 11;
		else if (i < n)
			*(char *)(f + 1 + i++) = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
	return p;
}

/* run the loaded program: base page bp, its stack sp; tail n bytes at t */
static void
run(bp, sp, n, t, f1, f2)
	unsigned long bp, sp;
	int n;
	char *t, *f1, *f2;
{
	char tb[130], *p;

	if (n > 126)
		n = 126;
	*(char *)(bp + 0x80) = n;
	memcpy((char *)bp + 0x81, t, n);
	*(char *)(bp + 0x81 + n) = 0;
	if (f1) {
		memcpy((char *)bp + 0x5c, f1, 36);
		memcpy((char *)bp + 0x38, f2, 36);
	} else {
		memcpy(tb, t, n);
		tb[n] = 0;
		p = tailfcb(tb, bp + 0x5c);
		tailfcb(p, bp + 0x38);
	}
	b3_dma = b3_dma0 = bp + 0x80;
	sp -= 4;
	put32(sp, retstub);
	flush();
	cpm_enter(get32(bp + 8), sp);
}

/* console n for this process's I/O: 0 if it is there, -1 if nobody is at it */
int
b3_setcon(n)
	int n;
{
	char in[1200], out[1200];
	int i, o, t = 0;

	flush();
	if (n == curcon)
		return 0;
	if (n == homecon) {
		i = homein, o = homeout, t = hometty;
	} else if (n == 0) {
		i = con0in, o = con0out, t = con0tty;
	} else {
		fifo(n, in, out);
		if ((o = open(out, O_WRONLY | O_NONBLOCK)) < 0)
			return -1;
		if ((i = open(in, O_RDONLY | O_NONBLOCK)) < 0) {
			close(o);
			return -1;
		}
		fcntl(i, F_SETFL, 0);
		fcntl(o, F_SETFL, 0);
		fcntl(i, F_SETFD, 1);
		fcntl(o, F_SETFD, 1);
	}
	if (curcon != homecon && curcon != 0) {
		close(cin);
		close(cout);
	}
	cin = i;
	cout = o;
	contty = t;
	curcon = n;
	peeked = -1;
	eof = 0;
	return 0;
}

/*
 * Warm boot: the program the CCP left in its state, otherwise the CCP.
 * A process made by function 144 ends here.
 */
void
cpm_ccprun()
{
	unsigned long lo = xpbase(xmypart0), hi = lo + XPSIZE, bp, sp;
	long r;

	if (!session)
		xexit(0);
	b3_xdos(148L, (unsigned long)homecon);
	xwarm();
	if (sv[SV_PEND] || sv[SV_PEND + 1]) {
		sv[SV_PEND] = sv[SV_PEND + 1] = 0;
		if ((r = b3_ldfile((unsigned long)sv + SV_FCB, lo, hi, &bp, &sp)) == 0) {
			xname((char *)sv + SV_FCB + 1);
			run(bp, sp, sv[SV_TLEN], (char *)sv + SV_TAIL, (char *)sv + SV_FCB1,
			    (char *)sv + SV_FCB2);
		}
		cputs(r == 2 ? "\nProgram Load Error\n" : r == 3 ? "\nFile is not executable\n" :
		    "\nInsufficient memory\n");
	}
	if (b3_ldimg(ccpimg, ccplen, lo, hi, 0, &bp, &sp) != 0) {
		cputs("\nNo room for CCP.68K\n");
		quit(1);
	}
	xname("CCP     ");
	run(bp, sp, 0, "", (char *)0, (char *)0);
}

void
b3_wboot()
{
	cpm_shimabort();
	flush();
	cpm_wboot();
}

/* function 144's process, after the fork: run what the parent loaded */
void
b3_child(bp, sp, n, t)
	unsigned long bp, sp;
	int n;
	char *t;
{
	session = 0;
	isroot = 0;
	b3_conshared = 1;
	nobuf = 0;
	xforked();
	b3_settpa(bp, bp + XPSIZE);
	setmrt(bp, bp + XPSIZE);
	run(bp, sp, n, t, (char *)0, (char *)0);
}

/* ---- consoles ---- */

static void
fifo(n, in, out)
	int n;
	char *in, *out;
{
	sprintf(in, "%s/.con/%d.in", tmpdir, n);
	sprintf(out, "%s/.con/%d.out", tmpdir, n);
}

/* wait for console n to be joined, then start its CCP */
static void
conjoin(n)
	int n;
{
	char in[1200], out[1200];
	int k;

	fifo(n, in, out);
	for (;;) {
		while ((cin = open(in, O_RDONLY)) < 0)
			if (errno != EINTR)
				_exit(1);
		while ((cout = open(out, O_WRONLY)) < 0)
			if (errno != EINTR)
				_exit(1);
		fcntl(cin, F_SETFD, 1);
		fcntl(cout, F_SETFD, 1);
		homein = cin;
		homeout = cout;
		curcon = homecon = n;
		peeked = -1;
		eof = 0;
		nobuf = 0;
		if ((k = xjoined()) >= 0)
			break;
		cputs("\nNo memory for console\n");
		flush();
		close(cin);
		close(cout);
		xidle();
	}
	b3_settpa(xpbase(k), xpbase(k) + XPSIZE);
	setmrt(xpbase(k), xpbase(k) + XPSIZE);
	memset((char *)sv, 0, SVLEN);
	cpm_bdos3(13L, 0L, 0L);
	cpm_bdos3(32L, 0L, 0L);
	cpm_wboot();
}

/* the joined terminal went away: wait for the next */
static void
conlost()
{
	nobuf = 0;
	close(cin);
	close(cout);
	xidle();
	conjoin(xcon());
}

static void
consoles()
{
	char in[1200], out[1200];
	int n, fd;

	if (tmpdir[0] == 0)
		return;
	sprintf(in, "%s/.con", tmpdir);
	mkdir(in, 0700);
	/* the sessions keep this terminal, for function 148 */
	con0in = dup(0);
	con0out = dup(1);
	con0tty = isatty(0);
	fcntl(con0in, F_SETFD, 1);
	fcntl(con0out, F_SETFD, 1);
	for (n = 1; n < xncon; n++) {
		fifo(n, in, out);
		unlink(in);
		unlink(out);
		if (mkfifo(in, 0600) < 0 || mkfifo(out, 0600) < 0)
			continue;
		if (fork() == 0) {
			isroot = 0;
			tty = contty = hometty = 0;
			if ((fd = open("/dev/null", O_RDWR)) >= 0) {
				dup2(fd, 0);
				dup2(fd, 1);
				dup2(fd, 2);
				close(fd);
			}
			signal(SIGPIPE, SIG_IGN);
			signal(SIGINT, SIG_IGN);
			signal(SIGQUIT, SIG_IGN);
			xsession(n);
			conjoin(n);
		}
	}
}

/* startcpm -c n: this terminal is console n until its session ends */
static int
join(n)
	int n;
{
	char in[1200], out[1200], b[512];
	struct pollfd pf[2];
	struct flock fl;
	int fi, fo, lk, k;

	fifo(n, in, out);
	sprintf(b, "%s/.con/%d.lk", tmpdir, n);
	if (n < 1 || n >= CONS) {
		fprintf(stderr, "startcpm: consoles are 1 to %d\n", CONS - 1);
		return 2;
	}
	memset((char *)&fl, 0, sizeof fl);
	fl.l_type = F_WRLCK;
	if ((lk = open(b, O_RDWR | O_CREAT, 0600)) < 0 || fcntl(lk, F_SETLK, &fl) < 0 ||
	    (fi = open(in, O_WRONLY | O_NONBLOCK)) < 0) {
		fprintf(stderr, "startcpm: console %d is in use or the environment is not running\n", n);
		return 1;
	}
	fcntl(fi, F_SETFL, 0);
	if ((fo = open(out, O_RDONLY)) < 0)
		die(out);
	signal(SIGPIPE, SIG_IGN);
	rawtty();
	pf[0].fd = 0;
	pf[0].events = POLLIN;
	pf[1].fd = fo;
	pf[1].events = POLLIN;
	for (;;) {
		if (poll(pf, 2L, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pf[1].revents) {
			if ((k = read(fo, b, sizeof b)) <= 0)
				break;
			write(1, b, k);
		}
		if (pf[0].revents) {
			if ((k = read(0, b, sizeof b)) <= 0 || write(fi, b, k) != k)
				break;
		}
	}
	restore();
	return 0;
}

/* ---- start ---- */

static void
mkroot()
{
	char *p;

	for (p = root + 1; (p = strchr(p, '/')) != 0; p++) {
		*p = 0;
		mkdir(root, 0755);
		*p = '/';
	}
	if (mkdir(root, 0755) < 0 && errno != EEXIST)
		die(root);
}

/* exception stubs: moveq #v,d1; moveq #EXC,d0; trap #3 */
static void
vectors()
{
	unsigned long a;
	int v;

	excstub = galloc(6L * 48);
	for (v = 2; v < 48; v++) {
		a = excstub + 6 * v;
		put16(a, 0x7200L | v);
		put16(a + 2, 0x7000L | EXC);
		put16(a + 4, 0x4e43L);
		put32(v * 4L, a);
	}
	put32(34 * 4L, (unsigned long)cpm_t2);
	put32(35 * 4L, (unsigned long)cpm_t3);
	retstub = galloc(4L);
	put32(retstub, 0x70004e42L);	/* moveq #0,d0; trap #2 */
	mrt = galloc(10L);
	put16(mrt, 1L);
}

static void
loadccp()
{
	struct stat sb;
	int fd;

	if ((fd = open(CCP, O_RDONLY)) < 0 || fstat(fd, &sb) < 0)
		die(CCP);
	ccplen = sb.st_size;
	if ((ccpimg = malloc(ccplen)) == 0 || read(fd, (char *)ccpimg, ccplen) != ccplen)
		die(CCP);
	close(fd);
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct passwd *pw;
	struct rlimit rl;
	unsigned long lo;
	static char *envs[] = { "ccpm", 0 };
	char *env = 0, *def = 0, *base = 0, *h, *dv[NDRV], meta[1100];
	struct stat sb;
	int c, fd, nd = 0, con = 0, mb = MEMMB, list = 0;

	while ((c = getopt(argc, argv, "e:b:d:D:m:c:l")) != -1)
		if (c == 'e')
			env = optarg;
		else if (c == 'b')
			base = optarg;
		else if (c == 'd')
			def = optarg;
		else if (c == 'l')
			list = 1;
		else if (c == 'D' && nd < NDRV)
			dv[nd++] = optarg;
		else if (c == 'm' && (mb = atoi(optarg)) >= 2 && mb <= 256)
			;
		else if (c == 'c' && (con = atoi(optarg)) > 0)
			;
		else {
			fprintf(stderr, "usage: startcpm [-e env [-b base]] [-d env] [-m MB] [-D X=path] ...\n"
			    "       startcpm [-e env] -c console\n"
			    "       startcpm -d env | -l\n");
			return 2;
		}
	h = getenv("HOME");
	if ((h == 0 || *h == 0) && (pw = getpwuid(getuid())) != 0)
		h = pw->pw_dir;
	if (list) {
		envlist("cpm", h, "ccpm", envs);
		return 0;
	}
	if (def && envsetdefault("startcpm", "cpm", h, def) < 0)
		return 1;
	if (def && !env)
		return 0;
	if (base && (env == 0 || con)) {
		fprintf(stderr, "startcpm: -b goes with -e\n");
		return 2;
	}
	env = envchoose("cpm", env, h, "ccpm");
	if (envhome("cpm", env, "ccpm", h, root, sizeof root) < 0) {
		fprintf(stderr, "startcpm: %s: not an environment name\n", env);
		return 1;
	}
	if (strcmp(env, "ccpm") == 0)
		env = 0;
	home = h;
	if (con) {
		tmppath(env, tmpdir);
		return join(con);
	}
	if (base) {
		if (env == 0 || (envmeta("cpm", root, meta, sizeof meta) == 0 && stat(meta, &sb) == 0)) {
			fprintf(stderr, "startcpm: %s exists; -b makes a new environment\n", env);
			return 1;
		}
		if (envclone("startcpm", "cpm", env, base, h, "ccpm", root) < 0)
			return 1;
	}
	mkroot();
	if (envlock("startcpm", "cpm", root) == -1)
		return 1;
	mem = (unsigned long)mb << 20;
	if (getrlimit(RLIMIT_VMEM, &rl) == 0 && rl.rlim_cur < mem + (8L << 20)) {
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_VMEM, &rl);
	}
	if (envmem_region(0L, XPRIV, 0, -1) < 0 || envmem_region(XPRIV, mem - XPRIV, 1, -1) < 0)
		die("guest memory");
	loadccp();
	drives(env, nd, dv);
	if (xinit(mem, CONS, tmpdir[0] ? tmpdir : root) < 0) {
		fprintf(stderr, "startcpm: no memory for a partition\n");
		return 1;
	}
	vectors();
	lo = xpbase(xmypart0);
	setmrt(lo, lo + XPSIZE);
	b3_init((unsigned long)cpm_t2, lo, lo + XPSIZE, galloc(16L + 65536L / 8 + 128));
	if ((fd = open("/dev/cpm", O_RDWR)) < 0)
		die("/dev/cpm");
	fcntl(fd, F_SETFD, 1);
	if (ioctl(fd, CPMIOC_ENTER, 0) < 0)
		die("CPMIOC_ENTER");
	close(fd);
	signal(SIGCLD, SIG_IGN);
	signal(SIGHUP, onsig);
	signal(SIGTERM, onsig);
	/* Ctrl-C belongs to the program inside, never the session */
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	signal(SIGPIPE, onsig);
	signal(SIGSEGV, onsig);
	signal(SIGBUS, onsig);
	signal(SIGILL, onsig);
	consoles();
	rawtty();
	cpm_wboot();
	return 0;
}
