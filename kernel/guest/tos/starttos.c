/*
 * starttos -- run Atari TOS as this process: EmuTOS, or the user's ROM image.
 *
 *	starttos [-rom file] [-c cartridge] [-d disk] [-m megabytes] [-M] [-v]
 *
 * ST-RAM is a shared mapping at 0, the ROM a read-only copy at its own
 * base, the machine-layer cartridge at $FA0000 with drive C: (a FAT
 * image, 512-byte sectors).  A child process owns the display session:
 * it converts the TOS screen to the frame buffer and passes keyboard
 * and mouse to the IKBD.  The parent enters the ROM's reset code.
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
#include "dsio.h"
#include "tosio.h"

#define	CART	0xfa0000
#define	CARTSZ	0x20000

static char *rom = "/etc/tos/emutos.img";	/* the free TOS; -rom for the user's */
static char *cart = "/etc/tos/tosml.img";
static char *disk = "/etc/tos/c.img";
static long ramsize = 4L << 20;
static int mono, verbose;
static int tfd;

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
	int c;
	pid_t pid;

	for (c = 1; c < argc; c++)
		if (strcmp(argv[c], "-rom") == 0)
			argv[c] = "-r";
		else if (argv[c][0] == '-' && strchr("rcdm", argv[c][1]) && argv[c][2] == 0)
			c++;		/* skip the option's argument */
	while ((c = getopt(argc, argv, "r:c:d:m:Mv")) != -1)
		switch (c) {
		case 'r': rom = optarg; break;
		case 'c': cart = optarg; break;
		case 'd': disk = optarg; break;
		case 'm': ramsize = atol(optarg) << 20; break;
		case 'M': mono = 1; break;
		case 'v': verbose = 1; break;
		default:
			fprintf(stderr, "usage: starttos [-rom file] [-c cartridge] [-d disk] [-m MB] [-M] [-v]\n");
			return 2;
		}
	if (ramsize < (1L << 20) || ramsize > (14L << 20)) {
		fprintf(stderr, "starttos: ST-RAM is 1 to 14 MB\n");
		return 2;
	}
	/* the guest can make system calls: it gets no descriptors but its own */
	for (c = 3; c < 256; c++)
		close(c);
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
	region((unsigned long)CART, (unsigned long)CARTSZ, 0);
	(void)readall(cart, (char *)CART, (long)CARTSZ);
	drivec();

	/* warm-boot system variables: TOS skips memory sizing */
	put32(0x420L, 0x752019f3L);		/* memvalid */
	*(char *)0x424 = 0x0a;			/* memctrl */
	put32(0x42eL, (unsigned long)ramsize);	/* phystop */
	put32(0x43aL, 0x237698aaL);		/* memval2 */
	put32(0x51aL, 0x5555aaaaL);		/* memval3 */
	put32(0x5a4L, 0L);			/* ramtop: no TT-RAM */
	put32(0x5a8L, 0x1357bd13L);		/* ramvalid */
	put32(0x4baL, 16000L);			/* _hz_200: disks had 80 s to spin up */

	if ((pid = fork()) < 0)
		die("fork");
	if (pid == 0) {
		disp(tfd, verbose, (unsigned long)ramsize);
		_exit(0);
	}
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
