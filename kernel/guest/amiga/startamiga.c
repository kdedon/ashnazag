/*
 * startamiga -- validate local Kickstart media and enter an Amiga profile.
 * SYS: is ~/Amiga for environment 3.2, ~/Amiga/env for others, copied
 * from /amiga/sys at first use and with -R, keeping the user's other
 * files; one writable session each.  A new name is a copy of base (-b),
 * by default the default.  Without -e it is the default, which -d sets;
 * -l lists them.
 */
#include <sys/types.h>
static char *amiga_device;
#include "../include/envmem.h"
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
#include <siginfo.h>
#include <poll.h>
#include <pwd.h>
#include "amigaio.h"
#include "dsio.h"
#include "rtgshare.h"
#include "inputshare.h"
#include "hostfswire.h"
#include "sndshare.h"
#include "floppyshare.h"
#include "earlyshare.h"
#include "w3dshare.h"
#include "hostcall.h"
#include "sysroot.h"
#include "envroot.h"
#include "miglog.h"

#define ROMBASE 0xf80000UL
#define ROMSIZE 0x80000UL
#define CHIPSIZE 0x200000UL
#define FASTBASE 0x08000000UL
#define BOOTBASE 0x00f00000UL
#define BOOTSIZE 0x80000UL
#define FITROOM 0x400000UL

extern int mprotect(), munmap();
extern char etext;
extern int migdisp(), migdisp_open(), migkick, migmenu;
extern unsigned int migdisp_formats;
extern unsigned long migdisp_gsize;

/* shared: the helpers forked meanwhile reserve no copy of their own */
static unsigned char *rombuf;
static int displaylife = -1, gofd = -1;
static void fail(char *);
static int romok();
static unsigned long get32(unsigned char *);
static unsigned int get16();

/* stubs for the patched Kickstart, in the boot extension's last page */
#define PVSTUB 0x00f7f000UL
#define PVENABLE PVSTUB
#define PVECLOCK (PVSTUB + 64)
#define PVTSTOP (PVSTUB + 0x50)	/* the interval timer stopped */
#define PVTSTART (PVSTUB + 0x60)	/* started, d0 E-clock ticks */
#define PVHOST3 (MIG_HOST_PAGE + 24)	/* the interrupt words changed */
#define PVHOST4 (MIG_HOST_PAGE + 32)	/* the interval timer started */
#define PVSETREQ (PVSTUB + 0xb0)	/* jsr, then the INTREQ word */
#define PVSETENA (PVSETREQ + 0x30)	/* the same for INTENA */
#define PVD0REQ (PVSETENA + 0x30)	/* move.w d0,INTREQ */
#define PVA1REQ (PVD0REQ + 0x30)	/* move.w 14(a1),INTREQ */
#define PVLEVEL4 (PVA1REQ + 0x20)	/* the audio levels pending */
#define PVICRA (PVLEVEL4 + 0x20)	/* move.b CIA-A ICR,d2 */
#define PVICRB (PVICRA + 0x20)
#define PVSETICR (PVICRB + 0x20)	/* SetICR's read: either CIA by the resource */
#define PVGFX (PVSTUB + 0x400)	/* graphics' routines to the CPU, 0x20 each */
#define PVD0ENA (PVSTUB + 0x600)	/* move.w d0,INTENA */
#define PVVBENA (PVD0ENA + 0x30)	/* graphics' VERTB enables, 0x10 each */
#define NVBENA 4
#define PVABLEICR (PVVBENA + 0x10 * NVBENA)	/* AbleICR's write: the mask in memory */
#define PV(o) (AMIGA_PV_BASE + (o))
static unsigned char pvstub[PVABLEICR + 0x30 - PVSTUB];
static int pvstubbed;
static char *earlyroot;
static unsigned long fastlen;
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
		0x4a79, AMIGA_PV_BASE,			/* 1: tst.w pv */
		0x4ef9, PVHOST3				/* jmp host call */
	};
	/* timer.device's E-clock read: timer B high, low, high into d3, d4, d2 */
	static unsigned long eclock[] = {
		0x1639, PV(4),				/* move.b pv+4,d3 */
		0x1839, PV(5),				/* move.b pv+5,d4 */
		0x1403,					/* move.b d3,d2 */
		0x4e75
	};
	/* its interval timer: a0 = the timer's control register, then its latch */
	static unsigned long tstop[] = { 0x51f9, PV(14), 0x4e75 };	/* sf pv+14 */
	static unsigned long tstart[] = {
		0x206e, 0x0178,				/* move.l 376(a6),a0 */
		0x50f9, PV(14),				/* st pv+14 */
		0x4ef9, PVHOST4				/* jmp host call */
	};
	/* host calls: illegal, rts */
	static unsigned long host[] = { 0x4afc, 0x4e75, 0x4e71, 0x4e71 };
	/* the word after the jsr ORed into the interrupt word, then the host told */
	static unsigned long setreq[] = {
		0x48e7, 0x8080,				/* movem.l d0/a0,-(sp) */
		0x206f, 0x0008,				/* move.l 8(sp),a0 */
		0x54af, 0x0008,				/* addq.l #2,8(sp) */
		0x3010,					/* move.w (a0),d0 */
		0x0240, 0x7fff,				/* and.w #$7fff,d0 */
		0x8179, PV(8),				/* or.w d0,pv+8 */
		0x4cdf, 0x0101,				/* movem.l (sp)+,d0/a0 */
		0x4ef9, PVHOST3				/* jmp host call */
	};
	/* bit 15 sets, else clears */
	static unsigned long d0req[] = {
		0x4a40,					/* tst.w d0 */
		0x6b0e,					/* bmi.s 1f */
		0x3f00,					/* move.w d0,-(sp) */
		0x4640,					/* not.w d0 */
		0xc179, PV(8),				/* and.w d0,pv+8 */
		0x301f,					/* move.w (sp)+,d0 */
		0x4e75,					/* rts */
		0x3f00,					/* 1: move.w d0,-(sp) */
		0x0240, 0x7fff,				/* and.w #$7fff,d0 */
		0x8179, PV(8),				/* or.w d0,pv+8 */
		0x301f,					/* move.w (sp)+,d0 */
		0x4ef9, PVHOST3				/* jmp host call */
	};
	static unsigned long a1req[] = {
		0x3f00,					/* move.w d0,-(sp) */
		0x3029, 0x000e,				/* move.w 14(a1),d0 */
		0x4640,					/* not.w d0 */
		0xc179, PV(8),				/* and.w d0,pv+8 */
		0x301f,					/* move.w (sp)+,d0 */
		0x4e75
	};
	static unsigned long level4[] = {
		0x323c, 0x0780,				/* move.w #$780,d1 */
		0xc279, PV(10),				/* and.w pv+10,d1 */
		0xc279, PV(8),				/* and.w pv+8,d1 */
		0x4e75
	};
	/* an ICR read clears what it returns; the host only adds bits */
	static unsigned long icr[] = {
		0x1439, PV(12),				/* move.b pv+12,d2 */
		0xb539, PV(12),				/* eor.b d2,pv+12 */
		0x4a02,					/* tst.b d2 */
		0x4e75
	};
	static unsigned long seticr[] = {
		0x206e, 0x0022,				/* move.l 34(a6),a0 */
		0x3208,					/* move.w a0,d1 */
		0xb27c, 0xe001,				/* cmp.w #$e001,d1 */
		0x660e,					/* bne.s 1f */
		0x1239, PV(12),				/* move.b pv+12,d1 */
		0xb339, PV(12),				/* eor.b d1,pv+12 */
		0x4a01,					/* tst.b d1 */
		0x4e75,					/* rts */
		0x1239, PV(13),				/* 1: move.b pv+13,d1 */
		0xb339, PV(13),				/* eor.b d1,pv+13 */
		0x4a01,					/* tst.b d1 */
		0x4e75
	};
	/* bit 7 of d0 sets the other bits in the CIA's mask, else clears them */
	static unsigned long ableicr[] = {
		0x206e, 0x0022,				/* move.l 34(a6),a0 */
		0x3208,					/* move.w a0,d1 */
		0xb27c, 0xe001,				/* cmp.w #$e001,d1 */
		0x43f9, PV(16),				/* lea pv+16,a1 */
		0x6702,					/* beq.s 1f */
		0x5249,					/* addq.w #1,a1 */
		0x1200,					/* 1: move.b d0,d1 */
		0x0881, 0x0007,				/* bclr #7,d1 */
		0x6606,					/* bne.s 2f */
		0x4601,					/* not.b d1 */
		0xc311,					/* and.b d1,(a1) */
		0x6002,					/* bra.s 3f */
		0x8311,					/* 2: or.b d1,(a1) */
		0x4ef9, PVHOST3				/* 3: jmp host call */
	};
	int i;
	pvcode(pvstub, enable, sizeof enable / sizeof enable[0]);
	pvcode(pvstub + (PVABLEICR - PVSTUB), ableicr, sizeof ableicr / sizeof ableicr[0]);
	pvcode(pvstub + (PVSETICR - PVSTUB), seticr, sizeof seticr / sizeof seticr[0]);
	pvcode(pvstub + (PVSETREQ - PVSTUB), setreq, sizeof setreq / sizeof setreq[0]);
	setreq[10] = PV(10);
	pvcode(pvstub + (PVSETENA - PVSTUB), setreq, sizeof setreq / sizeof setreq[0]);
	pvcode(pvstub + (PVD0REQ - PVSTUB), d0req, sizeof d0req / sizeof d0req[0]);
	pvcode(pvstub + (PVA1REQ - PVSTUB), a1req, sizeof a1req / sizeof a1req[0]);
	pvcode(pvstub + (PVLEVEL4 - PVSTUB), level4, sizeof level4 / sizeof level4[0]);
	pvcode(pvstub + (PVICRA - PVSTUB), icr, sizeof icr / sizeof icr[0]);
	icr[1] = icr[3] = PV(13);
	pvcode(pvstub + (PVICRB - PVSTUB), icr, sizeof icr / sizeof icr[0]);
	for (i = 0; i < 5; i++)
		pvcode(pvstub + (MIG_HOST_PAGE - PVSTUB) + 8 * i, host, 4);
	pvcode(pvstub + (PVECLOCK - PVSTUB), eclock, sizeof eclock / sizeof eclock[0]);
	pvcode(pvstub + (PVTSTOP - PVSTUB), tstop, sizeof tstop / sizeof tstop[0]);
	pvcode(pvstub + (PVTSTART - PVSTUB), tstart, sizeof tstart / sizeof tstart[0]);
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
    } else {
        miglog(1, "container boot extension %.500s: its last page is not free", path);
        exit(1);
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

static void
rombuffer()
{
	int fd;

	if (rombuf)
		return;
	if ((fd = open("/dev/zero", O_RDWR)) < 0 ||
	    (rombuf = (unsigned char *)mmap((caddr_t)0, ROMSIZE, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0)) == (unsigned char *)-1)
		fail("Kickstart buffer");
	close(fd);
}

static int
readrom(path)
	char *path;
{
	unsigned long n = 0;
	int fd, count;
	unsigned char extra;

	rombuffer();
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

static char kickname[64];		/* the Kickstart last verified */
static unsigned long kickflags;		/* AMIGAF_* its machine needs */

/* the RomTag named name, whose ID starts with id: its offset + 1, or 0 */
static int
romtag(b, name, id)
	unsigned char *b;
	char *name, *id;
{
	unsigned long i, n, d;

	for (i = 0; i + 26 <= ROMSIZE; i += 2) {
		if (get16(b + i) != 0x4afc || get32(b + i + 2) != ROMBASE + i)
			continue;
		n = get32(b + i + 14) - ROMBASE;
		d = get32(b + i + 18) - ROMBASE;
		if (n < ROMSIZE - 64 && d < ROMSIZE - 64 &&
		    !strcmp((char *)b + n, name) && !strncmp((char *)b + d, id, strlen(id)))
			return i + 1;
	}
	return 0;
}

/* a 512 KB Kickstart, by sum and header */
static int
romok(b, name)
	unsigned char *b;
	char *name;
{
	unsigned long sum = 0, prev, i;

	for (i = 0; i < ROMSIZE; i += 4) {
		prev = sum;
		sum = (sum + get32(b + i)) & 0xffffffffUL;
		if (sum < prev)
			sum++;
	}
	if (sum != 0xffffffffUL || get32(b) != 0x11144ef9UL ||
	    get32(b + 4) < ROMBASE || get32(b + 4) >= ROMBASE + ROMSIZE) {
		miglog(1, "%.500s: not a 512 KB Kickstart ROM", name);
		return 0;
	}
	if (get16(b + 12) < 39) {
		miglog(1, "%.500s: Kickstart %u is older than 3.0", name, get16(b + 12));
		return 0;
	}
	sprintf(kickname, "Kickstart %u.%u", get16(b + 12), get16(b + 14));
	/* the A3000's own SCSI driver */
	kickflags = romtag(b, "scsi.device", "scsidisk") ? AMIGAF_SCSI : 0;
	return 1;
}

/*
 * Fast RAM nobody chose: the largest step the system can reserve, one
 * step less when that is short of mb, so the helpers and X keep room.
 */
static unsigned long
fitfast(mb)
	unsigned long mb;
{
	static unsigned long step[] = { 64, 48, 32, 24, 16, 12, 8, 4 };
	int i, n = sizeof step / sizeof step[0];

	for (i = 0; i < n; i++) {
		if (step[i] > mb)
			continue;
		/* with room for the regions reserved after it */
		if (envmem_region(FASTBASE, (step[i] << 20) + FITROOM, 0, -1) == 0) {
			munmap((caddr_t)FASTBASE, (step[i] << 20) + FITROOM);
			if (step[i] < mb && i + 1 < n)
				i++;
			miglog(0, "fast RAM %lu MB, fitted to free memory", step[i]);
			return step[i];
		}
	}
	return mb;
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
				fprintf(stderr, "startamiga: %s: %s is not 0 to 128\n", file, key);
		}
	fclose(f);
	return found;
}

/*
 * Kickstart code, found by its first m words, and the n words that replace
 * it; patched only when it matches exactly want times.  Where near is set,
 * only the first match within near bytes after the site before counts.
 */
static struct { int m, n, near, want; unsigned short was[9], now[9]; } sites[] = {
	/* timer.device: the two E-clock reads, through the stub */
	{ 6, 6, 0, 2, { 0x1629, 0x0700, 0x1829, 0x0600, 0x1429, 0x0700 },
	  { 0x4eb9, PVECLOCK >> 16, PVECLOCK & 0xffff, 0x4e71, 0x4e71, 0x4e71 } },
	/*
	 * exec's task switch: fsave, which user mode cannot run, becomes an
	 * idle frame so the FPU registers are always saved; frestore skips it.
	 */
	{ 3, 4, 0, 1, { 0xf325, 0x4a15, 0x6700 }, { 0x2b3c, 0x4100, 0x0000, 0x4e71 } },
	{ 1, 1, 256, 1, { 0xf35d }, { 0x588d } },
	/* potgo.resource's WritePotgo: nothing reads the pins back */
	{ 3, 3, 0, 1, { 0x33c0, 0x00df, 0xf034 }, { 0x4e71, 0x4e71, 0x4e71 } },
	/*
	 * gameport.device's VBL reads: no buttons, counters still.  The
	 * pointer comes from the host's input.
	 */
	{ 4, 2, 0, 1, { 0x3028, 0x0016, 0xe268, 0x4640 }, { 0x303c, 0xff00 } },
	{ 4, 2, 0, 1, { 0x3030, 0x100a, 0x3400, 0xe058 }, { 0x4240, 0x4e71 } },
	{ 4, 2, 0, 1, { 0x3030, 0x000a, 0x0240, 0x0303 }, { 0x4240, 0x4e71 } },
	{ 4, 3, 0, 1, { 0x0339, 0x00bf, 0xe001, 0x6604 }, { 0x033c, 0x00ff, 0x4e71 } },
	/*
	 * The power line frequency, timed by CIA-A's TOD against timer B:
	 * 50 Hz, 20000 us per tick, as it finds on a PAL machine.
	 */
	{ 6, 9, 0, 1, { 0x41f9, 0x00bf, 0xe001, 0x43f9, 0x00bf, 0xe001 },
	  { 0x303c, 50, 0x323c, 20000, 0x3540, 0x0028, 0x3541, 0x002a, 0x4e75 } }
};
#define TODSITE	(NSITE - 1)
#define NSITE	(sizeof sites / sizeof sites[0])

static unsigned long pvflags;	/* AMIGAF_PVINT, AMIGAF_PVICR, AMIGAF_PVTIMER as patched */

/* the next code at or after *a matching w under mask m (n words); 0 at the end */
static int
pvfind(rom, a, w, m, n)
	unsigned char *rom;
	unsigned long *a;
	unsigned short *w, *m;
	int n;
{
	int k;
	for (; *a + 2 * n <= ROMSIZE; *a += 2) {
		for (k = 0; k < n && (get16(rom + *a + 2 * k) & m[k]) == w[k]; k++)
			;
		if (k == n)
			return 1;
	}
	return 0;
}

static void
pvjsr(p, to, len)
	unsigned char *p;
	unsigned long to;
	int len;
{
	put16(p, 0x4eb9);
	put32(p + 2, to);
	for (len -= 6; len > 0; len -= 2, p += 2)
		put16(p + 6, 0x4e71);
}

static unsigned long pagesize;

/*
 * INTENA writes of d0 (exec's AddIntServer) and graphics' VERTB set and
 * clear through its custom base, to stubs on the interrupt words.
 */
static int
pvenable(rom)
	unsigned char *rom;
{
	/* move.w d0,INTENA; move.l 44(sp),a0; move.w #imm,$9a(a0) */
	static unsigned short d0ena[] = { 0x33c0, 0x00df, 0xf09a },
	    vb[] = { 0x206f, 0x002c, 0x317c, 0, 0x009a }, vbm[] = { 0xffff, 0xffff, 0xffff, 0, 0xffff },
	    full[] = { 0xffff, 0xffff, 0xffff };
	/* bit 15 sets, else clears; d0 kept */
	static unsigned long d0[] = {
		0x4a40,					/* tst.w d0 */
		0x6b0e,					/* bmi.s 1f */
		0x3f00,					/* move.w d0,-(sp) */
		0x4640,					/* not.w d0 */
		0xc179, PV(10),				/* and.w d0,pv+10 */
		0x301f,					/* move.w (sp)+,d0 */
		0x4e75,					/* rts */
		0x3f00,					/* 1: move.w d0,-(sp) */
		0x0240, 0x3fff,				/* and.w #$3fff,d0 */
		0x8179, PV(10),				/* or.w d0,pv+10 */
		0x301f,					/* move.w (sp)+,d0 */
		0x4ef9, PVHOST3				/* jmp host call */
	};
	unsigned long a, at[NVBENA], pg = PVSTUB & ~(pagesize - 1), w[4];
	int i, k = 0, n = 0;
	unsigned int v;

	for (a = 0; pvfind(rom, &a, vb, vbm, 5); a += 2)
		if (k < NVBENA)
			at[k++] = a + 4;
	if (mprotect((caddr_t)pg, BOOTBASE + BOOTSIZE - pg, PROT_READ | PROT_WRITE) < 0)
		return 0;
	pvcode((unsigned char *)PVD0ENA, d0, sizeof d0 / sizeof d0[0]);
	for (a = 0; pvfind(rom, &a, d0ena, full, 3); a += 6, n++)
		pvjsr(rom + a, PVD0ENA, 6);
	for (i = 0; i < k; i++, n++) {
		v = get16(rom + at[i] + 2);
		/* or.w #v,pv+10; jmp host call, or and.w #~v,pv+10; rts */
		w[0] = v & 0x8000 ? 0x0079 : 0x0279;
		w[1] = v & 0x8000 ? v & 0x3fff : ~v & 0xffff;
		w[2] = PV(10);
		w[3] = v & 0x8000 ? 0x4ef9 : 0x4e75;
		pvcode((unsigned char *)PVVBENA + 0x10 * i, w, 4);
		if (v & 0x8000)
			put32((unsigned char *)PVVBENA + 0x10 * i + 10, PVHOST3);
		pvjsr(rom + at[i], PVVBENA + 0x10 * i, 6);
	}
	if (mprotect((caddr_t)pg, BOOTBASE + BOOTSIZE - pg, PROT_READ | PROT_EXEC) < 0)
		fail("boot extension protection");
	return n;
}

/*
 * Exec's interrupt entry and acknowledges on the interrupt words in guest
 * memory, all or none; cia.resource's ICR reads on its pending bytes.
 */
static int
pvints(rom)
	unsigned char *rom;
{
	/* move.w $1c(a0),d1; btst #14,d1; beq.s; and.w $1e(a0),d1 */
	static unsigned short entry[] = { 0x3228, 0x001c, 0x0801, 0x000e, 0x6700, 0xc268, 0x001e },
	    entrym[] = { 0xffff, 0xffff, 0xffff, 0xffff, 0xff00, 0xffff, 0xffff },
	    l4[] = { 0x323c, 0x0780, 0xc268, 0x001c, 0xc268, 0x001e },
	    a1req[] = { 0x33e9, 0x000e, 0x00df, 0xf09c },
	    full[] = { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff },
	    /* move.w #imm,INTREQ or INTENA; move.w d0,INTREQ; tst.w INTREQR */
	    imm[] = { 0x33fc, 0, 0x00df, 0xf098 }, immm[] = { 0xffff, 0, 0xffff, 0xfff9 },
	    d0req[] = { 0x33c0, 0x00df, 0xf09c }, tst[] = { 0x4a79, 0x00df, 0xf01e },
	    icra[] = { 0x1439, 0x00bf, 0xed01 }, icrb[] = { 0x1439, 0x00bf, 0xdd00 },
	    /* SetICR: move.l 34(a6),a0; move.b $d00(a0),d1 */
	    seticr[] = { 0x206e, 0x0022, 0x1228, 0x0d00 },
	    /* AbleICR: move.l 34(a6),a0; move.b d0,$d00(a0) */
	    ableicr[] = { 0x206e, 0x0022, 0x1140, 0x0d00 },
	    enar[] = { 0, 0x00df, 0xf01c }, enarop[] = { 0x3039, 0x3239, 0x3f39, 0x3d79, 0x4a79 };
	unsigned long a, at[4], v, pv;
	int n = 0, ne = 0, nl = 0, na = 0, k;

	for (a = 0; pvfind(rom, &a, entry, entrym, 7); a += 2)
		ne++;
	for (a = 0; pvfind(rom, &a, l4, full, 6); a += 2)
		nl++;
	for (a = 0; pvfind(rom, &a, a1req, full, 4); a += 2)
		na++;
	if (ne != 6 || nl != 1 || na != 1) {
		miglog(0, "Kickstart copy: interrupt entry found %d, %d, %d times, left alone", ne, nl, na);
		return 0;
	}
	for (a = 0; pvfind(rom, &a, entry, entrym, 7); a += 14, n++) {
		put16(rom + a, 0x3239); put32(rom + a + 2, PV(10));
		put16(rom + a + 6, 0xc279); put32(rom + a + 8, PV(8));
		put16(rom + a + 12, 0x4e71);
	}
	for (a = 0; pvfind(rom, &a, l4, full, 6); a += 12, n++)
		pvjsr(rom + a, PVLEVEL4, 12);
	for (a = 0; pvfind(rom, &a, a1req, full, 4); a += 8, n++)
		pvjsr(rom + a, PVA1REQ, 8);
	for (a = 0; pvfind(rom, &a, d0req, full, 3); a += 6, n++)
		pvjsr(rom + a, PVD0REQ, 6);
	n += pvenable(rom);
	for (a = 0; pvfind(rom, &a, tst, full, 3); a += 6, n++)
		put32(rom + a + 2, PV(8));
	/* INTENAR reads (move.w to d0, d1, -(sp), d16(a6); tst.w) read the INTENA word */
	for (k = 0; k < 5; k++) {
		enar[0] = enarop[k];
		for (a = 0; pvfind(rom, &a, enar, full, 3); a += 6, n++)
			put32(rom + a + 2, PV(10));
	}
	/* immediates: a clear in place, a set through its stub; the master bit is Enable's */
	for (a = 0; pvfind(rom, &a, imm, immm, 4); a += 2) {
		v = get16(rom + a + 2);
		pv = get16(rom + a + 6) == 0xf09c ? PV(8) : PV(10);
		if (get16(rom + a + 6) != 0xf09c && get16(rom + a + 6) != 0xf09a)
			continue;
		if (pv == PV(10) && (v & 0x4000))
			continue;
		if (v & 0x8000) {
			pvjsr(rom + a, pv == PV(8) ? PVSETREQ : PVSETENA, 6);
			put16(rom + a + 6, v);
		} else {
			put16(rom + a, 0x0279);
			put16(rom + a + 2, ~v & 0xffff);
			put32(rom + a + 4, pv);
		}
		a += 6;
		n++;
	}
	pvflags = AMIGAF_PVINT;
	for (k = 0, a = 0; pvfind(rom, &a, icra, full, 3); a += 2)
		at[0] = a, k++;
	if (k == 1) {
		for (k = 0, a = 0; pvfind(rom, &a, icrb, full, 3); a += 2)
			at[1] = a, k++;
	} else
		k = 0;
	if (k == 1) {
		for (k = 0, a = 0; pvfind(rom, &a, seticr, full, 4); a += 2)
			at[2] = a, k++;
	}
	if (k == 1) {
		for (k = 0, a = 0; pvfind(rom, &a, ableicr, full, 4); a += 2)
			at[3] = a, k++;
	}
	if (k == 1) {
		pvjsr(rom + at[0], PVICRA, 6);
		pvjsr(rom + at[1], PVICRB, 6);
		pvjsr(rom + at[2], PVSETICR, 8);
		pvjsr(rom + at[3], PVABLEICR, 8);
		pvflags |= AMIGAF_PVICR;
		n += 4;
	} else
		miglog(0, "Kickstart copy: cia.resource ICR accesses not found, left alone");
	return n;
}

/*
 * timer.device's interval timer: its stops and its start through the
 * stubs, so the host runs it; both forms or neither.
 */
static int
pvtimer(rom)
	unsigned char *rom;
{
	/* move.l 372(a6),a0; and.b #$ce,(a0); or.b #8,(a0) */
	static unsigned short stop[] = { 0x206e, 0x0174, 0x0210, 0x00ce, 0x0010, 0x0008 },
	    /* move.l 376(a6),a0; move.b d0,(a0); lsr.w #8,d0; move.b d0,$100(a0) */
	    start[] = { 0x206e, 0x0178, 0x1080, 0xe048, 0x1140, 0x0100 },
	    full[] = { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff };
	unsigned long a;
	int ns = 0, nt = 0;

	for (a = 0; pvfind(rom, &a, stop, full, 6); a += 2)
		ns++;
	for (a = 0; pvfind(rom, &a, start, full, 6); a += 2)
		nt++;
	if (ns != 2 || nt != 1) {
		miglog(0, "Kickstart copy: interval timer found %d, %d times, left alone", ns, nt);
		return 0;
	}
	for (a = 0; pvfind(rom, &a, stop, full, 6); a += 12)
		pvjsr(rom + a + 4, PVTSTOP, 8);
	for (a = 0; pvfind(rom, &a, start, full, 6); a += 12)
		pvjsr(rom + a, PVTSTART, 12);
	pvflags |= AMIGAF_PVTIMER;
	return 3;
}

/*
 * graphics.library's drawing cores: their first 10 bytes jump to a stub
 * that calls the boot extension's CPU version, and runs them and the rest
 * when it declines.  The area fill sits inside AreaEnd: when drawn it goes
 * on past the blit.
 */
static unsigned long pagesize;

#define NGFX 7

static int
pvgfx(rom)
	unsigned char *rom;
{
	/* movem.l d2-d7/a2-a3,-(sp); movem.w 36(a1),d2-d3; movem.w d0-d1,36(a1); move.l 4(a1),a2 */
	static unsigned short line[] = { 0x48e7, 0x3f30, 0x4ca9, 0x000c, 0x0024, 0x48a9, 0x0003, 0x0024,
	    0x2469, 0x0004 },
	    /* movem.l d2-d7/a2-a6,-(sp); move.l 16(a0),a6; move.l a1,a3; addq.w #1,170(a6) */
	    rect[] = { 0x48e7, 0x3f3e, 0x2c68, 0x0010, 0x2649, 0x526e, 0x00aa },
	    /* BltClear: move.l d6,-(sp); lea $dff000,a0; clr.w d6; addq.w #1,170(a6); beq.s */
	    clear[] = { 0x2f06, 0x41f9, 0x00df, 0xf000, 0x4246, 0x526e, 0x00aa, 0x6704 },
	    /* BltPattern per rectangle: sub.w #76,sp; movem.l; move.l d0,d7; move.l 124(sp),a3; ... */
	    pattern[] = { 0x9efc, 0x004c, 0x48e7, 0x3f3c, 0x2e00, 0x266f, 0x007c, 0x2849, 0x2a48, 0x302b,
	    0x0006 },
	    /* AreaEnd's fill: lea $dff000,a0; add.l d4,a1; subq.l #2,a1; moveq #-1,d0; d5 = -2 * d4 */
	    fill[] = { 0x41f9, 0x00df, 0xf000, 0xd3c4, 0x5589, 0x70ff, 0x3a04, 0xda45, 0x4445, 0x526e,
	    0x00aa },
	    /* BltBitMap's core: movem.l d2-a5,-(sp); sub.w #18,sp; move.l sp,a5; moveq #0,d6; depth */
	    bbm[] = { 0x48e7, 0x3ffc, 0x9efc, 0x0012, 0x2a4f, 0x7c00, 0x1c28, 0x0005 },
	    /* BltTemplate per rectangle: move.l a4,-(sp); tst.b 5(a4); beq.s; movem.l a0-a1,-(sp); ... */
	    tmpl[] = { 0x2f0c, 0x4a2c, 0x0005, 0x6700, 0x48e7, 0x00c0, 0x3654, 0x226c, 0x0008 },
	    tmplm[] = { 0xffff, 0xffff, 0xffff, 0xff00, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff },
	    /* the fill's DisownBlitter: subq.w #1,170(a6); blt.s; jsr */
	    disown[] = { 0x536e, 0x00aa, 0x6d04, 0x4eba },
	    /* BltTemplate's end: DisownBlitter; move.l (sp)+,a4; rts */
	    tmplend[] = { 0x4eae, 0xfe32, 0x285f, 0x4e75 },
	    full[] = { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
	    0xffff };
	static unsigned short *form[] = { line, rect, clear, pattern, fill, bbm, tmpl },
	    *mask[] = { full, full, full, full, full, full, tmplm },
	    /* where the stub goes on when drawn, after the copied code (0: rts) */
	    *end[] = { 0, 0, 0, 0, disown, 0, tmplend };
	/* the patch's offset in the form and the end's past its form */
	static int len[] = { 10, 7, 8, 11, 11, 8, 9 }, skip[] = { 0, 0, 0, 0, 0, 0, 8 },
	    past[] = { 0, 0, 0, 0, 10, 0, 4 };
	static char *what[] = { "line", "rectangle", "BltClear", "BltPattern", "area fill", "BltBitMap",
	    "BltTemplate" };
	unsigned char *s = (unsigned char *)PVGFX;
	unsigned long a, at[NGFX], done[NGFX], pg = PVSTUB & ~(pagesize - 1);
	int i, k, n = 0;
	for (i = 0; i < NGFX; i++) {
		for (k = 0, a = 0; pvfind(rom, &a, form[i], mask[i], len[i]); a += 2)
			at[i] = a + skip[i], k++;
		done[i] = 0;
		if (k == 1 && end[i]) {
			a = at[i];
			if (pvfind(rom, &a, end[i], full, 4) && a - at[i] < 0x100)
				done[i] = ROMBASE + a + past[i];
			else
				k = 0;
		}
		if (k != 1) {
			miglog(0, "Kickstart copy: graphics %s found %d times, left alone", what[i], k);
			at[i] = 0;
		}
	}
	for (i = 0; i < NGFX && !at[i]; i++)
		;
	if (i == NGFX)
		return 0;
	if (mprotect((caddr_t)pg, BOOTBASE + BOOTSIZE - pg, PROT_READ | PROT_WRITE) < 0)
		return 0;
	for (i = 0; i < NGFX; i++, s += 0x20) {
		if (!at[i])
			continue;
		/* jsr entry; beq.s 1f; rts or jmp past; 1: the first 10 bytes; jmp the rest */
		put16(s, 0x4eb9); put32(s + 2, MIG_PVVEC + 6 * i);
		put16(s + 6, 0x6706);
		if (done[i]) {
			put16(s + 8, 0x4ef9); put32(s + 10, done[i]);
		} else
			put16(s + 8, 0x4e75);
		memcpy(s + 14, rom + at[i], 10);
		put16(s + 24, 0x4ef9); put32(s + 26, ROMBASE + at[i] + 10);
		pvjsr(rom + at[i], PVGFX + 0x20 * i, 10);
		put16(rom + at[i], 0x4ef9);
		n++;
	}
	if (mprotect((caddr_t)pg, BOOTBASE + BOOTSIZE - pg, PROT_READ | PROT_EXEC) < 0)
		fail("boot extension protection");
	return n;
}

/*
 * The blitter is idle whenever the CPU looks, graphics drawing on the
 * CPU: WaitBlit returns at once, and its inline form (tst.b and btst of
 * DMACONR through an address register, beq.s, bsr.w WaitBlit) branches past.
 */
static int
pvwaits(rom)
	unsigned char *rom;
{
	/* tst.b $dff002; btst #6,$dff002; bne.s; rts */
	static unsigned short wb[] = { 0x4a39, 0x00df, 0xf002, 0x0839, 0x0006, 0x00df, 0xf002, 0x6602, 0x4e75 },
	    full[] = { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff };
	unsigned long a = 0, w = 0;
	int k, n = 0, r;
	for (k = 0; pvfind(rom, &a, wb, full, 9); a += 2)
		w = a, k++;
	if (k != 1) {
		miglog(0, "Kickstart copy: WaitBlit found %d times, left alone", k);
		return 0;
	}
	for (a = 0; a + 16 <= ROMSIZE; a += 2) {
		r = get16(rom + a) - 0x4a28;
		if (r < 0 || r > 6 || get16(rom + a + 2) != 2 || get16(rom + a + 4) != 0x0828 + r ||
		    get16(rom + a + 6) != 6 || get16(rom + a + 8) != 2 || get16(rom + a + 10) != 0x6704 ||
		    get16(rom + a + 12) != 0x6100 || a + 14 + (short)get16(rom + a + 14) != w)
			continue;
		put16(rom + a, 0x600e);
		n++;
	}
	put16(rom + w, 0x4e75);
	return n + 1;
}

/*
 * The beam stands still at line 0.  Graphics' waits on it fall through:
 * for a horizontal position (moveq #24,d2; add.b d5,d2; 1: move.w (a4),d1;
 * cmp.b d2,d1; bls.s 1b) and, at its start, for a line from 20 to 160
 * (1: bsr.w vpos; moveq #80,d1; add.l d1,d1; cmp.l d1,d0; bhi.s 1b;
 * bsr.w vpos; moveq #20,d1; cmp.l d1,d0; bcs.s 1b).  So does its sync
 * test's wait for CIA-A timer A (1: tst.b TODLO; beq.s 2f; bset #3,207(a6);
 * 2: move.b TAHI,d0; bmi.s 1b), and every wait for the next line
 * (jsr VBeamPos(a6); move.l d0,d5; 1: jsr VBeamPos(a6); cmp.l d5,d0; beq.s 1b).
 */
static int
pvbeam(rom)
	unsigned char *rom;
{
	static unsigned short h[] = { 0x7418, 0xd405, 0x3214, 0xb202, 0x63fa },
	    full[] = { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
		0xffff, 0xffff, 0xffff, 0xffff, 0xffff },
	    v[] = { 0x6100, 0, 0x7250, 0xd281, 0xb081, 0x62f4, 0x6100, 0, 0x7214, 0xb081, 0x65ea },
	    vm[] = { 0xffff, 0, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0xffff, 0xffff, 0xffff },
	    t[] = { 0x4a39, 0x00bf, 0xe801, 0x6706, 0x08ee, 0x0003, 0x00cf, 0x1039, 0x00bf, 0xe501, 0x6bea },
	    l[] = { 0x4eae, 0xfe80, 0x2a00, 0x4eae, 0xfe80, 0xb085, 0x67f8 };
	static struct { unsigned short *w, *m; int n, b[2]; char *what; } f[] = {
		{ h, full, 5, { 4, 4 }, "horizontal" },
		{ v, vm, 11, { 5, 10 }, "vertical" },
		{ t, full, 11, { 10, 10 }, "timer" },
		{ l, full, 7, { 6, 6 }, "next line" }
	};
	unsigned long a, at = 0;
	int i, k, n = 0;

	for (i = 0; i < 4; i++) {
		for (k = 0, a = 0; pvfind(rom, &a, f[i].w, f[i].m, f[i].n); a += 2)
			at = a, k++;
		/* the next-line wait has several sites */
		if (i == 3 && k > 0) {
			for (a = 0; pvfind(rom, &a, l, full, 7); a += 14, n++)
				put16(rom + a + 12, 0x4e71);
			continue;
		}
		if (k != 1) {
			miglog(0, "Kickstart copy: %s wait found %d times, left alone", f[i].what, k);
			continue;
		}
		put16(rom + at + 2 * f[i].b[0], 0x4e71);
		put16(rom + at + 2 * f[i].b[1], 0x4e71);
		n++;
	}
	return n;
}

/* pvpatch's forms for code loaded from disk, as hostcall.h lays them out */
static unsigned long pvseg[] = {
	4, 0x33fc, 0x4000, 0x00df, 0xf09a, 0x33fc, 0x4000, AMIGA_PV_BASE >> 16, AMIGA_PV_BASE & 0xffff,
	4, 0x33fc, 0xc000, 0x00df, 0xf09a, 0x4eb9, PVENABLE >> 16, PVENABLE & 0xffff, 0x4e71,
	3, 0x33c0, 0x00df, 0xf09c, 0x4eb9, PVD0REQ >> 16, PVD0REQ & 0xffff,
	3, 0x33c0, 0x00df, 0xf09a, 0x4eb9, PVD0ENA >> 16, PVD0ENA & 0xffff,
	3, 0x4a79, 0x00df, 0xf01e, 0x4a79, PV(8) >> 16, PV(8) & 0xffff,
	/*
	 * A VBL server driving the native display: its long-frame bit,
	 * vertical blank start, copper list, total and the line count in
	 * CIA-B's TOD, which nothing reads, skipped.
	 */
	14, 0x3028, 0x0004, 0x6b0c, 0x3028, 0x0004, 0x807c, 0x8000, 0x3140, 0x002a,
	0x2142, 0x01cc, 0x2169, 0x0032, 0x0084,
	0x601a, 0x0004, 0x6b0c, 0x3028, 0x0004, 0x807c, 0x8000, 0x3140, 0x002a,
	0x2142, 0x01cc, 0x2169, 0x0032, 0x0084,
	6, 0x2c42, 0x317c, 0x020c, 0x01c8, 0x382e, 0x0028,
	0x2c42, 0x4e71, 0x4e71, 0x4e71, 0x382e, 0x0028,
	17, 0x4df9, 0x00bf, 0xd000, 0x022e, 0x007f, 0x0f00, 0x7000, 0x1d40, 0x0a00,
	0x1d40, 0x0900, 0x1d68, 0x0006, 0x0800, 0x002e, 0x0080, 0x0f00,
	0x4df9, 0x00bf, 0xd000, 0x601a, 0x007f, 0x0f00, 0x7000, 0x1d40, 0x0a00,
	0x1d40, 0x0900, 0x1d68, 0x0006, 0x0800, 0x002e, 0x0080, 0x0f00,
	0
};

#define PVSEGINT	(2 * 9)	/* the forms before need only AMIGAF_PV */
#define PVSEGVBL	(PVSEGINT + 3 * 7)

/* graphics' own native-display VBL forms, as in code loaded from disk */
static int
pvforms(rom)
	unsigned char *rom;
{
	unsigned long a;
	int i, k, w, n = 0;

	for (i = PVSEGVBL; (w = pvseg[i]) != 0; i += 1 + 2 * w)
		for (a = 0; a + 2 * w <= ROMSIZE; a += 2) {
			for (k = 0; k < w && get16(rom + a + 2 * k) == pvseg[i + 1 + k]; k++)
				;
			if (k < w)
				continue;
			for (k = 0; k < w; k++)
				put16(rom + a + 2 * k, pvseg[i + 1 + w + k]);
			n++;
		}
	return n;
}

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
	unsigned long i, sum, prev, a, last, at[NSITE][2];
	int n = 0, j, k, found[NSITE];
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
	for (j = 0, last = 0; j < NSITE; j++) {
		found[j] = 0;
		for (a = sites[j].near ? last : 0; a + 12 <= ROMSIZE; a += 2) {
			if (sites[j].near && a > last + sites[j].near)
				break;
			for (k = 0; k < sites[j].m && get16(rom + a + 2 * k) == sites[j].was[k]; k++)
				;
			if (k < sites[j].m)
				continue;
			if (found[j] < 2)
				at[j][found[j]] = a;
			found[j]++;
			if (sites[j].near)
				break;
		}
		last = found[j] ? at[j][0] : ROMSIZE;
		if (found[j] != sites[j].want)
			miglog(0, "Kickstart copy: site %d found %d times, left alone", j, found[j]);
	}
	n += pvints(rom);
	n += pvforms(rom);
	n += pvgfx(rom);
	n += pvwaits(rom);
	n += pvbeam(rom);
	/* the host's E-clock and interval timer need the ICR bits in memory */
	if (found[0] == sites[0].want && found[TODSITE] == sites[TODSITE].want &&
	    (pvflags & AMIGAF_PVICR))
		n += pvtimer(rom);
	/* fsave and frestore change the frame together */
	if ((found[1] == sites[1].want) != (found[2] == sites[2].want)) {
		miglog(0, "Kickstart copy: fsave and frestore not both found, both left alone");
		found[1] = found[2] = 0;
	}
	for (j = 0; j < NSITE; j++)
		for (i = 0; found[j] == sites[j].want && i < found[j]; i++, n++)
			for (k = 0; k < sites[j].n; k++)
				put16(rom + at[j][i] + 2 * k, sites[j].now[k]);
	/* the A3000's SCSI driver would find no chip: it is not started */
	if ((kickflags & AMIGAF_SCSI) && (a = romtag(rom, "scsi.device", "scsidisk")) != 0) {
		rom[a - 1 + 10] &= ~1;
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
pvsegs()
{
	unsigned char *p = (unsigned char *)MIG_PVSEG;
	unsigned long at = MIG_PVSEG & ~(pagesize - 1), len = BOOTBASE + BOOTSIZE - at;
	int i, n = sizeof pvseg / sizeof pvseg[0];
	if (!(pvflags & AMIGAF_PVINT))
		n = PVSEGINT;
	if (mprotect((caddr_t)at, len, PROT_READ | PROT_WRITE) < 0)
		return;
	for (i = 0; i < n; i++)
		put16(p + 4 + 2 * i, pvseg[i]);
	put16(p + 4 + 2 * n, 0);
	put32(p, MIG_PVSEG_MAGIC);
	if (mprotect((caddr_t)at, len, PROT_READ | PROT_EXEC) < 0)
		fail("boot extension protection");
}

static void
region(address, size, shared)
	unsigned long address, size;
	int shared;
{
	char what[48];
	sprintf(what, "guest memory %#lx+%#lx", address, size);
	if (envmem_region(address, size, shared, -1) < 0)
		fail(what);
}

/* a fault in host code: where, for the log; then the default action */
static void
hostfault(sig, si, uc)
	int sig;
	siginfo_t *si;
	char *uc;
{
	/* the ucontext's registers start 36 bytes in: d0-d7, a0-a7, pc */
	unsigned long *g = (unsigned long *)(uc + 36), *sp = (unsigned long *)g[15];
	struct mig_early_status *es = (struct mig_early_status *)MIG_EARLY_STATUS;
	unsigned char *pc = (unsigned char *)g[16];
	char code[3 * 16 + 1];
	int i;

	miglog(1, "host fault: signal %d at pc %#lx, address %#lx, sp %#lx", sig,
	    g[16], si ? (unsigned long)si->si_addr : 0UL, g[15]);
	miglog(1, "d %lx %lx %lx %lx %lx %lx %lx %lx", g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
	miglog(1, "a %lx %lx %lx %lx %lx %lx %lx", g[8], g[9], g[10], g[11], g[12], g[13], g[14]);
	miglog(1, "early bind: status %u step %u", es->status, es->step);
	/* the code around pc, to find whose it is; pc itself may be unreadable */
	if (sig != SIGSEGV || !si || (unsigned long)si->si_addr - g[16] + 8 > 72) {
		for (i = 0; i < 16; i++)
			sprintf(code + 3 * i, " %02x", pc[i - 8]);
		miglog(1, "code at pc-8:%s", code);
	}
	signal(sig, SIG_DFL);
	if (((unsigned long)sp >= 0x400 && (unsigned long)sp <= CHIPSIZE - 32) ||
	    ((unsigned long)sp >= FASTBASE && (unsigned long)sp <= FASTBASE + fastlen - 32))
		miglog(1, "stack %lx %lx %lx %lx %lx %lx %lx %lx",
		    sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7]);
	kill(getpid(), sig);
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
    caddr_t t, m;
    unsigned short was;
    unsigned int want = r->ram != 0;
    char c = 1;
    (void)sig;
    /* the kernel holds Amiga interrupts while this runs on its own stack */
    if (pvmode) {
        was = *pv;
        *pv = 0x4000;
    }
    /* the display seen through a second mapping meanwhile: no second copy in RAM */
    if (want != r->inram &&
        (t = mmap((caddr_t)0, winsize, PROT_READ | PROT_WRITE, MAP_SHARED, movefd, 0)) != (caddr_t)-1) {
        if (want) {
            m = mmap((caddr_t)MIG_RTG_VRAM, winsize, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_FIXED, zfd, 0);
            if (m != (caddr_t)-1)
                memcpy((char *)MIG_RTG_VRAM, t, winsize);
        } else {
            memcpy(t, (char *)MIG_RTG_VRAM, winsize);
            m = mmap((caddr_t)MIG_RTG_VRAM, winsize, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED, movefd, 0);
        }
        if (m != (caddr_t)-1)
            r->inram = want;
        else if (want)
            /* no memory for the copy: a failed fixed mapping leaves a hole */
            mmap((caddr_t)MIG_RTG_VRAM, winsize, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FIXED, movefd, 0);
        munmap(t, winsize);
    }
    write(ackfd, &c, 1);
    /* last: an interrupt held meanwhile reaches the guest, not this handler */
    if (pvmode)
        *pv = was;
}

static void
displaygone(sig)
    int sig;
{
    static char message[] = "startamiga: helper process exited\n";
    (void)sig;
    write(2, message, sizeof message - 1);
    _exit(1);
}

/* the guest's part of the session, page-rounded; the card's RAM follows */
#define VRAMEND(fi) (MIG_RTG_VRAM + migdisp_gsize)
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

/* a mode's size and timing into its MIHD chunk */
static void
mihd(c, w, h)
	unsigned char *c;
	unsigned long w, h;
{
	unsigned long ht = (w * 5 / 4 + 7) & ~7UL, vt = h + 45, best = 0, k, d, dbest = ~0UL;
	for (k = 0; k < 4; k++) {
		d = clocks[k] > ht * vt * 60 ? clocks[k] - ht * vt * 60 : ht * vt * 60 - clocks[k];
		if (d < dbest) dbest = d, best = k;
	}
	put16(c + 4, w); put16(c + 6, h);
	put16(c + 10, ht); put16(c + 20, vt);
	c[28] = best; put32(c + 30, clocks[best]);
}

/* the native resolution and its modes at w x h; 1 if found */
static int
p96native(b, n, w, h)
	unsigned char *b;
	unsigned long n, w, h;
{
	unsigned long i;
	unsigned char *c;
	int in = 0, found = 0;
	long len;
	if (n < 12 || memcmp(b, "FORM", 4) || memcmp(b + 8, "P96S", 4)) return 0;
	for (i = 12; (len = chunk(b, i, n)) >= 0; i += 8 + ((len + 1) & ~1L)) {
		c = b + i + 8;
		if (!memcmp(b + i, "RSHD", 4)) {
			in = len >= 8 && get32(c) == NATIVE_ID;
			if (in) {
				put16(c + 4, w); put16(c + 6, h);
				found = 1;
			}
		} else if (in && !memcmp(b + i, "MIHD", 4) && len >= 34)
			mihd(c, w, h);
	}
	return found;
}

/*
 * The modes for BestModeID, ID, width and height: the display's, then
 * smaller ones.  Sizes of smallsz the settings lack are added to the copy
 * the guest binds before DOS, each a copy of the display's resolution.
 */
unsigned long amode[6][3];
int amodes;
static int amodes0;
static unsigned long smallsz[][2] = { { 640, 480 }, { 800, 600 } };
static unsigned char p96ext[8192];

/* b, n bytes with the native resolution at w x h, into out; its length */
static unsigned long
p96small(b, n, out, w, h)
	unsigned char *b, *out;
	unsigned long n, w, h;
{
	unsigned long i, j, k, id, rw, rh, nat = 0, natlen = 0, used = 0, o = n;
	unsigned char *c;
	long len;
	amodes = 1;
	amode[0][0] = NATIVE_ID; amode[0][1] = w; amode[0][2] = h;
	for (i = 12; (len = chunk(b, i, n)) >= 0; i += 8 + ((len + 1) & ~1L)) {
		if (memcmp(b + i, "RSHD", 4) || len < 8)
			continue;
		c = b + i + 8;
		id = get32(c); rw = get16(c + 4); rh = get16(c + 6);
		if ((id & 0xfff0ffffUL) == (NATIVE_ID & 0xfff0ffffUL))
			used |= 1UL << (id >> 16 & 15);
		if (id == NATIVE_ID) {
			nat = i;
			for (j = i; (len = chunk(b, j, n)) >= 0 && (j == i || !memcmp(b + j, "MIHD", 4));
			    j += 8 + ((len + 1) & ~1L))
				;
			natlen = j - i;
		} else if (rw <= w && rh <= h && rw * rh < w * h && amodes < 6) {
			amode[amodes][0] = id; amode[amodes][1] = rw; amode[amodes++][2] = rh;
		}
	}
	amodes0 = amodes;
	memcpy(out, b, n);
	for (k = 0; k < sizeof smallsz / sizeof smallsz[0] && nat; k++) {
		rw = smallsz[k][0]; rh = smallsz[k][1];
		for (i = 0; i < (unsigned long)amodes && (amode[i][1] != rw || amode[i][2] != rh); i++)
			;
		if (i < (unsigned long)amodes || rw > w || rh > h || rw * rh >= w * h || amodes == 6 ||
		    o + natlen > sizeof p96ext)
			continue;
		for (j = 0; j < 16 && used & 1UL << j; j++)
			;
		if (j == 16)
			break;
		used |= 1UL << j;
		id = (NATIVE_ID & 0xfff0ffffUL) | j << 16;
		memcpy(out + o, b + nat, natlen);
		c = out + o + 8;
		put32(c, id); put16(c + 4, rw); put16(c + 6, rh);
		if (get32(out + o + 4) >= 36) {
			memset(c + 14, 0, 22);
			sprintf((char *)c + 14, "Container:%lux%lu", rw, rh);
		}
		for (i = o + 8 + ((get32(out + o + 4) + 1) & ~1UL); i < o + natlen;
		    i += 8 + ((get32(out + i + 4) + 1) & ~1UL))
			if (get32(out + i + 4) >= 34)
				mihd(out + i + 8, rw, rh);
		o += natlen;
		amode[amodes][0] = id; amode[amodes][1] = rw; amode[amodes++][2] = rh;
	}
	put32(out + 4, o - 8);
	return o;
}

/* DEPTH=n in /etc/default/display: the Workbench's depth; 0 if unset */
static unsigned long
cfgdepth()
{
	FILE *f = fopen("/etc/default/display", "r");
	char l[80];
	unsigned long d = 0;
	if (!f) return 0;
	while (fgets(l, sizeof l, f))
		if (!strncmp(l, "DEPTH=", 6))
			d = strtoul(l + 6, 0, 10);
	fclose(f);
	return d;
}

/*
 * What P96 adds to a resolution's mode ID for depth d on this display: the
 * depth's class in bits 8-11, the format's place in the class below
 * (R5G6B5 0x102, A8R8G8B8 0x300, B8G8R8A8 0x303).  0 for 8 bits or a depth
 * the display lacks.
 */
unsigned long
p96depth(d)
	unsigned long d;
{
	if (d > 16)
		return migdisp_formats & 1U << MIG_RTG_A8R8G8B8 ? 0x300 :
		    migdisp_formats & 1U << MIG_RTG_B8G8R8A8 ? 0x303 : 0;
	return d > 8 && migdisp_formats & 1U << MIG_RTG_R5G6B5 ? 0x102 : 0;
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
	unsigned long n, m, i, id, d, k;
	long len;
	int p96 = 0;
	*w = 640; *h = 480;
	if ((n = sysread(root, p96path, p96set[0], sizeof p96set[0])) != 0) {
		memcpy(p96set[1], p96set[0], n);
		if ((p96 = p96native(p96set[1], n, fi->fi_width, fi->fi_height)) != 0)
			mig_hostfs_overlay(p96path, p96set[0], p96set[1], n);
		p96data = p96set[p96 != 0];
		p96len = n;
		if (p96) {
			p96len = p96small(p96set[1], n, p96ext, fi->fi_width, fi->fi_height);
			p96data = p96ext;
		}
	}
	m = sysread(root, scrmpath, scrm[0], sizeof scrm[0]);
	memcpy(scrm[1], scrm[0], m);
	for (i = 12; (len = chunk(scrm[1], i, m)) >= 0; i += 8 + ((len + 1) & ~1L))
		if (!memcmp(scrm[1] + i, "SCRM", 4) && len >= 28) {
			id = get32(scrm[1] + i + 24);
			if (p96 && (id & ~0xfffUL) == NATIVE_ID) {
				d = get16(scrm[1] + i + 32);
				if ((k = cfgdepth()) != 0)
					d = k;
				/* a depth the display lacks: 8 bits */
				if ((k = p96depth(d)) == 0)
					d = 8;
				put32(scrm[1] + i + 24, NATIVE_ID + k);
				put16(scrm[1] + i + 28, fi->fi_width);
				put16(scrm[1] + i + 30, fi->fi_height);
				put16(scrm[1] + i + 32, d);
				mig_hostfs_overlay(scrmpath, scrm[0], scrm[1], m);
			}
			smode = scrm[1] + i + 24;
			*w = get16(scrm[1] + i + 28);
			*h = get16(scrm[1] + i + 30);
			return p96 && (id & ~0xfffUL) == NATIVE_ID;
		}
	return 0;
}

/*
 * The board's files from SYS: into the boot extension, so the guest binds
 * it before DOS.  An rtg.library whose settings table the guest cannot find
 * binds after DOS as before.
 */
static char infopath[] = "DEVS/Monitors/Container.info";

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
	int all = amodes;
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
	amodes = amodes0;
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
	amodes = all;
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
	int direct, i;
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
	r->formats = migdisp_formats;
	/* smaller modes, shown from the top left, at depths with their own VRAM */
	for (i = 1; direct && (fi->fi_flags & FBF_OWNV) && i < amodes && i <= 4; i++) {
		r->small[i - 1][0] = amode[i][1];
		r->small[i - 1][1] = amode[i][2];
	}
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
	for (i = 0; i < migdisp_gsize; i += pagesize)
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

/* the floppy helper: DF0-DF3 on host drives or ADF files */
static void
startfloppy(dev, spec)
    int dev;
    char **spec;
{
    int ready[2], life[2], n;
    pid_t pid;
    char success;
    struct pollfd p;
    if (pipe(ready) < 0 || pipe(life) < 0)
        fail("floppy pipe");
    pid = fork();
    if (pid < 0) fail("floppy fork");
    if (pid == 0) {
        close(ready[0]); close(life[1]); close(gofd);
        if (ackfd >= 0) close(ackfd);
        if (displaylife >= 0) close(displaylife);
        _exit(mig_fd_helper(ready[1], life[0], dev, spec));
    }
    close(ready[1]); close(life[0]);
    p.fd = ready[0]; p.events = POLLIN;
    do { n = poll(&p, 1, 5000); } while (n < 0 && errno == EINTR);
    if (n <= 0 || read(ready[0], &success, 1) != 1 || success != 1) {
        miglog(1, "floppy helper did not become ready");
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
        sprintf(path, "%s/.startamiga.log", root);
        unlink(path);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    }
    if (fd < 0) {
        sprintf(path, "/tmp/startamiga.%ld.log", (long)getuid());
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
	char sysroot[1024], *home, *env = 0, *def = 0, *base = 0, *b;
	static char *envs[] = { "3.2", 0 };
	struct stat sb;
	struct passwd *pw;
	int list = 0, refresh = 0, readonly = 0, rootreadonly = 0, census = 0, kick = 1, romarg = 0;
	unsigned long fastmb = 64;
	char envfile[1100];
	int fastarg = 0, fit = 0, pv = 1, n, dfd, direct, go[2], ack[2];
	stack_t ss;
	static char altstack[16384];
	unsigned long w, h;
	struct fbinfo fi;
	struct amigaenter ae;
	struct amigainfo info;
	struct amigahostpc hostpc;
	struct sigaction sa;
	struct rlimit rl;
	int i, check = 0, probe = 0, fd, lockfd = -1;
	char *df[MIG_FD_UNITS] = { "drive", 0, 0, 0 };

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
		else if (!strcmp(argv[i], "-d") && i + 1 < argc)
			def = argv[++i];
		else if (!strcmp(argv[i], "-b") && i + 1 < argc)
			base = argv[++i];
		else if (!strcmp(argv[i], "-R"))
			refresh = 1;
		else if (!strcmp(argv[i], "-l"))
			list = 1;
		else if (argv[i][0] == '-' && argv[i][1] == 'd' && argv[i][2] == 'f' &&
		    argv[i][3] >= '0' && argv[i][3] <= '3' && !argv[i][4] && i + 1 < argc) {
			n = argv[i][3] - '0';
			df[n] = strcmp(argv[++i], "none") ? argv[i] : 0;
		} else if (!strcmp(argv[i], "-boot") && i + 1 < argc)
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
	home = getenv("HOME");
	if ((!home || !*home) && (pw = getpwuid(getuid())) != 0)
		home = pw->pw_dir;
	if (list) {
		envlist("amiga", home, "3.2", envs);
		return 0;
	}
	if (def && envsetdefault("startamiga", "amiga", home, def) < 0)
		return 1;
	if (def && !env && !base && !refresh)
		return 0;
	if (!probe && !check) {
		env = envchoose("amiga", env, home, "3.2");
		if ((b = envbase("startamiga", "amiga", env, base, home, "3.2", envs)) == 0 ||
		    envhome("amiga", env, "3.2", home, sysroot, sizeof sysroot) < 0)
			return 1;
		/* the user's copy, Startup-Sequence last so an interrupted one is
		 * redone; else for 3.2 the system's, read-only */
		sprintf(envfile, "%.1000s/%s", sysroot, startpath);
		if (refresh || stat(envfile, &sb) < 0) {
			if (home && stat(home, &sb) == 0 && access(home, W_OK) == 0 &&
			    envcopy("startamiga", "amiga", env, b, "/amiga/sys", sysroot,
			    "grep -v '^./S/Startup-Sequence$' |") == 0 &&
			    envcopy("startamiga", "amiga", (char *)0, b, "/amiga/sys", sysroot,
			    "grep '^./S/Startup-Sequence$' |") == 0)
				fprintf(stderr, "startamiga: %s is SYS: of %s\n", sysroot, env);
			else if (strcmp(env, "3.2") || refresh ||
			    mig_sysroot((char *)0, "/amiga/sys", sysroot, sizeof sysroot, &rootreadonly) < 0)
				fail("Amiga system directory; install /amiga/sys");
		}
		if (rootreadonly)
			fprintf(stderr, "startamiga: SYS: is /amiga/sys, read-only\n");
		/* fast RAM: -m, else the environment's, else the system's; read before
		 * the lock, which closing the metadata file would drop */
		sprintf(envfile, "%.1000s/.env", sysroot);
		fit = !fastarg && !setting(envfile, "fastmb", &fastmb) &&
		    !setting("/etc/default/amiga", "FAST_MB", &fastmb);
		readonly |= rootreadonly;
		if (!readonly && (lockfd = envlock("startamiga", "amiga", sysroot)) == -1)
			return 1;
		openlog(sysroot, !readonly);
		miglog(0, "SYS: is %.500s%s", sysroot, readonly ? ", read-only" : "");
		if (!fit)
			miglog(0, "fast RAM %lu MB", fastmb);
	}
	if (!romarg && access(rom, 0) < 0)
		rom = "/etc/amiga/kicka3000.rom";
	/* the file now if named; else after the machine's own Kickstart is tried */
	if (romarg || check) {
		if (!readrom(rom))
			return 1;
		miglog(0, "Kickstart %.500s verified", rom);
	}
	if (check) {
		printf("%s: checksum verified, flags %#lx\n", kickname, kickflags);
		return 0;
	}
	if (!probe && !checksystem(sysroot)) {
		miglog(1, "%.500s has no readable S/Startup-Sequence", sysroot);
		return 1;
	}
	for (i = 3; i < 256; i++)
		if (i != lockfd && i != miglog_fd)
			close(i);
	amiga_device = "/dev/amigaguest";
	fd = open(amiga_device, O_RDWR);
	if (fd < 0 && errno == ENOENT) {
		amiga_device = "/dev/amiga";
		fd = open(amiga_device, O_RDWR);
	}
	if (fd < 0 && (errno == ENXIO || errno == ENODEV)) {
		miglog(1, "%s: Amiga module not loaded; as root run /usr/sbin/amigareg /usr/aux/lib/mod.d", amiga_device);
		return 1;
	}
	if (fd < 0 && errno == EACCES) {
		miglog(1, "%s: permission denied; the display group may use it", amiga_device);
		return 1;
	}
	if (fd < 0)
		fail(amiga_device);
	if (ioctl(fd, AMIGAIOC_INFO, &info) < 0)
		fail("AMIGAIOC_INFO");
	if (info.ai_version != AMIGA_ABI_VERSION) {
		miglog(1, "Amiga module ABI %lu, expected %d", info.ai_version, AMIGA_ABI_VERSION);
		return 1;
	}
	if (!probe && !(info.ai_features & (AMIGA_FEAT_BOOT | AMIGA_FEAT_EXPERIMENTAL))) {
		miglog(1, "Amiga environment needs a 68040 or 68060");
		return 1;
	}
	if (!romarg) {
		if (ioctl(fd, AMIGAIOC_MAPROM, 0) > 0 && romok((unsigned char *)ROMBASE, "the machine's Kickstart")) {
			/* patched as a copy */
			rombuffer();
			memcpy((char *)rombuf, (char *)ROMBASE, ROMSIZE);
			munmap((caddr_t)ROMBASE, ROMSIZE);
			miglog(0, "Kickstart: the machine's own");
		} else {
			if (!readrom(rom))
				return 1;
			miglog(0, "Kickstart %.500s verified", rom);
		}
	}
	/*
	 * the guest's memory exceeds the default soft limit on mappings, and
	 * Mesa, Glide and Warp3D textures the 16 MB one on the heap
	 */
	if (getrlimit(RLIMIT_VMEM, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_VMEM, &rl);
	}
	if (getrlimit(RLIMIT_DATA, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_DATA, &rl);
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
		region(MIG_FD_BASE, MIG_FD_MAP_SIZE, 1);
		if ((dfd = migdisp_open(&fi)) < 0)
			return 1;
		if (!native(&fi, sysroot, &w, &h))
			miglog(0, "Workbench mode %lux%lu, not the display's", w, h);
		direct = vram(&fi, w, h);
#ifdef WARP3D
		if (direct)
			w3d_fbfd = dfd;
#endif
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
			if (mmap((caddr_t)MIG_RTG_VRAM, migdisp_gsize, PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_FIXED, dfd, 0) == (caddr_t)-1)
				fail("display mapping");
			pretouch(&fi, dfd);
		}
		startfilesystem(fd, go[0], sysroot, readonly);
		/* the driver's mixing task is woken by the helper, never polls */
		if (migkick >= 0 && (info.ai_features & AMIGA_FEAT_SNDBELL))
			startsound(fd);
		startfloppy(fd, df);
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
	if (fit && !probe)
		fastmb = fitfast(fastmb);
	if (fastmb && envmem_region(FASTBASE, fastmb << 20, 0, -1) < 0) {
		miglog(1, "fast RAM of %lu MB: %s; free memory and swap are too small. "
		    "Add swap, or choose less with -m, fastmb= in the environment's .env "
		    "or FAST_MB= in /etc/default/amiga", fastmb, strerror(errno));
		return 1;
	}
	if (!probe) {
		region(BOOTBASE, BOOTSIZE, 0);
		region(MIG_W3D_BASE, MIG_W3D_MAP_SIZE, 0);
#ifdef WARP3D
		w3d_init((struct mig_w3d *)MIG_W3D_BASE);
#endif
		earlyroot = sysroot;
		loadboot(boot);
	}
	region(ROMBASE, ROMSIZE, 0);
	memcpy((char *)ROMBASE, rombuf, ROMSIZE);
	munmap((caddr_t)rombuf, ROMSIZE);
	if (!probe) {
		/* the guest touches no hardware: every patch, or no session */
		if (!pv || !pvstubbed) {
			miglog(1, "the Amiga environment needs its Kickstart copy patched%s",
			    pv ? " and a boot extension" : ": --nopv is not supported");
			return 1;
		}
		n = pvpatch((unsigned char *)ROMBASE);
		miglog(0, "Kickstart copy: %d changes", n);
		if ((AMIGAF_PV | pvflags) != AMIGAF_PVALL) {
			miglog(1, "%s: its hardware access is not all patchable; it cannot run here", kickname);
			return 1;
		}
		ae.ae_flags |= AMIGAF_PVALL;
		pvsegs();
	} else
		ae.ae_flags |= AMIGAF_PVALL;	/* attached and detached only */
	if (mprotect((caddr_t)ROMBASE, ROMSIZE, PROT_READ | PROT_EXEC) < 0)
		fail("mprotect");
	/* SIGUSR2 carries virtual interrupts and must be caught */
	memset((char *)&sa, 0, sizeof sa);
	sa.sa_handler = nothing;
	sa.sa_flags = SA_NODEFER;
	if (sigaction(SIGUSR2, &sa, (struct sigaction *)0) < 0)
		fail("SIGUSR2");
	sa.sa_handler = hostfault;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, (struct sigaction *)0);
	sigaction(SIGBUS, &sa, (struct sigaction *)0);
	sigaction(SIGILL, &sa, (struct sigaction *)0);
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
	ae.ae_fastsize = fastlen = fastmb << 20;
	ae.ae_flags |= AMIGAF_PAL | (census ? AMIGAF_CENSUS : 0) | kickflags;
	if (ioctl(fd, AMIGAIOC_ENTER, &ae) < 0)
		fail("Amiga session (AMIGAIOC_ENTER)");
	/*
	 * faults and FPU emulation in Mesa, linked last, are AMIX's; the code
	 * before it reaches guest registers through the guest's faults
	 */
#ifdef WARP3D
	hostpc.ah_lo = (unsigned long)w3d_text;
#else
	hostpc.ah_lo = (unsigned long)&etext;
#endif
	hostpc.ah_hi = (unsigned long)&etext;
#ifdef WARP3D
	/* and the card's DRI driver, loaded since */
	if (w3d_textend > hostpc.ah_hi)
		hostpc.ah_hi = w3d_textend;
#endif
	if (ioctl(fd, AMIGAIOC_HOSTPC, &hostpc) < 0)
		fail("AMIGAIOC_HOSTPC");
	miglog(0, "host PC range %lx-%lx", hostpc.ah_lo, hostpc.ah_hi);
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
	fprintf(stderr, "startamiga: ROM execution requires m68k\n");
	return 1;
#endif
	return 0;
usage:
	fprintf(stderr, "usage: startamiga [-rom file] [-boot file] [-e env [-b base] [-R]] [-d env] [-m fast-MB] [-dfN drive|file.adf|none] [--readonly] [--census] [--nokick] [--nopv] [--menu] [--screens] [--check | --probe]\n");
	return 2;
}
