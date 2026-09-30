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
	if (ds_disp.d_dafb && s->s_dhi > s->s_dlo) {
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
	register unsigned long size = ds_disp.d_info.fi_size, i, pa;

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
	kmem_free((caddr_t)s->s_pfn, (ds_disp.d_info.fi_size >> DS_PGSHIFT) * 4);
	kmem_free(s->s_mem, s->s_memsize);
	s->s_mem = s->s_shadow = 0;
	s->s_pfn = 0;
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
	if (ds_shadow(s) == 0) {
		s->s_used = 0;
		*errp = ENOMEM;
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

/* next live user session after s by creation order, else the console */
static struct dssess *
ds_after(s)
struct dssess *s;
{
	register struct dssess *n = 0, *lo = 0, *t;
	register int i;

	for (i = 1; i <= DS_NSESS; i++) {
		t = &ds_sess[i];
		if (!t->s_used || t->s_dead || t == s)
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
		if (ds_evh[i].e_used && ds_evh[i].e_sess == s)
			ds_evh[i].e_sess = 0;
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
			else
				ds_evpost(s, 0, IE_KEY, c, 0L, sec, us);
		}
		DS_SPLX(x);
	}
	x = DS_SPL(DS_HI);
	if (s->s_btn) {
		s->s_btn = 0;
		ds_evpost(s, 1, IE_BTN, 1, 0L, sec, us);
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
	register unsigned long n = ds_disp.d_info.fi_size;
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
		ds_lcopy((unsigned long *)from->s_shadow, (unsigned long *)vram, n);
	if (from == &ds_sess[0])
		fbcons.fc_m.fm_base = (unsigned long)ds_sess[0].s_shadow +
		    ds_disp.d_info.fi_offset;
	ds_lcopy((unsigned long *)vram, (unsigned long *)to->s_shadow, n);
	if (to == &ds_sess[0])
		fbcons.fc_m.fm_base = ds_disp.d_base;
	DS_CPUSHA();
	x = DS_SPL(DS_HI);
	ds_front = to;
	ds_serial++;
	if (ds_disp.d_dafb)
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
	ds_front = &ds_sess[0];
	fbcons.fc_m.fm_base = ds_disp.d_base;
	if (ds_disp.d_dafb)
		ds_clutload(&ds_sess[0], 0, (int)ds_disp.d_info.fi_cmapsize);
}

/* ------------------------------------------------------------- input */

static int
ds_hotmods()
{
	return (BIT(ds_phys, K_CTL) || BIT(ds_phys, K_RCTL)) &&
	    (BIT(ds_phys, K_OPT) || BIT(ds_phys, K_ROPT)) && BIT(ds_phys, K_CMD);
}

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
	ds_disp.d_dafb = m->fm_base >= DAFB_VRAM && m->fm_base < DAFB_REG;
	fi->fi_type = ds_disp.d_dafb ? FBT_DAFB : FBT_NUBUS;
	fi->fi_layout = FBL_PACKED;
	fi->fi_width = m->fm_width;
	fi->fi_height = m->fm_height;
	fi->fi_depth = d;
	fi->fi_rowbytes = m->fm_row;
	fi->fi_offset = m->fm_base - ds_disp.d_page;
	fi->fi_size = (fi->fi_offset + m->fm_row * m->fm_height + DS_PGOFF) & ~DS_PGOFF;
	end = ds_disp.d_dafb ? DAFB_REG : (ds_disp.d_page & 0xFF000000) + 0x01000000;
	if (fi->fi_size > end - ds_disp.d_page)
		return ENXIO;
	if (d == 1)
		fi->fi_visual = FBV_MONO;
	else if (d <= 8)
		fi->fi_visual = FBV_PSEUDO;
	else {
		fi->fi_visual = FBV_TRUE;
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
	fi->fi_flags = ds_disp.d_dafb ? FBF_BLANK | FBF_CMAP : 0;	/* FBF_VBL once seen */
	for (i = 0; "DAFB"[i]; i++)
		fi->fi_name[i] = ds_disp.d_dafb ? "DAFB"[i] : "slot"[i];

	ds_sess[0].s_used = 1;
	ds_sess[0].s_cache = FBC_CI;
	for (i = 0; "console"[i]; i++)
		ds_sess[0].s_name[i] = "console"[i];
	ds_stdcmap(&ds_sess[0], (int)d);
	ds_sess[0].s_dlo = 256;
	ds_sess[0].s_dhi = 0;
	ds_front = &ds_sess[0];

	ds_qi.qi_srvp = ds_srv;
	ds_q.q_qinfo = &ds_qi;
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
