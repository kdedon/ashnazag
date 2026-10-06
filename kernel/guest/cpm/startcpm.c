/*
 * startcpm -- run CP/M-68K as this process.
 *
 *	startcpm [-e env]
 *
 * DRI's CPM.SYS (CCP and BDOS, linked with a sample BIOS) is loaded at
 * its own address in memory mapped at 0, and the process becomes a lone
 * guest: CP/M runs natively, its supervisor state virtual, its traps
 * through its own vector table.  The sample BIOS's init is skipped in
 * the loaded copy; trap #3 comes here instead.  The console is the
 * caller's terminal.  Drives A: to P: are ~/CPM/x.img (or, with -e,
 * ~/CPM/env/x.img) in the layout cpmfs.h describes; a directory x/
 * there instead is copied into a drive at start, and changes to that
 * drive are not kept.  Without A:, an 8 MB a.img is made holding the
 * distribution's files.  EXIT ends the session.
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
#include "cpmfs.h"
#include "../include/envroot.h"

#define	MEM	0x400000L	/* memory at 0 */
#define	TABLES	0x400L		/* BIOS tables, up to CP/M */
#define	SYS	"/cpm/sys/CPM.SYS"
#define	DIST	"/cpm/dist"
#define	QUIT	0x7f		/* BIOS function: end the session, d1 the status */

extern void cpm_t3(), cpm_go();

char cpm_stk[0x8000];		/* the BIOS's stack */
long cpm_ccp;			/* the CCP's warm start */

static struct drive {
	struct cpmimg	d;
	unsigned long	dph;
} drv[16];
static char root[1024];
static unsigned long tables = TABLES, tablim, mrt;
static int cur = -1, trk, sec, iobyte, lst = -1;
static unsigned long dma = 0x80;
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

/* ---- files into drives ---- */

static char *
readfile(path, lenp)
	char *path;
	long *lenp;
{
	struct stat sb;
	char *b;
	int fd;

	if ((fd = open(path, O_RDONLY)) < 0)
		return 0;
	if (fstat(fd, &sb) < 0 || sb.st_size > CPM_HDMAX || (b = malloc(sb.st_size + 1)) == 0 ||
	    read(fd, b, sb.st_size) != sb.st_size) {
		close(fd);
		return 0;
	}
	close(fd);
	*lenp = sb.st_size;
	return b;
}

/* dir's files with 8.3 names into user 0; alias: X.REL also as X.68K */
static void
putdir(d, dir, alias)
	struct cpmimg *d;
	char *dir;
	int alias;
{
	char path[1100], n[11];
	struct dirent *de;
	struct stat sb;
	DIR *dp;
	char *b;
	long len;

	if ((dp = opendir(dir)) == 0)
		return;
	while ((de = readdir(dp)) != 0) {
		sprintf(path, "%.900s/%.100s", dir, de->d_name);
		if (stat(path, &sb) < 0 || (sb.st_mode & S_IFMT) != S_IFREG ||
		    cpm_name(de->d_name, n) < 0)
			continue;
		if ((b = readfile(path, &len)) == 0)
			continue;
		if (cpm_put(d, 0, n, b, len) < 0)
			fprintf(stderr, "startcpm: %s: no room or name taken\n", path);
		if (alias && memcmp(n + 8, "REL", 3) == 0) {
			memcpy(n + 8, "68K", 3);
			cpm_put(d, 0, n, b, len);
		}
		free(b);
	}
	closedir(dp);
}

/* a: made from the distribution */
static int
newa(path, d)
	char *path;
	struct cpmimg *d;
{
	if ((d->fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644)) < 0)
		return -1;
	cpm_format(CPM_HDMAX, &d->f);
	if (cpm_mkfs(d) < 0)
		die(path);
	putdir(d, DIST, 1);
	cpm_put(d, 0, "EXIT    68K", (char *)exitprg, (long)sizeof exitprg);
	fprintf(stderr, "startcpm: made A: in %s\n", path);
	return 0;
}

/* a host directory, copied into a drive of its own size */
static int
snapshot(dir, d)
	char *dir;
	struct cpmimg *d;
{
	char path[1100], n[11];
	struct dirent *de;
	struct stat sb;
	DIR *dp;
	long size = 64L << 10;

	if ((dp = opendir(dir)) == 0)
		return -1;
	while ((de = readdir(dp)) != 0) {
		sprintf(path, "%.900s/%.100s", dir, de->d_name);
		if (stat(path, &sb) == 0 && (sb.st_mode & S_IFMT) == S_IFREG &&
		    cpm_name(de->d_name, n) == 0)
			size += (sb.st_size + 4095) & ~4095L;
	}
	closedir(dp);
	size = (size + (32L << 10) + 16383) & ~16383L;	/* + the directory */
	if (size < CPM_HDMIN)
		size = CPM_HDMIN;
	if (size > CPM_HDMAX)
		size = CPM_HDMAX;
	cpm_format(size, &d->f);
	if ((d->mem = malloc(size)) == 0)
		return -1;
	memset(d->mem, 0xe5, size);
	putdir(d, dir, 0);
	return 0;
}

/* the drive's DPH, DPB, allocation vector and sector table */
static void
dph(v)
	struct drive *v;
{
	static unsigned long dirbuf;
	struct cpmfmt *f = &v->d.f;
	unsigned long dpb, xlt = 0;
	int i;

	if (!dirbuf)
		dirbuf = galloc(128L);
	if (f->skew) {
		xlt = galloc((unsigned long)f->spt * 2);
		for (i = 0; i < f->spt; i++)
			put16(xlt + 2 * i, (unsigned long)f->skew[i]);
	}
	dpb = galloc(16L);
	put16(dpb, (unsigned long)f->spt);
	((char *)dpb)[2] = f->bsh;
	((char *)dpb)[3] = (1 << f->bsh) - 1;
	((char *)dpb)[4] = f->exm;
	put16(dpb + 6, (unsigned long)f->dsm);
	put16(dpb + 8, (unsigned long)f->drm);
	put16(dpb + 10, (unsigned long)f->al);
	put16(dpb + 12, 0L);			/* CKS: fixed media */
	put16(dpb + 14, (unsigned long)f->off);
	v->dph = galloc(26L);
	put32(v->dph, xlt);
	put32(v->dph + 10, dirbuf);
	put32(v->dph + 14, dpb);
	put32(v->dph + 18, galloc(4L));
	put32(v->dph + 22, galloc((unsigned long)f->dsm / 8 + 1));
}

static void
drives()
{
	char path[1100];
	struct stat sb;
	struct drive *v;
	int c;

	for (c = 0; c < 16; c++) {
		v = &drv[c];
		sprintf(path, "%s/%c.img", root, 'a' + c);
		if (stat(path, &sb) == 0) {
			if ((sb.st_mode & S_IFMT) != S_IFREG) {
				fprintf(stderr, "startcpm: %s: not a file\n", path);
				continue;
			}
			if ((v->d.fd = open(path, O_RDWR)) < 0) {
				v->d.ro = 1;
				if ((v->d.fd = open(path, O_RDONLY)) < 0)
					die(path);
			}
			if (cpm_format((long)sb.st_size, &v->d.f) < 0) {
				fprintf(stderr, "startcpm: %s: size is not a known format\n", path);
				close(v->d.fd);
				memset((char *)&v->d, 0, sizeof v->d);
				continue;
			}
		} else {
			sprintf(path, "%s/%c", root, 'a' + c);
			if (stat(path, &sb) == 0 && (sb.st_mode & S_IFMT) == S_IFDIR &&
			    snapshot(path, &v->d) < 0)
				die(path);
			if (c == 0 && !v->d.f.size) {
				sprintf(path, "%s/a.img", root);
				if (newa(path, &v->d) < 0)
					die(path);
			}
		}
		if (v->d.f.size)
			dph(v);
	}
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

static long
rw(wr)
	int wr;
{
	struct drive *v;

	if (cur < 0 || dma > MEM - 128)
		return 1;
	v = &drv[cur];
	return cpm_io(&v->d, cpm_secoff(&v->d.f, (long)trk, (long)sec), (char *)dma, 128, wr) ? 1 : 0;
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
	case 8:
		trk = 0;
		return 0;
	case 9:
		d1 &= 0xff;
		if (d1 > 15 || !drv[d1].d.f.size)
			return 0;
		cur = d1;
		return drv[cur].dph;
	case 10:
		trk = d1 & 0xffff;
		return 0;
	case 11:
		sec = d1 & 0xffff;
		return 0;
	case 12:
		dma = d1;
		return 0;
	case 13:
		return rw(0);
	case 14:
		return rw(1);
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
		if (d1 != 35)		/* trap #3 stays the BIOS */
			put32(d1 * 4, d2);
		return old;
	case QUIT:
		quit((int)(d1 & 0xff));
	}
	return 0;
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
	drives();
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
