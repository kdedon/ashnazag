/*
 * startcpm -- run CP/M-68K as this process.
 *
 *	startcpm [-e env]
 *
 * DRI's CPM.SYS (CCP and BDOS, linked with a sample BIOS) is loaded at
 * its own address in memory mapped at 0, and the process becomes a lone
 * guest: CP/M runs natively, its supervisor state virtual, its traps
 * through its own vector table.  The sample BIOS's init is skipped in
 * the loaded copy; trap #3 comes here instead, and trap #2 goes to our
 * own CP/M 3 BDOS.  The console is the caller's terminal.  Drives A: to
 * P: are the directories ~/CPM/A to ~/CPM/P (or, with -e, ~/CPM/env/A
 * ...).  Without A:, one is made holding links to the distribution's
 * files.  EXIT ends the session.
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
#include "tosio.h"
#include "bdos3.h"
#include "../include/envroot.h"

#define	MEM	0x400000L	/* memory at 0 */
#define	TABLES	0x400L		/* BIOS tables, up to CP/M */
#define	SYS	"/cpm/sys/CPM.SYS"
#define	DIST	"/cpm/dist"
#define	QUIT	0x7f		/* BIOS function: end the session, d1 the status */

extern void cpm_t2(), cpm_t3(), cpm_go(), cpm_wboot();

char cpm_stk[0x8000];		/* the BIOS's stack */
long cpm_ccp;			/* the CCP's warm start */

#define	root	b3_root
char *b3_mem;
static unsigned long tables = TABLES, tablim, mrt;
static int iobyte, lst = -1;
static int tty, peeked = -1, eof;
static struct termio tio;

/* EXIT.68K: move.w #QUIT,d0; moveq #0,d1; trap #3 */
static unsigned char exitprg[] = {
	0x60, 0x1a, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0x30, 0x3c, 0, QUIT, 0x72, 0, 0x4e, 0x43,
	0, 0, 0, 0, 0, 0, 0, 0		/* no relocations */
};

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

/* A:, made from the distribution when missing */
static void
drivea()
{
	char path[1100];
	struct stat sb;

	sprintf(path, "%s/a", root);
	if (stat(path, &sb) == 0)
		return;
	sprintf(path, "%s/A", root);
	if (stat(path, &sb) == 0)
		return;
	if (hf_mkdist(path, DIST, (char *)exitprg, "exit.68k", (int)sizeof exitprg) < 0)
		die(path);
	fprintf(stderr, "startcpm: made A: in %s\n", path);
	sprintf(path, "%s/a.img", root);
	if (stat(path, &sb) == 0)
		fprintf(stderr, "startcpm: %s is no longer used; cpmtools copies its files out\n", path);
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
	struct tosenter te;
	struct passwd *pw;
	unsigned long pc;
	char *env = 0, *h;
	int c, fd;

	while ((c = getopt(argc, argv, "e:")) != -1)
		if (c == 'e')
			env = optarg;
		else {
			fprintf(stderr, "usage: startcpm [-e env]\n");
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
	if ((fd = open("/dev/zero", O_RDWR)) < 0)
		die("/dev/zero");
	if (mmap((caddr_t)0, MEM, PROT_READ | PROT_WRITE | PROT_EXEC,
	    MAP_PRIVATE | MAP_FIXED, fd, 0) == (caddr_t)-1)
		die("mmap");
	close(fd);
	pc = loadsys();
	drivea();
	b3_init((unsigned long)cpm_t2, get32(mrt + 2), MEM, galloc(16L + 65536L / 8 + 128));
	put32(34 * 4L, (unsigned long)cpm_t2);
	put32(35 * 4L, (unsigned long)cpm_t3);
	if ((fd = open("/dev/tos", O_RDWR)) < 0)
		die("/dev/tos");
	fcntl(fd, F_SETFD, 1);
	te.te_ramsize = MEM;
	te.te_flags = TEF_NOMACH;
	if (ioctl(fd, TOSIOC_ENTER, &te) < 0)
		die("TOSIOC_ENTER");
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
