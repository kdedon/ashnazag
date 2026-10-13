/*
 * drmtest -- a display session in RGB565 drawn through the DRM:
 * version, bus id, session bind, maps, a context and the lock; a red
 * band written through the frame buffer map, then a green rectangle
 * filled by the card's 2D engine through the register map.
 *
 *   drmtest [seconds]     hold the picture that long (default 30)
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include "dsio.h"
#include "drm.h"

#define	MODE16		0xb0		/* the RTG mode at 16 bits a pixel */
#define	STATUS		0x00
#define	BUSY		(1 << 9)
#define	R2D		0x100000	/* 2D registers */
#define	CLIP0MIN	0x08
#define	CLIP0MAX	0x0c
#define	DSTBASE		0x10
#define	DSTFORMAT	0x14
#define	CMDEXTRA	0x38
#define	COLORFORE	0x64
#define	DSTSIZE		0x68
#define	DSTXY		0x6c
#define	COMMAND		0x70
#define	RECTFILL	5
#define	GO		0x100

static volatile unsigned char *reg;

static void
alarmed(s)
int s;
{
}
static int swap;

static void
die(what)
char *what;
{
	fprintf(stderr, "drmtest: %s: %s\n", what, strerror(errno));
	exit(1);
}

static unsigned long
sw(v)
unsigned long v;
{
	return swap ? v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24 : v;
}

static void
wl(r, v)
int r;
unsigned long v;
{
	*(volatile unsigned long *)(reg + r) = sw(v);
}

static unsigned long
rl(r)
int r;
{
	return sw(*(volatile unsigned long *)(reg + r));
}

static void *
map(fd, off, size)
int fd;
unsigned long off, size;
{
	void *p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off);

	if (p == (void *)-1)
		die("mmap");
	return p;
}

int
main(argc, argv)
int argc;
char **argv;
{
	struct fbacq a;
	struct fbinfo fi;
	drm_version_t v;
	drm_unique_t u;
	drm_dsbind_t b;
	drm_map_t m, regm, fbm, shm;
	drm_ctx_t ctx;
	drm_lock_t lk;
	char dev[16], name[32], date[32], desc[64], bus[64];
	volatile unsigned short *fb;
	volatile unsigned int *lock;
	unsigned long w, h, x, y, n;
	int fd, dfd, i, secs = argc > 1 ? atoi(argv[1]) : 30;

	if ((fd = open("/dev/fb0", O_RDWR)) < 0)
		die("/dev/fb0");
	memset(&a, 0, sizeof a);
	a.fa_kind = FBK_USER;
	a.fa_flags = FBA_FRONT;
	strcpy(a.fa_name, "drmtest");
	if (ioctl(fd, FBIOACQUIRE, &a) < 0)
		die("FBIOACQUIRE");
	if (ioctl(fd, FBIOSMODE, MODE16) < 0)
		die("FBIOSMODE");
	if (ioctl(fd, FBIOGINFO, &fi) < 0)
		die("FBIOGINFO");
	w = fi.fi_width;
	h = fi.fi_height;

	/* the card behind the display is the one whose DRM takes the session */
	for (i = 0; i < 2; i++) {
		sprintf(dev, "/dev/dri/card%d", i);
		b.fd = fd;
		if ((dfd = open(dev, O_RDWR)) >= 0 && ioctl(dfd, DRM_IOCTL_DSBIND, &b) == 0)
			break;
		if (dfd >= 0)
			close(dfd);
	}
	if (i == 2)
		die("no DRM for the display");
	memset(&v, 0, sizeof v);
	v.name = name; v.name_len = sizeof name - 1;
	v.date = date; v.date_len = sizeof date - 1;
	v.desc = desc; v.desc_len = sizeof desc - 1;
	if (ioctl(dfd, DRM_IOCTL_VERSION, &v) < 0)
		die("VERSION");
	name[v.name_len < sizeof name ? v.name_len : sizeof name - 1] = 0;
	date[v.date_len < sizeof date ? v.date_len : sizeof date - 1] = 0;
	desc[v.desc_len < sizeof desc ? v.desc_len : sizeof desc - 1] = 0;
	printf("drmtest: version %d.%d.%d %s %s (%s)\n", v.version_major, v.version_minor,
	    v.version_patchlevel, name, date, desc);
	if (strcmp(name, "tdfx") != 0) {
		fprintf(stderr, "drmtest: a Voodoo3 test; the display is on %s\n", name);
		return 1;
	}
	memset(&u, 0, sizeof u);
	u.unique = bus;
	u.unique_len = sizeof bus - 1;
	if (ioctl(dfd, DRM_IOCTL_GET_UNIQUE, &u) < 0)
		die("GET_UNIQUE");
	bus[u.unique_len < sizeof bus ? u.unique_len : sizeof bus - 1] = 0;
	printf("drmtest: unique %s\n", bus);
	printf("drmtest: %s, session %ld frame 0x%lx, free VRAM 0x%lx+0x%lx\n",
	    dev, b.session, b.offset, b.vram, b.vramsize);

	memset(&regm, 0, sizeof regm); fbm = shm = regm;
	for (i = 0; ; i++) {
		memset(&m, 0, sizeof m);
		m.offset = i;
		if (ioctl(dfd, DRM_IOCTL_GET_MAP, &m) < 0)
			break;
		printf("drmtest: map %d offset 0x%lx size 0x%lx type %d flags 0x%x\n", i,
		    m.offset, m.size, (int)m.type, (int)m.flags);
		if (m.type == _DRM_REGISTERS)
			regm = m;
		else if (m.type == _DRM_FRAME_BUFFER)
			fbm = m;
		else if (m.type == _DRM_SHM)
			shm = m;
	}
	if (!regm.size || !fbm.size || !shm.size) {
		fprintf(stderr, "drmtest: maps missing\n");
		return 1;
	}
	reg = map(dfd, regm.offset, 0x200000UL);
	fb = map(dfd, fbm.offset + b.offset, (w * h * 2 + 4095) & ~4095UL);
	lock = map(dfd, shm.offset, shm.size);
	swap = (*(volatile unsigned long *)(reg + STATUS) & 0x1f) == 0;
	printf("drmtest: status 0x%lx%s\n", rl(STATUS), swap ? " (byte swapped)" : "");

	memset(&ctx, 0, sizeof ctx);
	if (ioctl(dfd, DRM_IOCTL_ADD_CTX, &ctx) < 0)
		die("ADD_CTX");
	lk.context = ctx.handle;
	lk.flags = 0;
	if (ioctl(dfd, DRM_IOCTL_LOCK, &lk) < 0)
		die("LOCK");
	printf("drmtest: context %d, lock word 0x%x\n", (int)ctx.handle, *lock);
	if (*lock != (ctx.handle | _DRM_LOCK_HELD)) {
		fprintf(stderr, "drmtest: lock not held\n");
		return 1;
	}

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			fb[y * w + x] = y < h / 4 ? 0xf800 : 0x0000;
	wl(R2D + CLIP0MIN, 0);
	wl(R2D + CLIP0MAX, 0x0fff0fffUL);
	wl(R2D + CMDEXTRA, 0);
	wl(R2D + DSTBASE, b.offset);
	wl(R2D + DSTFORMAT, w * 2 | 3UL << 16);
	wl(R2D + COLORFORE, 0x07e0);
	wl(R2D + DSTSIZE, w / 2 | (h / 2) << 16);
	wl(R2D + DSTXY, w / 4 | (h * 3 / 8) << 16);
	wl(R2D + COMMAND, 0xccUL << 24 | GO | RECTFILL);
	for (n = 0, i = 0; n < 1000000 && i < 3; n++)
		i = rl(STATUS) & BUSY ? 0 : i + 1;
	sleep(1);
	printf("drmtest: 2D fill %s, centre pixel 0x%04x\n", n < 1000000 ? "done" : "busy",
	    fb[(h / 2) * w + w / 2]);

	if (ioctl(dfd, DRM_IOCTL_UNLOCK, &lk) < 0)
		die("UNLOCK");
	printf("drmtest: unlocked, lock word 0x%x\n", *lock);

	/* hidden, the session gets no lock: the wait ends only by the alarm */
	if (ioctl(fd, FBIOSWITCH, 0) < 0)
		die("FBIOSWITCH");
	signal(SIGALRM, alarmed);
	alarm(3);
	i = ioctl(dfd, DRM_IOCTL_LOCK, &lk);
	printf("drmtest: hidden, lock %s\n", i < 0 && errno == EINTR ? "refused" : "granted");
	if (ioctl(fd, FBIOSWITCH, (int)b.session) < 0)
		die("FBIOSWITCH");
	if (i == 0 && ioctl(dfd, DRM_IOCTL_UNLOCK, &lk) < 0)
		die("UNLOCK");
	printf("drmtest: ok\n");
	fflush(stdout);
	sleep(secs);
	return 0;
}
