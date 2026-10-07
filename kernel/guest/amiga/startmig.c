/*
 * startmig -- validate local Kickstart media and enter an Amiga profile.
 * SYS: is ~/Amiga, or ~/Amiga/env with -e; one writable session each.
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
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
#include "amigaio.h"
#include "dsio.h"
#include "rtgshare.h"
#include "inputshare.h"
#include "hostfswire.h"
#include "sndshare.h"
#include "earlyshare.h"
#include "sysroot.h"
#include "envroot.h"
#include "miglog.h"

#define ROMBASE 0xf80000UL
#define ROMSIZE 0x80000UL
#define CHIPSIZE 0x200000UL
#define FASTBASE 0x08000000UL
#define BOOTBASE 0x00f00000UL
#define BOOTSIZE 0x80000UL

extern int mprotect(), munmap();
extern int migdisp(), migdisp_open(), migkick, migmenu;

static unsigned char rombuf[ROMSIZE];
static int displaylife = -1, gofd = -1;
static void fail(char *);
static int romok();
static unsigned long get32(unsigned char *);
static unsigned int get16();

/* stubs for the patched Kickstart, in the boot extension's last page */
#define PVSTUB 0x00f7f000UL
#define PVENABLE PVSTUB
#define PVECLOCK (PVSTUB + 64)
static unsigned char pvstub[128];
static int pvstubbed;
static char *earlyroot;
static int twoscreens;
static void preload();

static void
put16(p, v)
	unsigned char *p;
	unsigned long v;
{
	p[0] = v >> 8; p[1] = v;
}

static void
put32(p, v)
	unsigned char *p;
	unsigned long v;
{
	put16(p, v >> 16); put16(p + 2, v);
}

/* the stubs' code: words, then a long address where the word is 1 */
static void
pvcode(p, w, n)
	unsigned char *p;
	unsigned long *w;
	int n;
{
	int i;
	for (i = 0; i < n; i++, p += 2)
		if (w[i] >= 0x10000UL)
			put32(p, w[i]), p += 2;
		else
			put16(p, w[i]);
}

static void
pvstubs()
{
	/* Enable: the master bit in memory; INTENA itself if an interrupt waits */
	static unsigned long enable[] = {
		0x33fc, 0xc000, AMIGA_PV_BASE,		/* move.w #$c000,pv */
		0x4a79, AMIGA_PV_BASE + 2,		/* tst.w pv+2 */
		0x6608,					/* bne.s 1f */
		0x4a79, AMIGA_PV_BASE,			/* tst.w pv: flags as the write */
		0x4e75,					/* rts */
		0x33fc, 0xc000, 0xdff09aUL,		/* 1: move.w #$c000,$dff09a */
		0x4e75
	};
	/*
	 * timer.device's E-clock read (a1 = $BFE001): timer B high, low, high
	 * into d3, d4, d2 from the module's copy; every 64th read the chip.
	 */
	static unsigned long eclock[] = {
		0x5339, AMIGA_PV_BASE + 6,		/* subq.b #1,pv+6 */
		0x6510,					/* bcs.s 1f */
		0x1639, AMIGA_PV_BASE + 4,		/* move.b pv+4,d3 */
		0x1839, AMIGA_PV_BASE + 5,		/* move.b pv+5,d4 */
		0x1403,					/* move.b d3,d2 */
		0x4e75,					/* rts */
		0x13fc, 0x003f, AMIGA_PV_BASE + 6,	/* 1: move.b #63,pv+6 */
		0x1629, 0x0700,				/* move.b $700(a1),d3 */
		0x1829, 0x0600,				/* move.b $600(a1),d4 */
		0x1429, 0x0700,				/* move.b $700(a1),d2 */
		0x4e75
	};
	pvcode(pvstub, enable, sizeof enable / sizeof enable[0]);
	pvcode(pvstub + (PVECLOCK - PVSTUB), eclock, sizeof eclock / sizeof eclock[0]);
}

static void
loadboot(char *path)
{
    unsigned char *p = (unsigned char *)BOOTBASE, extra;
    unsigned long n = 0;
    int fd, count;
    fd = open(path, O_RDONLY);
    if (fd < 0) fail(path);
    while (n < BOOTSIZE) {
        count = read(fd, (char *)p + n, BOOTSIZE - n);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) fail("read boot extension");
        if (!count) break;
        n += count;
    }
    do { count = read(fd, (char *)&extra, 1); } while (count < 0 && errno == EINTR);
    close(fd);
    if (n != BOOTSIZE || count != 0 || p[0] != 0x4a || p[1] != 0xfc ||
        get32(p + 2) != BOOTBASE || get32(p + 6) <= BOOTBASE + 26 ||
        get32(p + 6) > BOOTBASE + BOOTSIZE || !(p[10] & 1) ||
        get32(p + 22) < BOOTBASE + 26 || get32(p + 22) >= BOOTBASE + BOOTSIZE) {
        miglog(1, "invalid container boot extension %.500s", path);
        exit(1);
    }
    for (n = PVSTUB - BOOTBASE; n < BOOTSIZE - 4 && !p[n]; n++)
        ;
    if (n == BOOTSIZE - 4) {
        pvstubs();
        memcpy((char *)PVSTUB, (char *)pvstub, sizeof pvstub);
        pvstubbed = 1;
    }
    if (earlyroot)
        preload(earlyroot);
    if (mprotect((caddr_t)BOOTBASE, BOOTSIZE, PROT_READ | PROT_EXEC) < 0)
        fail("boot extension protection");
}

static void
fail(message)
	char *message;
{
	miglog(1, "%.500s: %s", message, strerror(errno));
	exit(1);
}

static unsigned long
get32(p)
	unsigned char *p;
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
	    (unsigned long)p[2] << 8 | p[3];
}

static int
readrom(path)
	char *path;
{
	unsigned long n = 0;
	int fd, count;
	unsigned char extra;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		fail(path);
	while (n < ROMSIZE) {
		count = read(fd, (char *)rombuf + n, ROMSIZE - n);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			fail(path);
		if (count == 0)
			break;
		n += count;
	}
	do { count = read(fd, (char *)&extra, 1); } while (count < 0 && errno == EINTR);
	if (count < 0)
		fail(path);
	close(fd);
	if (n != ROMSIZE || count != 0) {
		miglog(1, "%.500s: ROM must be exactly 512 KiB", path);
		return 0;
	}
	return romok(rombuf, path);
}

/* the A4000 Kickstart 3.2 (47.96), by sum, CRC and header */
static int
romok(b, name)
	unsigned char *b;
	char *name;
{
	unsigned long sum = 0, prev, crc = 0xffffffffUL, i;
	int bit;

	for (i = 0; i < ROMSIZE; i += 4) {
		prev = sum;
		sum = (sum + get32(b + i)) & 0xffffffffUL;
		if (sum < prev)
			sum++;
	}
	for (i = 0; i < ROMSIZE; i++) {
		crc ^= b[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
	}
	if (sum != 0xffffffffUL || (crc ^ 0xffffffffUL) != 0x9bb8fc93UL ||
	    get32(b) != 0x11144ef9UL || get32(b + 4) != 0xf800d2UL) {
		miglog(1, "%.500s: expected the A4000 Kickstart 3.2 (47.96) ROM", name);
		return 0;
	}
	return 1;
}

/* KEY=value from FILE (1), or 0; a number in megabytes */
static int
setting(file, key, mb)
	char *file, *key;
	unsigned long *mb;
{
	char line[128], *end;
	unsigned long v;
	int n = strlen(key), found = 0;
	FILE *f = fopen(file, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, key, n) && line[n] == '=') {
			errno = 0;
			v = strtoul(line + n + 1, &end, 10);
			if (!errno && end != line + n + 1 && (*end == '\n' || !*end) && v <= 128) {
				*mb = v;
				found = 1;
			} else
				fprintf(stderr, "startmig: %s: %s is not 0 to 128\n", file, key);
		}
	fclose(f);
	return found;
}

/* code in the A4000 3.2 (47.96) Kickstart, and what replaces it */
static struct { unsigned long at; int n; unsigned short was[6], now[6]; } sites[] = {
	/* timer.device: the two E-clock reads, through the stub */
	{ 0xfd4f82, 6, { 0x1629, 0x0700, 0x1829, 0x0600, 0x1429, 0x0700 },
	  { 0x4eb9, PVECLOCK >> 16, PVECLOCK & 0xffff, 0x4e71, 0x4e71, 0x4e71 } },
	{ 0xfd4f9e, 6, { 0x1629, 0x0700, 0x1829, 0x0600, 0x1429, 0x0700 },
	  { 0x4eb9, PVECLOCK >> 16, PVECLOCK & 0xffff, 0x4e71, 0x4e71, 0x4e71 } },
	/*
	 * exec's task switch: fsave, which user mode cannot run, becomes an
	 * idle frame so the FPU registers are always saved; frestore skips it.
	 */
	{ 0xf81722, 4, { 0xf325, 0x4a15, 0x6700, 0xfe56 }, { 0x2b3c, 0x4100, 0x0000, 0x4e71 } },
	{ 0xf81770, 1, { 0xf35d }, { 0x588d } }
};

/*
 * Patches the loaded Kickstart copy: inline Disable and Enable write the
 * INTENA master bit to guest memory, Enable through its stub, and the
 * sites above.  The checksum's fix-up long keeps the reset check passing.
 * Returns the number of changes.
 */
static int
pvpatch(rom)
	unsigned char *rom;
{
	unsigned long i, sum, prev, a;
	int n = 0, j, k;
	for (i = 0; i + 8 <= ROMSIZE; i += 2) {
		if (get32(rom + i + 4) != 0xdff09aUL || rom[i] != 0x33 || rom[i + 1] != 0xfc ||
		    rom[i + 3] != 0 || (rom[i + 2] != 0x40 && rom[i + 2] != 0xc0))
			continue;
		if (rom[i + 2] == 0x40) {
			a = AMIGA_PV_BASE;
			rom[i + 4] = a >> 24; rom[i + 5] = a >> 16; rom[i + 6] = a >> 8; rom[i + 7] = a;
		} else {
			a = PVENABLE;
			rom[i] = 0x4e; rom[i + 1] = 0xb9;
			rom[i + 2] = a >> 24; rom[i + 3] = a >> 16; rom[i + 4] = a >> 8; rom[i + 5] = a;
			rom[i + 6] = 0x4e; rom[i + 7] = 0x71;
		}
		n++;
		i += 6;
	}
	for (j = 0; j < sizeof sites / sizeof sites[0]; j++) {
		a = sites[j].at - ROMBASE;
		for (k = 0; k < sites[j].n && get16(rom + a + 2 * k) == sites[j].was[k]; k++)
			;
		if (k < sites[j].n) {
			miglog(0, "Kickstart copy: %lx differs, left alone", sites[j].at);
			continue;
		}
		for (k = 0; k < sites[j].n; k++)
			put16(rom + a + 2 * k, sites[j].now[k]);
		n++;
	}
	memset(rom + 0x7ffe8, 0, 4);
	for (sum = 0, i = 0; i < ROMSIZE; i += 4) {
		prev = sum;
		sum += get32(rom + i);
		if (sum < prev)
			sum++;
	}
	put32(rom + 0x7ffe8, ~sum);
	return n;
}

static void
region(address, size, shared)
	unsigned long address, size;
	int shared;
{
	int fd = open("/dev/zero", O_RDWR);
	char what[48];
	if (fd < 0)
		fail("/dev/zero");
	sprintf(what, "mmap %#lx+%#lx", address, size);
	if (mmap((caddr_t)address, size, PROT_READ | PROT_WRITE | PROT_EXEC,
	    (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED, fd, 0) == (caddr_t)-1)
		fail(what);
	close(fd);
}

static void
nothing(sig)
    int sig;
{
    (void)sig;
}

/*
 * The display's part of card memory, moved by the guest itself at the
 * display helper's SIGUSR1: to RAM while the helper copies another screen
 * to the display, back to the display when the display-sized one returns.
 * Each move keeps the contents; Amiga interrupts wait meanwhile.
 */
static int movefd = -1, ackfd = -1, zfd = -1, pvmode;
static unsigned long winsize;

static void
movecard(sig)
    int sig;
{
    volatile struct mig_rtg *r = (volatile struct mig_rtg *)MIG_RTG_BASE;
    volatile unsigned short *pv = (volatile unsigned short *)AMIGA_PV_BASE;
    volatile unsigned short *intena = (volatile unsigned short *)0xdff09aUL;
    volatile unsigned short *intenar = (volatile unsigned short *)0xdff01cUL;
    caddr_t t, m;
    unsigned short was;
    unsigned int want = r->ram != 0;
    char c = 1;
    (void)sig;
    if (pvmode) {
        was = *pv;
        *pv = 0x4000;
    } else {
        was = *intenar & 0x4000 ? 0xc000 : 0x4000;
        *intena = 0x4000;
    }
    if (want != r->inram &&
        (t = mmap((caddr_t)0, winsize, PROT_READ | PROT_WRITE, MAP_PRIVATE, zfd, 0)) != (caddr_t)-1) {
        memcpy(t, (char *)MIG_RTG_VRAM, winsize);
        m = want ? mmap((caddr_t)MIG_RTG_VRAM, winsize, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_FIXED, zfd, 0) :
            mmap((caddr_t)MIG_RTG_VRAM, winsize, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_FIXED, movefd, 0);
        if (m != (caddr_t)-1) {
            memcpy((char *)MIG_RTG_VRAM, t, winsize);
            r->inram = want;
        }
        munmap(t, winsize);
    }
    write(ackfd, &c, 1);
    /* last: an interrupt held meanwhile reaches the guest, not this handler */
    if (pvmode)
        *pv = was;
    else
        *intena = was;
}

static void
displaygone(sig)
    int sig;
{
    static char message[] = "startmig: helper process exited\n";
    (void)sig;
    write(2, message, sizeof message - 1);
    _exit(1);
}

/* the session's mapping, page-rounded; the card's RAM follows */
#define VRAMEND(fi) (MIG_RTG_VRAM + (((fi)->fi_size + pagesize - 1) & ~(pagesize - 1)))
static unsigned long pagesize = 4096;

static unsigned int
get16(p)
	unsigned char *p;
{
	return p[0] << 8 | p[1];
}

/*
 * The display-sized mode.  SYS: shows its resolution in the P96 settings,
 * and the Workbench mode when the screen mode preferences select it, at
 * the display's size, without changing the files.
 */
#define NATIVE_ID 0x50011000UL
static char p96path[] = "Devs/Picasso96Settings";
static char scrmpath[] = "Prefs/Env-Archive/Sys/screenmode.prefs";
static unsigned char p96set[2][8192], scrm[2][512];
static unsigned long clocks[] = { 25175000, 40000000, 65000000, 108000000 };
/* the settings and Workbench mode the guest will read, for the early bind */
static unsigned char *p96data, *smode;
static unsigned long p96len;

/* SYS:path into b; its size, or 0 */
static unsigned long
sysread(root, path, b, max)
	char *root, *path;
	unsigned char *b;
	unsigned long max;
{
	struct mig_hostfs *fs = mig_hostfs_create();
	static struct mig_fs_request r;
	unsigned long n = 0;
	unsigned int id;
	if (!fs) return 0;
	if (mig_hostfs_mount(fs, 0, "Amiga", root, 1) == 0) {
		memset(&r, 0, sizeof r);
		r.op = MIG_FS_OPEN;
		strcpy(r.path, path);
		mig_hostfs_dispatch(fs, &r);
		id = r.result;
		while (!r.error && n < max) {
			memset(&r, 0, sizeof r);
			r.op = MIG_FS_READ; r.handle = id;
			r.length = max - n > MIG_FS_DATA ? MIG_FS_DATA : max - n;
			mig_hostfs_dispatch(fs, &r);
			if (r.error || r.result <= 0) break;
			memcpy(b + n, r.data, r.result);
			n += r.result;
		}
		/* larger than b: not used */
		if (n == max) n = 0;
	}
	mig_hostfs_destroy(fs);
	return n;
}

/* IFF chunk at b + i of n bytes: its length, or -1 at the end */
static long
chunk(b, i, n)
	unsigned char *b;
	unsigned long i, n;
{
	unsigned long len;
	if (i + 8 > n) return -1;
	len = get32(b + i + 4);
	return len > n - i - 8 ? -1 : (long)len;
}

/* the native resolution and its modes at w x h; 1 if found */
static int
p96native(b, n, w, h)
	unsigned char *b;
	unsigned long n, w, h;
{
	unsigned long i, ht = (w * 5 / 4 + 7) & ~7UL, vt = h + 45, best = 0, k, d, dbest = ~0UL;
	unsigned char *c;
	int in = 0, found = 0;
	long len;
	if (n < 12 || memcmp(b, "FORM", 4) || memcmp(b + 8, "P96S", 4)) return 0;
	for (k = 0; k < 4; k++) {
		d = clocks[k] > ht * vt * 60 ? clocks[k] - ht * vt * 60 : ht * vt * 60 - clocks[k];
		if (d < dbest) dbest = d, best = k;
	}
	for (i = 12; (len = chunk(b, i, n)) >= 0; i += 8 + ((len + 1) & ~1L)) {
		c = b + i + 8;
		if (!memcmp(b + i, "RSHD", 4)) {
			in = len >= 8 && get32(c) == NATIVE_ID;
			if (in) {
				put16(c + 4, w); put16(c + 6, h);
				found = 1;
			}
		} else if (in && !memcmp(b + i, "MIHD", 4) && len >= 34) {
			put16(c + 4, w); put16(c + 6, h);
			put16(c + 10, ht); put16(c + 20, vt);
			c[28] = best; put32(c + 30, clocks[best]);
		}
	}
	return found;
}

/*
 * The Workbench mode into w, h: the native one at the display's size when
 * selected, then shown so.  Returns 1 when it is the native mode.
 */
static int
native(fi, root, w, h)
	struct fbinfo *fi;
	char *root;
	unsigned long *w, *h;
{
	unsigned long n, m, i;
	long len;
	int p96 = 0;
	*w = 640; *h = 480;
	if ((n = sysread(root, p96path, p96set[0], sizeof p96set[0])) != 0) {
		memcpy(p96set[1], p96set[0], n);
		if ((p96 = p96native(p96set[1], n, fi->fi_width, fi->fi_height)) != 0)
			mig_hostfs_overlay(p96path, p96set[0], p96set[1], n);
		p96data = p96set[p96 != 0];
		p96len = n;
	}
	m = sysread(root, scrmpath, scrm[0], sizeof scrm[0]);
	memcpy(scrm[1], scrm[0], m);
	for (i = 12; (len = chunk(scrm[1], i, m)) >= 0; i += 8 + ((len + 1) & ~1L))
		if (!memcmp(scrm[1] + i, "SCRM", 4) && len >= 28) {
			if (p96 && get32(scrm[1] + i + 24) == NATIVE_ID) {
				put16(scrm[1] + i + 28, fi->fi_width);
				put16(scrm[1] + i + 30, fi->fi_height);
				put16(scrm[1] + i + 32, 8);
				mig_hostfs_overlay(scrmpath, scrm[0], scrm[1], m);
			}
			smode = scrm[1] + i + 24;
			*w = get16(scrm[1] + i + 28);
			*h = get16(scrm[1] + i + 30);
			return p96 && get32(scrm[1] + i + 24) == NATIVE_ID;
		}
	return 0;
}

/*
 * The board's files from SYS: into the boot extension, so the guest binds
 * it before DOS.  Only for the rtg.library whose workings that relies on;
 * any other binds after DOS as before.
 */
#define RTG_SIZE 216548UL
#define RTG_CRC 0x7336517cUL
static char infopath[] = "DEVS/Monitors/Container.info";

static unsigned long
crc32(p, n)
	unsigned char *p;
	unsigned long n;
{
	unsigned long c = 0xffffffffUL;
	int k;
	while (n--)
		for (c ^= *p++, k = 0; k < 8; k++)
			c = c >> 1 ^ (0xedb88320UL & -(c & 1));
	return ~c & 0xffffffffUL;
}

/* an icon's tool types into t, each NUL-terminated; their length, or 0 */
static unsigned long
tooltypes(b, n, t, max)
	unsigned char *b, *t;
	unsigned long n, max;
{
	unsigned long i = 78, k, len, count, out = 0, img;
	if (n < 78 || get16(b) != 0xe310 || !get32(b + 54))
		return 0;
	if (get32(b + 66))
		i += 56;
	/* the images, then the default tool */
	for (k = 22; k <= 26; k += 4)
		if (get32(b + k)) {
			if (i + 20 > n) return 0;
			img = get32(b + i + 10) ? (get16(b + i + 4) + 15) / 16 * 2 *
			    get16(b + i + 6) * get16(b + i + 8) : 0;
			i += 20 + img;
		}
	if (get32(b + 50)) {
		if (i + 4 > n) return 0;
		i += 4 + get32(b + i);
	}
	if (i + 4 > n) return 0;
	count = get32(b + i) / 4 - 1;
	for (i += 4; count--; i += len) {
		if (i + 4 > n) return 0;
		len = get32(b + i);
		i += 4;
		if (i + len > n || out + len + 1 > max) return 0;
		for (k = 0; k < len && b[i + k]; k++)
			t[out++] = b[i + k];
		t[out++] = 0;
	}
	return out;
}

/* the value of tool type key (upper case) in a NUL-separated list, or 0 */
static char *
tooltype(t, n, key)
	char *t, *key;
	unsigned long n;
{
	unsigned long k;
	char *e = t + n;
	for (; t < e; t += strlen(t) + 1) {
		for (k = 0; key[k] && (t[k] & ~32) == key[k]; k++)
			;
		if (!key[k] && t[k] == '=')
			return t + k + 1;
	}
	return 0;
}

static void
preload(root)
	char *root;
{
	static unsigned char info[4096];
	static char card[80];
	struct mig_early *d = (struct mig_early *)MIG_EARLY_BASE;
	unsigned char *at = (unsigned char *)d + sizeof *d, *tt;
	unsigned long n, i;
	char *board, *why = 0;
	static char *files[][2] = {
		{ "libs/picasso96/rtg.library", "LIBS/Picasso96/rtg.library" },
		{ "libs/iffparse.library", "LIBS/iffparse.library" },
		{ "libs/picasso96/fastlayers.library", "LIBS/Picasso96/fastlayers.library" },
		{ "prefs/env-archive/picasso96/disableamigablitter",
		  "Prefs/Env-Archive/Picasso96/DisableAmigaBlitter" },
		{ "c/container-input", "C/container-input" },
		{ card + 32, card }
	};
	memset((char *)d, 0, sizeof *d);
	if (!p96len || !smode || !(get32(smode) & 0xf0000000UL)) {
		miglog(0, "Picasso96 binds after DOS: Workbench is not in a board mode");
		return;
	}
	tt = at;
	n = tooltypes(info, sysread(root, infopath, info, sizeof info), tt, 2048);
	board = n ? tooltype((char *)tt, n, "BOARDTYPE") : 0;
	if (!board || strlen(board) > 20 || tooltype((char *)tt, n, "SETTINGSFILE")) {
		miglog(0, "Picasso96 binds after DOS: %s has no BOARDTYPE, or a SETTINGSFILE", infopath);
		return;
	}
	sprintf(card, "LIBS/Picasso96/%s.card", board);
	for (i = 0; card[i]; i++)
		card[32 + i] = card[i] >= 'A' && card[i] <= 'Z' ? card[i] + 32 : card[i];
	card[32 + i] = 0;
	strcpy(d->file[0].name, MIG_EARLY_TOOLTYPES);
	d->file[0].offset = at - (unsigned char *)d;
	d->file[0].size = n;
	at += (n + 3) & ~3UL;
	strcpy(d->file[1].name, MIG_EARLY_SCREENMODE);
	d->file[1].offset = at - (unsigned char *)d;
	d->file[1].size = 12;
	memcpy(at, smode, 12);
	at += 12;
	strcpy(d->file[2].name, "devs/picasso96settings");
	d->file[2].offset = at - (unsigned char *)d;
	d->file[2].size = p96len;
	memcpy(at, p96data, p96len);
	at += (p96len + 3) & ~3UL;
	d->count = 3;
	for (i = 0; i < sizeof files / sizeof files[0]; i++) {
		n = sysread(root, files[i][1], at, MIG_EARLY_END - (unsigned long)at);
		if (!i && (n != RTG_SIZE || crc32(at, n) != RTG_CRC))
			why = "rtg.library is not 40.3945";
		if (!n && (i < 2 || files[i][1] == card))
			why = "a file is missing";
		if (why) {
			miglog(0, "Picasso96 binds after DOS: %s", why);
			d->count = 0;
			return;
		}
		if (!n)
			continue;
		strcpy(d->file[d->count].name, files[i][0]);
		d->file[d->count].offset = at - (unsigned char *)d;
		d->file[d->count++].size = n;
		at += (n + 3) & ~3UL;
	}
	if (twoscreens && d->count < MIG_EARLY_FILES) {
		strcpy(d->file[d->count].name, MIG_EARLY_SCREENS);
		d->file[d->count].offset = at - (unsigned char *)d;
		d->file[d->count++].size = 0;
	}
	d->magic = MIG_EARLY_MAGIC;
	miglog(0, "Picasso96 binds before DOS: %lu KB from SYS:", (unsigned long)(at - (unsigned char *)d) >> 10);
}

/*
 * The container's startup, shown once the board bound before DOS: its
 * output then opens the boot shell on the card.  Bound late, the shell
 * would open Workbench in a native mode, so it stays silent.
 */
static char startpath[] = "S/Startup-Sequence";
static unsigned char startup[2][4096];

static void
showstartup(root)
	char *root;
{
	unsigned long n = sysread(root, startpath, startup[0], sizeof startup[0]), i;
	unsigned char *v = startup[1], *line;
	if (n < 20 || memcmp(startup[0], "; Container startup", 19))
		return;
	memcpy(v, startup[0], n);
	for (i = 0, line = v; i + 5 <= n; i++) {
		if (v[i] == '\n')
			line = v + i + 1;
		else if (!memcmp(v + i, ">NIL:", 5) && memcmp(line, "EndCLI", 6))
			memset(v + i, ' ', 5);
	}
	mig_hostfs_overlay_when(startpath, startup[0], startup[1], n,
	    &((struct mig_early_status *)MIG_EARLY_STATUS)->status, MIG_EARLY_BOUND);
}

/*
 * The card's memory.  The display is the card's memory when the Workbench
 * mode is as wide as the display or its rows: then from the first row,
 * centred vertically; the card gives display-wide bitmaps the display's
 * rows.  Otherwise RAM, which the host copies to the display.  Returns 1
 * for the display.
 */
static int
vram(fi, w, h)
	struct fbinfo *fi;
	unsigned long w, h;
{
	unsigned long y = 0, rb = fi->fi_rowbytes, base;
	int direct;
	struct mig_rtg *r = (struct mig_rtg *)MIG_RTG_BASE;
	direct = (w == fi->fi_width || ((w + 3) & ~3UL) == rb) && h <= fi->fi_height;
	if (direct) {
		y = (fi->fi_height - h) / 2;
		base = MIG_RTG_VRAM + fi->fi_offset + y * rb;
		mig_rtg_init(r, fi->fi_width, fi->fi_height, base, rb,
		    VRAMEND(fi) - base + MIG_RTG_EXTRA);
	} else {
		/* before the helpers fork, so the display helper shares it */
		region(MIG_RTG_VRAM, MIG_RTG_EXTRA, 1);
		mig_rtg_init(r, fi->fi_width, fi->fi_height, MIG_RTG_VRAM, rb, MIG_RTG_EXTRA);
		r->copy = r->track = 1;
	}
	r->cursor = 1;
	miglog(0, "card memory: %lu KB %s (%lux%lu screen, display %lux%lu rows %lu)",
	    r->memory_size >> 10, direct ? "is the display" : "in RAM, copied",
	    w, h, fi->fi_width, fi->fi_height, rb);
	return direct;
}

/*
 * Loads the card's view of the display now, while the session is in front,
 * so that it maps the display memory the helper draws the pointer into.
 */
static void
pretouch(fi, fd)
	struct fbinfo *fi;
	int fd;
{
	struct fbstate st;
	volatile unsigned char *p = (volatile unsigned char *)MIG_RTG_VRAM;
	unsigned long i, sum = 0;
	/* the switch to the front completes asynchronously */
	for (i = 0; i < 100 && ioctl(fd, FBIOGSTATE, &st) == 0 && st.st_front != st.st_session; i++)
		poll((struct pollfd *)0, 0, 20);
	if (i == 100)
		miglog(0, "display session %ld not in front (%ld)", st.st_session, st.st_front);
	for (i = 0; i < fi->fi_size; i += pagesize)
		sum += p[i];
	(void)sum;
}

static void
startdisplay(fd, go, ack, fi)
    int fd, go, ack;
    struct fbinfo *fi;
{
    int ready[2], life[2], n;
    pid_t pid;
    char success;
    struct pollfd p;
    struct sigaction sa;
    memset((char *)&sa, 0, sizeof sa);
    sa.sa_handler = displaygone;
    sa.sa_flags = SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, (struct sigaction *)0) < 0)
        fail("SIGCHLD");
    if (pipe(ready) < 0 || pipe(life) < 0)
        fail("display pipe");
    pid = fork();
    if (pid < 0)
        fail("display fork");
    if (pid == 0) {
        close(ready[0]); close(life[1]); close(gofd);
        if (ackfd >= 0) close(ackfd);
        _exit(migdisp(ready[1], life[0], go, ack, fd, fi));
    }
    close(ready[1]); close(life[0]);
    displaylife = life[1];
    /* Keeping the write end open ties the display to this process. */
    p.fd = ready[0]; p.events = POLLIN;
    do { n = poll(&p, 1, 5000); } while (n < 0 && errno == EINTR);
    if (n <= 0 || read(ready[0], &success, 1) != 1 || success != 1) {
        miglog(1, "display session did not become ready");
        kill(pid, SIGTERM);
        exit(1);
    }
    close(ready[0]);
}

static void
startfilesystem(dev, go, root, readonly)
    int dev, go, readonly;
    char *root;
{
    int ready[2], life[2], n;
    pid_t pid;
    char success;
    struct pollfd p;
    if (pipe(ready) < 0 || pipe(life) < 0)
        fail("filesystem pipe");
    pid = fork();
    if (pid < 0) fail("filesystem fork");
    if (pid == 0) {
        /* dev stays open: the broker sleeps on the guest's doorbell through it */
        close(ready[0]); close(life[1]); close(gofd);
        if (ackfd >= 0) close(ackfd);
        if (displaylife >= 0) close(displaylife);
        _exit(mig_fs_broker(ready[1], life[0], go, root, readonly));
    }
    close(ready[1]); close(life[0]);
    p.fd = ready[0]; p.events = POLLIN;
    do { n = poll(&p, 1, 5000); } while (n < 0 && errno == EINTR);
    if (n <= 0 || read(ready[0], &success, 1) != 1 || success != 1) {
        miglog(1, "filesystem helper did not become ready");
        kill(pid, SIGTERM);
        exit(1);
    }
    close(ready[0]);
}

/* the sound helper: the AHI driver's output to the host's sound service */
static void
startsound(dev)
    int dev;
{
    int ready[2], life[2], n;
    pid_t pid;
    char success;
    struct pollfd p;
    if (pipe(ready) < 0 || pipe(life) < 0)
        fail("sound pipe");
    pid = fork();
    if (pid < 0) fail("sound fork");
    if (pid == 0) {
        close(ready[0]); close(life[1]); close(gofd);
        if (ackfd >= 0) close(ackfd);
        if (displaylife >= 0) close(displaylife);
        _exit(mig_snd_helper(ready[1], life[0], dev));
    }
    close(ready[1]); close(life[0]);
    p.fd = ready[0]; p.events = POLLIN;
    do { n = poll(&p, 1, 5000); } while (n < 0 && errno == EINTR);
    if (n <= 0 || read(ready[0], &success, 1) != 1 || success != 1) {
        miglog(1, "sound helper did not become ready");
        kill(pid, SIGTERM);
        exit(1);
    }
    close(ready[0]);
}

/*
 * The log goes in a writable SYS:, otherwise /tmp; a black screen
 * leaves the startup's progress there.
 */
static void
openlog(root, writable)
    char *root;
    int writable;
{
    char path[1100];
    int fd = -1;
    if (writable && strlen(root) < 1000) {
        sprintf(path, "%s/.startmig.log", root);
        unlink(path);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    }
    if (fd < 0) {
        sprintf(path, "/tmp/startmig.%ld.log", (long)getuid());
        unlink(path);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    }
    if (fd < 0) return;
    miglog_fd = fd;
    miglog(1, "log in %s", path);
}

static int
checksystem(char *root)
{
    struct mig_hostfs *fs = mig_hostfs_create();
    struct mig_fs_request request;
    int valid;
    if (!fs) return 0;
    if (mig_hostfs_mount(fs, 0, "Amiga", root, 1) < 0) {
        mig_hostfs_destroy(fs);
        return 0;
    }
    memset(&request, 0, sizeof request);
    request.op = MIG_FS_OPEN;
    strcpy(request.path, "S/Startup-Sequence");
    mig_hostfs_dispatch(fs, &request);
    valid = !request.error;
    mig_hostfs_destroy(fs);
    return valid;
}

int
main(argc, argv)
	int argc;
	char **argv;
{
	char *rom = "/etc/amiga/kicka4000.rom", *boot = "/etc/amiga/container-boot.rom", *end;
	char sysroot[1024], *home, *env = 0;
	struct stat sb;
	struct passwd *pw;
	int readonly = 0, rootreadonly = 0, census = 0, kick = 1, romarg = 0, hostrom = 0;
	unsigned long fastmb = 64;
	char envfile[1100];
	int fastarg = 0, pv = 1, n, dfd, zerofd, direct, go[2], ack[2];
	stack_t ss;
	static char altstack[16384];
	unsigned long w, h;
	struct fbinfo fi;
	struct amigaenter ae;
	struct amigainfo info;
	struct sigaction sa;
	struct rlimit rl;
	int i, check = 0, probe = 0, fd, lockfd = -1;

	miglog_start(-1);
	memset((char *)&ae, 0, sizeof ae);
	if (sysconf(_SC_PAGESIZE) > 0)
		pagesize = sysconf(_SC_PAGESIZE);

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--check")) check = 1;
		else if (!strcmp(argv[i], "--probe")) probe = 1;
		else if (!strcmp(argv[i], "--readonly")) readonly = 1;
		else if (!strcmp(argv[i], "--census")) census = 1;
		else if (!strcmp(argv[i], "--nokick")) kick = 0;
		else if (!strcmp(argv[i], "--nopv")) pv = 0;
		else if (!strcmp(argv[i], "--menu")) migmenu = 1;
		else if (!strcmp(argv[i], "--screens")) twoscreens = 1;
		else if ((!strcmp(argv[i], "-r") || !strcmp(argv[i], "-rom")) && i + 1 < argc)
			rom = argv[++i], romarg = 1;
		else if (!strcmp(argv[i], "-e") && i + 1 < argc)
			env = argv[++i];
		else if (!strcmp(argv[i], "-boot") && i + 1 < argc)
			boot = argv[++i];
		else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
			char *value = argv[++i];
			errno = 0;
			fastmb = strtoul(value, &end, 10);
			if (errno || end == value || *end || fastmb > 128)
				goto usage;
			fastarg = 1;
		} else goto usage;
	}
	if (check + probe > 1)
		goto usage;
	if (readonly && (check || probe)) goto usage;
	if (!probe && !check) {
		home = getenv("HOME");
		if ((!home || !*home) && (pw = getpwuid(getuid())) != 0)
			home = pw->pw_dir;
		if (env && strcmp(env, "default") != 0) {
			if (envroot("amiga", env, home, sysroot, sizeof sysroot) < 0 ||
			    stat(sysroot, &sb) < 0 || (sb.st_mode & S_IFMT) != S_IFDIR) {
				fprintf(stderr, "startmig: no environment %s; run makeamiga -e %s\n", env, env);
				return 1;
			}
		} else if (mig_sysroot(home, "/amiga/sys", sysroot, sizeof sysroot, &rootreadonly) < 0)
			fail("Amiga system directory; install /amiga/sys and run makeamiga");
		if (rootreadonly)
			fprintf(stderr, "startmig: SYS: is /amiga/sys, read-only; run makeamiga for your own\n");
		/* fast RAM: -m, else the environment's, else the system's; read before
		 * the lock, which closing the metadata file would drop */
		sprintf(envfile, "%.1000s/.env", sysroot);
		if (!fastarg && !setting(envfile, "fastmb", &fastmb))
			setting("/etc/default/amiga", "FAST_MB", &fastmb);
		readonly |= rootreadonly;
		if (!readonly && (lockfd = envlock("startmig", "amiga", sysroot)) == -1)
			return 1;
		openlog(sysroot, !readonly);
		miglog(0, "SYS: is %.500s%s", sysroot, readonly ? ", read-only" : "");
		miglog(0, "fast RAM %lu MB", fastmb);
	}
	/* the file now if named; else after the machine's own Kickstart is tried */
	if (romarg || check) {
		if (!readrom(rom))
			return 1;
		miglog(0, "Kickstart %.500s verified", rom);
	}
	if (check) {
		printf("A4000 Kickstart 3.2 (47.96): checksum and CRC verified\n");
		return 0;
	}
	if (!probe && !checksystem(sysroot)) {
		miglog(1, "%.500s has no readable S/Startup-Sequence; run makeamiga -f", sysroot);
		return 1;
	}
	for (i = 3; i < 256; i++)
		if (i != lockfd && i != miglog_fd)
			close(i);
	fd = open("/dev/amiga", O_RDWR);
	if (fd < 0 && (errno == ENXIO || errno == ENODEV)) {
		miglog(1, "/dev/amiga: Amiga module not loaded; as root run /usr/sbin/amigareg /usr/aux/lib/mod.d");
		return 1;
	}
	if (fd < 0 && errno == EACCES) {
		miglog(1, "/dev/amiga: permission denied; the display group may use it");
		return 1;
	}
	if (fd < 0)
		fail("/dev/amiga");
	if (ioctl(fd, AMIGAIOC_INFO, &info) < 0)
		fail("AMIGAIOC_INFO");
	if (info.ai_version != AMIGA_ABI_VERSION) {
		miglog(1, "Amiga module ABI %lu, expected %d", info.ai_version, AMIGA_ABI_VERSION);
		return 1;
	}
	if (!probe && !(info.ai_features & (AMIGA_FEAT_BOOT | AMIGA_FEAT_EXPERIMENTAL))) {
		miglog(1, "Amiga environment needs a 68040");
		return 1;
	}
	if (!romarg) {
		if (ioctl(fd, AMIGAIOC_MAPROM, 0) > 0 && romok((unsigned char *)ROMBASE, "the machine's Kickstart")) {
			hostrom = 1;
			miglog(0, "Kickstart: the machine's own");
		} else {
			if (!readrom(rom))
				return 1;
			miglog(0, "Kickstart %.500s verified", rom);
		}
	}
	/* the guest's memory exceeds the default soft limit on mappings */
	if (getrlimit(RLIMIT_VMEM, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_VMEM, &rl);
	}
	/* the guest rings the helpers; unless --nokick, they ring it on input */
	if (!probe && !(info.ai_features & AMIGA_FEAT_KICK)) {
		miglog(1, "Amiga module has no doorbell; install the current one");
		return 1;
	}
	mig_fs_bell = fd;
	if (kick)
		migkick = fd;
	region(0UL, CHIPSIZE, 1);
	/* helpers fork before the private regions, which each fork would reserve again */
	if (!probe) {
		region(MIG_RTG_BASE, MIG_RTG_MAP_SIZE, 1);
		region(MIG_INPUT_BASE, MIG_INPUT_MAP_SIZE, 1);
		region(MIG_FS_BASE, MIG_FS_MAP_SIZE, 1);
		region(MIG_SND_BASE, MIG_SND_MAP_SIZE, 1);
		if ((dfd = migdisp_open(&fi)) < 0)
			return 1;
		if (!native(&fi, sysroot, &w, &h))
			miglog(0, "Workbench mode %lux%lu, not the display's", w, h);
		direct = vram(&fi, w, h);
		showstartup(sysroot);
		/* the helpers sleep until the guest has entered, when this closes */
		if (pipe(go) < 0)
			fail("pipe");
		gofd = go[1];
		ack[0] = -1;
		if (direct) {
			if (pipe(ack) < 0 || (zfd = open("/dev/zero", O_RDWR)) < 0)
				fail("card memory moves");
			ackfd = ack[1];
			movefd = dfd;
			winsize = VRAMEND(&fi) - MIG_RTG_VRAM;
		}
		startdisplay(dfd, go[0], ack[0], &fi);
		if (ack[0] >= 0)
			close(ack[0]);
		if (direct) {
			if (mmap((caddr_t)MIG_RTG_VRAM, fi.fi_size, PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_FIXED, dfd, 0) == (caddr_t)-1)
				fail("display mapping");
			pretouch(&fi, dfd);
		}
		startfilesystem(fd, go[0], sysroot, readonly);
		/* the driver's mixing task is woken by the helper, never polls */
		if (migkick >= 0 && (info.ai_features & AMIGA_FEAT_SNDBELL))
			startsound(fd);
		if (direct)
			region(VRAMEND(&fi), MIG_RTG_EXTRA, 0);
		close(go[0]);
		miglog(0, "display and SYS: helpers ready");
		/* the helpers run first when both wait for the same tick */
		nice(1);
	}
	/*
	 * Private memory: the kernel reserves memory and swap for all of it
	 * up front, though pages cost nothing until touched.
	 */
	if (fastmb && mmap((caddr_t)FASTBASE, fastmb << 20, PROT_READ | PROT_WRITE | PROT_EXEC,
	    MAP_PRIVATE | MAP_FIXED, zerofd = open("/dev/zero", O_RDWR), 0) == (caddr_t)-1) {
		miglog(1, "fast RAM of %lu MB: %s; free memory and swap are too small. "
		    "Add swap, or choose less with -m, fastmb= in the environment's .env "
		    "or FAST_MB= in /etc/default/amiga", fastmb, strerror(errno));
		return 1;
	}
	if (fastmb)
		close(zerofd);
	if (!probe) {
		region(BOOTBASE, BOOTSIZE, 0);
		earlyroot = sysroot;
		loadboot(boot);
	}
	if (!hostrom) {
		region(ROMBASE, ROMSIZE, 0);
		memcpy((char *)ROMBASE, rombuf, ROMSIZE);
		if (!probe && pv && pvstubbed) {
			n = pvpatch((unsigned char *)ROMBASE);
			miglog(0, "Kickstart copy: %d changes", n);
			if (n)
				ae.ae_flags |= AMIGAF_PV;
		}
		if (mprotect((caddr_t)ROMBASE, ROMSIZE, PROT_READ | PROT_EXEC) < 0)
			fail("mprotect");
	}
	/* SIGUSR2 carries virtual interrupts and must be caught */
	memset((char *)&sa, 0, sizeof sa);
	sa.sa_handler = nothing;
	sa.sa_flags = SA_NODEFER;
	if (sigaction(SIGUSR2, &sa, (struct sigaction *)0) < 0)
		fail("SIGUSR2");
	/* on its own stack: the guest's is Amiga memory */
	if (ackfd >= 0) {
		pvmode = (ae.ae_flags & AMIGAF_PV) != 0;
		ss.ss_sp = altstack;
		ss.ss_size = sizeof altstack;
		ss.ss_flags = 0;
		sa.sa_handler = movecard;
		sa.sa_flags = SA_ONSTACK;
		sigemptyset(&sa.sa_mask);
		sigaddset(&sa.sa_mask, SIGUSR2);
		if (sigaltstack(&ss, (stack_t *)0) < 0 || sigaction(SIGUSR1, &sa, (struct sigaction *)0) < 0)
			fail("SIGUSR1");
	}
	if (!probe)
		miglog(1, "starting Kickstart; Workbench follows (the hot key returns here)");
	ae.ae_version = AMIGA_ABI_VERSION;
	ae.ae_chipsize = CHIPSIZE;
	ae.ae_fastsize = fastmb << 20;
	ae.ae_flags |= AMIGAF_PAL | (census ? AMIGAF_CENSUS : 0);
	if (ioctl(fd, AMIGAIOC_ENTER, &ae) < 0)
		fail("Amiga session (AMIGAIOC_ENTER)");
	if (gofd >= 0)
		close(gofd);
	if (probe) {
		if (ioctl(fd, AMIGAIOC_LEAVE, 0) < 0)
			fail("AMIGAIOC_LEAVE");
		close(fd);
		printf("Amiga profile attached and detached; ROM execution skipped\n");
		return 0;
	}
#ifdef __m68k__
	__asm__ __volatile__("mov.l %0,%%sp\n\tjmp (%1)" : : "d" (0x400L), "a" (get32((unsigned char *)ROMBASE + 4)));
#else
	fprintf(stderr, "startmig: ROM execution requires m68k\n");
	return 1;
#endif
	return 0;
usage:
	fprintf(stderr, "usage: startmig [-rom file] [-boot file] [-e env] [-m fast-MB] [--readonly] [--census] [--nokick] [--nopv] [--menu] [--screens] [--check | --probe]\n");
	return 2;
}
