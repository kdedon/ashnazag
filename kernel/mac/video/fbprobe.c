/*
 * fbprobe.c -- find the screen mode the firmware left.  The ROM's
 * declaration data is read; ROM code is never called.
 *
 * Direct boot: the boot record's BI_MAC_VADDR/VROW/VDEPTH/VDIM.
 *
 * A/UX Startup: just before the jump it switches every display to its
 * lowest-depth mode (1 bpp) and leaves ScrnBase and ScreenRow as they
 * were for the Monitors depth.  So, from the Mac OS low-memory copy at
 * info - 0x3C00 (A/UX <mac/sysequ.h> offsets):
 *   1. the built-in declaration ROM (ROMBase): the video sResource with
 *      a mode whose row bytes are ScreenRow and whose base offset puts
 *      the device base in the built-in video memory, preferring the
 *      Monitors depth (main PixMap pixelSize or ChunkyDepth, when those
 *      survived) and the monitor sense code's size; the console takes
 *      that sResource's lowest-depth mode: device base + its base
 *      offset, its row bytes and bounds.
 *   2. otherwise 1 bpp at ScrnBase with ScreenRow (DAFB keeps the row
 *      bytes across 1-8 bpp in most configurations); size from the sense
 *      code, or the row bytes and ScreenBytes.
 * All reads are plain loads, MMU off; low-memory pointers are checked
 * against MemTop and ROM pointers against the ROM's own size.  Video
 * hardware: a read of the DAFB sense lines, on built-in video at 1 bpp
 * a write of colour-table entries 0 and 1, and on a Quadra 800 DAFB's
 * VBL interrupt masked.
 *
 * All state is initialised data (runs from aux_entry).
 */
#include "fbcons.h"

#define BI_LAST		0x0000
#define BI_MAC_MODEL	0x8000
#define BI_MAC_VADDR	0x8001
#define BI_MAC_VDEPTH	0x8002
#define BI_MAC_VROW	0x8003
#define BI_MAC_VDIM	0x8004
#define BI_MAC_AUXINFO	0x8F00

#define PIGS		0x50696773
#define LOWOFF		0x3C00
#define PG_MACH		0xB0
#define MACH_Q800	33
#define MODEL_Q800	35	/* boot record's machine number */

/* Mac OS low memory (A/UX <mac/sysequ.h>) */
#define LM_SCRROW	0x106	/* ScreenRow (word) */
#define LM_MEMTOP	0x108	/* MemTop */
#define LM_SCRNBASE	0x824	/* ScrnBase */
#define LM_CRSRPIN	0x834	/* CrsrPin: top, left, bottom, right */
#define LM_CRSRBASE	0x898	/* CrsrBase */
#define LM_MAINDEV	0x8A4	/* MainDevice (GDHandle) */
#define LM_CRSRROW	0x8AC	/* CrsrRow (word) */
#define LM_SCRBYTES	0xC24	/* ScreenBytes */
#define LM_ROMBASE	0x2AE	/* ROMBase */
#define LM_CHUNKY	0xD60	/* ChunkyDepth (word) */
#define GD_PMAP		22	/* GDevice.gdPMap */
#define PM_ROW		4	/* PixMap.rowBytes (flags in bits 14-15) */
#define PM_BOUNDS	6
#define PM_PIXSIZE	32

/*
 * Declaration data (Designing Cards and Drivers, "Declaration ROM"):
 * format block in the last 20 bytes of the ROM, lists of 4-byte entries
 * {id, 24-bit signed offset from the entry}, ending with id 0xFF.
 */
#define ROM_SIZE	0x40	/* ROM header: size in bytes */
#define DR_TESTPAT	0x5A932BC7
#define DR_MAXENT	255	/* entries walked per list */
#define SR_TYPE		1	/* sRsrcType -> category, cType, DrSW, DrHW */
#define CAT_DISPLAY	3
#define TYP_VIDEO	1
#define MV_PARAMS	1	/* mVidParams -> length, VPBlock */
#define VP_BASE		0	/* VPBlock: vpBaseOffset */
#define VP_ROW		4	/* vpRowBytes */
#define VP_BOUNDS	6	/* top, left, bottom, right */
#define VP_PIXSIZE	32	/* vpPixelSize */
#define VP_LEN		42

/* built-in video (A/UX <sys/obvideo.h>) */
#define OBV_VIDEO_BASE	0xF9000000
#define OBV_REGBASE	0xF9800000
#define OBV_SENSELINES	(OBV_REGBASE + 0x1C)
#define OBV_CLUTADDR	(OBV_REGBASE + 0x200)	/* colour table index */
#define OBV_CLUTDATA	(OBV_REGBASE + 0x213)	/* R, G, B in turn */
#define OBV_INTMASK	(OBV_REGBASE + 0x104)
#define OBV_INTCLEAR	(OBV_REGBASE + 0x10C)

#ifdef FB_HOST
extern unsigned long fbh_peekl(), fbh_peekw();
#define PEEKL(a)	fbh_peekl((unsigned long)(a))
#define PEEKW(a)	fbh_peekw((unsigned long)(a))
#define POKEL(a, v)	((void)0)
#define POKEB(a, v)	((void)0)
#else
#define PEEKL(a)	(*(VOL unsigned long *)(a))
#define PEEKW(a)	((unsigned long)*(VOL unsigned short *)(a))
#define POKEL(a, v)	(*(VOL unsigned long *)(a) = (v))
#define POKEB(a, v)	(*(VOL unsigned char *)(a) = (v))
#endif

extern void mac_puts(), mac_puthex();

/* what was seen, for fbcons_report() */
struct fbprobe {
	int		fp_how;		/* FP_* */
	unsigned long	fp_lowmem;	/* low-memory copy address */
	int		fp_haslm;	/* low memory was read */
	unsigned long	fp_scrnbase, fp_scrrow, fp_chunky, fp_scrbytes;
	unsigned long	fp_pin[2];	/* CrsrPin top<<16|left, bottom<<16|right */
	unsigned long	fp_pixsize;	/* PixMap pixelSize, 0 if not reached */
	long		fp_sense;	/* -1 if not read */
	unsigned long	fp_why;		/* index into fp_whytab when rejected */
	unsigned long	fp_olddepth;	/* Monitors depth, 0 if unknown */
	unsigned long	fp_rombase, fp_romsize;
	int		fp_romok;	/* declaration data found */
	int		fp_rsrc;	/* matched sResource id, 0 none */
	int		fp_score;	/* its match score (1-4) */
	int		fp_namb;	/* equal matches giving another mode */
	unsigned long	fp_romrow, fp_rombase1;	/* ROM mode, for the report */
};
struct fbprobe fbprobe = { 0, 0, 0, 0, 0, 0, 0, { 0, 0 }, 0, -1, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0 };

#define FP_NONE		0
#define FP_BOOTINFO	1
#define FP_PIXMAP	2
#define FP_CHUNKY	3
#define FP_SENSE	4
#define FP_GUESS	5
#define FP_ROM		6
#define FP_ONEBIT	7

static char *fp_howtab[] = {
	"none", "boot record", "main PixMap", "ChunkyDepth", "sense code + row bytes",
	"guess: depth from row bytes", "declaration ROM, lowest mode",
	"assumed 1 bpp at ScrnBase/ScreenRow",
};
static char *fp_whytab[] = {
	"", "no screen base", "screen base is not NuBus/built-in video",
	"mode rejected", "region runs past the video memory",
};

/* sense code -> visible size (Apple standard codes, as <sys/obvideo.h>) */
static unsigned short fp_sensedim[8][2] = {
	{ 1152, 870 }, { 640, 870 }, { 512, 384 }, { 1152, 870 },
	{ 0, 0 }, { 640, 870 }, { 640, 480 }, { 0, 0 },
};

/* end of the video memory that holds base, or 0 if base is not video */
static unsigned long
fp_vramend(base)
unsigned long base;
{
	if (base >= OBV_VIDEO_BASE && base < OBV_REGBASE)
		return OBV_REGBASE;		/* built-in: registers follow */
	if (base >= 0xFA000000 && base < 0xFF000000)
		return (base & 0xFF000000) + 0x01000000;	/* slot A-E */
	return 0;
}

static int
fp_depthok(d)
unsigned long d;
{
	return d == 1 || d == 2 || d == 4 || d == 8 || d == 16 || d == 32;
}

static int
fp_ptrok(p, top)
unsigned long p, top;
{
	return p >= 0x100 && p + 64 <= top && !(p & 1);
}

static int
fp_accept(m)
register struct fbmode *m;
{
	register unsigned long end = fp_vramend(m->fm_base);

	if (m->fm_base == 0) {
		fbprobe.fp_why = 1;
		return 0;
	}
	if (end == 0) {
		fbprobe.fp_why = 2;
		return 0;
	}
	if (m->fm_row > end - m->fm_base ||
	    m->fm_height > (end - m->fm_base) / m->fm_row) {
		fbprobe.fp_why = 4;
		return 0;
	}
	if (!fbcons_attach(m)) {
		fbprobe.fp_why = 3;
		return 0;
	}
	fbprobe.fp_why = 0;
	return 1;
}

/* built-in video's monitor sense code, or -1 */
static long
fp_sense(base, mach)
unsigned long base, mach;
{
	if (mach != MACH_Q800 || base < OBV_VIDEO_BASE || base >= OBV_REGBASE)
		return -1;
	return (long)(~PEEKL(OBV_SENSELINES) & 7);
}

/*
 * Size when only depth and row bytes are known: the sense code's, else
 * CrsrPin's, else the row's width and ScreenBytes / row (if sb) or 480
 * lines.
 */
static void
fp_size(m, sb)
register struct fbmode *m;
int sb;
{
	register unsigned long t, b;

	m->fm_width = m->fm_height = 0;
	if (fbprobe.fp_sense >= 0 && fp_sensedim[fbprobe.fp_sense][0] &&
	    fp_sensedim[fbprobe.fp_sense][0] * m->fm_depth / 8 <= m->fm_row) {
		m->fm_width = fp_sensedim[fbprobe.fp_sense][0];
		m->fm_height = fp_sensedim[fbprobe.fp_sense][1];
		return;
	}
	/* Mac OS pins the cursor to the main screen: 0, 0, bottom, right */
	t = fbprobe.fp_pin[1];
	if (fbprobe.fp_pin[0] == 0 && (t & 0xFFFF) >= 512 &&
	    (t & 0xFFFF) * m->fm_depth / 8 <= m->fm_row &&
	    (t >> 16) >= 342 && (t >> 16) <= 1024) {
		m->fm_width = t & 0xFFFF;
		m->fm_height = t >> 16;
		return;
	}
	m->fm_width = m->fm_row * 8 / m->fm_depth;
	if (m->fm_width > 1152)
		m->fm_width = 1152;
	b = sb ? fbprobe.fp_scrbytes : 0;
	t = b / m->fm_row;
	m->fm_height = (b % m->fm_row == 0 && t >= 200 && t <= 1024) ? t : 480;
}

/* ------------------------------------------------ declaration ROM */

/* a..a+n-1 lies inside the ROM */
static int
dr_in(a, n)
unsigned long a, n;
{
	return a >= fbprobe.fp_rombase && a + n > a &&
	    a + n <= fbprobe.fp_rombase + fbprobe.fp_romsize;
}

/* target of the entry at p, 0 if outside the ROM */
static unsigned long
dr_ptr(p)
unsigned long p;
{
	register unsigned long o, t;

	o = PEEKL(p) & 0xFFFFFF;
	t = (o & 0x800000) ? p - (0x1000000 - o) : p + o;
	return dr_in(t, (unsigned long)4) ? t : 0;
}

/* entry id of the list at p, 0 if absent */
static unsigned long
dr_find(p, id)
register unsigned long p;
unsigned long id;
{
	register int i;
	register unsigned long e;

	for (i = 0; i < DR_MAXENT && dr_in(p, (unsigned long)4); i++, p += 4) {
		e = PEEKL(p) >> 24;
		if (e == 0xFF)
			break;
		if (e == id)
			return p;
	}
	return 0;
}

/* VPBlock of the mode list at ml, 0 if unusable */
static unsigned long
dr_vp(ml)
unsigned long ml;
{
	register unsigned long e, v;

	if ((e = dr_find(ml, (unsigned long)MV_PARAMS)) == 0 || (v = dr_ptr(e)) == 0 ||
	    !dr_in(v, (unsigned long)(4 + VP_LEN)))
		return 0;
	v += 4;
	if (!fp_depthok(PEEKW(v + VP_PIXSIZE)) || PEEKW(v + VP_ROW) == 0 ||
	    PEEKL(v + VP_BOUNDS) != 0 || PEEKW(v + VP_BOUNDS + 4) == 0 ||
	    PEEKW(v + VP_BOUNDS + 6) == 0)
		return 0;
	return v;
}

/*
 * A/UX Startup's 1-bpp mode from the ROM: see the head of the file.
 * Scores: 1 row and base fit, +2 depth = Monitors depth, +1 size = the
 * sense code's.  The best wins, the first of equals.  Fills m.
 */
static int
fp_rom(lm, mach, m)
unsigned long lm, mach;
register struct fbmode *m;
{
	unsigned long rb, sz, top, dir, r, e, t, ml, v, lo, dev, blo, bdev, id;
	register int i, j, sc, bsc, best;
	unsigned long sw, sh;

	rb = PEEKL(lm + LM_ROMBASE);
	if (mach != MACH_Q800 || rb < 0x40000000 || rb >= 0x50000000 || (rb & 0xFFFF))
		return 0;
	sz = PEEKL(rb + ROM_SIZE);
	if (sz < 0x40000 || sz > 0x400000 || (sz & (sz - 1)))
		return 0;
	fbprobe.fp_rombase = rb;
	fbprobe.fp_romsize = sz;
	top = rb + sz - 20;
	if (PEEKL(top + 14) != DR_TESTPAT || (PEEKW(top + 18) & 0xFF) != 0x0F ||
	    (dir = dr_ptr(top)) == 0)
		return 0;
	fbprobe.fp_romok = 1;
	sw = sh = 0;
	if (fbprobe.fp_sense >= 0) {
		sw = fp_sensedim[fbprobe.fp_sense][0];
		sh = fp_sensedim[fbprobe.fp_sense][1];
	}
	bsc = 0;
	blo = bdev = 0;
	for (i = 0; i < DR_MAXENT && dr_in(dir, (unsigned long)4); i++, dir += 4) {
		if ((id = PEEKL(dir) >> 24) == 0xFF)
			break;
		if ((r = dr_ptr(dir)) == 0 || (e = dr_find(r, (unsigned long)SR_TYPE)) == 0 ||
		    (t = dr_ptr(e)) == 0 || PEEKW(t) != CAT_DISPLAY || PEEKW(t + 2) != TYP_VIDEO)
			continue;
		lo = dev = 0;
		best = 0;
		for (j = 0, e = r; j < DR_MAXENT && dr_in(e, (unsigned long)4); j++, e += 4) {
			if ((PEEKL(e) >> 24) == 0xFF)
				break;
			if ((PEEKL(e) >> 24) < 0x80 || (ml = dr_ptr(e)) == 0 || (v = dr_vp(ml)) == 0)
				continue;
			if (lo == 0 || PEEKW(v + VP_PIXSIZE) < PEEKW(lo + VP_PIXSIZE))
				lo = v;
			if (PEEKW(v + VP_ROW) != fbprobe.fp_scrrow)
				continue;
			t = fbprobe.fp_scrnbase - PEEKL(v + VP_BASE);
			if (t < OBV_VIDEO_BASE || t >= OBV_REGBASE)
				continue;
			sc = 1;
			if (fbprobe.fp_olddepth && PEEKW(v + VP_PIXSIZE) == fbprobe.fp_olddepth)
				sc += 2;
			if (sw && PEEKW(v + VP_BOUNDS + 6) == sw && PEEKW(v + VP_BOUNDS + 4) == sh)
				sc += 1;
			if (sc > best) {
				best = sc;
				dev = t;
			}
		}
		if (best == 0)
			continue;
		if (best > bsc) {
			bsc = best;
			blo = lo;
			bdev = dev;
			fbprobe.fp_rsrc = id;
			fbprobe.fp_namb = 0;
		} else if (best == bsc && (dev + PEEKL(lo + VP_BASE) != bdev + PEEKL(blo + VP_BASE) ||
		    PEEKW(lo + VP_ROW) != PEEKW(blo + VP_ROW) ||
		    PEEKL(lo + VP_BOUNDS + 4) != PEEKL(blo + VP_BOUNDS + 4) ||
		    PEEKW(lo + VP_PIXSIZE) != PEEKW(blo + VP_PIXSIZE)))
			fbprobe.fp_namb++;
	}
	if (bsc == 0)
		return 0;
	fbprobe.fp_score = bsc;
	m->fm_base = bdev + PEEKL(blo + VP_BASE);
	m->fm_row = PEEKW(blo + VP_ROW);
	m->fm_depth = PEEKW(blo + VP_PIXSIZE);
	m->fm_width = PEEKW(blo + VP_BOUNDS + 6);
	m->fm_height = PEEKW(blo + VP_BOUNDS + 4);
	fbprobe.fp_rombase1 = m->fm_base;
	fbprobe.fp_romrow = m->fm_row;
	return 1;
}

/* ------------------------------------------------------ low memory */

/*
 * The Monitors depth, from the main GDevice's PixMap if that part of
 * the system heap survived the load, else ChunkyDepth when the cursor
 * screen is the main one; 0 if neither.  Only a hint for fp_rom.
 */
static unsigned long
fp_olddepth(lm)
unsigned long lm;
{
	register unsigned long top, h, p;

	top = PEEKL(lm + LM_MEMTOP);
	if (top > 0x40000000)
		top = 0x40000000;
	h = PEEKL(lm + LM_MAINDEV);
	if (fp_ptrok(h, top) && fp_ptrok(p = PEEKL(h), top) &&
	    fp_ptrok(p = PEEKL(p + GD_PMAP), top) && fp_ptrok(p = PEEKL(p), top)) {
		fbprobe.fp_pixsize = PEEKW(p + PM_PIXSIZE);
		if (PEEKL(p) == fbprobe.fp_scrnbase &&
		    (PEEKW(p + PM_ROW) & 0x3FFF) == fbprobe.fp_scrrow &&
		    PEEKL(p + PM_BOUNDS) == 0 && fp_depthok(fbprobe.fp_pixsize))
			return fbprobe.fp_pixsize;
	}
	if (PEEKL(lm + LM_CRSRBASE) == fbprobe.fp_scrnbase &&
	    (PEEKW(lm + LM_CRSRROW) & 0x3FFF) == fbprobe.fp_scrrow &&
	    fp_depthok(fbprobe.fp_chunky))
		return fbprobe.fp_chunky;
	return 0;
}

/*
 * A/UX Startup greys the screen by setting both 1-bpp colour table
 * entries to grey.  Make pixel 0 white and 1 black again, with the
 * ROM's accesses: a long 0 to the index register, then R, G, B bytes.
 */
static void
fp_clut1(base, mach)
unsigned long base, mach;
{
	register int i;

	if (mach != MACH_Q800 || base < OBV_VIDEO_BASE || base >= OBV_REGBASE)
		return;
	POKEL(OBV_CLUTADDR, 0);
	for (i = 0; i < 6; i++)
		POKEB(OBV_CLUTDATA, i < 3 ? 0xFF : 0);
}

/*
 * The ROM leaves DAFB's VBL interrupt on and nothing here takes it.
 * Its slot line would stay asserted and swallow every later VIA2 CA1
 * edge, the Ethernet's included.  Mask it and drop a pending one.
 */
static void
fp_novbl()
{
	POKEL(OBV_INTMASK, 0);
	POKEL(OBV_INTCLEAR, 0);
}

/*
 * A/UX low memory at lm, after A/UX Startup's switch to 1 bpp.
 * Returns 1 when the console is on.
 */
static int
fp_lowmem(lm, mach)
unsigned long lm, mach;
{
	struct fbmode m;

	fbprobe.fp_lowmem = lm;
	fbprobe.fp_haslm = 1;
	fbprobe.fp_pixsize = fbprobe.fp_rombase = fbprobe.fp_romsize = 0;
	fbprobe.fp_romok = fbprobe.fp_rsrc = fbprobe.fp_score = fbprobe.fp_namb = 0;
	fbprobe.fp_scrnbase = PEEKL(lm + LM_SCRNBASE);
	fbprobe.fp_scrrow = PEEKW(lm + LM_SCRROW) & 0x3FFF;
	fbprobe.fp_chunky = PEEKW(lm + LM_CHUNKY);
	fbprobe.fp_scrbytes = PEEKL(lm + LM_SCRBYTES);
	fbprobe.fp_pin[0] = PEEKL(lm + LM_CRSRPIN);
	fbprobe.fp_pin[1] = PEEKL(lm + LM_CRSRPIN + 4);
	fbprobe.fp_sense = fp_sense(fbprobe.fp_scrnbase, mach);
	if (fbprobe.fp_scrnbase == 0 || fbprobe.fp_scrrow == 0) {
		fbprobe.fp_why = 1;
		return 0;
	}
	fbprobe.fp_olddepth = fp_olddepth(lm);

	if (fp_rom(lm, mach, &m) && fp_accept(&m)) {
		if (m.fm_depth == 1)
			fp_clut1(m.fm_base, mach);
		fbprobe.fp_how = FP_ROM;
		return 1;
	}

	m.fm_base = fbprobe.fp_scrnbase;
	m.fm_row = fbprobe.fp_scrrow;
	m.fm_depth = 1;
	fp_size(&m, 1);
	if (fp_accept(&m)) {
		fp_clut1(m.fm_base, mach);
		fbprobe.fp_how = FP_ONEBIT;
		return 1;
	}
	return 0;
}

/*
 * From aux_entry, right after BSS is cleared: info is the 'Pigs' block
 * (a0 of the hand-off), the low-memory copy lies 0x3C00 below it.
 */
int
fbcons_auxinit(info)
unsigned long info;
{
	if (fbcons.fc_on)
		return 1;
	if (info < LOWOFF || info >= 0x100000 || (info & 1) || PEEKL(info) != PIGS)
		return 0;
	if (PEEKW(info + PG_MACH) == MACH_Q800)
		fp_novbl();
	return fp_lowmem(info - LOWOFF, PEEKW(info + PG_MACH));
}

/*
 * From mac_shim_main with the raw boot record: a direct boot's video
 * tags, else (if aux_entry could not) the A/UX low memory it names.
 */
int
fbcons_biinit(bi)
unsigned char *bi;
{
	struct fbmode m;
	register unsigned long n, tag, size, *p;
	unsigned long depth, dim, lm, model;

	if (fbcons.fc_on)
		return 1;
	m.fm_base = m.fm_row = depth = dim = lm = model = 0;
	for (n = 0; n + 4 <= 1024; n += size) {
		tag = PEEKW(bi + n);
		size = PEEKW(bi + n + 2);
		if (tag == BI_LAST || size < 4 || (size & 1))
			break;
		p = (unsigned long *)(bi + n + 4);
		switch (tag) {
		case BI_MAC_MODEL:	model = PEEKL(p); break;
		case BI_MAC_VADDR:	m.fm_base = PEEKL(p); break;
		case BI_MAC_VROW:	m.fm_row = PEEKL(p); break;
		case BI_MAC_VDEPTH:	depth = PEEKL(p); break;
		case BI_MAC_VDIM:	dim = PEEKL(p); break;
		case BI_MAC_AUXINFO:
			if (size >= 16)
				lm = PEEKL(p);		/* the 'Pigs' block */
			break;
		}
	}
	if (model == MODEL_Q800)
		fp_novbl();
	if (m.fm_base && m.fm_row && depth && dim) {
		m.fm_depth = depth;
		m.fm_width = dim & 0xFFFF;		/* height << 16 | width */
		m.fm_height = dim >> 16;
		if (fp_accept(&m)) {
			fbprobe.fp_how = FP_BOOTINFO;
			return 1;
		}
		return 0;
	}
	if (lm)
		return fbcons_auxinit(lm);
	if (m.fm_base && m.fm_row) {
		fbprobe.fp_scrbytes = 0;
		for (m.fm_depth = 32; m.fm_depth > 1; m.fm_depth >>= 1)
			if (m.fm_row * 8 / m.fm_depth >= 640)
				break;
		if (fp_depthok(depth))
			m.fm_depth = depth;
		fp_size(&m, 0);
		if (fp_accept(&m)) {
			fbprobe.fp_how = fp_depthok(depth) ? FP_BOOTINFO : FP_GUESS;
			return 1;
		}
	}
	return 0;
}

static void
fp_kv(k, v)
char *k;
unsigned long v;
{
	mac_puts(k);
	mac_puthex(v);
}

/* config(): mode and source, then every candidate and raw value seen */
void
fbcons_report()
{
	register struct fbmode *m = &fbcons.fc_m;

	if (fbcons.fc_on) {
		fp_kv("config: fbcons ", m->fm_base);
		fp_kv(" row ", m->fm_row);
		fp_kv(" depth ", m->fm_depth);
		fp_kv(" ", m->fm_width);
		fp_kv("x", m->fm_height);
		mac_puts(" (");
		mac_puts(fp_howtab[fbprobe.fp_how]);
		mac_puts(")\n");
	} else {
		mac_puts("config: fbcons off: ");
		mac_puts(fp_whytab[fbprobe.fp_why]);
		mac_puts("\n");
	}
	if (!fbprobe.fp_haslm)
		return;
	fp_kv("config: lowmem ", fbprobe.fp_lowmem);
	fp_kv(" ScrnBase ", fbprobe.fp_scrnbase);
	fp_kv(" ScreenRow ", fbprobe.fp_scrrow);
	fp_kv(" ChunkyDepth ", fbprobe.fp_chunky);
	fp_kv(" pixelSize ", fbprobe.fp_pixsize);
	mac_puts("\n");
	fp_kv("config: lowmem CrsrPin ", fbprobe.fp_pin[0]);
	fp_kv(" ", fbprobe.fp_pin[1]);
	fp_kv(" ScreenBytes ", fbprobe.fp_scrbytes);
	fp_kv(" sense ", (unsigned long)fbprobe.fp_sense);
	fp_kv(" Monitors depth ", fbprobe.fp_olddepth);
	mac_puts("\n");
	fp_kv("config: ROM ", fbprobe.fp_rombase);
	fp_kv(" size ", fbprobe.fp_romsize);
	if (!fbprobe.fp_romok)
		mac_puts(" no declaration data");
	else if (!fbprobe.fp_rsrc)
		mac_puts(" no video sResource matches ScrnBase/ScreenRow");
	else {
		fp_kv(" sRsrc ", (unsigned long)fbprobe.fp_rsrc);
		fp_kv(" score ", (unsigned long)fbprobe.fp_score);
		fp_kv(" others ", (unsigned long)fbprobe.fp_namb);
		fp_kv(" -> base ", fbprobe.fp_rombase1);
		fp_kv(" row ", fbprobe.fp_romrow);
	}
	mac_puts("\n");
}
