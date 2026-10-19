/*
 * starttos -- run Atari TOS as this process: EmuTOS, or the user's ROM image.
 *
 *	starttos [-rom file] [-c cartridge] [-e env [-b base] [-R] | -C dir | -i disk] [-d env]
 *		[-D L=dir] [-u dir | -U] [-m megabytes] [-M] [-S] [-v]
 *	starttos -d env | -l
 *
 * ST-RAM is a shared mapping at 0, the ROM file mapped read-only at its own
 * base, the machine-layer cartridge at $FA0000.  Drives are host
 * directories used with the caller's permissions: C:, the boot drive,
 * the environment's folder: ~/TOS for emutos, ~/TOS/env for the others,
 * copied from /tos/sys (and /tos/xaaes for xaaes) at first use and with
 * -R, keeping the user's other files.  A new name is a copy of base, by
 * default the default.  Without -e it is the default, which -d sets; -l
 * lists them.  U: is "/";
 * others come from /tos/sys/drives, ~/TOS/drives and -D.  -i makes C: a FAT image
 * (512-byte sectors) instead.  One writable session per environment:
 * a second is refused.  A child process owns the display session:
 * it converts the TOS screen to the frame buffer and passes keyboard
 * and mouse to the IKBD.  The parent enters the ROM's reset code.
 * -S keeps GEM on the ST screen: fVDI gets no frame buffer, for
 * programs that also write and read that screen themselves.  Files a -C
 * folder lacks, such as AUTO, come from the system's fVDI folder, so
 * it boots with fVDI unless -M or -S asks for the ST screen.
 * A second child, the sound pump, plays the guest's DMA sound on the
 * host's sound service.  A
 * third, the spawner, runs the MiNT command-line programs the guest
 * starts as Unix processes under mintrun, each on a pseudo-terminal.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <pwd.h>
#include "dsio.h"
#include "tosio.h"
#include "tosfb.h"
#include "hostcall.h"
#include "../include/envroot.h"
#include "../include/envmem.h"
#include <stropts.h>

#define	CART	0xfa0000
#define	CARTSZ	0x20000
#define	ALIAS	0xff000000
#define	IOPG	0x1000

/* the I/O pages, after the RAM in its object; each also at its 32-bit alias */
static unsigned long iowin[] = { 0xff8000, 0xff9000, 0xfff000 };
#define	NIO	(sizeof iowin / sizeof iowin[0])

static char *rom = "/etc/tos/emutos.img";	/* the free TOS; -rom for the user's */
static char *cart = "/etc/tos/tosml.img";
static char *disk;
static char *cdir;
static char *env, *ebase;
static int refresh;
static char *udir = "/";
static char *xdrv[26];			/* -D */
static char *tab[26];			/* drive -> host directory */
static char tro[26];			/* read-only */
static long ramsize = 4L << 20;
static int mono, stscreen, verbose, cfold;
static int tfd;
static struct fbinfo hfi;		/* the host screen, for 1-bit hosts */
extern int fbpipe;
extern char *tos_nvram;
extern unsigned long hc_stack[];
int tos_bell = -1;			/* the sound pump's doorbell */
unsigned long tos_romlo, tos_romsz;	/* the ROM, for the sound pump */

extern void disp(), tossnd();
extern int tos_patch();
extern char etext;
extern void _start();

static void
die(what)
	char *what;
{
	fprintf(stderr, "starttos: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void
put32(a, v)
	unsigned long a, v;
{
	unsigned char *p = (unsigned char *)a;

	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static unsigned long
get32(p)
	unsigned char *p;
{
	return (unsigned long)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* anonymous memory at a fixed address */
static void
region(a, n, shared)
	unsigned long a, n;
	int shared;
{
	if (envmem_region(a, n, shared, -1) < 0)
		die("guest memory");
}

static long
readall(name, buf, max)
	char *name, *buf;
	long max;
{
	int fd = open(name, O_RDONLY);
	long n = 0, k = 0;
	char c;

	if (fd < 0)
		die(name);
	while (n < max && (k = read(fd, buf + n, max - n)) > 0)
		n += k;
	if (k < 0)
		die(name);
	if (n == max && read(fd, &c, 1) == 1) {
		fprintf(stderr, "starttos: %s: larger than %ld bytes\n", name, max);
		exit(1);
	}
	close(fd);
	if (n < 16) {
		fprintf(stderr, "starttos: %s: too short\n", name);
		exit(1);
	}
	return n;
}

/*
 * The ROM, its size; its first 16 bytes in h.  A file stays open in
 * romfd for mapping; anything else (a pipe) is read into romtmp.
 */
static int romfd = -1;
static char *romtmp;

static long
romopen(name, h)
	char *name, *h;
{
	struct stat sb;
	long n = 0, k = 0;

	if ((romfd = open(name, O_RDONLY)) < 0 || fstat(romfd, &sb) < 0)
		die(name);
	if (S_ISREG(sb.st_mode))
		n = read(romfd, h, 16) == 16 ? (long)sb.st_size : 0;
	else {
		if ((romtmp = malloc(0x80001)) == 0)
			die(name);
		while (n <= 0x80000 && (k = read(romfd, romtmp + n, 0x80001 - n)) > 0)
			n += k;
		if (k < 0)
			die(name);
		memcpy(h, romtmp, 16);
	}
	if (n < 16 || n > 0x80000) {
		fprintf(stderr, "starttos: %s: not a TOS image\n", name);
		exit(1);
	}
	return n;
}

/* drive C:: the image's descriptor and BPB into the cartridge */
static void
drivec()
{
	unsigned char b[512], *p = (unsigned char *)CART + 0x44;
	unsigned int bps, spc, res, nfat, root, fsz, rdlen, dat, fat16;
	unsigned long tot;
	int fd = open(disk, O_RDWR);

	if (fd < 0 || read(fd, (char *)b, 512) != 512) {
		fprintf(stderr, "starttos: %s: no drive C:\n", disk);
		if (fd >= 0)
			close(fd);
		return;
	}
	bps = b[11] | b[12] << 8;
	spc = b[13];
	res = b[14] | b[15] << 8;
	nfat = b[16];
	root = b[17] | b[18] << 8;
	tot = b[19] | b[20] << 8;
	if (tot == 0)
		tot = b[32] | b[33] << 8 | (unsigned long)b[34] << 16 |
		    (unsigned long)b[35] << 24;
	fsz = b[22] | b[23] << 8;
	if (bps != 512 || spc == 0 || nfat == 0 || fsz == 0) {
		fprintf(stderr, "starttos: %s: not a FAT image with 512-byte sectors\n", disk);
		close(fd);
		return;
	}
	rdlen = root * 32 / bps;
	dat = res + nfat * fsz + rdlen;
	fat16 = (tot - dat) / spc > 4084;
	/* recsiz clsiz clsizb rdlen fsiz fatrec datrec numcl bflags */
	p[0] = bps >> 8; p[1] = bps;
	p[2] = 0; p[3] = spc;
	p[4] = (bps * spc) >> 8; p[5] = bps * spc;
	p[6] = rdlen >> 8; p[7] = rdlen;
	p[8] = fsz >> 8; p[9] = fsz;
	p[10] = (res + fsz) >> 8; p[11] = res + fsz;
	p[12] = dat >> 8; p[13] = dat;
	p[14] = ((tot - dat) / spc) >> 8; p[15] = (tot - dat) / spc;
	p[16] = 0; p[17] = fat16;
	put32((unsigned long)CART + 0x40, (unsigned long)fd);
}

#define	SYSDIR	"/tos/sys"
#define	FVDIDIR	"/tos/fvdi"

/* LETTER PATH [ro] lines of file f into the table, letters D to T */
static void
drvtab(f)
	char *f;
{
	char l[1100], p[1024], lt[4], o[8];
	FILE *fp = fopen(f, "r");
	int n, d;

	if (fp == 0)
		return;
	while (fgets(l, sizeof l, fp)) {
		if (l[0] == '#' || (n = sscanf(l, " %1s %1023s %7s", lt, p, o)) < 2)
			continue;
		d = lt[0] & ~040;
		if (d < 'D' || d > 'T') {
			fprintf(stderr, "starttos: %s: drive %c: not D: to T:\n", f, lt[0]);
			continue;
		}
		tab[d - 'A'] = strdup(p);
		tro[d - 'A'] = n == 3 && strcmp(o, "ro") == 0;
	}
	fclose(fp);
}

/* the drive table and the time zone into the cartridge */
static void
drives()
{
	char path[1024], home[1024], *h, *p = (char *)CART + 0x80, *e = p + 0x1000 - 1;
	struct passwd *pw;
	struct stat sb;
	time_t now = time((time_t *)0);
	struct tm *tm = localtime(&now);
	int d, n;

	h = getenv("HOME");
	if ((h == 0 || *h == 0) && (pw = getpwuid(getuid())) != 0)
		h = pw->pw_dir;
	sprintf(home, "%.1000s/TOS", h ? h : "/");
	if (disk == 0 && cdir == 0) {
		if (stat(home, &sb) == 0 && (sb.st_mode & S_IFMT) == S_IFDIR)
			cdir = strdup(home);
		else {
			fprintf(stderr, "starttos: no %s: drive C: is %s, read-only\n",
			    home, SYSDIR);
			cdir = SYSDIR;
			tro[2] = 1;
		}
	}
	tab[2] = cdir;
	drvtab(SYSDIR "/drives");
	if (cdir) {
		sprintf(path, "%.1000s/drives", cdir);
		if (strcmp(cdir, SYSDIR) != 0)
			drvtab(path);
	}
	for (d = 0; d < 26; d++)
		if (xdrv[d]) {
			tab[d] = xdrv[d];
			tro[d] = 0;
		}
	tab[20] = udir;
	for (d = 0; d < 26; d++) {
		if (tab[d] == 0)
			continue;
		if (realpath(tab[d], path) == 0 || stat(path, &sb) < 0 ||
		    (sb.st_mode & S_IFMT) != S_IFDIR) {
			fprintf(stderr, "starttos: %s: no drive %c:\n", tab[d], 'A' + d);
			continue;
		}
		if ((n = strlen(path)) > 255 || p + n + 3 > e) {
			fprintf(stderr, "starttos: %s: path too long for drive %c:\n", path, 'A' + d);
			continue;
		}
		*p++ = 'A' + d;
		*p++ = tro[d];
		strcpy(p, path);
		p += n + 1;
		if (verbose)
			fprintf(stderr, "starttos: %c: %s%s\n", 'A' + d, path, tro[d] ? " (read-only)" : "");
	}
	/* the system fVDI as C:'s read-only fallback (flag 2) */
	if (cfold && !mono && !stscreen && stat(FVDIDIR, &sb) == 0 &&
	    p + sizeof FVDIDIR + 2 < e) {
		*p++ = 'C';
		*p++ = 3;
		strcpy(p, FVDIDIR);
		p += sizeof FVDIDIR;
	}
	*p = 0;
	put32((unsigned long)CART + 0x64, (unsigned long)-(tm->tm_isdst > 0 ? altzone : timezone));
}

/* the session's frame buffer, from the display process, into the guest */
static void
fbmap(fd)
	int fd;
{
	struct strrecvfd rf;
	struct fbinfo fi;
	unsigned char *p = (unsigned char *)TFB_CART;
	unsigned long a, pl;

	if (ioctl(fd, I_RECVFD, &rf) < 0) {
		close(fd);
		return;
	}
	close(fd);
	/* a 1-bit screen: TOS on a monochrome monitor */
	if (ioctl(rf.fd, FBIOGINFO, &fi) == 0 && fi.fi_depth == 1) {
		mono = 1;
		hfi = fi;
	}
	if (!stscreen && ioctl(rf.fd, FBIOGINFO, &fi) == 0 &&
	    (fi.fi_layout == FBL_PLANES ? fi.fi_depth <= 8 && fi.fi_planebytes % 8 == 0 &&
	    fi.fi_planebytes < 0x80000 : fi.fi_depth == 8) &&
	    fi.fi_width < 0x10000 && fi.fi_height < 0x10000) {
		pl = fi.fi_layout == FBL_PLANES ? fi.fi_planebytes / 8 : 0;
		ioctl(rf.fd, FBIOCACHE, FBC_WT);	/* NuBus refuses; it stays inhibited */
		a = (unsigned long)mmap((caddr_t)0x1000000, fi.fi_size, PROT_READ | PROT_WRITE,
		    MAP_SHARED, rf.fd, 0);
		if (a != (unsigned long)-1 && a >= 0x1000000 && a + fi.fi_size <= 0xf0000000) {
			put32((unsigned long)p, a + fi.fi_offset);
			p[4] = fi.fi_width >> 8; p[5] = fi.fi_width;
			p[6] = fi.fi_height >> 8; p[7] = fi.fi_height;
			put32((unsigned long)p + 8, fi.fi_rowbytes);
			p[12] = 0; p[13] = fi.fi_depth;
			p[14] = pl >> 8; p[15] = pl;
		} else if (a != (unsigned long)-1)
			munmap((caddr_t)a, fi.fi_size);
	}
	close(rf.fd);
}

/* the sound pump, rung through a pipe the cartridge writes to */
static void
sndstart()
{
	struct tossnd *sd = (struct tossnd *)TOSSND;
	int b[2];
	pid_t pid;

	sd->sd_bell = -1;
	if (pipe(b) < 0)
		return;
	if ((pid = fork()) == 0) {
		close(b[1]);
		tossnd(tfd, b[0], (unsigned long)ramsize);
		_exit(0);
	}
	close(b[0]);
	if (pid < 0) {
		close(b[1]);
		return;
	}
	fcntl(b[1], F_SETFL, O_NDELAY);		/* full: the pump has been rung */
	sd->sd_bell = tos_bell = b[1];
}

/* C: from the environment, locked for this session */
/* ---- the spawner ---- */

static long
readn(fd, b, n)
	int fd;
	char *b;
	long n;
{
	long r, got = 0;

	while (got < n) {
		r = read(fd, b + got, (unsigned)(n - got));
		if (r <= 0)
			return got;
		got += r;
	}
	return got;
}

/*
 * One request: directory, program and arguments, each ending in a zero,
 * then an empty one.  The program runs under mintrun on a new
 * pseudo-terminal; the guest gets {1, pid, 0} and the master, or
 * {1, -errno, 0}.
 */
static void
spawn1(fd, req, n)
	int fd;
	char *req;
	long n;
{
	static char *av[66];
	extern char *ptsname();
	struct winsize ws;
	long rec[3];
	char *dir = req, *s, *name;
	int m, sl, i, ac = 0;
	pid_t me = getpid(), pid;

	av[ac++] = "mintrun";
	for (s = req + strlen(req) + 1; s < req + n && *s && ac < 65; s += strlen(s) + 1)
		av[ac++] = s;
	av[ac] = 0;
	rec[0] = 1;
	rec[2] = 0;
	if (ac < 2 || (m = open("/dev/ptmx", O_RDWR)) < 0) {
		rec[1] = -(ac < 2 ? EINVAL : errno);
		write(fd, (char *)rec, sizeof rec);
		return;
	}
	grantpt(m);
	if (getpid() != me)
		_exit(1);
	unlockpt(m);
	name = ptsname(m);
	if (!name || (pid = fork()) < 0) {
		rec[1] = -EAGAIN;
		close(m);
		write(fd, (char *)rec, sizeof rec);
		return;
	}
	if (pid == 0) {
		setsid();
		if ((sl = open(name, O_RDWR)) < 0)
			_exit(127);
		ioctl(sl, I_PUSH, "ptem");
		ioctl(sl, I_PUSH, "ldterm");
		ioctl(sl, I_PUSH, "ttcompat");
		memset((char *)&ws, 0, sizeof ws);
		ws.ws_row = 24;
		ws.ws_col = 80;
		ioctl(sl, TIOCSWINSZ, &ws);
		for (i = 0; i < 3; i++)
			dup2(sl, i);
		for (i = 3; i < 256; i++)
			close(i);
		chdir(dir);
		putenv("TERM=vt52");
		putenv("COLUMNS=80");
		putenv("LINES=24");
		signal(SIGPIPE, SIG_DFL);
		execv("/tos/bin/mintrun", av);
		execv("/usr/bin/mintrun", av);
		_exit(127);
	}
	rec[1] = pid;
	write(fd, (char *)rec, sizeof rec);
	ioctl(fd, I_SENDFD, m);
	close(m);
}

/* requests until the guest goes; each program's end reported */
static void
spawner(fd)
	int fd;
{
	static char req[2048 + 4];
	struct pollfd p;
	long n, rec[3];
	int st;
	pid_t pid;

	for (;;) {
		p.fd = fd;
		p.events = POLLIN;
		p.revents = 0;
		n = poll(&p, 1L, 250);
		while ((pid = waitpid((pid_t)-1, &st, WNOHANG)) > 0) {
			rec[0] = 2;
			rec[1] = pid;
			rec[2] = st;
			write(fd, (char *)rec, sizeof rec);
		}
		if (n <= 0)
			continue;
		if (p.revents & (POLLHUP | POLLERR | POLLNVAL))
			break;
		if (readn(fd, (char *)&n, 4L) != 4 || n <= 0 || n > 2048 || readn(fd, req, n) != n)
			break;
		req[n] = 0;
		spawn1(fd, req, n);
	}
	_exit(0);
}

static void
spawnstart()
{
	int s[2];
	pid_t pid;

	if (pipe(s) < 0)
		return;
	if ((pid = fork()) == 0) {
		close(s[0]);
		close(tfd);
		signal(SIGPIPE, SIG_IGN);
		spawner(s[1]);
	}
	close(s[1]);
	if (pid < 0) {
		close(s[0]);
		return;
	}
	put32((unsigned long)CART + 0x1080, (unsigned long)s[0]);
}

#define	XADIR	"/tos/xaaes"

static char **
tosenvs()
{
	static char *e[] = { "emutos", "xaaes", 0 };
	struct stat sb;

	if (stat(XADIR, &sb) < 0)
		e[1] = 0;
	return e;
}

static char *
homedir()
{
	struct passwd *pw;
	char *h = getenv("HOME");

	if ((h == 0 || *h == 0) && (pw = getpwuid(getuid())) != 0)
		h = pw->pw_dir;
	return h;
}

/* environment id's C:, a copy of installed environment base; the metadata goes last */
static int
tosmake(id, base, path)
	char *id, *base, *path;
{
	char x[1100];
	int xa = strcmp(base, "xaaes") == 0;

	/* the system's drive table stays the system's: ~/TOS/drives adds to it */
	if (envcopy("starttos", "tos", xa ? (char *)0 : id, base, SYSDIR, path,
	    "grep -v '^./drives$' |") < 0)
		return -1;
	if (xa) {
		sprintf(x, "%.1000s/CLIPBRD", path);
		if ((mkdir(x, 0755) < 0 && errno != EEXIST) ||
		    envcopy("starttos", "tos", id, base, XADIR, path, (char *)0) < 0)
			return -1;
	}
	fprintf(stderr, "starttos: %s is drive C: of %s\n", path, id);
	return 0;
}

static int
envsetup()
{
	char path[1024], meta[1100], *h, *id, *b;
	struct stat sb;
	int made;

	if (cdir && (stat(cdir, &sb) < 0 || (sb.st_mode & S_IFMT) != S_IFDIR)) {
		fprintf(stderr, "starttos: %s: no such folder\n", cdir);
		return -1;
	}
	if (disk || cdir)
		return 0;
	h = homedir();
	id = envchoose("tos", env, h, "emutos");
	if ((b = envbase("starttos", "tos", id, ebase, h, "emutos", tosenvs())) == 0 ||
	    envhome("tos", id, "emutos", h, path, sizeof path) < 0 ||
	    envmeta("tos", path, meta, sizeof meta) < 0)
		return -1;
	/* an interrupted copy has no metadata yet and is redone */
	if (!(made = stat(meta, &sb) == 0) && tosmake(id, b, path) < 0) {
		if (strcmp(id, "emutos") == 0)
			return 0;	/* C: is the system's, read-only */
		return -1;
	}
	cdir = strdup(path);
	if (envlock("starttos", "tos", cdir) == -1)
		return -1;
	return made && refresh ? tosmake(id, b, path) : 0;
}

static void
nothing(sig)
	int sig;
{
}

/* what the chips read as, for good: no interrupt asserted, nothing received, nothing busy */
static void
iopage()
{
	unsigned char *io = (unsigned char *)0xff0000L;

	io[0xfa01] = mono ? 0x7f : 0xff;	/* MFP GPIP: bit 7 the monitor */
	io[0xfa2d] = 0x80;			/* USART transmitter empty */
	io[0xfc00] = io[0xfc04] = 2;		/* ACIAs: transmitter empty */
	io[0x8e09] = 1;				/* memory configured: warm boot */
	io[0x9200] = 0x40;			/* DIP switches */
	io[0x9201] = 0xff;
	io[0x8607] = 1;				/* DMA: no error */
	io[0x8260] = mono ? 2 : 0;
	io[0x8262] = !mono ? 4 : hfi.fi_width >= 1280 && hfi.fi_height >= 960 ? 6 : 2;
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	char rbuf[16], *def = 0;
	struct tosenter te;
	struct tosowner to;
	struct sigaction sa;
	unsigned long base, pc, n;
	int c, pfd[2];
	pid_t pid;

	for (c = 1; c < argc; c++)
		if (strcmp(argv[c], "-rom") == 0)
			argv[c] = "-r";
		else if (argv[c][0] == '-' && strchr("rcCdiDebmu", argv[c][1]) && argv[c][2] == 0)
			c++;		/* skip the option's argument */
	while ((c = getopt(argc, argv, "r:c:C:d:i:D:e:b:Rlu:Um:MSv")) != -1)
		switch (c) {
		case 'r': rom = optarg; break;
		case 'c': cart = optarg; break;
		case 'C': cdir = optarg; disk = 0; cfold = 1; break;
		case 'i': disk = optarg; cdir = 0; cfold = 0; break;
		case 'd': def = optarg; break;
		case 'e': env = optarg; break;
		case 'b': ebase = optarg; break;
		case 'R': refresh = 1; break;
		case 'l':
			envlist("tos", homedir(), "emutos", tosenvs());
			return 0;
		case 'D':
			c = optarg[0] & ~040;
			if (c < 'D' || c > 'T' || optarg[1] != '=') {
				fprintf(stderr, "starttos: -D L=dir, L from D to T\n");
				return 2;
			}
			xdrv[c - 'A'] = optarg + 2;
			break;
		case 'u': udir = optarg; break;
		case 'U': udir = 0; break;
		case 'm': ramsize = atol(optarg) << 20; break;
		case 'M': mono = 1; break;
		case 'S': stscreen = 1; break;
		case 'v': verbose = 1; break;
		default:
			fprintf(stderr, "usage: starttos [-rom file] [-c cartridge] [-e env [-b base] [-R] | -C dir | -i disk] [-d env] [-D L=dir] [-u dir | -U] [-m MB] [-M] [-S] [-v]\n"
			    "       starttos -d env | -l\n");
			return 2;
		}
	if ((env || ebase || refresh) && (cdir || disk)) {
		fprintf(stderr, "starttos: -e, -C and -i exclude each other\n");
		return 2;
	}
	if (def) {
		if (envsetdefault("starttos", "tos", homedir(), def) < 0)
			return 1;
		if (env == 0 && cdir == 0 && disk == 0)
			return 0;
	}
	if (ramsize < (1L << 20) || ramsize > (14L << 20)) {
		fprintf(stderr, "starttos: ST-RAM is 1 to 14 MB\n");
		return 2;
	}
	/* the guest can make system calls: it gets no descriptors but its own */
	for (c = 3; c < 256; c++)
		close(c);
	if (envsetup() < 0)
		return 1;
	if ((tfd = open("/dev/tos", O_RDWR)) < 0)
		die("/dev/tos");
	if (ioctl(tfd, TOSIOC_OWNER, &to) == 0 && to.to_pid) {
		fprintf(stderr, "starttos: TOS already runs as pid %ld (uid %ld)\n",
		    to.to_pid, to.to_uid);
		return 1;
	}

	n = romopen(rom, rbuf);
	base = get32((unsigned char *)rbuf + 8);
	pc = get32((unsigned char *)rbuf + 4);
	if ((base != 0xe00000 && base != 0xfc0000) || pc < base || pc >= base + n) {
		fprintf(stderr, "starttos: %s: not a TOS image\n", rom);
		return 1;
	}
	/* ST-RAM at 0 and at its 24-bit alias */
	/* the I/O pages, then a spare the module maps for a write to them */
	if (ioctl(tfd, TOSIOC_RAM, ramsize + (NIO + 1) * IOPG) < 0 ||
	    mmap((caddr_t)0, ramsize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED | MAP_FIXED,
	    tfd, (off_t)0) == (caddr_t)-1 ||
	    mmap((caddr_t)ALIAS, ramsize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED | MAP_FIXED,
	    tfd, (off_t)0) == (caddr_t)-1)
		die("guest memory");
	/* the module maps the last page's alias: no mapping can end at 2^32 */
	for (c = 0; c < NIO; c++)
		if (mmap((caddr_t)iowin[c], IOPG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, tfd,
		    (off_t)(ramsize + c * IOPG)) == (caddr_t)-1 ||
		    (c < NIO - 1 && mmap((caddr_t)(ALIAS | iowin[c]), IOPG, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_FIXED, tfd, (off_t)(ramsize + c * IOPG)) == (caddr_t)-1))
			die("I/O pages");
	/* a file's pages stay in the page cache, shared by sessions */
	if (romtmp) {
		region(base, (n + 0xfff) & ~0xfffL, 0);
		memcpy((char *)base, romtmp, n);
		free(romtmp);
	} else if (mmap((caddr_t)base, (n + 0xfff) & ~0xfffL, PROT_READ | PROT_WRITE | PROT_EXEC,
	    MAP_PRIVATE | MAP_FIXED, romfd, 0) == (caddr_t)-1)
		die(rom);
	close(romfd);
	tos_romlo = base;
	tos_romsz = n;
	if (tos_patch(base, n, (unsigned long)ramsize, tfd) < 0)
		return 1;
	if (mprotect((caddr_t)base, (n + 0xfff) & ~0xfffL, PROT_READ | PROT_EXEC) < 0)
		die("mprotect");
	/* shared: the display process posts input in its last page */
	region((unsigned long)CART, (unsigned long)CARTSZ, 1);
	(void)readall(cart, (char *)CART, TOSPV - CART);
	put32((unsigned long)CART + 0x78, (unsigned long)tfd);	/* sockets */
	if (disk)
		drivec();
	drives();
	if (cdir && !tro[2] && (tos_nvram = malloc(strlen(cdir) + 8)) != 0)
		sprintf(tos_nvram, "%s/.nvram", cdir);
	spawnstart();

	/* warm-boot system variables: TOS skips memory sizing */
	put32(0x420L, 0x752019f3L);		/* memvalid */
	*(char *)0x424 = 0x0a;			/* memctrl */
	put32(0x42eL, (unsigned long)ramsize);	/* phystop */
	put32(0x43aL, 0x237698aaL);		/* memval2 */
	put32(0x51aL, 0x5555aaaaL);		/* memval3 */
	put32(0x5a4L, 0L);			/* ramtop: no TT-RAM */
	put32(0x5a8L, 0x1357bd13L);		/* ramvalid */
	put32(0x4baL, 16000L);			/* _hz_200: disks had 80 s to spin up */

	if (pipe(pfd) < 0)
		die("pipe");
	if ((pid = fork()) < 0)
		die("fork");
	if (pid == 0) {
		close(pfd[0]);
		fbpipe = pfd[1];
		disp(tfd, verbose, (unsigned long)ramsize);
		_exit(0);
	}
	close(pfd[1]);
	fbmap(pfd[0]);
	sndstart();
	memset((char *)&sa, 0, sizeof sa);
	sa.sa_handler = nothing;
	sa.sa_flags = SA_NODEFER;
	sigaction(TOS_SIG, &sa, (struct sigaction *)0);
	/* a guest's send on a reset connection fails instead of ending the session */
	signal(SIGPIPE, SIG_IGN);
	iopage();
	for (c = 0; c < NIO; c++)
		if (mprotect((caddr_t)iowin[c], IOPG, PROT_READ) < 0 ||
		    (c < NIO - 1 && mprotect((caddr_t)(ALIAS | iowin[c]), IOPG, PROT_READ) < 0))
			die("I/O pages");
	memset((char *)&te, 0, sizeof te);
	te.te_ramsize = ramsize;
	te.te_w = hfi.fi_width;
	te.te_h = hfi.fi_height;
	te.te_depth = hfi.fi_depth;
	te.te_flags = mono ? TEF_MONO : 0;
	te.te_hostlo = (unsigned long)_start;
	te.te_hosthi = (unsigned long)&etext;
	te.te_stklo = (unsigned long)hc_stack;
	te.te_stkhi = (unsigned long)(hc_stack + HC_NSTACK);
	if (ioctl(tfd, TOSIOC_ENTER, &te) < 0)
		die("TOSIOC_ENTER");
	__asm__ __volatile__("mov.l %0,%%sp\n\tjmp (%1)" : : "d" (0x8000L), "a" (pc));
	return 0;
}
