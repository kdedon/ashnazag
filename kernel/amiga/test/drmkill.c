/*
 * drmkill -- clients of the Voodoo3's DRM that die or stall holding
 * the lock must not wedge the card or the display:
 *   half    killed after half a command packet: the next client's
 *           packet still lands
 *   random  killed at a random moment while drawing in a loop
 *   fast    killed holding the lock taken in user space
 *   orphan  exited holding it while a child keeps its file open
 *   switch  alive and holding it: a switch away still goes ahead
 * Each round takes the lock within 5 s and writes a word to VRAM
 * through the command fifo.  Output: one line per check, then PASS or
 * FAIL.
 *
 *   drmkill [rounds]     (default 10)
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include "dsio.h"
#include "drm.h"

/* the kernel's lock block, which the shared area starts with */
typedef struct {
	volatile unsigned int lock;
	char padding[60];
} drm_hw_lock_t;
#include "drm_sarea.h"
#include "tdfx_sarea.h"

#define	MODE16		0xb0
#define	STATUS		0x00
#define	BUSY		(1 << 9)
#define	RDPTR		0x8002c		/* command fifo 0 */
#define	DEPTH		0x80044
#define	NOPCMD		0x120

static volatile unsigned char *reg, *vram;
static volatile unsigned int *lock;
static tdfx_sarea_t *sp;
static int fbfd, swap, fails;
static long session;
static unsigned long *wp;	/* next fifo word */
static void give();

static void
alarmed(s)
int s;
{
	signal(SIGALRM, alarmed);
}

static void
die(what)
char *what;
{
	fprintf(stderr, "drmkill: %s: %s\n", what, strerror(errno));
	exit(1);
}

static unsigned long
sw(v)
unsigned long v;
{
	return swap ? v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24 : v;
}

static unsigned long
rl(r)
int r;
{
	return sw(*(volatile unsigned long *)(reg + r));
}

/* fifo words and VRAM longs through the aperture have their halves swapped */
static unsigned long
ws(v)
unsigned long v;
{
	return v << 16 | v >> 16;
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

static int
idle()
{
	long n;
	int i;

	for (n = 0, i = 0; n < 2000000 && i < 3; n++)
		i = rl(STATUS) & BUSY ? 0 : i + 1;
	return i == 3;
}

/* a DRM file bound to our session, with a context */
static int
client(ctx)
int *ctx;
{
	drm_dsbind_t b;
	drm_ctx_t c;
	int fd;

	if ((fd = open("/dev/dri/card0", O_RDWR)) < 0)
		die("/dev/dri/card0");
	b.fd = fbfd;
	if (ioctl(fd, DRM_IOCTL_DSBIND, &b) < 0)
		die("DSBIND");
	memset(&c, 0, sizeof c);
	if (ioctl(fd, DRM_IOCTL_ADD_CTX, &c) < 0)
		die("ADD_CTX");
	*ctx = c.handle;
	return fd;
}

/* the lock within 5 s */
static int
take(fd, ctx)
int fd, ctx;
{
	drm_lock_t l;
	time_t t = time(0);
	int r;

	l.context = ctx;
	l.flags = 0;
	alarm(5);
	r = ioctl(fd, DRM_IOCTL_LOCK, &l);
	alarm(0);
	if (r == 0 && time(0) - t > 6) {
		give(fd, ctx);
		r = -1;
	}
	return r == 0;
}

static void
give(fd, ctx)
int fd, ctx;
{
	drm_lock_t l;

	l.context = ctx;
	l.flags = 0;
	ioctl(fd, DRM_IOCTL_UNLOCK, &l);
}

/* the user-space lock: held to free and back without the kernel */
static int
cas(old, new)
unsigned int old, new;
{
	unsigned int o = old;

	__asm__ __volatile__("cas.l %0,%2,(%1)" : "=d" (o) : "a" (lock), "d" (new), "0" (o)
	    : "memory", "cc");
	return o == old;
}

/* where the card reads next, as a client takes the fifo over */
static void
import()
{
	unsigned long rp;
	long n;

	for (n = 0; n < 2000000 && (rl(DEPTH) & 0xffff) != 0; n++)
		;
	rp = rl(RDPTR);
	if (rp < sp->fifoOffset || rp + 256 > sp->fifoOffset + sp->fifoSize) {
		if (rp >= sp->fifoOffset && rp < sp->fifoOffset + sp->fifoSize) {
			*(volatile unsigned long *)(vram + rp) = ws(sp->fifoOffset << 4 | 3 << 3);
			idle();
		}
		rp = sp->fifoOffset;
	}
	wp = (unsigned long *)(vram + rp);
}

static void
put(v)
unsigned long v;
{
	*(volatile unsigned long *)wp++ = ws(v);
}

/* a word written to VRAM through the fifo, read back */
static int
probe(fd, ctx, v)
int fd, ctx;
unsigned long v;
{
	volatile unsigned long *t = (volatile unsigned long *)(vram + sp->textureOffset);
	int ok;

	if (!take(fd, ctx))
		return 0;
	*t = 0;
	import();
	put(1UL << 3 | 5);		/* one word to the linear frame buffer */
	put((unsigned long)sp->textureOffset);
	put(v);
	ok = idle() && ws(*t) == v;
	sp->fifoOwner = sp->ctxOwner = sp->texOwner = 0;
	give(fd, ctx);
	return ok;
}

static void
check(name, ok, n, rounds)
char *name;
int ok, n, rounds;
{
	printf("drmkill: %s %s: %d of %d rounds\n", ok ? "ok" : "FAIL", name, n, rounds);
	fflush(stdout);
	if (!ok)
		fails++;
}

/* fork a holder; it writes 'H' once it holds */
static pid_t
holder(kind, p)
int kind, *p;
{
	int fd, ctx, i;
	pid_t pid;
	char c = 'H';

	if ((pid = fork()) != 0)
		return pid;
	close(p[0]);
	fd = client(&ctx);
	if (!take(fd, ctx))
		_exit(1);
	switch (kind) {
	case 'h':		/* half a packet: 3 of 100 register writes */
		import();
		put(100UL << 16 | NOPCMD << 1 | 1);
		put(0); put(0); put(0);
		break;
	case 'f':
		if (!cas(ctx | _DRM_LOCK_HELD, ctx) || !cas(ctx, ctx | _DRM_LOCK_HELD))
			_exit(1);
		break;
	case 'r':		/* whole packets, the lock dropped and taken each frame */
		write(p[1], &c, 1);
		for (;;) {
			import();
			for (i = 0; i < 40 * 21; i++)
				put(i % 21 ? 0 : 20UL << 16 | NOPCMD << 1 | 1);
			if (!cas(ctx | _DRM_LOCK_HELD, ctx))
				give(fd, ctx);
			if (!cas(ctx, ctx | _DRM_LOCK_HELD) && !take(fd, ctx))
				_exit(1);
		}
	case 'o':		/* a child keeps the file */
		if (fork() == 0) {
			close(p[1]);
			sleep(30);
			_exit(0);
		}
		write(p[1], &c, 1);
		_exit(0);
	}
	write(p[1], &c, 1);
	pause();
	_exit(0);
}

static int
trial(kind, fd, ctx, r)
int kind, fd, ctx, r;
{
	int p[2], ok, st;
	pid_t pid;
	char c = 0;
	time_t t;

	if (pipe(p) < 0)
		die("pipe");
	pid = holder(kind, p);
	close(p[1]);
	read(p[0], &c, 1);
	close(p[0]);
	if (c != 'H') {
		waitpid(pid, &st, 0);
		return 0;
	}
	if (kind == 'r')
		poll((struct pollfd *)0, 0, rand() % 400);
	if (kind == 's') {
		t = time(0);
		ok = ioctl(fbfd, FBIOSWITCH, 0) == 0 && time(0) - t <= 3;
		if (ioctl(fbfd, FBIOSWITCH, (int)session) < 0)
			ok = 0;
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return ok && probe(fd, ctx, 0x5a000000UL | r);
	}
	if (kind != 'o')
		kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	/* the exited holder's lock must outlive it, or this is no test */
	if (kind == 'o' && !(*lock & _DRM_LOCK_HELD && (*lock & ~_DRM_LOCK_CONT) != (ctx | _DRM_LOCK_HELD))) {
		printf("drmkill: orphan: lock 0x%x free at exit\n", *lock);
		return 0;
	}
	ok = probe(fd, ctx, 0xa5000000UL | (unsigned long)kind << 16 | r);
	return ok;
}

int
main(argc, argv)
int argc;
char **argv;
{
	struct fbacq a;
	drm_dsbind_t b;
	drm_map_t m, regm, fbm, shm;
	static char kinds[] = "hrfos", *names[] = { "half", "random", "fast", "orphan", "switch" };
	int fd, ctx, i, k, n, rounds = argc > 1 ? atoi(argv[1]) : 10;

	signal(SIGALRM, alarmed);
	srand((unsigned)time(0));
	if ((fbfd = open("/dev/fb0", O_RDWR)) < 0)
		die("/dev/fb0");
	memset(&a, 0, sizeof a);
	a.fa_kind = FBK_USER;
	a.fa_flags = FBA_FRONT;
	strcpy(a.fa_name, "drmkill");
	if (ioctl(fbfd, FBIOACQUIRE, &a) < 0)
		die("FBIOACQUIRE");
	if (ioctl(fbfd, FBIOSMODE, MODE16) < 0)
		die("FBIOSMODE");
	session = a.fa_id;
	if ((fd = open("/dev/dri/card0", O_RDWR)) < 0)
		die("/dev/dri/card0");
	b.fd = fbfd;
	if (ioctl(fd, DRM_IOCTL_DSBIND, &b) < 0)
		die("DSBIND");
	memset(&regm, 0, sizeof regm); fbm = shm = regm;
	for (i = 0; ; i++) {
		memset(&m, 0, sizeof m);
		m.offset = i;
		if (ioctl(fd, DRM_IOCTL_GET_MAP, &m) < 0)
			break;
		if (m.type == _DRM_REGISTERS)
			regm = m;
		else if (m.type == _DRM_FRAME_BUFFER)
			fbm = m;
		else if (m.type == _DRM_SHM)
			shm = m;
	}
	if (!regm.size || !fbm.size || !shm.size) {
		fprintf(stderr, "drmkill: maps missing\n");
		return 1;
	}
	reg = map(fd, regm.offset, 0x200000UL);
	lock = map(fd, shm.offset, shm.size);
	sp = (tdfx_sarea_t *)((char *)lock + sizeof(drm_sarea_t));
	if (sp->fifoSize == 0) {
		fprintf(stderr, "drmkill: no room for 3D\n");
		return 1;
	}
	vram = map(fd, fbm.offset, (unsigned long)sp->textureOffset + 4096);
	swap = (*(volatile unsigned long *)(reg + STATUS) & 0x1f) == 0;
	close(fd);
	fd = client(&ctx);
	printf("drmkill: fifo 0x%x+0x%x, scratch 0x%x\n", sp->fifoOffset, sp->fifoSize, sp->textureOffset);
	check("start", probe(fd, ctx, 0x12345678UL), 1, 1);
	for (k = 0; k < 5; k++) {
		for (n = i = 0; i < rounds; i++)
			if (trial(kinds[k], fd, ctx, i))
				n++;
		check(names[k], n == rounds, n, rounds);
	}
	printf("drmkill: %s\n", fails ? "FAIL" : "PASS");
	return fails != 0;
}
