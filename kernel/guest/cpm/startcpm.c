/*
 * startcpm -- run CP/M-68K as this process.
 *
 *	startcpm [-e env] [-d X=path] ...
 *
 * DRI's CPM.SYS (CCP and BDOS, linked with a sample BIOS) is loaded at
 * its own address in memory mapped at 0, and the process becomes a lone
 * guest: CP/M runs natively, its supervisor state virtual, its traps
 * through its own vector table.  The sample BIOS's init is skipped in
 * the loaded copy; trap #3 comes here instead, and trap #2 goes to our
 * own CP/M 3 BDOS.  The console is the caller's terminal.  Drives:
 *	A:	~/CPM (with -e, ~/CPM/env), user areas as subdirectories
 *	H:	home
 *	M:	~/.cpm/tmp (with -e, ~/.cpm/tmp/env), private, emptied at start
 *	O:	/
 *	P:	DRI's programs, read-only; programs not found elsewhere run from here
 * The CCP knows only A: to P:.  Lines X=path in ~/CPM/DRIVES, then -d,
 * add or replace drives; MOUNT (on P:) changes them in a session.  EXIT ends the session.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
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
#include "cpmshim.h"
#include "../include/envroot.h"
#include "../include/envmem.h"

#define	MEM	0x400000L	/* memory at 0 */
#define	TABLES	0x400L		/* BIOS tables, up to CP/M */
#define	SYS	"/cpm/sys/CPM.SYS"
#define	DIST	"/cpm/dist"
#define	QUIT	0x7f		/* BIOS function: end the session, d1 the status */
#define	MOUNT	0x7e		/* BIOS function: MOUNT, d1 its base page */

extern void cpm_t2(), cpm_t3(), cpm_go(), cpm_wboot();

char cpm_stk[0x8000];		/* the BIOS's stack */
long cpm_ccp;			/* the CCP's warm start */

#define	root	b3_root
char *b3_mem;
static unsigned long tables = TABLES, tablim, mrt;
static int iobyte, lst = -1;
static int tty, peeked = -1, eof;
static struct termio tio;

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

static void
flush()
{
	int n = nobuf;

	nobuf = 0;
	if (n)
		write(1, obuf, n);
}

static void
restore()
{
	if (tty)
		ioctl(0, TCSETAW, &tio);
	tty = 0;
}

static void
quit(st)
	int st;
{
	flush();
	restore();
	exit(st);
}

static void
onsig(sig)
	int sig;
{
	quit(128 + sig);
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
		tty = 1;
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
	if (env) {
		sprintf(path + strlen(path), "/%s", env);
		mkdir(path, 0700);
	}
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
			fprintf(stderr, "startcpm: -d %s: not X=path\n", dv[i]);
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

	if (bp < TABLES || bp > MEM - 0x100)
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
	unsigned char c;
	int n;

	flush();
	if (peeked >= 0) {
		c = peeked;
		peeked = -1;
	} else {
		do
			n = read(0, (char *)&c, 1);
		while (n < 0 && errno == EINTR);
		if (n <= 0)
			quit(0);
	}
	if (c == 0x7f)
		return 8;
	return !tty && c == '\n' ? '\r' : c;
}

static int
constat()
{
	struct pollfd pf;
	unsigned char c;

	flush();
	if (peeked >= 0 || eof)
		return 0xff;
	pf.fd = 0;
	pf.events = POLLIN;
	if (poll(&pf, 1L, 0) <= 0)
		return 0;
	if (read(0, (char *)&c, 1) == 1)
		peeked = c;
	else
		eof = 1;	/* conin ends the session */
	return 0xff;
}

long
cpm_bios(fn, d1, d2)
	long fn, d1, d2;
{
	unsigned char c;
	long old;

	switch (fn & 0xffff) {
	case 2:
		return constat();
	case 3:
		return conin();
	case 4:
		if (nobuf == sizeof obuf)
			flush();
		obuf[nobuf++] = d1;
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
		if (d2 && (d2 >= MEM || d2 + 2 * d1 > MEM - 2))
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
	return constat();
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
	return cpm_bios(fn, d1, d2);
}

void
b3_wboot()
{
	cpm_shimabort();
	flush();
	cpm_wboot();
}

/* ---- start ---- */

/*
 * CPM.SYS at its own address.  Its cold start calls the BIOS's init
 * first, which in this copy only returns drive A:; the warm start is
 * where the cold start's stack setup recurs.
 */
static unsigned long
loadsys()
{
	unsigned char h[28];
	unsigned long ts, td, tb, base, cold, init, a;
	int fd;

	if ((fd = open(SYS, O_RDONLY)) < 0)
		die(SYS);
	if (read(fd, (char *)h, 28) != 28)
		goto bad;
	ts = get32((unsigned long)h + 2);
	td = get32((unsigned long)h + 6);
	tb = get32((unsigned long)h + 10);
	base = get32((unsigned long)h + 22);
	if (get16((unsigned long)h) != 0x601a || get16((unsigned long)h + 26) != 0xffff ||
	    base < 0x4000 || base + ts + td + tb > MEM / 2 ||
	    lseek(fd, 28L, 0) != 28 || read(fd, (char *)base, ts + td) != ts + td)
		goto bad;
	close(fd);
	tablim = base;
	cold = get32(base + 2);
	if (get16(base) != 0x4ef9 || cold < base || cold >= base + ts || get16(cold) != 0x4ff9)
		goto bad;
	for (a = cold + 6, init = 0; a < cold + 64 && !init; a += 2)
		if (get16(a) == 0x4eb9)
			init = get32(a + 2);
	for (a = cold + 6, cpm_ccp = 0; a < cold + 256 && !cpm_ccp; a += 2)
		if (get16(a) == 0x4ff9 && get32(a + 2) == get32(cold + 2))
			cpm_ccp = a;
	if (init < base || init >= base + ts || !cpm_ccp ||
	    get16(init) != 0x41f9 || get32(init + 6) != 0x23c80000 || get16(init + 10) != 0x8c)
		goto bad;
	put32(init, 0x70004e75L);	/* moveq #0,d0; rts */
	mrt = galloc(10L);
	a = (base + ts + td + tb + 0xfff) & ~0xfffL;
	put16(mrt, 1L);
	put32(mrt + 2, a);
	put32(mrt + 6, MEM - a);
	return base;
bad:
	fprintf(stderr, "startcpm: %s: not the CP/M-68K 1.3 system this BIOS knows\n", SYS);
	exit(1);
}

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

int
main(argc, argv)
	int argc;
	char **argv;
{
	struct passwd *pw;
	unsigned long pc;
	char *env = 0, *h, *dv[NDRV];
	int c, fd, nd = 0;

	while ((c = getopt(argc, argv, "e:d:")) != -1)
		if (c == 'e')
			env = optarg;
		else if (c == 'd' && nd < NDRV)
			dv[nd++] = optarg;
		else {
			fprintf(stderr, "usage: startcpm [-e env] [-d X=path] ...\n");
			return 2;
		}
	h = getenv("HOME");
	if ((h == 0 || *h == 0) && (pw = getpwuid(getuid())) != 0)
		h = pw->pw_dir;
	if (envroot("cpm", env, h, root, sizeof root) < 0) {
		fprintf(stderr, "startcpm: %s: not an environment name\n", env ? env : "~/CPM");
		return 1;
	}
	mkroot();
	if (envlock("startcpm", "cpm", root) == -1)
		return 1;
	if (envmem_region(0, MEM, 0, -1) < 0)
		die("guest memory");
	pc = loadsys();
	home = h;
	drives(env, nd, dv);
	b3_init((unsigned long)cpm_t2, get32(mrt + 2), MEM, galloc(16L + 65536L / 8 + 128));
	put32(34 * 4L, (unsigned long)cpm_t2);
	put32(35 * 4L, (unsigned long)cpm_t3);
	if ((fd = open("/dev/cpm", O_RDWR)) < 0)
		die("/dev/cpm");
	fcntl(fd, F_SETFD, 1);
	if (ioctl(fd, CPMIOC_ENTER, 0) < 0)
		die("CPMIOC_ENTER");
	close(fd);
	signal(SIGHUP, onsig);
	signal(SIGTERM, onsig);
	signal(SIGINT, onsig);
	signal(SIGQUIT, onsig);
	signal(SIGPIPE, onsig);
	signal(SIGSEGV, onsig);
	signal(SIGBUS, onsig);
	signal(SIGILL, onsig);
	rawtty();
	cpm_go(pc);
	return 0;
}
