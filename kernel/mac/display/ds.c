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
#include "sys/time.h"
#ifdef DS_ATARI
#include "sys/cred.h"
#include "sys/proc.h"
#include "sys/disp.h"
#include "sys/signal.h"
#endif
#include "fbcons.h"
#include "adb.h"
#include "ds.h"

extern timestruc_t hrestime;
extern long lbolt;
extern int hat_cm_fb_add();
extern void adbkbd_cons();
extern void (*fbcons_panicfn)();
extern int fbcons_grab();
extern struct fbpmode fbp_mode[];
#ifdef DS_ATARI
extern void ata_dspoll(), ata_vsave(), ata_vload(), ata_vput(), ata_bsave(), ata_bload();
extern int ata_vget();
extern int ata_vnative(), ata_brun();
extern unsigned long ata_pool;
static struct dsvid ds_vcons;		/* the mode under a Videl session */
#endif
extern int fbp_nmode, fbp_cur;

struct dsdisp ds_disp;
struct dssess ds_sess[1 + DS_NSESS];
struct dssess *ds_front = &ds_sess[0];
unsigned long ds_gen, ds_serial, ds_vblcount;
unsigned long ds_nhwvbl, ds_nswvbl;	/* VBLs from DAFB, from the tick */

static long ds_nextid = 1;
static int ds_pend = -1;		/* hotkey digit to act on, -1 none */
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
#define K_ROPT		0x7C
#define K_RCTL		0x7D

/* ADB key code of the digits 0-9 */
static unsigned char ds_digit[10] = {
	0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19
};

/* ------------------------------------------------------------ CLUT */

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

#ifdef DS_ATARI
#define HWCLUT	1
#define FAL_PAL(i)	(*(VOL unsigned long *)(0xFFFF9800 + 4 * (i)))
#define ST_PAL(i)	(*(VOL unsigned short *)(0xFFFF8240 + 2 * (i)))

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
	s->s_dhi = ds_disp.d_info.fi_cmapsize;
	DS_SPLX(x);
}

/* ------------------------------------------------------------- VBL */

static void
ds_vbl()
{
	register struct dssess *s = ds_front;

	ds_vblcount++;
#ifdef DS_ATARI
	if (s->s_vid)
		s->s_dlo = 256, s->s_dhi = 0;
#endif
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
	*(VOL unsigned long *)(DAFB_REG + DAFB_INTCLEAR) = 0;
	if (!ds_disp.d_on)
		return;
	ds_nhwvbl++;
	ds_hwlast = lbolt;
	ds_disp.d_info.fi_flags |= FBF_VBL;
	ds_vbl();
}

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
	if (ds_pend >= 0)
		qenable(&ds_q);
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
#ifdef DS_ATARI
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

static caddr_t
ds_shadow(s)
register struct dssess *s;
{
	register unsigned long size = s->s_size, i, pa;

	s->s_memsize = size + DS_PGSIZE;
	s->s_mem = (caddr_t)kmem_zalloc(s->s_memsize, KM_NOSLEEP);
	if (s->s_mem == 0)
		return 0;
	s->s_pfn = (unsigned long *)kmem_zalloc((size >> DS_PGSHIFT) * 4, KM_NOSLEEP);
	if (s->s_pfn == 0) {
		kmem_free(s->s_mem, s->s_memsize);
		s->s_mem = 0;
		return 0;
	}
	s->s_shadow = (caddr_t)(((unsigned long)s->s_mem + DS_PGOFF) & ~DS_PGOFF);
	for (i = 0; i < size >> DS_PGSHIFT; i++) {
		pa = vtop(s->s_shadow + (i << DS_PGSHIFT), (caddr_t)0);
		if (pa == 0) {
			kmem_free((caddr_t)s->s_pfn, (size >> DS_PGSHIFT) * 4);
			kmem_free(s->s_mem, s->s_memsize);
			s->s_mem = 0;
			return 0;
		}
		s->s_pfn[i] = pa >> DS_PGSHIFT;
	}
	DS_CPUSHA();
	return s->s_shadow;
}

static void
ds_unshadow(s)
register struct dssess *s;
{
	if (s->s_mem == 0)
		return;
	kmem_free((caddr_t)s->s_pfn, (s->s_size >> DS_PGSHIFT) * 4);
	kmem_free(s->s_mem, s->s_memsize);
	s->s_mem = s->s_shadow = 0;
	s->s_pfn = 0;
#ifdef DS_ATARI
	if (s->s_vid)
		kmem_free((caddr_t)s->s_vid, sizeof *s->s_vid);
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
	return ds_mksess(uid, name, errp, 0);
}

/* as ds_newsess; vid: the session owns the Videl (FBA_VIDEL) */
struct dssess *
ds_mksess(uid, name, errp, vid)
long uid;
char *name;
int *errp, vid;
{
	register struct dssess *s = 0;
	register int i;

	if (ds_sess[0].s_mem == 0 && ds_shadow(&ds_sess[0]) == 0) {
		*errp = ENOMEM;
		return 0;
	}
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
#ifdef DS_ATARI
	if (vid) {
		for (i = 1; i <= DS_NSESS; i++)
			if (ds_sess[i].s_used && !ds_sess[i].s_dead && ds_sess[i].s_vid) {
				s->s_used = 0;
				*errp = EBUSY;
				return 0;
			}
		s->s_size = ds_disp.d_vsize;
		s->s_vid = (struct dsvid *)kmem_zalloc(sizeof *s->s_vid, KM_NOSLEEP);
	}
	if (vid && s->s_vid == 0) {
		s->s_used = 0;
		*errp = ENOMEM;
		return 0;
	}
#else
	if (vid) {
		s->s_used = 0;
		*errp = EINVAL;
		return 0;
	}
#endif
	if (ds_shadow(s) == 0) {
#ifdef DS_ATARI
		if (s->s_vid)
			kmem_free((caddr_t)s->s_vid, sizeof *s->s_vid);
		s->s_vid = 0;
#endif
		s->s_used = 0;
		*errp = ENOMEM;
		return 0;
	}
#ifdef DS_ATARI
	if (s->s_vid)
		ata_vsave(s->s_vid);	/* starts in the console's mode */
#endif
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
#ifdef DS_ATARI
			else if (s->s_vid)
				ds_evpost(s, 0, IE_KEY, ata_vnative(c), 0L, sec, us);
#endif
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
 * until it draws) to VRAM; its CLUT is loaded at once.
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
	if (!ds_disp.d_on || !to->s_used || to->s_dead || ds_sess[0].s_mem == 0)
		return EINVAL;
	if (!fbcons_grab())
		return EBUSY;
	ds_gen++;
	ds_unloadsess(from);
	ds_unloadsess(to);
	DS_CPUSHA();
	if (!from->s_dead)
		ds_lcopy((unsigned long *)from->s_shadow, (unsigned long *)vram, from->s_size);
	if (from == &ds_sess[0])
		fbcons.fc_m.fm_base = (unsigned long)ds_sess[0].s_shadow +
		    ds_disp.d_info.fi_offset;
#ifdef DS_ATARI
	/* the Videl's mode and palette go with the session that owns it */
	/* and so does the blitter, idle between a guest's runs */
	/* a guest's mode is what it wrote, which the Videl may read back otherwise */
	if (from->s_vid && !from->s_dead)
		ata_bsave(from->s_vid->v_blt);
	else if (!from->s_vid && to->s_vid) {
		ata_vsave(&ds_vcons);
		ata_bsave(ds_vcons.v_blt);
	}
	if (to->s_vid) {
		ata_vload(to->s_vid);
		ata_bload(to->s_vid->v_blt);
	} else if (from->s_vid) {
		ata_vload(&ds_vcons);
		ata_bload(ds_vcons.v_blt);
	}
#endif
	ds_lcopy((unsigned long *)vram, (unsigned long *)to->s_shadow, to->s_size);
	if (to == &ds_sess[0])
		fbcons.fc_m.fm_base = ds_disp.d_base;
	DS_CPUSHA();
	x = DS_SPL(DS_HI);
	ds_front = to;
	ds_serial++;
#ifdef DS_ATARI
	wakeup((caddr_t)&ds_front);	/* blits waiting for the front */
#endif
#ifdef DS_ATARI
	if (!to->s_vid)
#endif
	if (HWCLUT)
		ds_clutload(to, 0, (int)ds_disp.d_info.fi_cmapsize);
	to->s_dlo = 256;
	to->s_dhi = 0;
	DS_SPLX(x);
	ds_release(from);
	ds_note(from, FBN_HIDDEN);
	ds_note(to, FBN_SHOWN);
	fbcons_unlock();
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
	register int d, sr, x;
	register struct dssess *s;

	__asm__ __volatile__("mov.w %%sr,%0" : "=d" (sr));
	DS_SPLX(sr & ~0x700);
	x = DS_SPL(DS_HI);
	d = ds_pend;
	ds_pend = -1;
	DS_SPLX(x);
	if (d >= 0) {
		s = d == 0 ? &ds_sess[0] : ds_nth(d);
		if (ds_front->s_dead && s == 0)
			s = ds_after(ds_front);
		if (s && ds_switch(s) == EBUSY && ds_pend < 0)
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
	ds_front = &ds_sess[0];
	fbcons.fc_m.fm_base = ds_disp.d_base;
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
#ifdef DS_ATARI
	if (s->s_vid)
		ds_evpost(s, 0, IE_KEY, ata_vnative(c), up ? 0L : 1L, sec, us);
	else
#endif
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
	if (!fbcons.fc_on)
		return ENXIO;
	bzero((caddr_t)fi, sizeof *fi);
	d = m->fm_depth;
	ds_disp.d_base = m->fm_base;
	ds_disp.d_page = m->fm_base & ~DS_PGOFF;
#ifdef DS_ATARI
	ds_disp.d_dafb = 0;
	fi->fi_type = FBT_VIDEL;
	fi->fi_layout = d == 1 || d > 8 ? FBL_PACKED : FBL_IPLAN2;
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
#ifdef DS_ATARI
	/* the console's screen opens the pool, which ends at the top of ST-RAM */
	if (ds_disp.d_page >= ata_pool && ds_disp.d_page - ata_pool < DS_VPOOL &&
	    ata_pool + DS_VPOOL - ds_disp.d_page > fi->fi_size)
		ds_disp.d_vsize = ata_pool + DS_VPOOL - ds_disp.d_page;
#endif
	end = ds_disp.d_dafb ? DAFB_REG : (ds_disp.d_page & 0xFF000000) + 0x01000000;
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
#ifdef DS_ATARI
	fi->fi_cmapbits = 6;
	fi->fi_flags = d <= 8 ? FBF_BLANK | FBF_CMAP : 0;
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
#else
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
/* ---------------------------------------------------- Videl passthrough */

/*
 * The calling process passes the guest's Videl writes for s, whose
 * region it maps at win.  Process context.
 */
int
ds_vidpass(s, win)
register struct dssess *s;
unsigned long win;
{
	if (s->s_vid == 0)
		return EINVAL;
	if (win & DS_PGOFF)
		return EINVAL;
	s->s_vproc = curproc;
	s->s_vpid = curproc->p_pid;
	s->s_vwin = win;
	return 0;
}

static struct dssess *
ds_vowner()
{
	register struct dssess *s;
	register int i;

	for (i = 1; i <= DS_NSESS; i++) {
		s = &ds_sess[i];
		if (s->s_used && !s->s_dead && s->s_vid && s->s_vproc == curproc &&
		    s->s_vpid == curproc->p_pid)
			return s;
	}
	return 0;
}

#define VREG(o)		((o) >= 0xFF8200 && (o) < 0xFF82C4)
#define VPAL(o)		((o) >= 0xFF9800 && (o) < 0xFF9C00)
#define VBASE(o)	((o) == 0xFF8201 || (o) == 0xFF8203 || (o) == 0xFF820D)

/*
 * A guest write of sz bytes at a, from the owner's process.  It is
 * kept in the session's state and, while the session is in front,
 * written to the Videl.  The screen address is the guest's: it becomes
 * the same place in the pool, or the pool's start when it lies outside
 * the region.  The address counters are not written.
 */
void
ds_vidput(a, sz, v)
unsigned long a, v;
int sz;
{
	register struct dssess *s;
	register struct dsvid *d;
	register unsigned char *p;
	register unsigned long o, g;
	register int i, base = 0, other = 0;

	a &= 0xFFFFFF;
	if (sz < 1 || sz > 4 || !((VREG(a) && VREG(a + sz - 1)) ||
	    (VPAL(a) && VPAL(a + sz - 1))) || (s = ds_vowner()) == 0)
		return;
	d = s->s_vid;
	for (i = 0; i < sz; i++) {
		o = a + i;
		p = VPAL(o) ? (unsigned char *)d->v_pal + (o - 0xFF9800) : &d->v_reg[o - 0xFF8200];
		if (VBASE(o)) {
			g = s->s_vgbase;
			g &= ~(0xFFL << (o == 0xFF8201 ? 16 : o == 0xFF8203 ? 8 : 0));
			g |= ((v >> 8 * (sz - 1 - i)) & 0xFF) << (o == 0xFF8201 ? 16 : o == 0xFF8203 ? 8 : 0);
			s->s_vgbase = g;
			base = 1;
		} else if (o >= 0xFF8205 && o <= 0xFF8209)
			;
		else {
			*p = v >> 8 * (sz - 1 - i);
			other = 1;
		}
		if (o == 0xFF8260)
			d->v_st = 1;
		else if (o == 0xFF8266 || o == 0xFF8267)
			d->v_st = 0;
	}
	if (base) {
		o = s->s_vgbase - s->s_vwin;
		if (o >= s->s_size)
			o = 0;
		o += ds_disp.d_page;
		d->v_reg[0x01] = o >> 16;
		d->v_reg[0x03] = o >> 8;
		d->v_reg[0x0D] = o;
	}
	if (s != ds_front)
		return;
	if (other && !base && !(a <= 0xFF8209 && a + sz > 0xFF8205))
		ata_vput(a, sz, v);
	else if (other)
		for (i = 0; i < sz; i++)
			if (!VBASE(a + i) && (a + i < 0xFF8205 || a + i > 0xFF8209))
				ata_vput(a + i, 1, v >> 8 * (sz - 1 - i));
	if (base) {
		ata_vput(0xFF8201L, 1, (unsigned long)d->v_reg[0x01]);
		ata_vput(0xFF8203L, 1, (unsigned long)d->v_reg[0x03]);
		ata_vput(0xFF820DL, 1, (unsigned long)d->v_reg[0x0D]);
	}
	/* the ST shift mode also sets the line width and the video mode */
	if (a <= 0xFF8260 && a + sz > 0xFF8260)
		for (i = 0; i < 2; i++) {
			d->v_reg[0x10 + i] = ata_vget(0xFF8210L + i);
			d->v_reg[0xC2 + i] = ata_vget(0xFF82C2L + i);
		}
}
#endif

#ifdef DS_ATARI
/* ---------------------------------------------------- blitter passthrough */

#define BW(o)	(b[o] << 8 | b[(o) + 1])
#define BL(o)	((unsigned long)BW(o) << 16 | BW((o) + 2))
#define PB(p, v, n)	{ register int k_; for (k_ = 0; k_ < (n); k_++) (p)[k_] = (v) >> 8 * ((n) - 1 - k_); }
#define PG(a)	((a) & ~(unsigned long)DS_PGOFF)
#define NONE	1UL		/* no address: odd */

/* a blit in progress, from the guest's registers */
struct dsblit {
	struct dsbrun	t_r;
	unsigned long	t_sa, t_da;	/* the guest's next source read, destination */
	unsigned long	t_p1;		/* its last source read, or NONE */
	unsigned long	t_xn;
	int		t_line;
	int		t_op, t_hop, t_smudge, t_fxsr, t_nfsr, t_skew, t_src;
};

/* reads of the source at word j of the line, FXSR's aside */
#define NREAD(j)	(t->t_src && !((j) == t->t_xn && t->t_nfsr && t->t_xn > 1))

/*
 * The physical address of the owner's word at guest address g, where
 * the blitter reaches (ST-RAM, or the ROM when read), or NONE.  fault:
 * bring the page in first, for writing when wr, so that it is dirty.
 */
static unsigned long
ds_bpa(g, wr, fault)
unsigned long g;
int wr, fault;
{
	unsigned long pa;
	char c;

	if (g == NONE)
		return 0;
	if (fault && (copyin((caddr_t)g, &c, 1) || (wr && copyout(&c, (caddr_t)g, 1))))
		return NONE;
	pa = vtop((caddr_t)g, curproc);
	if (pa == (unsigned long)-1 || pa >= (wr ? 0xE00000L : 0xF00000L))
		return NONE;
	return pa;
}

/* whether the words at g and g + inc are adjacent in physical memory too */
static int
ds_bnext(g, inc, wr, fault)
unsigned long g;
long inc;
int wr, fault;
{
	unsigned long p, q;

	if (PG(g + inc) == PG(g))
		return 1;
	p = ds_bpa(g, wr, fault);
	q = ds_bpa(g + inc, wr, fault);
	return !(p & 1) && !(q & 1) && q == p + inc;
}

/* one run reading from sg (NONE: no read) and writing at dg; see ds_bline */
static int
ds_bstep(t, sg, dg, mode)
register struct dsblit *t;
unsigned long sg, dg;
int mode;
{
	t->t_r.r_sa = ds_bpa(sg, 0, mode == 0);
	t->t_r.r_da = ds_bpa(dg, 1, mode == 0);
	if (t->t_r.r_sa & 1 || t->t_r.r_da & 1)
		return -1;
	return mode == 2 ? ata_brun(&t->t_r) : 0;
}

/*
 * One line of t: mode 0 brings its pages in, 1 checks they are still
 * there, 2 runs it and moves t to the next line.  A run covers the part
 * of the line whose source and destination are each contiguous in
 * physical memory, so the screen's lines never split.  The source
 * buffer carries over from one run to the next, as it does from line
 * to line.  The FXSR word, when not adjacent to the first, is loaded
 * by a run that writes nothing.  A last word on its own under NFSR
 * rereads the word before and drops it, as the blitter does on a
 * one-word line.  0, or -1.
 */
static int
ds_bline(t0, b, mode)
struct dsblit *t0;
register unsigned char *b;
int mode;
{
	struct dsblit w;
	register struct dsblit *t = &w;
	unsigned long a, e, j, f, rd, sap, x;

	w = *t0;
	for (a = 1; a <= t->t_xn; a = e + 1) {
		/* f: the FXSR read; rd: word a's */
		f = NONE;
		rd = t->t_sa;
		if (t->t_src && a == 1 && t->t_fxsr) {
			f = t->t_sa;
			rd = t->t_sa + t->t_r.r_sxi;
		} else if (t->t_src && a > 1 && !NREAD(a))
			rd = t->t_p1;
		t->t_r.r_skew = t->t_skew;
		t->t_r.r_ctl = t->t_smudge | t->t_line;
		if (f != NONE && !ds_bnext(f, (long)t->t_r.r_sxi, 0, mode == 0)) {
			t->t_r.r_hop = 2;
			t->t_r.r_op = 3;
			t->t_r.r_em[0] = 0;
			t->t_r.r_xn = 1;
			if (ds_bstep(t, f, t->t_da, mode))
				return -1;
			f = NONE;
		}
		x = t->t_da;
		sap = rd;
		for (e = a; e < t->t_xn; e++) {
			if (!ds_bnext(x, (long)t->t_r.r_dxi, 1, mode == 0))
				break;
			x += t->t_r.r_dxi;
			if (NREAD(e + 1)) {
				if (!ds_bnext(sap, (long)t->t_r.r_sxi, 0, mode == 0))
					break;
				sap += t->t_r.r_sxi;
			}
		}
		t->t_r.r_hop = t->t_hop;
		t->t_r.r_op = t->t_op;
		t->t_r.r_xn = e - a + 1;
		t->t_r.r_em[0] = a == 1 ? BW(0x28) : a == t->t_xn ? BW(0x2c) : BW(0x2a);
		t->t_r.r_em[1] = BW(0x2a);
		t->t_r.r_em[2] = e == t->t_xn ? BW(0x2c) : BW(0x2a);
		t->t_r.r_skew |= (f != NONE ? 0x80 : 0) | (e == t->t_xn && t->t_nfsr ? 0x40 : 0);
		if (ds_bstep(t, !t->t_src ? NONE : f != NONE ? f : rd, t->t_da, mode))
			return -1;
		for (j = a; j <= e; j++) {
			if (t->t_src && j == 1 && t->t_fxsr)
				t->t_sa += t->t_r.r_sxi;
			if (NREAD(j)) {
				t->t_p1 = t->t_sa;
				t->t_sa += j == t->t_xn || (j == t->t_xn - 1 && t->t_nfsr) ?
				    t->t_r.r_syi : t->t_r.r_sxi;
			}
			t->t_da += j == t->t_xn ? t->t_r.r_dyi : t->t_r.r_dxi;
		}
	}
	t->t_line = (t->t_line + (t->t_r.r_dyi < 0 ? -1 : 1)) & 15;
	if (mode == 2)
		*t0 = w;
	return 0;
}

/*
 * The owner's guest started its blitter: b is its view of the
 * registers.  Each line runs on the blitter while the session is in
 * front, with interrupts that could switch sessions held off; else it
 * waits for the front.  A signal while waiting leaves the blit busy,
 * to go on when the guest next looks.  b ends as the blitter would
 * leave it.
 */
void
ds_bltgo(b)
register unsigned char *b;
{
	register struct dssess *s;
	struct dsblit t;
	unsigned long yn;
	int i, x, n, rc = -1;

	if ((s = ds_vowner()) == 0)
		goto out;
	for (i = 0; i < 16; i++)
		t.t_r.r_ht[i] = BW(2 * i);
	t.t_r.r_sxi = (short)BW(0x20);
	t.t_r.r_syi = (short)BW(0x22);
	t.t_r.r_dxi = (short)BW(0x2e);
	t.t_r.r_dyi = (short)BW(0x30);
	t.t_sa = BL(0x24) & 0xFFFFFE;
	t.t_da = BL(0x32) & 0xFFFFFE;
	t.t_p1 = NONE;
	t.t_xn = BW(0x36) ? BW(0x36) : 0x10000;
	t.t_hop = b[0x3a] & 3;
	t.t_op = b[0x3b] & 15;
	t.t_line = b[0x3c] & 15;
	t.t_smudge = b[0x3c] & 0x20;
	t.t_fxsr = b[0x3d] & 0x80;
	t.t_nfsr = b[0x3d] & 0x40;
	t.t_skew = b[0x3d] & 15;
	t.t_src = (0x7bde >> t.t_op & 1) && ((t.t_hop & 2) || (t.t_hop == 1 && t.t_smudge));
	/* a line count of 0: the blit is over, as after a busy bit set again */
	for (yn = BW(0x38); yn; yn--) {
		for (n = 0;; n++) {
			while (s != ds_front)
				if (sleep((caddr_t)&ds_front, (PZERO + 1) | PCATCH)) {
					rc = 1;
					goto out;
				}
			if (n == 8 || ds_vowner() != s || ds_bline(&t, b, 0))
				goto out;
			x = DS_SPL(DS_HI);
			if (s == ds_front && ds_bline(&t, b, 1) == 0)
				break;
			DS_SPLX(x);
		}
		i = ds_bline(&t, b, 2);
		DS_SPLX(x);
		if (i)
			goto out;
		PB(b + 0x38, yn - 1, 2);
		PB(b + 0x24, t.t_sa, 4);
		PB(b + 0x32, t.t_da, 4);
		b[0x3c] = (b[0x3c] & 0xE0) | t.t_line;
		/* a long blit lets others run and a kill end it; b holds where it is */
		if (runrun)
			preempt();
		if (yn > 1 && (curproc->p_sig & sigmask(SIGKILL))) {
			rc = 1;
			goto out;
		}
	}
	rc = 0;
out:
	DS_CPUSHA();
	if (rc <= 0)
		b[0x3c] &= 0x7F;
}
#endif
