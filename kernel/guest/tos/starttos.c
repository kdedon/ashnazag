/*
 * starttos -- run Atari TOS as this process: EmuTOS, or the user's ROM image.
 *
 *	starttos [-rom file] [-c cartridge] [-e env | -C dir | -d disk] [-D L=dir] [-u dir | -U]
 *		[-m megabytes] [-M] [-S] [-v]
 *
 * ST-RAM is a shared mapping at 0, the ROM a read-only copy at its own
 * base, the machine-layer cartridge at $FA0000.  Drives are host
 * directories used with the caller's permissions: C:, the boot drive,
 * is ~/TOS (else /tos/sys, read-only) or, with -e, ~/TOS/env; U: is "/";
 * others come from /tos/sys/drives, ~/TOS/drives and -D.  -d makes C: a FAT image
 * (512-byte sectors) instead.  One writable session per environment:
 * a second is refused.  A child process owns the display session:
 * it converts the TOS screen to the frame buffer and passes keyboard
 * and mouse to the IKBD.  The parent enters the ROM's reset code.
 * -S keeps GEM on the ST screen: fVDI gets no frame buffer, for
 * programs that also write and read that screen themselves.
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
#include <time.h>
#include <pwd.h>
#include "dsio.h"
#include "tosio.h"
#include "tosfb.h"
#include "../include/envroot.h"
#include <stropts.h>

#define	CART	0xfa0000
#define	CARTSZ	0x20000

static char *rom = "/etc/tos/emutos.img";	/* the free TOS; -rom for the user's */
static char *cart = "/etc/tos/tosml.img";
static char *disk;
static char *cdir;
static char *env;
static char *udir = "/";
static char *xdrv[26];			/* -D */
static char *tab[26];			/* drive -> host directory */
static char tro[26];			/* read-only */
static long ramsize = 4L << 20;
static int mono, stscreen, verbose;
static int tfd;
extern int fbpipe;

extern void disp();

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
	static int zfd = -1;

	if (zfd < 0 && (zfd = open("/dev/zero", O_RDWR)) < 0)
		die("/dev/zero");
	if (mmap((caddr_t)a, n, PROT_READ | PROT_WRITE | PROT_EXEC,
	    (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED, zfd, 0) == (caddr_t)-1)
		die("mmap");
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
			cdir = home;
		else {
			fprintf(stderr, "starttos: no %s: drive C: is %s, read-only; run maketos for your own\n",
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
	unsigned long a;

	if (ioctl(fd, I_RECVFD, &rf) < 0) {
		close(fd);
		return;
	}
	close(fd);
	if (!stscreen && ioctl(rf.fd, FBIOGINFO, &fi) == 0 && fi.fi_depth == 8 &&
	    fi.fi_width < 0x10000 && fi.fi_height < 0x10000) {
		ioctl(rf.fd, FBIOCACHE, FBC_WT);	/* NuBus refuses; it stays inhibited */
		a = (unsigned long)mmap((caddr_t)0x1000000, fi.fi_size, PROT_READ | PROT_WRITE,
		    MAP_SHARED, rf.fd, 0);
		if (a != (unsigned long)-1 && a >= 0x1000000 && a + fi.fi_size <= 0xf0000000) {
			put32((unsigned long)p, a + fi.fi_offset);
			p[4] = fi.fi_width >> 8; p[5] = fi.fi_width;
			p[6] = fi.fi_height >> 8; p[7] = fi.fi_height;
			put32((unsigned long)p + 8, fi.fi_rowbytes);
			p[12] = 0; p[13] = 8;
		} else if (a != (unsigned long)-1)
			munmap((caddr_t)a, fi.fi_size);
	}
	close(rf.fd);
}

/* C: from the environment, locked for this session */
static int
envsetup()
{
	char path[1024], *h;
	struct passwd *pw;
	struct stat sb;

	if (disk || cdir)
		return 0;
	h = getenv("HOME");
	if ((h == 0 || *h == 0) && (pw = getpwuid(getuid())) != 0)
		h = pw->pw_dir;
	if (envroot("tos", env, h, path, sizeof path) < 0) {
		if (env == 0)
			return 0;
		fprintf(stderr, "starttos: %s: not an environment name\n", env);
		return -1;
	}
	if (stat(path, &sb) < 0 || (sb.st_mode & S_IFMT) != S_IFDIR) {
		if (env == 0 || strcmp(env, "default") == 0)
			return 0;
		fprintf(stderr, "starttos: no environment %s; run maketos -e %s\n", env, env);
		return -1;
	}
	cdir = strdup(path);
	return envlock("starttos", "tos", cdir) == -1 ? -1 : 0;
}

static void
nothing(sig)
	int sig;
{
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	static char rbuf[0x80000];
	struct tosenter te;
	struct tosowner to;
	struct sigaction sa;
	unsigned long base, pc, n;
	int c, pfd[2];
	pid_t pid;

	for (c = 1; c < argc; c++)
		if (strcmp(argv[c], "-rom") == 0)
			argv[c] = "-r";
		else if (argv[c][0] == '-' && strchr("rcCdDemu", argv[c][1]) && argv[c][2] == 0)
			c++;		/* skip the option's argument */
	while ((c = getopt(argc, argv, "r:c:C:d:D:e:u:Um:MSv")) != -1)
		switch (c) {
		case 'r': rom = optarg; break;
		case 'c': cart = optarg; break;
		case 'C': cdir = optarg; disk = 0; break;
		case 'd': disk = optarg; cdir = 0; break;
		case 'e': env = optarg; break;
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
			fprintf(stderr, "usage: starttos [-rom file] [-c cartridge] [-e env | -C dir | -d disk] [-D L=dir] [-u dir | -U] [-m MB] [-M] [-S] [-v]\n");
			return 2;
		}
	if (env && (cdir || disk)) {
		fprintf(stderr, "starttos: -e, -C and -d exclude each other\n");
		return 2;
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

	n = readall(rom, rbuf, (long)sizeof rbuf);
	base = get32((unsigned char *)rbuf + 8);
	pc = get32((unsigned char *)rbuf + 4);
	if ((base != 0xe00000 && base != 0xfc0000) || pc < base || pc >= base + n) {
		fprintf(stderr, "starttos: %s: not a TOS image\n", rom);
		return 1;
	}
	region(0L, (unsigned long)ramsize, 1);
	region(base, (n + 0xfff) & ~0xfffL, 0);
	memcpy((char *)base, rbuf, n);
	if (mprotect((caddr_t)base, (n + 0xfff) & ~0xfffL, PROT_READ | PROT_EXEC) < 0)
		die("mprotect");
	/* shared: the display process posts input in its last page */
	region((unsigned long)CART, (unsigned long)CARTSZ, 1);
	(void)readall(cart, (char *)CART, TOSPV - CART);
	if (disk)
		drivec();
	drives();

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
	memset((char *)&sa, 0, sizeof sa);
	sa.sa_handler = nothing;
	sa.sa_flags = SA_NODEFER;
	sigaction(TOS_SIG, &sa, (struct sigaction *)0);
	te.te_ramsize = ramsize;
	te.te_flags = mono ? TEF_MONO : 0;
	if (ioctl(tfd, TOSIOC_ENTER, &te) < 0)
		die("TOSIOC_ENTER");
	__asm__ __volatile__("mov.l %0,%%sp\n\tjmp (%1)" : : "d" (0x8000L), "a" (pc));
	return 0;
}
