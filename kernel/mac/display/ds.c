/*
 * ds.c -- display service core: the display, sessions, switching, the
 * colour table, VBL, and input routing with the hotkeys.
 *
 * The console (fbcons) is session 0 and always exists.  User sessions
 * come from FBIOACQUIRE.  One session is in front: it owns VRAM, the
 * CLUT and the input.  A hidden session keeps a shadow copy of the
 * mapped region; its mappings fault onto the shadow pages.
 *
 * A switch unloads user translations, so it runs only where no process
 * can be inside the HAT: in process context (ioctl, close) or from a
 * STREAMS service procedure, which the kernel runs on return to user
 * mode and in swtch().  Hotkeys arrive in the ADB soft interrupt and are
 * handed to that service procedure.
 */
#include "sys/types.h"
#include "sys/param.h"
#include "sys/sysmacros.h"
#include "sys/errno.h"
#include "sys/poll.h"
#include "sys/kmem.h"
#include "sys/stream.h"
#include "sys/cred.h"
#include "sys/proc.h"
#include "sys/disp.h"
#include "sys/session.h"
#include "sys/time.h"
#ifdef DS_ATARI
#include "sys/cred.h"
#include "sys/proc.h"
#include "sys/disp.h"
#include "sys/signal.h"
#include "vm/as.h"
#include "vm/seg.h"
#endif
#include "vm/page.h"
#include "sys/tuneable.h"
#include "fbcons.h"
#include "adb.h"
#include "ds.h"
#include "sys/conf.h"
extern struct streamtab coinfo;

extern timestruc_t hrestime;
extern long lbolt;
extern int availrmem, freemem, minfree;
extern int hat_cm_fb_add();
#ifdef DS_AMIGA
extern int cputype;
#endif
extern void adbkbd_cons();
extern void (*fbcons_panicfn)();
extern int fbcons_grab();
extern struct fbpmode fbp_mode[];
#ifdef DS_ATARI
extern void ata_dspoll(), ata_vsave(), ata_vload();
extern int ata_nvmode(), ata_vmfind();
extern void ata_vminfo(), ata_vmset();
extern unsigned long ata_vbytes();
extern unsigned long ata_pool, ata_poolsz;
extern int ata_tt;
static struct dsvid ds_vcons;		/* the mode under a Videl session */
#endif
#if defined(DS_ATARI) && defined(ATA_SV)
extern int ata_svfb;
#define DS_SV	ata_svfb	/* the display is the SuperVidel's native mode */
#else
#define DS_SV	0
#endif
extern int fbp_nmode, fbp_cur;
#ifdef DS_AMIGA
extern int am_dsinit(), am_nvmode(), am_vmfind();
extern void am_dsclut(), am_vminfo(), am_planes(), am_pan();
extern char *am_dsname;
extern int am_dscmapbits;
extern unsigned long am_dsvsize;
extern int am_dsfull;
extern int am_mtiming(), am_vmflags();
extern void am_rtgpanic();
extern unsigned char am_tried[];
#endif
extern unsigned char fb_font[256][16];

struct dsdisp ds_disp;
struct dssess ds_sess[1 + DS_NSESS];
struct dssess *ds_front = &ds_sess[0];
unsigned long ds_gen, ds_serial, ds_vblcount;
unsigned long ds_nhwvbl, ds_nswvbl;	/* VBLs from DAFB, from the tick */
void (*ds_frontfn)();
int (*ds_leavefn)();

static long ds_nextid = 1;
static int ds_pend = -1;		/* hotkey digit to act on, -1 none */
static long ds_pwait;			/* lbolt of its next try, after ENOMEM */
#ifdef DS_AMIGA
static int ds_nomem;			/* its tries refused for memory */
#define	DS_NOMEMTRY	40		/* then the shadow keeps what it has */
#endif
static long ds_hwlast;			/* lbolt at the last DAFB VBL */
static long ds_lsec, ds_lusec;		/* last timestamp given */
static unsigned char ds_phys[16];	/* keys physically down */
static unsigned char ds_eat[16];	/* key ups to swallow */
static int ds_srv();
static struct qinit ds_qi;
static queue_t ds_q;

#define BIT(m, c)	((m)[(c) >> 3] & (1 << ((c) & 7)))
#define SETB(m, c)	((m)[(c) >> 3] |= 1 << ((c) & 7))
#define CLRB(m, c)	((m)[(c) >> 3] &= ~(1 << ((c) & 7)))

#define VIA1		0x50F00000
#define VIA2		0x50F02000
#define VIA_PCR		0x1800
#define VIA_IER		0x1C00
#define VIA_T1CH	0x0A00
#define VIA_IFR		0x1A00
#define VIA_TICK	13054	/* T1 latch: 783360 Hz / 60 */

#define K_CTL		0x36
#define K_CMD		0x37
#define K_OPT		0x3A
#define K_ESC		0x35
#define K_RET		0x24
#define K_ROPT		0x7C
#define K_RCTL		0x7D

/* ADB key code of the digits 0-9 */
static unsigned char ds_digit[10] = {
	0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19
};

/* ------------------------------------------------------------ CLUT */

/* what FBIOGINFO reports for s: the display, or the mode s set */
void
ds_sinfo(s, fi)
register struct dssess *s;
register struct fbinfo *fi;
{
	*fi = ds_disp.d_info;
#ifdef DS_ATARI
	if (ata_nvmode())
		fi->fi_flags |= FBF_SETMODE;
	if (s && s->s_vmode)
		ata_vminfo(s->s_vmode - 1, fi);
#endif
#ifdef DS_AMIGA
	if (am_nvmode())
		fi->fi_flags |= FBF_SETMODE;
	if (s && s->s_vmode)
		am_vminfo(s->s_vmode - 1, fi);
#endif
	if (s && DS_FULL(s))
		fi->fi_flags |= FBF_OWNV;
	if (s)
		fi->fi_size = s->s_size;
	/* a smaller screen centred in its mode */
	if (s && s->s_rqw && s->s_rqw <= fi->fi_width && s->s_rqh <= fi->fi_height) {
		fi->fi_width = s->s_rqw;
		fi->fi_height = s->s_rqh;
		fi->fi_offset += s->s_vorg;
	}
}

/* entries in s's colour table */
int
ds_ncmap(s)
struct dssess *s;
{
	struct fbinfo fi;

	ds_sinfo(s, &fi);
	return (int)fi.fi_cmapsize;
}

/*
 * Mac standard tables, white at 0 and black at the last entry: 8 bpp is
 * the 6x6x6 cube (0xFF down to 0) then ramps of red, green, blue and
 * grey without the cube's levels; fewer bits get a grey ramp.
 */
static void
ds_stdcmap(s, depth)
register struct dssess *s;
int depth;
{
	static unsigned char ramp[10] = {
		0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11
	};
	register int i, n, c;

	if (depth > 8)
		return;
	n = 1 << depth;
	if (depth < 8) {
		for (i = 0; i < n; i++)
			s->s_cmap[0][i] = s->s_cmap[1][i] = s->s_cmap[2][i] =
			    0xFFFF - i * (0xFFFF / (n - 1));
		return;
	}
	for (i = 0; i < 215; i++) {
		s->s_cmap[0][i] = (5 - i / 36) * 0x3333;
		s->s_cmap[1][i] = (5 - i / 6 % 6) * 0x3333;
		s->s_cmap[2][i] = (5 - i % 6) * 0x3333;
	}
	for (i = 0; i < 40; i++) {
		c = ramp[i % 10] * 0x101;
		s->s_cmap[0][215 + i] = (i < 10 || i >= 30) ? c : 0;
		s->s_cmap[1][215 + i] = ((i >= 10 && i < 20) || i >= 30) ? c : 0;
		s->s_cmap[2][215 + i] = i >= 20 ? c : 0;
	}
	s->s_cmap[0][255] = s->s_cmap[1][255] = s->s_cmap[2][255] = 0;
}

#ifdef DS_AMIGA
#define HWCLUT 1
static void
ds_clutload(s, lo, hi)
struct dssess *s;
int lo, hi;
{
	am_dsclut(s, lo, hi);
}
#elif defined(DS_ATARI)
#define HWCLUT	1
#define FAL_PAL(i)	(*(VOL unsigned long *)(0xFFFF9800 + 4 * (i)))
#define ST_PAL(i)	(*(VOL unsigned short *)(0xFFFF8240 + 2 * (i)))
#define TT_PAL(i)	(*(VOL unsigned short *)(0xFFFF8400 + 2 * (i)))
#define TT_STHIGH	((*(VOL unsigned short *)0xFFFF8262 & 0x700) == 0x200)

/* 16-bit gun to the STE's 4 bits, low bit on top */
#define STE4(v)		((((v) >> 13) & 7) | (((v) >> 9) & 8))

/*
 * Entries lo..hi-1 into the Videl palette (black while blanked); the
 * first 16 also into the ST palette that ST-compatible modes use.
 */
static void
ds_clutload(s, lo, hi)
register struct dssess *s;
int lo, hi;
{
	register unsigned long r, g, b;
	register int i;

	for (i = lo; i < hi; i++) {
		r = s->s_blank ? 0 : s->s_cmap[0][i];
		g = s->s_blank ? 0 : s->s_cmap[1][i];
		b = s->s_blank ? 0 : s->s_cmap[2][i];
		/* 4 bits a gun; ST high shows entries 254 and 255, which bit 1 of entry 0 inverts */
		if (ata_tt) {
			TT_PAL(TT_STHIGH && i < 2 ? 254 + i : i) =
			    (r & 0xF000) >> 4 | (g & 0xF000) >> 8 | (b & 0xF000) >> 12;
			if (TT_STHIGH && i == 0)
				TT_PAL(0) = 0;
			continue;
		}
#if defined(ATA060)
		/* 8 bits a gun; no ST palette, a Videl register */
		if (DS_SV) {
			FAL_PAL(i) = (r & 0xFF00) << 16 | (g & 0xFF00) << 8 | (b & 0xFF00) >> 8;
			continue;
		}
#endif
		FAL_PAL(i) = (r & 0xFC00) << 16 | (g & 0xFC00) << 8 | (b & 0xFC00) >> 8;
		if (i < 16)
			ST_PAL(i) = STE4(r) << 8 | STE4(g) << 4 | STE4(b);
	}
}
#else
#define HWCLUT	ds_disp.d_dafb

/* entries lo..hi-1 of s's table into the DAFB (black while blanked) */
static void
ds_clutload(s, lo, hi)
register struct dssess *s;
int lo, hi;
{
	register VOL unsigned char *d = (unsigned char *)(DAFB_REG + DAFB_CLUTDATA);
	register int i;

	*(VOL unsigned long *)(DAFB_REG + DAFB_CLUTADDR) = lo;
	for (i = lo; i < hi; i++) {
		*d = s->s_blank ? 0 : s->s_cmap[0][i] >> 8;
		*d = s->s_blank ? 0 : s->s_cmap[1][i] >> 8;
		*d = s->s_blank ? 0 : s->s_cmap[2][i] >> 8;
	}
}
#endif

/* store entries; the front session's reach the hardware at the next VBL */
void
ds_setcmap(s, start, n, r, g, b)
register struct dssess *s;
int start, n;
unsigned short *r, *g, *b;
{
	register int i, x;

	x = DS_SPL(DS_HI);
	for (i = 0; i < n; i++) {
		s->s_cmap[0][start + i] = r[i];
		s->s_cmap[1][start + i] = g[i];
		s->s_cmap[2][start + i] = b[i];
	}
	if (start < s->s_dlo)
		s->s_dlo = start;
	if (start + n > s->s_dhi)
		s->s_dhi = start + n;
	DS_SPLX(x);
}

void
ds_setblank(s, on)
register struct dssess *s;
int on;
{
	register int x;

	x = DS_SPL(DS_HI);
	s->s_blank = on;
	s->s_dlo = 0;
	s->s_dhi = ds_ncmap(s);
	DS_SPLX(x);
}

/* ------------------------------------------------------------- VBL */

static void
ds_vbl()
{
	register struct dssess *s = ds_front;

	ds_vblcount++;
	if (HWCLUT && s->s_dhi > s->s_dlo) {
		ds_clutload(s, s->s_dlo, s->s_dhi);
		s->s_dlo = 256;
		s->s_dhi = 0;
	}
	wakeup((caddr_t)&ds_vblcount);
}

/* built-in video's slot line: VIA2 port A bit 6, IPL 2 */
void
ds_vblintr()
{
#ifndef DS_AMIGA
	*(VOL unsigned long *)(DAFB_REG + DAFB_INTCLEAR) = 0;
#endif
	if (!ds_disp.d_on)
		return;
	ds_nhwvbl++;
	ds_hwlast = lbolt;
	ds_disp.d_info.fi_flags |= FBF_VBL;
	ds_vbl();
}

#ifdef DS_AMIGA
static int ds_shadowk();
static void ds_lzero();
static unsigned long ds_vdirty;

void
ds_am_console()
{
	if (ds_disp.d_on) {
		ds_pend = 0;
		qenable(&ds_q);
	}
}

/* s's screen starts off bytes into its own VRAM, in its mode's rows */
int
ds_setpan(s, off)
register struct dssess *s;
unsigned long off;
{
	struct fbinfo fi;

	if (!DS_FULL(s))
		return ENXIO;
	/* the whole mode stays inside the region, not just the centred screen */
	if (s->s_vmode)
		am_vminfo(s->s_vmode - 1, &fi);
	else
		fi = ds_disp.d_info;
	if (off < s->s_vorg || !DS_PANOK(off - s->s_vorg, &fi, s->s_size))
		return EINVAL;
	s->s_vpan = off - s->s_vorg;
	if (s == ds_front)
		am_pan(s);
	return 0;
}

static void ds_tstart(), ds_tfree(), ds_lcopy();
static int ds_tpend;			/* 1 revert, 2 keep: for ds_srv */
static struct dssess *ds_trial;		/* front session showing an untried mode */

/* mode n's card timing has been kept before */
static int
ds_tried(n)
int n;
{
	unsigned long hz;

	return (am_vmflags(n, &hz) & FBM_TRIED) != 0;
}

/*
 * s takes mode n, its screen w by h at org bytes into it (0: the mode's
 * own): its own depth and size, its shadow sized to match.  A new size
 * needs s unmapped.  In front, an untried timing goes on trial.
 */
static int
ds_vset(s, n, w, h, org)
register struct dssess *s;
register int n;
unsigned long w, h, org;
{
	struct fbinfo fi;
	register int e, trial;

	if (ds_trial && ds_trial != s)
		return EBUSY;
	am_vminfo(n, &fi);
	if (fi.fi_size > ds_disp.d_vsize)
		return ENXIO;
	if (!fbcons_grab())
		return EBUSY;
	if (fi.fi_size != s->s_size && DS_OWNV(s)) {
		/* a mapping stays inside the session's own frame when it grows */
		if (fi.fi_size < s->s_size && ds_segcount(s)) {
			fbcons_unlock();
			return EBUSY;
		}
		s->s_size = fi.fi_size;
	} else if (fi.fi_size != s->s_size && !DS_FULL(s)) {
		if (ds_segcount(s)) {
			fbcons_unlock();
			return EBUSY;
		}
		if ((e = ds_shadowk(s, fi.fi_size, fi.fi_size, 0)) != 0) {
			fbcons_unlock();
			return e;
		}
	}
	/* a new mode during a trial ends it, a new trial if untried */
	if (ds_trial == s) {
		ds_tfree();
		ds_trial = 0;
		ds_tpend = 0;
	}
	trial = s == ds_front && !ds_trial && !ds_tried(n);
	if (s->s_vmode != n + 1)
		s->s_vpan = 0;
	s->s_vmode = n + 1;
	s->s_rqw = w;
	s->s_rqh = h;
	s->s_vorg = org;
	ds_stdcmap(s, (int)fi.fi_depth);
	if (s == ds_front) {
		am_planes(s);
		am_pan(s);
		ds_clutload(s, 0, ds_ncmap(s));
	}
	fbcons_unlock();
	if (trial)
		ds_tstart(s);
	ds_note(s, FBN_MODE);
	return 0;
}

int
ds_setmode(s, id)
register struct dssess *s;
unsigned long id;
{
	register int n;

	if ((n = am_vmfind(id)) < 0)
		return ENXIO;
	return ds_vset(s, n, 0L, 0L, 0L);
}

/*
 * s's region grows to n bytes of VRAM if nothing else holds them, so a
 * guest's screens and their double buffers fit.  Before s is mapped.
 */
int
ds_vsize(s, n)
register struct dssess *s;
unsigned long n;
{
	unsigned long old = s->s_size;
	int e = 0;

	n = (n + DS_PGOFF) & ~DS_PGOFF;
	if (!DS_FULL(s) || DS_OWNV(s))
		return ENXIO;
	if (n <= old)
		return 0;
	if (!fbcons_grab())
		return EBUSY;
	if (ds_segcount(s))
		e = EBUSY;
	else if (n > am_vheld(s))
		e = ENOSPC;
	else if ((e = ds_shadowk(s, n, n, 0)) == 0) {
		/* the new part may hold the 3D driver's old buffers */
		if (s == ds_front)
			ds_lzero((VOL unsigned long *)(ds_disp.d_page + old), n - old);
		else if (ds_vdirty < n)
			ds_vdirty = n;
	}
	fbcons_unlock();
	return e;
}
#endif

/* ------------------------------------------------------ mode requests */

#ifdef DS_AMIGA
#define DS_NVMODE()		am_nvmode()
#define DS_VMINFO(n, fi)	am_vminfo(n, fi)
#define DS_VMFLAGS(n, hz)	am_vmflags(n, hz)
#else
#define DS_NVMODE()		0
#define DS_VMINFO(n, fi)	(*(fi) = ds_disp.d_info)
#define DS_VMFLAGS(n, hz)	(*(hz) = 0, FBM_TRIED)
#endif

/*
 * The mode for a w by h screen at depth d, -1 if none (n -1: the
 * display's own, where it lists none).  rq gets the screen's place.
 */
static int
ds_grant(w, h, d, flags, tried, rq)
unsigned long w, h, d, flags;
int tried;
register struct fbreq *rq;
{
	struct fbinfo fi;
	unsigned long a, ba = 0, hz, x;
	register int n, nm = DS_NVMODE(), f, how, best = -2, bhow = 99;

	for (n = nm ? 0 : -1; n < nm; n++) {
		DS_VMINFO(n, &fi);
		f = DS_VMFLAGS(n, &hz);
		if (fi.fi_depth != d || (tried && !(f & FBM_TRIED)))
			continue;
		a = fi.fi_width * fi.fi_height;
		if (f & FBM_SCALE2) {
			if ((flags & FBR_NOSCALE) || fi.fi_width != 2 * w || fi.fi_height != 2 * h)
				continue;
			how = FBG_SCALE;
		} else if (fi.fi_width == w && fi.fi_height == h)
			how = FBG_EXACT;
		else if (fi.fi_width >= w && fi.fi_height >= h)
			how = FBG_CENTRE;
		else
			how = FBG_CLIP;
		if ((flags & FBR_EXACT) && how != FBG_EXACT)
			continue;
		if (how < bhow || (how == bhow &&
		    (how == FBG_CENTRE ? a < ba : how == FBG_CLIP && a > ba))) {
			best = n;
			bhow = how;
			ba = a;
			rq->rq_id = fi.fi_mode;
			rq->rq_x = rq->rq_y = 0;
			rq->rq_how = how;
			if (how == FBG_CENTRE) {
				/* long-aligned rows, word-aligned planes */
				x = (fi.fi_width - w) / 2;
				rq->rq_x = x - x % (fi.fi_layout == FBL_PLANES ? 16 : 32 / d ? 32 / d : 1);
				rq->rq_y = (fi.fi_height - h) / 2;
			}
		}
	}
	return best == -2 ? -1 : best == -1 ? nm : best;
}

/* bytes from a mode's first pixel to the granted screen's */
static unsigned long
ds_org(n, rq)
int n;
struct fbreq *rq;
{
	struct fbinfo fi;

	if (n < 0 || n >= DS_NVMODE())
		fi = ds_disp.d_info;
	else
		DS_VMINFO(n, &fi);
	return rq->rq_y * fi.fi_rowbytes +
	    (fi.fi_layout == FBL_PLANES ? rq->rq_x / 8 : rq->rq_x * fi.fi_depth / 8);
}

/* FBIOREQMODE */
int
ds_reqmode(s, rq)
register struct dssess *s;
register struct fbreq *rq;
{
	register int n;

	if (rq->rq_width == 0 || rq->rq_height == 0 ||
	    (rq->rq_flags & ~(FBR_EXACT | FBR_NOSCALE)))
		return EINVAL;
	if ((n = ds_grant(rq->rq_width, rq->rq_height, rq->rq_depth, rq->rq_flags, 0, rq)) < 0)
		return ENXIO;
#ifdef DS_AMIGA
	if (n < DS_NVMODE())
		return ds_vset(s, n, rq->rq_width, rq->rq_height, ds_org(n, rq));
#endif
#ifdef DS_ATARI
	if (s->s_vmode)
		return ENXIO;
#endif
	s->s_rqw = rq->rq_width;
	s->s_rqh = rq->rq_height;
	s->s_vorg = ds_org(n, rq);
	ds_note(s, FBN_MODE);
	return 0;
}

#ifdef DS_AMIGA
/*
 * Trial: a front session's mode with an untried timing.  A line at its
 * top asks for Return; after DS_TRIAL ticks, or when the session leaves
 * the front, the best tried mode for its screen comes back.
 */
#define DS_TRIAL	(15 * HZ)
static long ds_tend;
static int ds_tredraw;			/* a second has passed: for ds_srv */
static caddr_t ds_tsave;		/* pixels under the prompt, then the prompt's */
static caddr_t ds_tat;			/* where the prompt is drawn */
static unsigned long ds_trow, ds_tlen, ds_trows; /* bytes per prompt row, per copy, per screen row */
static int ds_tb;			/* bytes per pixel */
static char ds_tmsg[] = " New display mode: press Return to keep it ";

/* where the card shows s from, its rows and depth */
static caddr_t
ds_tframe(s, fi)
register struct dssess *s;
register struct fbinfo *fi;
{
	am_vminfo(s->s_vmode - 1, fi);
	return (caddr_t)ds_disp.d_page + fi->fi_offset + DS_VOFF(s) + s->s_vpan;
}

static void
ds_tfree()
{
	if (ds_tsave)
		kmem_free(ds_tsave, 2 * ds_tlen);
	ds_tsave = ds_tat = 0;
}

#define TPX(q)		(b == 1 ? *(VOL unsigned char *)(q) : \
			 b == 2 ? *(VOL unsigned short *)(q) : *(VOL unsigned long *)(q))
#define TSET(q, v)	(b == 1 ? (*(VOL unsigned char *)(q) = (v)) : \
			 b == 2 ? (*(VOL unsigned short *)(q) = (v)) : (*(VOL unsigned long *)(q) = (v)))

/*
 * The prompt at p and the pixels under it.  Shown (on 1), a pixel the
 * guest changed is saved and drawn over again (on 2: every pixel); taken
 * off, a pixel still showing the prompt gets the saved one back.
 */
static void
ds_tsync(p, on)
register caddr_t p;
int on;
{
	register caddr_t sv = ds_tsave, pr = ds_tsave + ds_tlen;
	register unsigned long x, v;
	register int y, b = ds_tb;

	for (y = 0; y < 16; y++, p += ds_trows, sv += ds_trow, pr += ds_trow)
		for (x = 0; x < ds_trow; x += b) {
			v = TPX(p + x);
			if (on == 2 || (on && v != TPX(pr + x))) {
				TSET(sv + x, v);
				TSET(p + x, TPX(pr + x));
			} else if (!on && v == TPX(pr + x))
				TSET(p + x, TPX(sv + x));
		}
}

/* the prompt drawn over s's top rows (on), or the rows put back */
static void
ds_tprompt(s, on)
register struct dssess *s;
int on;
{
	struct fbinfo fi;
	register caddr_t p = ds_tframe(s, &fi), q;
	register unsigned long fg = 0xfefefeUL, bg = 0x010101UL, x;
	register int y, c, b = fi.fi_depth / 8, i, lo = 0x7fffffff, hi = -1;
	unsigned long w = (sizeof ds_tmsg - 1) * 8;

	if (!on) {
		if (ds_tat)
			ds_tsync(ds_tat, 0);
		ds_tfree();
		return;
	}
	if (w > fi.fi_width)
		w = fi.fi_width & ~3UL;
	if (ds_tsave == 0) {
		ds_tb = b;
		ds_trow = w * b;
		ds_tlen = 16 * ds_trow;
		ds_trows = fi.fi_rowbytes;
		if ((ds_tsave = kmem_alloc(2 * ds_tlen, KM_NOSLEEP)) == 0)
			return;
	}
	/* colours a guest seldom draws, so its pixels and the prompt's differ */
	if (b == 2) {
		fg = 0xf7de;
		bg = 0x0821;
	} else if (b == 1)
		for (i = 1; i < 255; i++) {
			c = s->s_cmap[0][i] / 4 + s->s_cmap[1][i] / 2 + s->s_cmap[2][i] / 4;
			if (c < lo) {
				lo = c;
				bg = i;
			}
			if (c > hi) {
				hi = c;
				fg = i;
			}
		}
	q = ds_tsave + ds_tlen;
	for (y = 0; y < 16; y++, q += ds_trow)
		for (x = 0; x < w; x++) {
			c = fb_font[(unsigned char)ds_tmsg[x / 8]][y] & 0x80 >> (x & 7);
			TSET(q + x * b, c ? fg : bg);
		}
	/* moved by a pan: off the old place, everything under the new one saved */
	if (p != ds_tat) {
		if (ds_tat)
			ds_tsync(ds_tat, 0);
		ds_tat = p;
		ds_tsync(p, 2);
	} else
		ds_tsync(p, 1);
}

static void
ds_tstart(s)
struct dssess *s;
{
	register int x = DS_SPL(DS_HI);

	ds_tpend = 0;
	ds_tend = lbolt + DS_TRIAL;
	ds_trial = s;
	DS_SPLX(x);
	ds_tprompt(s, 1);
}

/*
 * s's picture, at org bytes into the old mode's frame of, moved into its
 * new mode's place and the rest of the frame cleared, for a guest that
 * does not redraw.
 */
static void
ds_tmove(s, p, of, org)
register struct dssess *s;
caddr_t p;
struct fbinfo *of;
unsigned long org;
{
	struct fbinfo fi;
	register VOL unsigned long *a, *b;
	register caddr_t f = ds_tframe(s, &fi), q = f + s->s_vorg;
	register unsigned long rn = fi.fi_rowbytes, ro = of->fi_rowbytes, n = s->s_rqw * fi.fi_depth / 8, i;
	register long y, h = s->s_rqh, x0 = s->s_vorg % rn, y0 = s->s_vorg / rn;

	if (n > rn - x0)
		n = rn - x0;
	if (n > ro - org % ro)
		n = ro - org % ro;
	if (h > fi.fi_height - y0)
		h = fi.fi_height - y0;
	if (h > of->fi_height - org / ro)
		h = of->fi_height - org / ro;
	DS_CPUSHA();
	if (((unsigned long)p | (unsigned long)q | ro | rn | n) & 3)
		h = 0;
	else if (q >= p && rn >= ro)
		for (y = h - 1; y >= 0; y--) {
			a = (VOL unsigned long *)(p + y * ro + n);
			b = (VOL unsigned long *)(q + y * rn + n);
			for (i = n / 4; i > 0; i--)
				*--b = *--a;
		}
	else if (q <= p && rn <= ro)
		for (y = 0; y < h; y++) {
			a = (VOL unsigned long *)(p + y * ro);
			b = (VOL unsigned long *)(q + y * rn);
			for (i = n / 4; i > 0; i--)
				*b++ = *a++;
		}
	else
		h = 0;
	for (y = 0; y < fi.fi_height; y++)
		for (i = 0; i < rn; i += 4)
			if (y < y0 || y >= y0 + h || i < x0 || i >= x0 + n)
				*(VOL unsigned long *)(f + y * rn + i) = 0;
}

/*
 * The trial ends: s keeps its mode, or takes the best tried one.
 * EBUSY: the screen was held, the trial stays on for a retry.
 */
static int
ds_tdone(keep)
int keep;
{
	register struct dssess *s = ds_trial;
	struct fbinfo fi;
	struct fbreq rq;
	register int n, t, e;
	unsigned long org;
	caddr_t p;

	ds_tpend = 0;
	if (s == 0)
		return 0;
	if (s == ds_front)
		ds_tprompt(s, 0);
	else
		ds_tfree();
	t = am_mtiming(s->s_vmode - 1);
	if (keep || !s->s_used || s->s_dead) {
		ds_trial = 0;
		if (keep && t >= 0 && t < 16)
			am_tried[t] = 1;
		return 0;
	}
	am_vminfo(s->s_vmode - 1, &fi);
	if (s->s_rqw == 0) {
		s->s_rqw = fi.fi_width;
		s->s_rqh = fi.fi_height;
	}
	org = s->s_vorg;
	p = ds_tframe(s, &fi) + org;
	bzero((caddr_t)&rq, sizeof rq);
	if ((n = ds_grant(s->s_rqw, s->s_rqh, fi.fi_depth, 0L, 1, &rq)) < 0 || n >= am_nvmode()) {
		ds_trial = 0;
		return 0;
	}
	/* ds_vset ends the trial */
	if ((e = ds_vset(s, n, s->s_rqw, s->s_rqh, ds_org(n, &rq))) != 0) {
		if (e != EBUSY || fbcons_grab()) {
			if (e == EBUSY)
				fbcons_unlock();
			ds_trial = 0;
			return 0;
		}
		return EBUSY;
	}
	if (s == ds_front)
		ds_tmove(s, p, &fi, org);
	return 0;
}

/* FBIOMODEOK: 0 keeps s's mode on trial; an id: that mode shows on this monitor */
int
ds_modeok(s, id)
struct dssess *s;
unsigned long id;
{
	register int n, t;

	if (id == 0) {
		if (ds_trial != s)
			return EINVAL;
		ds_tdone(1);
		return 0;
	}
	if ((n = am_vmfind(id)) < 0)
		return ENXIO;
	if ((t = am_mtiming(n)) >= 0 && t < 16)
		am_tried[t] = 1;
	return 0;
}
#else
int
ds_modeok(s, id)
struct dssess *s;
unsigned long id;
{
	return id == 0 ? EINVAL : 0;
}
#endif

/* every tick: VBL when DAFB gives none, and deferred switches */
static void
ds_tick()
{
	register int x;

	x = DS_SPL(DS_HI);
	if (lbolt - ds_hwlast > 3) {
		ds_nswvbl++;
		ds_vbl();
	}
	DS_SPLX(x);
#ifdef DS_ATARI
	ata_dspoll();
#endif
	if (ds_pend >= 0 && lbolt - ds_pwait >= 0)
		qenable(&ds_q);
#ifdef DS_AMIGA
	if (ds_trial && (ds_tpend || lbolt - ds_tend >= 0)) {
		if (!ds_tpend)
			ds_tpend = 1;
		qenable(&ds_q);
	} else if (ds_trial && (ds_tend - lbolt) % HZ == 0) {
		ds_tredraw = 1;
		qenable(&ds_q);
	}
#endif
	timeout(ds_tick, (caddr_t)0, 1);
}

/* ------------------------------------------------------- timestamps */

/* hrestime plus the elapsed part of the current tick (T1 high byte) */
void
ds_now(secp, usecp)
long *secp, *usecp;
{
	register long sec, us, hi, ifr;
	register int x;

	x = DS_SPL(7);
	sec = hrestime.tv_sec;
	us = hrestime.tv_nsec / 1000;
#if defined(DS_ATARI) || defined(DS_AMIGA)
	hi = ifr = 0;		/* tick resolution */
#else
	hi = *(VOL unsigned char *)(VIA1 + VIA_T1CH);
	ifr = *(VOL unsigned char *)(VIA1 + VIA_IFR);
	hi = (VIA_TICK - (hi << 8)) * 1277 / 1000;
	if (hi < 0)
		hi = 0;
	if (hi > 16666)
		hi = 16666;
	us += hi;
	if (ifr & 0x40)			/* tick due, not yet counted */
		us += 16667;
#endif
	while (us >= 1000000) {
		us -= 1000000;
		sec++;
	}
	if (sec < ds_lsec || (sec == ds_lsec && us <= ds_lusec)) {
		sec = ds_lsec;
		us = ds_lusec + 1;
		if (us >= 1000000) {
			us = 0;
			sec++;
		}
	}
	ds_lsec = sec;
	ds_lusec = us;
	DS_SPLX(x);
	*secp = sec;
	*usecp = us;
}

/* --------------------------------------------------------- sessions */

struct dssess *
ds_byid(id)
long id;
{
	register int i;

	if (id == 0)
		return &ds_sess[0];
	for (i = 1; i <= DS_NSESS; i++)
		if (ds_sess[i].s_used && !ds_sess[i].s_dead && ds_sess[i].s_id == id)
			return &ds_sess[i];
	return 0;
}

/* the n-th live user session by creation order, 0 if none */
static struct dssess *
ds_nth(n)
int n;
{
	register struct dssess *s = 0, *best;
	register int i, k;

	for (k = 1; ; k++) {
		best = 0;
		for (i = 1; i <= DS_NSESS; i++)
			if (ds_sess[i].s_used && !ds_sess[i].s_dead &&
			    (s == 0 || ds_sess[i].s_id > s->s_id) &&
			    (best == 0 || ds_sess[i].s_id < best->s_id))
				best = &ds_sess[i];
		if (best == 0)
			return 0;
		s = best;
		if (k == n)
			return s;
	}
}

void
ds_note(s, type)
register struct dssess *s;
int type;
{
	register int x, n;

	if (s == &ds_sess[0] || s->s_dead)
		return;
	x = DS_SPL(DS_HI);
	n = (s->s_nput + 1) % DS_NNOTE;
	if (n != s->s_nget) {
		s->s_note[s->s_nput].fn_type = type;
		s->s_note[s->s_nput].fn_serial = ds_serial;
		s->s_nput = n;
	}
	DS_SPLX(x);
	wakeup((caddr_t)s->s_note);
	pollwakeup(&s->s_ph, POLLIN | POLLRDNORM);
}

#ifdef DS_AMIGA
/* a card with deeper modes: the console's shadow keeps its 8-bit frame */
static unsigned long ds_keep;
/* bytes at d_page that may hold a hidden session's pixels */
static unsigned long ds_vdirty = ~0UL;
#endif

static void ds_shfree();

/* frees n frames from pfn */
static void
ds_pgfree(pfn, n)
unsigned long *pfn, n;
{
	register page_t *pp;
	register unsigned long i;

	for (i = 0; i < n; i++) {
		pp = page_numtopp(pfn[i]);
		if (--pp->p_keepcnt == 0)
			page_abort(pp);
	}
	pages_pp_kernel -= n;
	availrmem += n;
}

/*
 * s gets a zeroed shadow of size bytes in place of its old one: page frames
 * from the free list, outside the kernel heap.  The kernel reaches them
 * through the identity map below 1 GB.  Only the first keep bytes (page
 * multiple) get their own frames.  wait: may sleep for memory.
 * Process context; errno.
 */
static int
ds_shadowk(s, size, keep, wait)
register struct dssess *s;
unsigned long size, keep;
int wait;
{
	register unsigned long i, j, n;
	register page_t *pp;
	unsigned long *pfn;

#ifdef DS_AMIGA
	if (ds_keep && keep > ds_keep)
		keep = ds_keep;
	/* a full session's frames come as it draws (ds_grow) */
	if (DS_FULL(s))
		keep = 0;
#endif
	n = (keep >> DS_PGSHIFT) + (keep < size);
	pfn = (unsigned long *)kmem_zalloc((size >> DS_PGSHIFT) * 4, KM_NOSLEEP);
	if (pfn == 0)
		return ENOMEM;
	if (availrmem - (int)n < tune.t_minarmem) {
		kmem_free((caddr_t)pfn, (size >> DS_PGSHIFT) * 4);
		return ENOMEM;
	}
	availrmem -= n;
	pages_pp_kernel += n;
	for (i = 0; i < n; i++) {
		if ((pp = page_get(DS_PGSIZE, wait ? P_CANWAIT : P_NOSLEEP)) == 0)
			break;
		pfn[i] = page_pptonum(pp);
	}
	for (j = 0; j < i; j++)
		if (pfn[j] >= DS_IDMAX >> DS_PGSHIFT)
			break;
	if (i < n || j < n) {
		ds_pgfree(pfn, i);
		availrmem += n - i;
		pages_pp_kernel -= n - i;
		kmem_free((caddr_t)pfn, (size >> DS_PGSHIFT) * 4);
		return ENOMEM;
	}
	for (i = n; i < size >> DS_PGSHIFT; i++)
		pfn[i] = pfn[n - 1];
	DS_CPUSHA();
	for (i = 0; i < n; i++)
		bzero((caddr_t)(pfn[i] << DS_PGSHIFT), DS_PGSIZE);
	ds_shfree(s);
	s->s_pfn = pfn;
	s->s_npg = n;
	s->s_size = size;
	s->s_kept = keep;
	return 0;
}

#ifdef DS_ATARI
/*
 * s's shadow keeps keep bytes, more than before, without moving what it
 * kept.  Only while no translation reaches the shadow.  0 or ENOMEM.
 */
static int
ds_shgrow(s, keep)
register struct dssess *s;
unsigned long keep;
{
	register unsigned long i, n;
	register page_t *pp;

	if (keep <= s->s_kept || s->s_pfn == 0)
		return 0;
	n = (keep >> DS_PGSHIFT) + (keep < s->s_size);
	if (availrmem - (int)(n - s->s_npg) < tune.t_minarmem)
		return ENOMEM;
	for (i = s->s_npg; i < n; i++) {
		if ((pp = page_get(DS_PGSIZE, P_NOSLEEP)) == 0 ||
		    page_pptonum(pp) >= DS_IDMAX >> DS_PGSHIFT) {
			if (pp && --pp->p_keepcnt == 0)
				page_abort(pp);
			return ENOMEM;
		}
		availrmem--;
		pages_pp_kernel++;
		s->s_pfn[i] = page_pptonum(pp);
		s->s_npg = i + 1;
		bzero((caddr_t)(s->s_pfn[i] << DS_PGSHIFT), DS_PGSIZE);
	}
	for (i = n; i < s->s_size >> DS_PGSHIFT; i++)
		s->s_pfn[i] = s->s_pfn[n - 1];
	s->s_kept = keep;
	return 0;
}

/* the page-rounded part of s's region that its Videl mode shows */
static unsigned long
ds_vneed(s)
register struct dssess *s;
{
	unsigned long n;

	if ((n = ata_vbytes(s->s_vid)) == 0)
		return s->s_size;
	n = (n + DS_PGSIZE + DS_PGOFF) & ~DS_PGOFF;
	return n < s->s_size ? n : s->s_size;
}
#endif

#ifdef DS_AMIGA
/*
 * s's shadow owns zeroed frames for its first size bytes, in place of
 * the shared one.  wait: may sleep.  Process context; errno.
 */
int
ds_grow(s, size, wait)
register struct dssess *s;
unsigned long size;
int wait;
{
	register unsigned long n = size >> DS_PGSHIFT;
	register page_t *pp;
	unsigned long pfn;

	if (n > s->s_size >> DS_PGSHIFT)
		n = s->s_size >> DS_PGSHIFT;
	while (s->s_pfn && s->s_npg < n) {
		/* without waiting, the pager's reserve stays */
		if (availrmem - 1 < tune.t_minarmem || (!wait && freemem <= minfree))
			return ENOMEM;
		availrmem--;
		pages_pp_kernel++;
		if ((pp = page_get(DS_PGSIZE, wait ? P_CANWAIT : P_NOSLEEP)) == 0) {
			availrmem++;
			pages_pp_kernel--;
			return ENOMEM;
		}
		pfn = page_pptonum(pp);
		/* asleep, s may have ended or grown */
		if (pfn >= DS_IDMAX >> DS_PGSHIFT || s->s_pfn == 0 || s->s_npg >= n) {
			ds_pgfree(&pfn, 1L);
			if (pfn >= DS_IDMAX >> DS_PGSHIFT)
				return ENOMEM;
			continue;
		}
		DS_CPUSHA();
		bzero((caddr_t)(pfn << DS_PGSHIFT), DS_PGSIZE);
		s->s_pfn[s->s_npg++] = pfn;
	}
	return s->s_pfn ? 0 : ENXIO;
}

static unsigned long ds_extent();

/* the frames a refused switch asked for, from its owner's poll */
void
ds_wgrow(s)
register struct dssess *s;
{
	if (s->s_wgrow) {
		s->s_wgrow = 0;
		(void)ds_grow(s, ds_extent(s), 1);
	}
}
#endif

static void
ds_shfree(s)
register struct dssess *s;
{
	if (s->s_pfn == 0)
		return;
	ds_pgfree(s->s_pfn, s->s_npg);
	kmem_free((caddr_t)s->s_pfn, (s->s_size >> DS_PGSHIFT) * 4);
	s->s_pfn = 0;
	s->s_npg = 0;
}

static void
ds_unshadow(s)
register struct dssess *s;
{
	ds_shfree(s);
#ifdef DS_ATARI
	if (s->s_vid) {
		kmem_free((caddr_t)s->s_vid, sizeof *s->s_vid);
	}
	s->s_vid = 0;
#endif
}

/* a new user session, hidden; 0 and *errp on failure */
struct dssess *
ds_newsess(uid, name, errp)
long uid;
char *name;
int *errp;
{
	register struct dssess *s = 0;
	register int i;

	for (i = 1; i <= DS_NSESS; i++)
		if (!ds_sess[i].s_used) {
			s = &ds_sess[i];
			break;
		}
	if (s == 0) {
		*errp = EAGAIN;
		return 0;
	}
	bzero((caddr_t)s, sizeof *s);
	s->s_used = 1;			/* claimed while the shadow is allocated */
	s->s_dead = 1;
	s->s_size = ds_disp.d_info.fi_size;
#ifdef DS_AMIGA
	s->s_voff = am_vslot();
	s->s_vpan = 0;
	/* the frame may hold an earlier session's buffers */
	if (DS_OWNV(s))
		ds_lzero((VOL unsigned long *)(ds_disp.d_page + s->s_voff), ds_disp.d_vsize);
#endif
	if (!DS_OWNV(s) && (i = ds_shadowk(s, s->s_size, s->s_size, 1)) != 0) {
		s->s_used = 0;
		*errp = i;
		return 0;
	}
	for (i = 0; i < 15 && name[i]; i++)
		s->s_name[i] = name[i];
	s->s_uid = uid;
	s->s_cache = ds_disp.d_dafb ? FBC_WT : FBC_CI;
	ds_stdcmap(s, (int)ds_disp.d_info.fi_depth);
	s->s_dlo = 256;
	s->s_dhi = 0;
	s->s_id = ds_nextid++;
	s->s_dead = 0;
	return s;
}

/* frees s once nothing refers to it */
void
ds_sessgc(s)
register struct dssess *s;
{
	register int i;

	if (s == &ds_sess[0] || s == ds_front || !s->s_dead || ds_segcount(s))
		return;
	for (i = 0; i < DS_NFBH; i++)
		if (ds_fbh[i].h_used && ds_fbh[i].h_sess == s)
			return;
	s->s_used = 0;
}

/* next live session of s's owner after s by creation order, else the console */
static struct dssess *
ds_after(s)
struct dssess *s;
{
	register struct dssess *n = 0, *lo = 0, *t;
	register int i;

	for (i = 1; i <= DS_NSESS; i++) {
		t = &ds_sess[i];
		if (!t->s_used || t->s_dead || t == s || t->s_uid != s->s_uid)
			continue;
		if (t->s_id > s->s_id && (n == 0 || t->s_id < n->s_id))
			n = t;
		if (lo == 0 || t->s_id < lo->s_id)
			lo = t;
	}
	return n ? n : lo ? lo : &ds_sess[0];
}

/*
 * End s: no further faults, input unbound, front goes on.  The slot is
 * reused when its mappings and file are gone.  Process context.
 */
void
ds_endsess(s)
register struct dssess *s;
{
	register int i;

	if (s == &ds_sess[0] || s->s_dead)
		return;
	s->s_dead = 1;
	ds_gen++;
	ds_unloadsess(s);
	if (ds_front == s && ds_switch(ds_after(s)) != 0)
		ds_pend = 0;			/* the tick retries */
	for (i = 0; i < DS_NEVH; i++)
		if (ds_evh[i].e_used && ds_evh[i].e_sess == s) {
			ds_evh[i].e_sess = 0;
			wakeup((caddr_t)&ds_evh[i]);
			pollwakeup(&ds_evh[i].e_ph, POLLHUP);
		}
	wakeup((caddr_t)s->s_note);
	pollwakeup(&s->s_ph, POLLIN | POLLHUP);
	if (ds_front != s)
		ds_unshadow(s);
	ds_sessgc(s);
}

/* ------------------------------------------------------------ switch */

static void
ds_lcopy(d, s, n)
register VOL unsigned long *d, *s;
register unsigned long n;
{
	for (n >>= 4; n > 0; n--) {
		*d++ = *s++;
		*d++ = *s++;
		*d++ = *s++;
		*d++ = *s++;
	}
}

/* bytes of s's shadow a switch moves: all kept, or what s can have drawn */
static unsigned long
ds_extent(s)
register struct dssess *s;
{
#ifdef DS_AMIGA
	struct fbinfo fi;
	register unsigned long n;

	if (DS_FULL(s)) {
		ds_sinfo(s, &fi);
		n = s->s_vpan + fi.fi_offset + fi.fi_rowbytes * fi.fi_height;
		if (n < s->s_ext)
			n = s->s_ext;
		n = (n + DS_PGOFF) & ~DS_PGOFF;
		if (s->s_trim && n > s->s_trim)
			n = s->s_trim;
		return n < s->s_size ? n : s->s_size;
	}
	/* the console draws its 8-bit frame */
	if (s == &ds_sess[0] && ds_keep)
		return ds_keep;
#endif
	return s->s_kept;
}

#ifdef DS_AMIGA
/* s's extent in VRAM without its zero pages at the end, at least a page */
static unsigned long
ds_used(s, vram)
register struct dssess *s;
caddr_t vram;
{
	register unsigned long n;
	register VOL unsigned long *p, *e;

	s->s_trim = 0;
	for (n = ds_extent(s); n > DS_PGSIZE; n -= DS_PGSIZE) {
		e = (VOL unsigned long *)(vram + n);
		for (p = e - DS_PGSIZE / 4; p < e; p++)
			if (*p)
				return n;
	}
	return DS_PGSIZE;
}

static void
ds_lzero(d, n)
register VOL unsigned long *d;
register unsigned long n;
{
	for (n >>= 4; n > 0; n--) {
		*d++ = 0;
		*d++ = 0;
		*d++ = 0;
		*d++ = 0;
	}
}
#endif

/* VRAM to s's shadow, or the shadow to VRAM (out), a page at a time */
static void
ds_shcopy(s, vram, out)
register struct dssess *s;
caddr_t vram;
int out;
{
	register unsigned long i, *p, *v;

	for (i = 0; i < ds_extent(s) >> DS_PGSHIFT; i++) {
		p = (unsigned long *)(s->s_pfn[i] << DS_PGSHIFT);
		v = (unsigned long *)(vram + (i << DS_PGSHIFT));
#ifdef DS_AMIGA
		/* past its frames a full session has drawn nothing */
		if (DS_FULL(s) && i >= s->s_npg) {
			if (out)
				ds_lzero(v, (unsigned long)DS_PGSIZE);
			continue;
		}
#endif
		if (out)
			ds_lcopy(v, p, (unsigned long)DS_PGSIZE);
		else
			ds_lcopy(p, v, (unsigned long)DS_PGSIZE);
	}
}

/* up events for everything s saw go down */
static void
ds_release(s)
register struct dssess *s;
{
	register int c, x;
	long sec, us;

	if (s->s_dead)
		return;
	ds_now(&sec, &us);
	for (c = 0; c < 128; c++) {
		x = DS_SPL(DS_HI);
		if (BIT(s->s_keys, c)) {
			CLRB(s->s_keys, c);
			if (s == &ds_sess[0])
				adbkbd_cons(c | 0x80);
			else
				ds_evpost(s, 0, IE_KEY, c, 0L, sec, us);
		}
		DS_SPLX(x);
	}
	x = DS_SPL(DS_HI);
	if (s->s_btn) {
#ifdef DS_ATARI
		if (s->s_btn & 2)
			ds_evpost(s, 1, IE_BTN, 3, 0L, sec, us);
		if (s->s_btn & 1)
			ds_evpost(s, 1, IE_BTN, 1, 0L, sec, us);
		s->s_btn = 0;
#else
		s->s_btn = 0;
		ds_evpost(s, 1, IE_BTN, 1, 0L, sec, us);
#endif
		ds_evpost(s, 1, IE_SYN, 0, 0L, sec, us);
	}
	DS_SPLX(x);
}

/*
 * Bring `to' in front.  Safe context only (see the head of the file).
 * VRAM goes to the old session's shadow, the new one's shadow (zeros
 * until it draws) to VRAM, except for sessions with VRAM of their own;
 * the console keeps text and repaints.  The CLUT is loaded at once.
 */
int
ds_switch(to)
register struct dssess *to;
{
	register struct dssess *from = ds_front;
	register caddr_t vram = (caddr_t)ds_disp.d_page;
	register int x;

	if (to == from)
		return 0;
	if (!ds_disp.d_on || !to->s_used || to->s_dead)
		return EINVAL;
	if (!fbcons_grab())
		return EBUSY;
	if (ds_leavefn && (*ds_leavefn)(from, 0) != 0) {
		fbcons_unlock();
		return EBUSY;
	}
#ifdef DS_AMIGA
	/* the revert sets its mode under its own hold of the screen */
	if (ds_trial == from) {
		fbcons_unlock();
		if (ds_tdone(0) != 0 || !fbcons_grab()) {
			if (ds_leavefn)
				(void)(*ds_leavefn)(from, 1);
			return EBUSY;
		}
	}
#endif
#ifdef DS_AMIGA
	/* zeros at the end need no frames */
	if (!from->s_dead && DS_FULL(from) && !DS_OWNV(from))
		from->s_trim = ds_used(from, vram);
	if (!from->s_dead && DS_FULL(from) && !DS_OWNV(from) &&
	    ds_grow(from, ds_extent(from), 0) != 0 && ds_nomem < DS_NOMEMTRY) {
		/* its owner's poll takes the frames, waiting for the pager */
		ds_nomem++;
		from->s_wgrow = 1;
		wakeup((caddr_t)from->s_note);
		pollwakeup(&from->s_ph, POLLIN | POLLRDNORM);
		if (ds_leavefn)
			(void)(*ds_leavefn)(from, 1);
		fbcons_unlock();
		return ENOMEM;
	}
	ds_nomem = 0;
#endif
	ds_gen++;
	ds_unloadsess(from);
	ds_unloadsess(to);
	DS_CPUSHA();
#ifdef DS_ATARI
	/* the shadow grows to the mode the session left with */
	if (from->s_vid && !from->s_dead)
		(void)ds_shgrow(from, ds_vneed(from));
#endif
	if (from == &ds_sess[0])
		fbcons_hide();
	else if (!from->s_dead && !DS_OWNV(from))
		ds_shcopy(from, vram, 0);
#ifdef DS_AMIGA
	if (!DS_OWNV(from) && ds_extent(from) > ds_vdirty)
		ds_vdirty = ds_extent(from);
#endif
#ifdef DS_ATARI
	/* the Videl's mode and palette go with the session that set them */
	if (!from->s_vid && to->s_vid)
		ata_vsave(&ds_vcons);
	if (to->s_vid)
		ata_vload(to->s_vid);
	else if (from->s_vid)
		ata_vload(&ds_vcons);
#endif
#ifdef DS_AMIGA
	/* the card's byte order for to's depth before its pixels go in */
	if (am_dsvram && !DS_OWNV(to))
		am_planes(to);
#endif
	if (to == &ds_sess[0])
		fbcons_show();
	else if (!DS_OWNV(to))
		ds_shcopy(to, vram, 1);
#ifdef DS_AMIGA
	/* in front its frames are not needed: the pager gets them back */
	if (DS_FULL(to) && !DS_OWNV(to) && to->s_pfn && to->s_npg > 1) {
		ds_pgfree(to->s_pfn + 1, to->s_npg - 1);
		for (x = 1; x < to->s_npg; x++)
			to->s_pfn[x] = to->s_pfn[0];
		to->s_npg = 1;
	}
#endif
#ifdef DS_AMIGA
	/* what s has not drawn reads as zeros, not another session's pixels */
	if (DS_FULL(to) && !DS_OWNV(to) && ds_vdirty > ds_extent(to)) {
		ds_lzero((VOL unsigned long *)(vram + ds_extent(to)),
		    (ds_vdirty < to->s_size ? ds_vdirty : to->s_size) - ds_extent(to));
		ds_vdirty = ds_extent(to);
	}
	to->s_trim = 0;
#endif
	DS_CPUSHA();
	x = DS_SPL(DS_HI);
	ds_front = to;
	ds_serial++;
	if (ds_frontfn)
		(*ds_frontfn)(to);
	to->s_blank = 0;		/* the switch key counts as input */
	if (HWCLUT)
		ds_clutload(to, 0, ds_ncmap(to));
	to->s_dlo = 256;
	to->s_dhi = 0;
	DS_SPLX(x);
	if (ds_leavefn)
		(void)(*ds_leavefn)(from, 1);
	ds_release(from);
	ds_note(from, FBN_HIDDEN);
	ds_note(to, FBN_SHOWN);
	fbcons_unlock();
#ifdef DS_AMIGA
	if (to->s_vmode && !ds_trial && !ds_tried(to->s_vmode - 1))
		ds_tstart(to);
#endif
	if (from->s_dead) {
		ds_unshadow(from);
		ds_sessgc(from);
	}
	return 0;
}

/*
 * Hotkey switches, from queuerun.  Runs at IPL 0 so the clock and ADB
 * are served during the copy.
 */
static int
ds_srv(q)
queue_t *q;
{
	register int d, sr, x, e = 0;
	register struct dssess *s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (sr));
	DS_SPLX(sr & ~0x700);
#ifdef DS_AMIGA
	if (ds_tpend && fbcons_grab()) {
		/* not while another switch or mode set holds the screen */
		fbcons_unlock();
		ds_tdone(ds_tpend == 2);
	}
	/* a guest that redraws may have wiped the prompt */
	if (ds_tredraw && ds_trial == ds_front && fbcons_grab()) {
		ds_tredraw = 0;
		if (ds_trial == ds_front && !ds_tpend)
			ds_tprompt(ds_trial, 1);
		fbcons_unlock();
	}
#endif
	x = DS_SPL(DS_HI);
	d = ds_pend;
	ds_pend = -1;
	DS_SPLX(x);
	if (d >= 0) {
		s = d == 0 ? &ds_sess[0] : ds_nth(d);
		if (ds_front->s_dead && s == 0)
			s = ds_after(ds_front);
		/* the shadow takes the pages the pager frees meanwhile */
		if (s && (e = ds_switch(s)) == ENOMEM)
			ds_pwait = lbolt + HZ / 4;
		if (s && (e == EBUSY || e == ENOMEM) && ds_pend < 0)
			ds_pend = d;
	}
	DS_SPLX(sr);
	return 0;
}

/* panic: the console takes VRAM back as it is, with its own CLUT */
static void
ds_panic()
{
	if (!ds_disp.d_on || ds_front == &ds_sess[0])
		return;
#ifdef DS_ATARI
	if (ds_front->s_vid)
		ata_vload(&ds_vcons);
#endif
#ifdef DS_AMIGA
	ds_trial = 0;
	am_rtgpanic();
#endif
	ds_front = &ds_sess[0];
	fbcons_show();
	if (HWCLUT)
		ds_clutload(&ds_sess[0], 0, (int)ds_disp.d_info.fi_cmapsize);
}

/* ------------------------------------------------------------- input */

#ifdef DS_ATARI
/* Control-Alternate; Alternate arrives as Command */
static int
ds_hotmods()
{
	return BIT(ds_phys, K_CTL) && BIT(ds_phys, K_CMD);
}
#else
static int
ds_hotmods()
{
	return (BIT(ds_phys, K_CTL) || BIT(ds_phys, K_RCTL)) &&
	    (BIT(ds_phys, K_OPT) || BIT(ds_phys, K_ROPT)) && BIT(ds_phys, K_CMD);
}
#endif

/* ADB key consumer: (unit, KC_CHAR, code | up << 7, more), IPL 1 */
static void
ds_key(unit, kind, b, more)
int unit, kind, b, more;
{
	register struct dssess *s;
	register int c = b & 0x7F, up = b & 0x80, d;
	long sec, us;

	if (up)
		CLRB(ds_phys, c);
	else
		SETB(ds_phys, c);
	if (up && BIT(ds_eat, c)) {
		CLRB(ds_eat, c);
		return;
	}
	if (!up && ds_hotmods()) {
		for (d = 0; d < 10 && ds_digit[d] != c; d++)
			;
		if (c == K_ESC)
			d = 0;
		if (d < 10) {
			SETB(ds_eat, c);
			ds_pend = d;
			qenable(&ds_q);
			return;
		}
	}
#ifdef DS_AMIGA
	if (!up && c == K_RET && ds_trial && ds_trial == ds_front) {
		SETB(ds_eat, c);
		ds_tpend = 2;
		qenable(&ds_q);
		return;
	}
#endif
	s = ds_front;
	if (up) {
		if (!BIT(s->s_keys, c))
			return;
		CLRB(s->s_keys, c);
	} else
		SETB(s->s_keys, c);
	if (s == &ds_sess[0]) {
		adbkbd_cons(b);
		return;
	}
	ds_now(&sec, &us);
	ds_evpost(s, 0, IE_KEY, c, up ? 0L : 1L, sec, us);
}

static long
ds_sext7(v)
long v;
{
	v &= 0x7F;
	return (v & 0x40) ? v - 0x80 : v;
}

/* ADB mouse consumer: (unit, MOUSE_CHANGE, talk-R0, changed), IPL 1 */
static void
ds_mouse(unit, kind, r0, changed)
int unit, kind, r0, changed;
{
	register struct dssess *s = ds_front;
	register long dx, dy, btn;
	long sec, us;

	if (s == &ds_sess[0])
		return;
	ds_now(&sec, &us);
	btn = (r0 & 0x8000) ? 0 : 1;
	dy = ds_sext7((long)(r0 >> 8));
	dx = ds_sext7((long)r0);
	if (changed & 1) {
		if (btn && !s->s_btn) {
			s->s_btn = 1;
			ds_evpost(s, 1, IE_BTN, 1, 1L, sec, us);
		} else if (!btn && s->s_btn) {
			s->s_btn = 0;
			ds_evpost(s, 1, IE_BTN, 1, 0L, sec, us);
		}
	}
	if (changed & 2) {
		if (dx)
			ds_evpost(s, 1, IE_REL, IE_RELX, dx, sec, us);
		if (dy)
			ds_evpost(s, 1, IE_REL, IE_RELY, dy, sec, us);
	}
	ds_evpost(s, 1, IE_SYN, 0, 0L, sec, us);
}

#ifdef DS_ATARI
/* relative mouse packet: buttons (bit 0 left, bit 1 right), motion */
void
ds_relmouse(btn, dx, dy)
int btn, dx, dy;
{
	register struct dssess *s = ds_front;
	register int b, c;
	long sec, us;

	if (s == &ds_sess[0])
		return;
	ds_now(&sec, &us);
	for (b = 1; b <= 2; b++) {
		c = b == 1 ? 1 : 3;
		if ((btn & b) && !(s->s_btn & b)) {
			s->s_btn |= b;
			ds_evpost(s, 1, IE_BTN, c, 1L, sec, us);
		} else if (!(btn & b) && (s->s_btn & b)) {
			s->s_btn &= ~b;
			ds_evpost(s, 1, IE_BTN, c, 0L, sec, us);
		}
	}
	if (dx)
		ds_evpost(s, 1, IE_REL, IE_RELX, (long)dx, sec, us);
	if (dy)
		ds_evpost(s, 1, IE_REL, IE_RELY, (long)dy, sec, us);
	ds_evpost(s, 1, IE_SYN, 0, 0L, sec, us);
}
#endif

/* ------------------------------------------------------------- init */

/*
 * First open: take the screen fbcons found as display 0.  Process
 * context.
 */
int
ds_init()
{
	register struct fbinfo *fi = &ds_disp.d_info;
	register struct fbmode *m = &fbcons.fc_m;
	register unsigned long end, d;
	register int i;

	if (ds_disp.d_on)
		return 0;
#ifdef DS_AMIGA
	if ((i = am_dsinit()) != 0)
		return i;
#endif
	if (!fbcons.fc_on)
		return ENXIO;
#ifndef DS_AMIGA
	/* the console's text, to repaint after another session */
	if (fbcons.fc_text == 0) {
		register unsigned short *t;
		register long n = (long)fbcons.fc_cols * fbcons.fc_rows * 2;

		t = (unsigned short *)kmem_alloc(n, KM_SLEEP);
		if (fbcons.fc_text == 0 && fbcons_grab()) {
			fbcons_text(t);
			fbcons_unlock();
		} else
			kmem_free((caddr_t)t, n);
		if (ds_disp.d_on)	/* another open finished while this one slept */
			return 0;
		if (fbcons.fc_text == 0)
			return EBUSY;
	}
#endif
	bzero((caddr_t)fi, sizeof *fi);
	d = m->fm_depth;
	ds_disp.d_base = m->fm_base;
	ds_disp.d_page = m->fm_base & ~DS_PGOFF;
#ifdef DS_AMIGA
	ds_disp.d_dafb = 0;
	fi->fi_type = FBT_AMIGA;
	fi->fi_layout = FBL_PACKED;
#elif defined(DS_ATARI)
	ds_disp.d_dafb = 0;
	fi->fi_type = DS_SV ? FBT_SVIDEL : ata_tt ? FBT_TTSHIFT : FBT_VIDEL;
	fi->fi_layout = d == 1 || d > 8 || DS_SV ? FBL_PACKED : FBL_IPLAN2;
#else
	ds_disp.d_dafb = m->fm_base >= DAFB_VRAM && m->fm_base < DAFB_REG;
	fi->fi_type = ds_disp.d_dafb ? FBT_DAFB : FBT_NUBUS;
	fi->fi_layout = FBL_PACKED;
#endif
	fi->fi_width = m->fm_width;
	fi->fi_height = m->fm_height;
	fi->fi_depth = d;
	fi->fi_rowbytes = m->fm_row;
	fi->fi_offset = m->fm_base - ds_disp.d_page;
	fi->fi_size = (fi->fi_offset + m->fm_row * m->fm_height + DS_PGOFF) & ~DS_PGOFF;
	ds_disp.d_vsize = fi->fi_size;
#ifdef DS_AMIGA
	/* room for the deepest mode a session may take */
	if (am_dsvsize > ds_disp.d_vsize)
		ds_disp.d_vsize = am_dsvsize;
	/* a card with deeper modes: every session maps room for the deepest */
	if (am_dsfull) {
		ds_keep = fi->fi_size;
		fi->fi_size = ds_disp.d_vsize = (ds_disp.d_vsize + DS_PGOFF) & ~DS_PGOFF;
	}
#endif
#ifdef DS_ATARI
	/* the console's screen opens the pool, which ends at the top of ST-RAM */
	if (ds_disp.d_page >= ata_pool && ds_disp.d_page - ata_pool < ata_poolsz &&
	    ata_pool + ata_poolsz - ds_disp.d_page > fi->fi_size)
		ds_disp.d_vsize = ata_pool + ata_poolsz - ds_disp.d_page;
#endif
	end = ds_disp.d_dafb ? DAFB_REG : (ds_disp.d_page & 0xFF000000) + 0x01000000;
#ifdef ATA_SV
	if (DS_SV)
		end = 0xA8000000;	/* the SuperVidel's RAM */
#endif
	if (fi->fi_size > end - ds_disp.d_page)
		return ENXIO;
	if (d == 1)
		fi->fi_visual = FBV_MONO;
	else if (d <= 8)
		fi->fi_visual = FBV_PSEUDO;
	else {
		fi->fi_visual = FBV_TRUE;
#ifdef DS_ATARI
		if (d == 16) {
			fi->fi_rmask = 0xF800;
			fi->fi_gmask = 0x07E0;
			fi->fi_bmask = 0x001F;
		} else
#endif
		if (d == 16) {
			fi->fi_rmask = 0x7C00;
			fi->fi_gmask = 0x03E0;
			fi->fi_bmask = 0x001F;
		} else {
			fi->fi_rmask = 0xFF0000;
			fi->fi_gmask = 0x00FF00;
			fi->fi_bmask = 0x0000FF;
		}
	}
	fi->fi_cmapsize = d <= 8 ? 1 << d : 0;
	fi->fi_cmapbits = 8;
	fi->fi_mode = (fbp_cur >= 0 && fbp_cur < fbp_nmode) ? fbp_mode[fbp_cur].pm_id : 0x80;
#ifdef DS_AMIGA
	fi->fi_cmapbits = am_dscmapbits;
	fi->fi_flags = FBF_BLANK | FBF_CMAP;
	for (i = 0; am_dsname[i] && i < sizeof fi->fi_name - 1; i++)
		fi->fi_name[i] = am_dsname[i];
#elif defined(DS_ATARI)
	fi->fi_cmapbits = DS_SV ? 8 : ata_tt ? 4 : 6;
	fi->fi_flags = d <= 8 ? FBF_BLANK | FBF_CMAP : 0;
#ifdef ATA_SV
	if (DS_SV)
		for (i = 0; "SuperVidel"[i]; i++)
			fi->fi_name[i] = "SuperVidel"[i];
	else
#endif
	if (ata_tt)
		for (i = 0; "TT Shifter"[i]; i++)
			fi->fi_name[i] = "TT Shifter"[i];
	else
	for (i = 0; "Videl"[i]; i++)
		fi->fi_name[i] = "Videl"[i];
#else
	fi->fi_flags = ds_disp.d_dafb ? FBF_BLANK | FBF_CMAP : 0;	/* FBF_VBL once seen */
	for (i = 0; "DAFB"[i]; i++)
		fi->fi_name[i] = ds_disp.d_dafb ? "DAFB"[i] : "slot"[i];
#endif

	ds_sess[0].s_used = 1;
	ds_sess[0].s_size = fi->fi_size;
	ds_sess[0].s_cache = FBC_CI;
	for (i = 0; "console"[i]; i++)
		ds_sess[0].s_name[i] = "console"[i];
	ds_stdcmap(&ds_sess[0], (int)d);
	ds_sess[0].s_dlo = 256;
	ds_sess[0].s_dhi = 0;
	ds_front = &ds_sess[0];

	ds_qi.qi_srvp = ds_srv;
	ds_q.q_qinfo = &ds_qi;
#ifdef DS_ATARI
	ds_clutload(&ds_sess[0], 0, (int)fi->fi_cmapsize);
#ifdef ATA060
	/* user pages noncacheable but not serialised, so writes can be buffered */
	if (DS_SV)
		(void)hat_cm_fb_add(ds_disp.d_page >> DS_PGSHIFT,
		    (ds_disp.d_page + fi->fi_size + DS_PGOFF) >> DS_PGSHIFT);
#endif
#else
#ifdef DS_AMIGA
	/* 68060: user pages noncacheable but not serialised, so writes can be buffered */
	if (cputype == 60)
		(void)hat_cm_fb_add(ds_disp.d_page >> DS_PGSHIFT,
		    (ds_disp.d_page + fi->fi_size + DS_PGOFF) >> DS_PGSHIFT);
#endif
	if (ds_disp.d_dafb) {
		(void)hat_cm_fb_add(ds_disp.d_page >> DS_PGSHIFT,
		    (ds_disp.d_page + fi->fi_size) >> DS_PGSHIFT);
		*(VOL unsigned long *)(DAFB_REG + DAFB_INTCLEAR) = 0;
#ifdef BOOTDIAG
		{
			extern int diag_opts;

			if (diag_opts & 0x10) {	/* novbl: software VBL only */
				*(VOL unsigned long *)(DAFB_REG + DAFB_INTMASK) = 0;
				goto novbl;
			}
		}
#endif
		*(VOL unsigned long *)(DAFB_REG + DAFB_INTMASK) = DAFB_VBL;
		/* VIA2 CA1: slot interrupts, falling edge */
		i = DS_SPL(7);
		*(VOL unsigned char *)(VIA2 + VIA_PCR) &= ~1;
		*(VOL unsigned char *)(VIA2 + VIA_IER) = 0x82;
		DS_SPLX(i);
#ifdef BOOTDIAG
	novbl:	;
#endif
	}
#endif
	ds_hwlast = lbolt;
	fbcons_panicfn = ds_panic;
	(void)adb_keyhook(ds_key);
	(void)adb_mousehook(ds_mouse);
	ds_disp.d_on = 1;
	timeout(ds_tick, (caddr_t)0, 1);
	printf("ds: %s %dx%dx%d at 0x%x, row %d, map 0x%x\n", fi->fi_name,
	    (int)fi->fi_width, (int)fi->fi_height, (int)d, (int)ds_disp.d_base,
	    (int)fi->fi_rowbytes, (int)fi->fi_size);
	return 0;
}

#ifdef DS_ATARI
/*
 * s takes session mode id and with it the Videl and the whole pool.
 * The first mode is set before s is mapped.  Process context.
 */
int
ds_setmode(s, id)
register struct dssess *s;
unsigned long id;
{
	struct fbinfo fi;
	struct dsvid *v;
	register int n;

	if ((n = ata_vmfind(id)) < 0)
		return ENXIO;
	ata_vminfo(n, &fi);
	if (fi.fi_rowbytes * fi.fi_height > ds_disp.d_vsize)
		return ENXIO;		/* the Videl would scan past the pool */
	if (!fbcons_grab())
		return EBUSY;
	if (s->s_vmode == 0) {
		if (ds_segcount(s)) {
			fbcons_unlock();
			return EBUSY;
		}
		v = (struct dsvid *)kmem_zalloc(sizeof *v, KM_NOSLEEP);
		if (v == 0 || ds_shadowk(s, ds_disp.d_vsize,
		    (fi.fi_rowbytes * fi.fi_height + DS_PGOFF) & ~DS_PGOFF, 0) != 0) {
			if (v)
				kmem_free((caddr_t)v, sizeof *v);
			fbcons_unlock();
			return ENOMEM;
		}
		s->s_vmode = n + 1;
		s->s_vid = v;
		if (s == ds_front)	/* the console's mode is on the Videl */
			ata_vsave(&ds_vcons);
	}
	s->s_vmode = n + 1;
	ata_vmset(s->s_vid, n, ds_disp.d_page);
	ds_stdcmap(s, (int)fi.fi_depth);
	if (s != ds_front) {
		ds_unloadsess(s);
		(void)ds_shgrow(s, ds_vneed(s));
	}
	if (s == ds_front) {
		ata_vload(s->s_vid);
		ds_clutload(s, 0, ds_ncmap(s));
	}
	fbcons_unlock();
	return 0;
}
#endif

struct dsfbh ds_fbh[DS_NFBH];
struct dsevh ds_evh[DS_NEVH];
int (*ds_evpost_hook)();
int (*ds_unloadsess_hook)();
int (*ds_segcount_hook)();

void
ds_evpost(s, mouse, type, code, value, sec, usec)
struct dssess *s;
int mouse, type, code;
long value, sec, usec;
{
	if (ds_evpost_hook)
		(*ds_evpost_hook)(s, mouse, type, code, value, sec, usec);
}

void
ds_unloadsess(s)
struct dssess *s;
{
	if (ds_unloadsess_hook)
		(*ds_unloadsess_hook)(s);
}

int
ds_segcount(s)
struct dssess *s;
{
	return ds_segcount_hook ? (*ds_segcount_hook)(s) : 0;
}

void
ds_modinit()
{
	extern int dlm_creg();
	int i, e;

	for (i = 51; i <= 53; i++)
		if ((e = dlm_creg("dsuser", i)) != 0)
			printf("display: module registration %d: %d\n", i, e);
}
int
ds_owns(s)
struct dssess *s;
{
	register struct cred *cr = curproc->p_cred;

	return s->s_uid == cr->cr_uid || drv_priv(cr) == 0;
}

int
ds_mayfront(cr)
struct cred *cr;
{
	dev_t d;

	if (drv_priv(cr) == 0)
		return 1;
	if (ds_front != &ds_sess[0] && ds_front->s_uid == cr->cr_uid)
		return 1;
	d = cttydev(curproc);
	return d != NODEV && getmajor(d) < cdevcnt &&
	    cdevsw[getmajor(d)].d_str == &coinfo && getminor(d) == 0;
}
